#include "GlyphReader.h"
#include "DevSwitch.h"
#include "MiningData.h"
#include "EmbeddedData.h"
#include <map>
#include <set>
#include <chrono>
#include <algorithm>
#include <array>
#include <functional>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#endif

namespace SCMiner {

std::vector<cv::Mat> GlyphReader::pinTemplates_;
std::vector<cv::Mat> GlyphReader::mfdPinTemplates_;

// For the bench: what the reader's inner stages cost, added up until asked
// for (SCMINER_READER_STAGES=1, printed by --pipeline)
namespace {
std::map<std::string, std::pair<double, int>> g_readerStages;
const bool g_readerStagesOn = DEV_ENV("SCMINER_READER_STAGES") != nullptr;
struct ReaderClock {
    const char* name;
    std::chrono::steady_clock::time_point start;
    explicit ReaderClock(const char* n)
        : name(n), start(g_readerStagesOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point()) {}
    ~ReaderClock() {
        if (!g_readerStagesOn) return;
        auto& slot = g_readerStages[name];
        slot.first += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        slot.second++;
    }
};
}  // namespace

void reportReaderStages(std::ostream& out, int frames) {
    for (const auto& kv : g_readerStages) {
        out << "    " << std::left << std::setw(30) << kv.first << std::right << std::fixed << std::setprecision(1)
            << std::setw(8) << kv.second.first / (std::max)(1, frames) << " ms a frame  (" << kv.second.second / (std::max)(1, frames)
            << " calls a frame, " << std::setprecision(3) << kv.second.first / (std::max)(1, kv.second.second) << " ms each)"
            << std::endl;
    }
    g_readerStages.clear();
}

namespace {

// Pixels of the coloured plate (and its glow): clearly saturated, not dark.
// Judged by chroma rather than hue because the plate takes the ship UI colour.
constexpr int PLATE_CHROMA = 35;
constexpr int PLATE_BRIGHTNESS = 60;
// The plate must hold something at least this far above its own level for
// digits to exist; below it Otsu is splitting the plate's own noise
constexpr int MIN_DIGIT_CONTRAST = 40;
// A digit is about this wide relative to the line height (a 1 is narrower,
// a 4 slightly wider); shapes several times as wide are welded digits
constexpr double DIGIT_WIDTH_RATIO = 0.55;
// A glyph must be about as tall as the tallest one (drops the comma and the
// pin's hole) and must not be wider than a digit ever is
constexpr double MIN_RELATIVE_HEIGHT = 0.6;
constexpr double MAX_WIDTH_TO_HEIGHT = 1.3;
constexpr double MAX_DIGIT_ASPECT = 0.95;
// Shortest glyph in a row relative to the tallest (see plausibleRow).
// Measured on the benchmark: 0.75 read 46 of 50, 0.70 and 0.65 both 48, none
// wrong at any of them. 0.70 is the smallest step that admits the measured
// case - an 8 one pixel taller than its neighbours (12/17) after a plate
// mask shifted by a pixel.
constexpr double MIN_ROW_HEIGHT_RATIO = 0.70;
// How far apart two digits of one number may sit, as a fraction of their
// height - and the one exception.
//
// A number carries at most one thousands comma, the comma is below the
// baseline and shorter than a digit, so it is dropped with the pin's hole
// and leaves a hole in the row. On the canopy marker that hole is small
// enough to pass: 3,170 has digits 5 px wide and 12 tall. On the Drake
// instrument panel the same font is set wider - 3,900 measured 7 px wide
// and 11 tall, and the comma's hole came to 13 px against a limit of 8.8.
// Four clean digits, found by six of the seven channels, thrown away for
// the gap where the comma had been.
//
// So ONE gap may be a comma's width; every other gap still has to be
// text-spacing. That is what a number looks like, and it is a far
// narrower licence than simply raising the limit for all of them.
constexpr double MAX_DIGIT_GAP = 0.8;
constexpr double MAX_COMMA_GAP = 1.6;
// How many baselines out of one cluttered find are worth trying. Three
// covers a marker with the compass ribbon above and below it; past that a
// crop is not a marker crop and the pin scoring should not pay for it.
constexpr int MAX_BASELINE_ROWS = 3;
constexpr double MAX_DIGIT_FILL = 0.85;
// Which font a row is set in, by its thousands comma. The Drake face sets
// the comma's gap about one row height wide, the Prospector's under half:
// measured on real readings, Drake rows 0.85-1.24, Prospector rows
// 0.23-0.42 - and the wrong-pool readings a Prospector marker produces
// against the panel set (42,000 for a 60,000 at 93/95) sit at 0.39-0.55.
// The split is 0.65. SCMINER_FONT=canopy|panel forces.
constexpr double FONT_GAP_SPLIT = 0.65;
static int fontForced() {
    static const int forced = [] {
        const char* f = DEV_ENV("SCMINER_FONT");
        if (!f) return 0;
        const std::string s(f);
        return s == "panel" ? 1 : (s == "canopy" ? 2 : 0);
    }();
    return forced;
}
// The digit strokes' chroma as a share of the pin's, at most - see collectRows.
// Loosening it to 0.85 (for a marker whose white digits take the panel's
// amber glow, strokes at 0.73 of the pin) lets the HUD text over a 42,000
// through at 0.6-0.8, where it outranks the marker's own rows - and that
// marker fails on its pin regardless. The readout this rule is for sits at
// 1.0-1.1.
// Two table values one hesitant glyph apart - 60,000 and 80,000 on a night
// HUD, where a 6 and an 8 are seven pixels wide - cannot be told apart by
// the picture. When the runner-up of any glyph of the CHOSEN row is within
// this much of the best and spells another table value, nothing is shown:
// held out, twelve of 97 Prospector frames read the other value, every one
// a 6/8, 9/8 or 0/8. Judged on the chosen row only - rejecting the value
// inside the row search lets a worse row through (16,060 for 16,000).
// 0 switches it off.
static const double AMBIGUITY_LEAD = DEV_ENV("SCMINER_AMBIGUITY")
    ? std::atof(DEV_ENV("SCMINER_AMBIGUITY")) : 0.0;
static const double MAX_DIGIT_CHROMA_SHARE = DEV_ENV("SCMINER_DIGIT_CHROMA")
    ? std::atof(DEV_ENV("SCMINER_DIGIT_CHROMA")) : 0.5;   // 0.6 let a misread "785.5" through at 0.58
// How far a row hint is opened up into the region the thresholds are learnt
// over, in row heights: sideways enough for a digit the locator's row missed
// and the pin, above and below enough for the plate
constexpr double HINT_SIDE = 1.5;
constexpr double HINT_ABOVE = 0.5;
constexpr double HINT_BELOW = 0.5;
// ...and a single glyph is refused only as a solid block (the locator's own
// bound for a glyph-sized piece)
constexpr double SOLID_BLOCK_FILL = 0.95;
// A cut through a welded shape must hit a column that really is a gap.
// Measured on real captures: gaps between digits sit at 25-42 % of the
// densest column, the lumps sun glare welds together at 62-69 %.
// With the acceptance bar at 80/88, on the benchmark 0.68 reads 46 of 50,
// 0.70 and 0.72 read 48, none wrong; 0.75 produces a confident wrong
// reading. 0.70 is the smallest step that admits a sun-blown 12,000 whose
// one valley sits at 69 %. Under a lower acceptance bar even 0.58 cost more
// than it gained - 3,185 stopped reading and 12,000 became 4,000.
constexpr double MAX_CUT_FILL = 0.70;
constexpr int MIN_GLYPH_HEIGHT = 5;
// A pin template has to have contrast. A blank strip - harvested from a
// crop whose black margin the strip fell into - correlates with anything
// and makes every pin score 100 %.
constexpr double MIN_PIN_STDDEV = 12.0;
// The share of the digit threshold the thousands comma has to reach
constexpr double SEPARATOR_THRESHOLD_SHARE = 0.7;
// The pin glyph owns the left part of the plate; the digits never start there
constexpr double PIN_ZONE = 0.20;
// Crops shorter than this are analysed at double size - see analyseScene
constexpr int SMALL_MARKER_ROWS = 30;
// Digits shorter than this get a second look at double size - see read()
constexpr int SMALL_GLYPH_PX = 13;
// A crop this size or smaller (after any doubling) can BE a marker, so when no
// plate stands out in it the whole crop is taken as the plate
constexpr int MARKER_CROP_MAX_COLS = 520;
constexpr int MARKER_CROP_MAX_ROWS = 180;
// Tried and measured WORSE: rejecting low-variance pin strips (stddev < 0.1)
// to stop featureless background from correlating. It cost three real night
// readings (45/50 -> 43/50) and the false positives it was meant to stop are
// better handled by the score threshold itself.
// The plate around a digit must be at least this colourful
constexpr int RING_CHROMA = 15;
// ...unless the ship's lamp has washed the colour out of it. Measured on a
// lamp-lit 17,080 in the Prospector: the plate keeps only chroma 13 at
// brightness 203, while the blue world behind it runs the other way round -
// chroma 115 at brightness 65. The whole "coloured plate, neutral digits"
// assumption inverts. What survives is that the plate is BRIGHT and the
// strokes on it are brighter still, so a washed plate is allowed in on that
// instead. Both bars matter: without the brightness floor every dark HUD
// label would qualify.
constexpr int MIN_PLATE_LUMA = 120;
constexpr int MIN_RING_CONTRAST = 25;
// Digits are neutral: below this chroma a pixel counts as uncoloured
constexpr int DIGIT_MAX_CHROMA = 18;

// The box round the set pixels of an 8-bit mask, as cv::boundingRect
// gives it (empty when nothing is set)
cv::Rect nonZeroBox(const cv::Mat& mask) {
    int minX = mask.cols, maxX = -1, minY = mask.rows, maxY = -1;
    for (int y = 0; y < mask.rows; y++) {
        const uchar* m = mask.ptr<uchar>(y);
        int first = -1, last = -1;
        for (int x = 0; x < mask.cols; x++) {
            if (!m[x]) continue;
            if (first < 0) first = x;
            last = x;
        }
        if (first < 0) continue;
        minX = (std::min)(minX, first);
        maxX = (std::max)(maxX, last);
        minY = (std::min)(minY, y);
        maxY = y;
    }
    if (maxX < 0) return cv::Rect();
    return cv::Rect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

// How many pixels of an 8-bit mask are set
int countSet(const cv::Mat& mask) {
    int n = 0;
    for (int y = 0; y < mask.rows; y++) {
        const uchar* m = mask.ptr<uchar>(y);
        for (int x = 0; x < mask.cols; x++) n += m[x] != 0;
    }
    return n;
}

// Scales a binary crop into the template grid, keeping its proportions
cv::Mat normaliseGlyph(const cv::Mat& crop) {
    const double scale = (std::min)(static_cast<double>(GlyphReader::GLYPH_W) / crop.cols,
                                    static_cast<double>(GlyphReader::GLYPH_H) / crop.rows);
    const int w = (std::max)(1, static_cast<int>(std::lround(crop.cols * scale)));
    const int h = (std::max)(1, static_cast<int>(std::lround(crop.rows * scale)));

    cv::Mat scaled;
    cv::resize(crop, scaled, cv::Size(w, h), 0, 0, cv::INTER_AREA);

    // Centred on a blank grid, as floats of 0..1 - the scaling by 1/255 in
    // single precision, which is how convertTo did it
    cv::Mat result = cv::Mat::zeros(GlyphReader::GLYPH_H, GlyphReader::GLYPH_W, CV_32F);
    const int x0 = (GlyphReader::GLYPH_W - w) / 2;
    const int y0 = (GlyphReader::GLYPH_H - h) / 2;
    const float unit = static_cast<float>(1.0 / 255.0);
    for (int y = 0; y < h; y++) {
        const uchar* src = scaled.ptr<uchar>(y);
        float* dst = result.ptr<float>(y0 + y) + x0;
        for (int x = 0; x < w; x++) dst[x] = static_cast<float>(src[x]) * unit;
    }
    return result;
}

// The level an 8-bit pixel has to exceed for cv::compare to call it
// "greater than t": OpenCV takes the floor of a fractional t. Returns
// false when the answer is the same for every pixel, and says in `all`
// what it is (t below 0: everything; above 255: nothing).
inline bool greaterLevel8u(double t, int* level, bool* all) {
    if (t < 0.0) { *all = true; return false; }
    if (t > 255.0) { *all = false; return false; }
    *level = cvFloor(t);
    return true;
}

// Otsu's threshold over a histogram of `count` 8-bit values, step for step
// as OpenCV 4.12's getThreshVal_Otsu computes it (the same sums in the
// same order, so the same level comes out of it), without the copy of the
// samples cv::threshold needs - it runs 690 times a signature scan.
double otsuLevel(const int* h, int count) {
    const int N = 256;
    double mu = 0, scale = 1. / count;
    for (int i = 0; i < N; i++) mu += i * (double)h[i];
    mu *= scale;
    double mu1 = 0, q1 = 0;
    double max_sigma = 0, max_val = 0;
    for (int i = 0; i < N; i++) {
        double p_i, q2, mu2, sigma;
        p_i = h[i] * scale;
        mu1 *= q1;
        q1 += p_i;
        q2 = 1. - q1;
        if ((std::min)(q1, q2) < FLT_EPSILON || (std::max)(q1, q2) > 1. - FLT_EPSILON) continue;
        mu1 = (mu1 + i * p_i) / q1;
        mu2 = (mu - q1 * mu1) / q2;
        sigma = q1 * q2 * (mu1 - mu2) * (mu1 - mu2);
        if (sigma > max_sigma) {
            max_sigma = sigma;
            max_val = i;
        }
    }
    // cv::threshold hands the level back floored - it is a whole number
    return cvFloor(max_val);
}

// Otsu threshold over the channel values inside the mask - and, when
// `above` is 0 or more, only those over that level (the peel: the cut
// learnt again within the bright class of the first)
double learnThreshold(const cv::Mat& channel, const cv::Mat& mask, const cv::Rect& within, double above = -1.0) {
    int level = 0;
    bool all = true;
    const bool varies = above >= 0.0 ? greaterLevel8u(above, &level, &all) : false;
    int h[256] = {0};
    int count = 0;
    for (int y = within.y; y < within.br().y; y++) {
        const uchar* m = mask.ptr<uchar>(y);
        const uchar* c = channel.ptr<uchar>(y);
        for (int x = within.x; x < within.br().x; x++) {
            if (!m[x]) continue;
            if (varies ? c[x] <= level : !all) continue;
            h[c[x]]++;
            count++;
        }
    }
    if (count < 50) return -1.0;
    return otsuLevel(h, count);
}

// Cuts the bright shapes of one channel inside the plate into digit-sized
// pieces, left to right
std::vector<GlyphReader::Glyph> segmentChannel(const cv::Mat& channel, const cv::Mat& roi,
                                               const cv::Rect& plate, double threshold,
                                               const cv::Mat& chroma) {
    std::vector<GlyphReader::Glyph> glyphs;

    ReaderClock* maskClock = new ReaderClock("    seg: mask + components");
    // The pixels over the threshold inside the plate, in one pass into a
    // buffer kept between calls (two OpenCV expressions would cost two
    // allocations and two passes a channel). An empty mask has no
    // components, so it is not handed on.
    CV_Assert(channel.size() == roi.size() && channel.type() == CV_8U && roi.type() == CV_8U);
    int level = 0;
    bool all = false;
    const bool varies = greaterLevel8u(threshold, &level, &all);
    thread_local cv::Mat digitMask;
    digitMask.create(channel.size(), CV_8U);
    // The plate mask lies inside the plate's box (see analyseScene: the
    // box is the mask's, or the mask is the box), so that box is all the
    // mask that has to be looked at - plus one pixel up and left, which
    // the even alignment of the labelled box below can reach into and
    // which must not be left over from the last call
    const cv::Rect picture(0, 0, channel.cols, channel.rows);
    const cv::Rect plateBox = plate & picture;
    const cv::Rect within = cv::Rect(plateBox.x - 1, plateBox.y - 1, plateBox.width + 1, plateBox.height + 1) & picture;
    int minX = channel.cols, maxX = -1, minY = channel.rows, maxY = -1;
    for (int y = within.y; y < within.br().y; y++) {
        const uchar* c = channel.ptr<uchar>(y);
        const uchar* r = roi.ptr<uchar>(y);
        uchar* m = digitMask.ptr<uchar>(y);
        int first = -1, last = -1;
        for (int x = within.x; x < within.br().x; x++) {
            const bool on = r[x] != 0 && (varies ? c[x] > level : all);
            m[x] = on ? 255 : 0;
            if (on) {
                if (first < 0) first = x;
                last = x;
            }
        }
        if (first < 0) continue;
        minX = (std::min)(minX, first);
        maxX = (std::max)(maxX, last);
        minY = (std::min)(minY, y);
        maxY = y;
    }
    // ...and when nothing set is as tall as a glyph, no shape can be
    // (every shape would be dropped for its height below)
    static const bool tracePieces = DEV_ENV("SCMINER_GLYPH_TRACE") != nullptr;
    if (maxX < 0 || (!tracePieces && maxY - minY + 1 < MIN_GLYPH_HEIGHT)) {
        delete maskClock;
        return glyphs;
    }
    // The components are labelled inside the box round the set pixels
    // only; the rest of the crop is blank and labels nothing. The box
    // starts on an even row and an even column so that OpenCV's 2 x 2
    // block grid - and with it the order it hands the components back in,
    // which is the order of their first block - is the whole crop's.
    const cv::Rect sub(minX & ~1, minY & ~1, maxX - (minX & ~1) + 1, maxY - (minY & ~1) + 1);
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(digitMask(sub), labels, stats, centroids);
    delete maskClock;
    // The labels of one crop row, and a label at crop coordinates - 0
    // outside the labelled box
    auto labelRowPtr = [&](int y) -> const int* {
        return (y >= sub.y && y < sub.br().y) ? labels.ptr<int>(y - sub.y) : nullptr;
    };
    auto labelAt = [&](const int* row, int x) -> int {
        return (row && x >= sub.x && x < sub.br().x) ? row[x - sub.x] : 0;
    };
    ReaderClock* ringClock = new ReaderClock("    seg: ring tests");

    struct Piece { cv::Rect box; int index; };
    std::vector<Piece> pieces;
    int tallest = 0;
    for (int i = 1; i < count; i++) {
        cv::Rect box(stats.at<int>(i, cv::CC_STAT_LEFT) + sub.x, stats.at<int>(i, cv::CC_STAT_TOP) + sub.y,
                     stats.at<int>(i, cv::CC_STAT_WIDTH), stats.at<int>(i, cv::CC_STAT_HEIGHT));
        // Shapes running into the edge of the capture are the scene leaking in
        if (box.x <= 0 || box.y <= 0 ||
            box.br().x >= channel.cols || box.br().y >= channel.rows) continue;
        if (box.height < MIN_GLYPH_HEIGHT) continue;

        // A digit is a neutral shape embedded in the coloured plate, so the
        // pixels immediately around it must be coloured. Bright terrain or
        // sky that slipped into the plate area is surrounded by more of the
        // same and fails this.
        cv::Rect around(box.x - 2, box.y - 2, box.width + 4, box.height + 4);
        around &= cv::Rect(0, 0, channel.cols, channel.rows);
        // The ring is the shape grown by two pixels each way (a 5 x 5
        // dilation) less the shape itself, inside `around`. Summed in one
        // pass over that little rectangle; six OpenCV calls a component cost
        // 178 ms of a 388 ms signature scan with nothing on screen. The means
        // are formed the way cv::mean forms them - the sum times one over the
        // count - so the comparisons match cv::mean to the last bit.
        // The dilation is separable: each row of the shape is widened by
        // two pixels, then a pixel is in the grown shape when one of the
        // five widened rows about it is set there. Two little buffers of
        // `around`'s size, kept between components.
        const int aw = around.width, ah = around.height;
        thread_local std::vector<uchar> shapeBuf, wideBuf;
        shapeBuf.assign(static_cast<size_t>(aw) * ah, 0);
        wideBuf.assign(static_cast<size_t>(aw) * ah, 0);
        for (int y = 0; y < ah; y++) {
            const int* lab = labelRowPtr(around.y + y);
            uchar* sh = shapeBuf.data() + static_cast<size_t>(y) * aw;
            uchar* wd = wideBuf.data() + static_cast<size_t>(y) * aw;
            for (int x = 0; x < aw; x++) sh[x] = labelAt(lab, around.x + x) == i ? 1 : 0;
            int since = 1000;   // pixels since the last set one, looking left
            for (int x = 0; x < aw; x++) {
                since = sh[x] ? 0 : since + 1;
                wd[x] = since <= 2;
            }
            since = 1000;       // ...and looking right
            for (int x = aw - 1; x >= 0; x--) {
                since = sh[x] ? 0 : since + 1;
                if (since <= 2) wd[x] = 1;
            }
        }
        long long ringChroma = 0, ringLuma = 0, shapeLuma = 0;
        int ringCount = 0, shapeCount = 0;
        for (int y = 0; y < ah; y++) {
            const uchar* chr = chroma.ptr<uchar>(around.y + y) + around.x;
            const uchar* lum = channel.ptr<uchar>(around.y + y) + around.x;
            const uchar* sh = shapeBuf.data() + static_cast<size_t>(y) * aw;
            const int v0 = (std::max)(0, y - 2), v1 = (std::min)(ah - 1, y + 2);
            for (int x = 0; x < aw; x++) {
                if (sh[x]) {
                    shapeLuma += lum[x];
                    shapeCount++;
                    continue;
                }
                bool touches = false;
                for (int v = v0; v <= v1; v++) {
                    if (wideBuf[static_cast<size_t>(v) * aw + x]) {
                        touches = true;
                        break;
                    }
                }
                if (!touches) continue;
                ringChroma += chr[x];
                ringLuma += lum[x];
                ringCount++;
            }
        }
        if (ringCount == 0) continue;
        if (static_cast<double>(ringChroma) * (1.0 / ringCount) < RING_CHROMA) {
            // No colour left in the ring. Either this is terrain leaking into
            // the crop, or the lamp has bleached the plate - and those two are
            // told apart by brightness: a bleached plate stays bright and the
            // digit on it brighter still, terrain does neither.
            const double ringMean = static_cast<double>(ringLuma) * (1.0 / ringCount);
            const double shapeMean = static_cast<double>(shapeLuma) * (1.0 / shapeCount);
            if (ringMean < MIN_PLATE_LUMA) continue;
            if (shapeMean - ringMean < MIN_RING_CONTRAST) continue;
        }

        pieces.push_back({box, i});
        tallest = (std::max)(tallest, box.height);
    }
    delete ringClock;
    ReaderClock piecesClock("    seg: pieces + cuts");
    if (tracePieces) {
        std::cerr << "  [glyph]   raw pieces:";
        for (const auto& piece : pieces) {
            std::cerr << " " << piece.box.x << "," << piece.box.y << " "
                      << piece.box.width << "x" << piece.box.height;
        }
        std::cerr << std::endl;
    }
    if (tallest < MIN_GLYPH_HEIGHT) return glyphs;

    for (const auto& piece : pieces) {
        const cv::Rect& box = piece.box;
        if (box.height < tallest * MIN_RELATIVE_HEIGHT) continue;
        // The pin sits at the left end of the plate; nothing there is a digit
        if (box.x + box.width / 2 < plate.x + plate.width * PIN_ZONE) continue;

        // The shape's own pixels as a mask, straight from the labels
        cv::Mat crop(box.height, box.width, CV_8U);
        for (int y = 0; y < box.height; y++) {
            const int* lab = labels.ptr<int>(box.y - sub.y + y) + (box.x - sub.x);
            uchar* m = crop.ptr<uchar>(y);
            for (int x = 0; x < box.width; x++) m[x] = lab[x] == piece.index ? 255 : 0;
        }

        // The glow can weld neighbouring digits into one shape. A digit is
        // never much wider than it is tall, so a wide shape is several digits
        // and gets cut at its thinnest columns.
        // How many digits are welded into this block. Rounding the estimate
        // to nearest was losing one digit whenever the answer sat near a half:
        // measured on a sun-blown "12,000", the whole value came through as a
        // single 37x15 lump, 37 / (0.5 * 15) = 4.9 rounded to 4, and the four
        // pieces read 4394. Erring upwards costs nothing when it is wrong - an
        // over-split piece fails to match any digit template and the row is
        // rejected - while erring downwards silently produces a plausible
        // wrong number, which is the worse of the two.
        const int parts = (std::max)(1, static_cast<int>(std::ceil(
            box.width / (DIGIT_WIDTH_RATIO * tallest) - 0.25)));
        if (parts == 1 && box.width > box.height * MAX_WIDTH_TO_HEIGHT) continue;

        std::vector<int> cuts{0};
        bool cleanCuts = true;
        if (parts > 1) {
            // Set pixels per column, and the fullest column
            std::vector<int> profile(box.width, 0);
            for (int y = 0; y < box.height; y++) {
                const uchar* m = crop.ptr<uchar>(y);
                for (int x = 0; x < box.width; x++) profile[x] += m[x] != 0;
            }
            double maxColumn = 0.0;
            for (int x = 0; x < box.width; x++) maxColumn = (std::max)(maxColumn, static_cast<double>(profile[x]));
            for (int p = 1; p < parts; p++) {
                const int centre = box.width * p / parts;
                const int radius = (std::max)(1, box.width / (parts * 4));
                int bestX = centre;
                int bestSum = INT_MAX;
                const int from = (std::max)(cuts.back() + 2, centre - radius);
                const int to = (std::min)(box.width - 2, centre + radius);
                for (int x = from; x <= to; x++) {
                    const int columnSum = profile[x];
                    if (columnSum < bestSum) {
                        bestSum = columnSum;
                        bestX = x;
                    }
                }
                // A real gap between digits is nearly empty. When the glow
                // has filled it, cutting at the arithmetic centre lands
                // anywhere - and a 1 next to a 2 turns into two 6s - so a
                // weld without a true valley is left to the OCR fallback.
                // Overridable for measurement: SCMINER_CUT_FILL=0.70
                static const double cutFill = DEV_ENV("SCMINER_CUT_FILL")
                    ? std::atof(DEV_ENV("SCMINER_CUT_FILL")) : MAX_CUT_FILL;
                if (bestSum > maxColumn * cutFill) cleanCuts = false;
                if (tracePieces) {
                    std::cerr << "  [glyph]   cut " << box.width << "x" << box.height << " into "
                              << parts << " at " << bestX << ": column " << bestSum
                              << " of max " << static_cast<int>(maxColumn) << std::endl;
                }
                cuts.push_back(bestX);
            }
        }
        if (!cleanCuts) continue;
        cuts.push_back(box.width);

        for (size_t p = 0; p + 1 < cuts.size(); p++) {
            cv::Rect part(cuts[p], 0, cuts[p + 1] - cuts[p], box.height);
            if (part.width < 1) continue;
            cv::Mat partCrop = crop(part);
            // Trim the empty rows and columns a cut leaves behind
            const cv::Rect tight = nonZeroBox(partCrop);
            if (tracePieces) {
                std::cerr << "  [glyph]     part " << p << ": " << tight.width << "x" << tight.height
                          << ((tight.width < 1 || tight.height < MIN_GLYPH_HEIGHT) ? " dropped" : "")
                          << std::endl;
            }
            if (tight.width < 1 || tight.height < MIN_GLYPH_HEIGHT) continue;
            partCrop = partCrop(tight);

            GlyphReader::Glyph glyph;
            glyph.bitmap = normaliseGlyph(partCrop);
            glyph.box = cv::Rect(box.x + part.x + tight.x, box.y + tight.y,
                                 tight.width, tight.height);
            glyph.aspect = static_cast<double>(tight.width) / tight.height;
            glyph.fill = countSet(partCrop) / static_cast<double>(tight.area());
            glyphs.push_back(std::move(glyph));
        }
    }

    std::sort(glyphs.begin(), glyphs.end(),
              [](const GlyphReader::Glyph& a, const GlyphReader::Glyph& b) {
                  return a.box.x < b.box.x;
              });
    return glyphs;
}

// A signature is 4-6 digits of one height on one baseline, spaced like text
bool plausibleRow(const std::vector<GlyphReader::Glyph>& glyphs) {
    if (glyphs.size() < 4 || glyphs.size() > 6) return false;
    int minH = INT_MAX, maxH = 0, minY = INT_MAX, maxY = 0, widestGap = 0;
    int maxStep = 0;   // largest baseline shift between neighbours
    int commaGaps = 0;
    double fillSum = 0.0;
    for (size_t i = 0; i < glyphs.size(); i++) {
        const cv::Rect& box = glyphs[i].box;
        minH = (std::min)(minH, box.height);
        maxH = (std::max)(maxH, box.height);
        minY = (std::min)(minY, box.y);
        maxY = (std::max)(maxY, box.y);
        // Digits of this font are at most about 0.7 as wide as tall and are
        // strokes, not blobs. Sun glare bloats them into solid lumps that would
        // otherwise match some template confidently - and wrongly. The fill
        // is judged over the ROW, though: a Drake 8 at 7x12 px fills 86-91 %
        // of its box with glow round its bars and is still an 8 among four
        // digits at 58-80 %. Only a solid block is refused on its own.
        if (glyphs[i].aspect > MAX_DIGIT_ASPECT || glyphs[i].fill > SOLID_BLOCK_FILL) return false;
        fillSum += glyphs[i].fill;
        if (i > 0) widestGap = (std::max)(widestGap, box.x - glyphs[i - 1].box.br().x);
        if (i > 0) maxStep = (std::max)(maxStep, std::abs(box.y - glyphs[i - 1].box.y));
    }
    // Which gaps are too wide for plain text spacing. One of those is a
    // comma and is forgiven; a second one means this is not one number.
    for (size_t i = 1; i < glyphs.size(); i++) {
        const int gap = glyphs[i].box.x - glyphs[i - 1].box.br().x;
        if (gap > maxH * MAX_DIGIT_GAP) commaGaps++;
    }
    // How uniform the heights must be. Overridable for measurement:
    // SCMINER_ROW_HEIGHT=0.70. An 8480 at 12/16 = 0.75 drops to 12/17 = 0.71
    // when a one-pixel shift of the plate mask makes its 8 a pixel taller.
    static const double minHeightRatio = DEV_ENV("SCMINER_ROW_HEIGHT")
        ? std::atof(DEV_ENV("SCMINER_ROW_HEIGHT")) : MIN_ROW_HEIGHT_RATIO;
    if (commaGaps > 1) return false;
    if (fillSum / glyphs.size() > MAX_DIGIT_FILL) return false;
    const double gapLimit = commaGaps == 1 ? MAX_COMMA_GAP : MAX_DIGIT_GAP;
    // The baseline: level within 0.3 of the height, OR tilted evenly. The
    // Drake canopy plate is drawn in perspective and its digits climb a pixel
    // at a time - 16,840 measures y 53 to 57 across the row, over the 3.6 px
    // a 12 px digit is allowed, and with the level rule alone trimToRow sheds
    // the last digit and the row reads 1684. A row whose neighbours never jump
    // more than a fifth of the height is one line however far it drifts over
    // its length (bounded at 0.6); a broken row shows as a jump.
    const bool level = (maxY - minY) <= maxH * 0.3;
    const bool tilted = maxStep <= maxH * 0.2 && (maxY - minY) <= maxH * 0.6;
    return minH >= maxH * minHeightRatio && (level || tilted) &&
           widestGap <= maxH * gapLimit;
}

// Shedding one shape from either end of what a channel found. The pin on the
// left or a stray highlight on the right can ride along, and the digits still
// form a regular row without it.
void trimToRow(std::vector<GlyphReader::Glyph>& found) {
    if (plausibleRow(found) || found.size() <= 4) return;
    std::vector<GlyphReader::Glyph> withoutFirst(found.begin() + 1, found.end());
    std::vector<GlyphReader::Glyph> withoutLast(found.begin(), found.end() - 1);
    if (plausibleRow(withoutFirst)) { found = std::move(withoutFirst); return; }
    if (plausibleRow(withoutLast)) { found = std::move(withoutLast); return; }
    if (found.size() > 5) {
        std::vector<GlyphReader::Glyph> withoutBoth(found.begin() + 1, found.end() - 1);
        if (plausibleRow(withoutBoth)) found = std::move(withoutBoth);
    }
}

// More shapes than a signature has does not mean junk. The crop usually holds
// a second line of HUD text as well - under LAMP over a planet it is the
// compass ribbon a few pixels below the marker - and the marker's own digits
// sit among them in perfect order, while plausibleRow() refuses anything
// past six shapes.
//
// So the find is split by baseline first, using the same measure the row test
// uses for one row - tops within 0.3 of the tallest shape, or climbing evenly
// (the Drake canopy plate is drawn in perspective). Each group is then
// offered as its own candidate. Nothing downstream is relaxed: a group still
// has to pass plausibleRow, still gets its OWN pin strip checked, and still
// has to clear goodEnough. A group that is not the marker therefore costs
// exactly what finding nothing costs.
std::vector<std::vector<GlyphReader::Glyph>> rowsByBaseline(
        const std::vector<GlyphReader::Glyph>& glyphs) {
    std::vector<std::vector<GlyphReader::Glyph>> rows;
    std::vector<bool> taken(glyphs.size(), false);
    for (size_t i = 0; i < glyphs.size(); i++) {
        if (taken[i]) continue;
        std::vector<GlyphReader::Glyph> row{glyphs[i]};
        taken[i] = true;
        int top = glyphs[i].box.y, bottom = glyphs[i].box.y, tall = glyphs[i].box.height;
        int lastY = glyphs[i].box.y;   // the neighbour to the left, shapes come in x order
        for (size_t j = i + 1; j < glyphs.size(); j++) {
            if (taken[j]) continue;
            // A shape of another height is another line - or the pin. The
            // tolerance below scales with the tallest shape in the group, so
            // a 20 px pin fragment that seeds a group pulls 12 px digits in
            // across 6 px of baseline (pin, digits and a plate corner end up in
            // one group of eight, nothing left to read). A row's heights agree
            // within MIN_ROW_HEIGHT_RATIO anyway, so a shape outside that can be
            // refused here.
            const int h = glyphs[j].box.height;
            if ((std::min)(h, tall) < (std::max)(h, tall) * MIN_ROW_HEIGHT_RATIO) continue;
            const int y = glyphs[j].box.y;
            const int newTop = (std::min)(top, y), newBottom = (std::max)(bottom, y);
            const int newTall = (std::max)(tall, glyphs[j].box.height);
            // The same two shapes of a row plausibleRow() accepts: level
            // within 0.3 of the height, or climbing evenly - no step over a
            // fifth, the whole drift under 0.6. With only the level rule the
            // Drake canopy's 16,840 (y 53 to 57, 12 px tall) lost its last
            // digit HERE, before the tilt rule ever saw the row, and read 1684.
            const bool level = newBottom - newTop <= newTall * 0.3;
            const bool tilted = std::abs(y - lastY) <= newTall * 0.2 &&
                                newBottom - newTop <= newTall * 0.6;
            if (!level && !tilted) continue;
            // A row is contiguous: nothing joins across more than a comma's
            // gap, and the shapes come in x order, so past that gap there
            // is nothing left to join. Without this a 3 px sliver at the
            // plate's left edge seeded a group, took the four digits level
            // with it, and left the 6 of a Drake 68,000 - one pixel higher
            // than the rest - to itself: the row read 8,000, a table value.
            const int gap = glyphs[j].box.x - (row.back().box.x + row.back().box.width);
            if (gap > newTall * MAX_COMMA_GAP) break;
            row.push_back(glyphs[j]);
            taken[j] = true;
            top = newTop; bottom = newBottom; tall = newTall; lastY = y;
        }
        rows.push_back(std::move(row));
    }
    // The marker is the foreground element, so its digits are the tallest
    // thing on the crop; trying the tallest group first keeps the extra pin
    // scoring bounded when several lines are in view.
    std::sort(rows.begin(), rows.end(),
              [](const std::vector<GlyphReader::Glyph>& a,
                 const std::vector<GlyphReader::Glyph>& b) {
                  const auto tallest = [](const std::vector<GlyphReader::Glyph>& r) {
                      int h = 0;
                      for (const auto& g : r) h = (std::max)(h, g.box.height);
                      return h;
                  };
                  return tallest(a) > tallest(b);
              });
    return rows;
}

// The game writes every signature with a thousands separator: 8,390 - 16,960
// - 123,456. The HUD elements that keep being read as signatures do not: the
// status pills (61% 92% 93%), the mining module panel, a distance, an MFD
// label. All of them are a row of digits beside a coloured glyph on a plate,
// which is exactly what a marker is - the comma is the one thing they lack.
//
// It sits on the baseline in the gap three digits from the right end. Only
// the true gap between the two digits is examined: reaching into either digit
// to catch a comma the glow has welded on would find the digit itself and
// answer yes for everything.
bool hasThousandsSeparator(const cv::Mat& channel, double threshold,
                           const std::vector<GlyphReader::Glyph>& row) {
    if (row.size() < 4) return false;
    const cv::Rect& before = row[row.size() - 4].box;
    const cv::Rect& after = row[row.size() - 3].box;

    int top = INT_MAX, bottom = 0;
    for (const auto& g : row) {
        top = (std::min)(top, g.box.y);
        bottom = (std::max)(bottom, g.box.br().y);
    }
    const int height = bottom - top;
    if (height < 6) return false;

    cv::Rect gap(before.br().x, bottom - static_cast<int>(std::lround(height * 0.45)),
                 after.x - before.br().x,
                 static_cast<int>(std::lround(height * 0.7)));
    gap &= cv::Rect(0, 0, channel.cols, channel.rows);
    if (gap.width < 1 || gap.height < 2) return false;

    // A lower bar than the digits get: the comma is two pixels wide, so
    // antialiasing leaves it dimmer than any stroke the threshold was learnt
    // from. At the digit threshold it was found on only 13 of 46 real markers.
    int level = 0;
    bool all = false;
    const bool varies = greaterLevel8u(threshold * SEPARATOR_THRESHOLD_SHARE, &level, &all);
    int found = 0;
    for (int y = gap.y; y < gap.br().y; y++) {
        const uchar* c = channel.ptr<uchar>(y);
        for (int x = gap.x; x < gap.br().x; x++) found += (varies ? c[x] > level : all);
    }
    return found >= 2;
}

// The area the pin occupies relative to a digit row: just left of the first
// digit, a little taller than the digits (the pin rises above them)
cv::Rect pinStripFor(const std::vector<GlyphReader::Glyph>& row, const cv::Size& size) {
    int rowLeft = INT_MAX, rowTop = INT_MAX, rowBottom = 0;
    std::vector<int> heights;
    for (const auto& g : row) {
        rowLeft = (std::min)(rowLeft, g.box.x);
        rowTop = (std::min)(rowTop, g.box.y);
        rowBottom = (std::max)(rowBottom, g.box.br().y);
        heights.push_back(g.box.height);
    }
    std::sort(heights.begin(), heights.end());
    const double h = heights[heights.size() / 2];

    // Not used: a wider strip with the template slid across it, so the match
    // would not depend on the pin sitting exactly where the row predicts. It
    // gains nothing and narrows the gap to contact labels from 88-100 vs 53
    // down to 70-81 vs 53-63.
    cv::Rect strip(static_cast<int>(std::lround(rowLeft - 2.3 * h)),
                   static_cast<int>(std::lround(rowTop - 0.6 * h)),
                   static_cast<int>(std::lround(2.15 * h)),
                   static_cast<int>(std::lround(rowBottom - rowTop + 0.95 * h)));
    strip &= cv::Rect(0, 0, size.width, size.height);
    return strip;
}

// The area the pin occupies relative to the plate alone: its left end
cv::Rect pinStripForPlate(const cv::Rect& plate, const cv::Size& size) {
    cv::Rect strip(plate.x, plate.y, static_cast<int>(std::lround(plate.height * 1.0)), plate.height);
    strip &= cv::Rect(0, 0, size.width, size.height);
    return strip;
}

// Scales a chroma strip so its height matches the template grid, leaving the
// width free: the pin then occupies part of the strip and can be searched for
// rather than having to fill it exactly.
cv::Mat normalisePin(const cv::Mat& chromaStrip, int margin = 0) {
    cv::Mat scaled;
    cv::resize(chromaStrip, scaled,
               cv::Size(GlyphReader::PIN_W + 2 * margin, GlyphReader::PIN_H + 2 * margin),
               0, 0, cv::INTER_AREA);
    cv::Mat result;
    scaled.convertTo(result, CV_32F, 1.0 / 255.0);
    cv::GaussianBlur(result, result, cv::Size(3, 3), 0.8);
    return result;
}

// The strip, widened by the same fraction the normalised grid is widened
// by, so the pin inside it keeps its size and only gains room around it.
// Returns an empty rect when the picture has no room to give - at the edge
// of a capture the geometry would change instead, which is worse than not
// sliding at all.
cv::Rect grownStrip(const cv::Rect& strip, const cv::Size& bounds, int margin) {
    if (margin <= 0) return strip;
    const int dx = (int)std::lround((double)strip.width * margin / GlyphReader::PIN_W);
    const int dy = (int)std::lround((double)strip.height * margin / GlyphReader::PIN_H);
    const cv::Rect grown(strip.x - dx, strip.y - dy, strip.width + 2 * dx, strip.height + 2 * dy);
    if ((grown & cv::Rect(0, 0, bounds.width, bounds.height)) != grown) return {};
    return grown;
}

} // namespace

// How well the strip beside the digits looks like the map pin - judged on its
// SHAPE, not on its colour.
//
// The templates are harvested as chroma patches, but comparing the strip as
// chroma makes the answer depend on the pin being more colourful than the
// plate behind it, which is only true in some lighting: under HDR the
// relationship inverts and the same pin scores -21 % to -37 %, a perfect
// match with the contrast the wrong way round; against the sun it weakens to
// 63 %, under the locator's bar.
//
// So the strip is offered in four forms: its colourfulness and its brightness,
// each in both polarities. Normalised correlation already ignores brightness
// and contrast; taking the best of these four adds indifference to WHICH
// channel carries the pin and to which way round it sits. What is left is the
// shape, which is the only thing that actually identifies a map pin.
// A picture with its mean taken out, as a flat vector, and that vector's
// length - the two things a normalised correlation needs from each side
struct Centred {
    std::vector<float> values;
    double norm = 0.0;
};

Centred centre(const cv::Mat& input) {
    Centred c;
    cv::Mat m = input;
    if (m.type() != CV_32F) m.convertTo(m, CV_32F);
    const int n = m.rows * m.cols;
    c.values.assign(static_cast<size_t>(n), 0.0f);
    double mean = 0.0;
    for (int y = 0; y < m.rows; y++) {
        const float* row = m.ptr<float>(y);
        for (int x = 0; x < m.cols; x++) mean += row[x];
    }
    mean /= (std::max)(1, n);
    double squares = 0.0;
    int i = 0;
    for (int y = 0; y < m.rows; y++) {
        const float* row = m.ptr<float>(y);
        for (int x = 0; x < m.cols; x++, i++) {
            const double d = row[x] - mean;
            c.values[i] = static_cast<float>(d);
            squares += d * d;
        }
    }
    c.norm = std::sqrt(squares);
    return c;
}

// The pin templates, centred once at load - filled by loadTemplates, index
// for index with GlyphReader::pinTemplates_
std::vector<Centred> g_pinCentred;
// ...and the panel pins' own. pinScore takes the cache by index once the
// sizes agree, so a second set sharing the first's cache would correlate
// against the wrong vectors without a word - each set carries its own.
std::vector<Centred> g_mfdPinCentred;
// ...and per template family: the raw pins as loaded, and per scene family
// the legacy pins plus the family's own (see GlyphReader::buildFamilyPools)
std::map<std::string, std::vector<cv::Mat>> g_rawFamilyPins;
std::map<std::string, std::vector<cv::Mat>> g_familyPins;
std::map<std::string, std::vector<Centred>> g_familyPinCentred;

// "0_MB_124704_1_a70.png" / "pin_MB_124704.png" -> "MB"; "" when the name
// carries no family token (two capitals between the first two underscores)
std::string familyToken(const std::string& name) {
    const size_t first = name.find('_');
    if (first == std::string::npos) return "";
    const size_t second = name.find('_', first + 1);
    if (second == std::string::npos || second - first - 1 != 2) return "";
    const std::string token = name.substr(first + 1, 2);
    for (char c : token) {
        if (!std::isupper(static_cast<unsigned char>(c))) return "";
    }
    return token;
}

// `centred`, when given, receives what the score would have been WITHOUT
// sliding - the one alignment the crop produced. The difference is what
// says a reading was rescued rather than recognised.
// The correlation of a centred template with every window of a strip, as
// cv::matchTemplate's TM_CCOEFF_NORMED computes it - the same sums, the same
// rule for a window with no variance - but with the template's centred values
// and norm taken from the cache and the strip's window sums from two integral
// images made once per strip. A pin strip is 20 x 28 and a pin 16 x 24, so
// there are 25 windows of 384 pixels each; setting up cv::matchTemplate 66
// times a strip costs 2.6 ms, 161 ms of every signature scan with nothing on
// screen.
struct StripSums {
    cv::Mat sum, sqsum;   // integral images, CV_64F, (rows + 1) x (cols + 1)
};
double directCoeffNormed(const cv::Mat& strip, const StripSums& sums, int th, int tw, const Centred& tc, double* mid) {
    const int rows = strip.rows - th + 1, cols = strip.cols - tw + 1;
    double extreme = 0.0;
    double extremeAbs = -1.0;
    *mid = 0.0;
    const double area = (double)th * tw;
    const double normT2 = tc.norm * tc.norm;
    for (int y = 0; y < rows; y++) {
        const double* s0 = sums.sum.ptr<double>(y);
        const double* s1 = sums.sum.ptr<double>(y + th);
        const double* q0 = sums.sqsum.ptr<double>(y);
        const double* q1 = sums.sqsum.ptr<double>(y + th);
        for (int x = 0; x < cols; x++) {
            // the window's sum and sum of squares
            const double wndSum = s1[x + tw] - s1[x] - s0[x + tw] + s0[x];
            const double wndSum2 = q1[x + tw] - q1[x] - q0[x + tw] + q0[x];
            const double wndMean2 = wndSum * wndSum / area;
            // the numerator: the centred template against the raw window -
            // the template's values add up to nothing, so the window's mean
            // drops out
            double num = 0.0;
            const float* tv = tc.values.data();
            for (int v = 0; v < th; v++) {
                const float* row = strip.ptr<float>(y + v) + x;
                const float* trow = tv + v * tw;
                double acc = 0.0;
                for (int u = 0; u < tw; u++) acc += (double)row[u] * trow[u];
                num += acc;
            }
            // as OpenCV: a window with (as good as) no variance scores
            // nothing, and a little over one is one
            const double varWnd = (std::max)(0.0, wndSum2 - wndMean2);
            const double t = varWnd <= (std::min)(0.5, 10.0 * FLT_EPSILON * wndSum2) ? 0.0
                                                                                     : std::sqrt(varWnd * normT2);
            double c;
            if (std::fabs(num) < t) c = num / t;
            else if (std::fabs(num) < t * 1.125) c = num > 0 ? 1.0 : -1.0;
            else c = 0.0;
            if (y == rows / 2 && x == cols / 2) *mid = c;
            if (std::fabs(c) > extremeAbs) {
                extremeAbs = std::fabs(c);
                extreme = c;
            }
        }
    }
    return extreme;
}

double pinScore(const std::vector<cv::Mat>& strips, const std::vector<cv::Mat>& templates,
                double* centred = nullptr,
                const std::vector<Centred>* cache = &g_pinCentred) {
    // SCMINER_PIN_OPENCV=1 goes back to cv::matchTemplate for the strips
    // bigger than the template, to measure the direct correlation against it
    static const bool noDirect = DEV_ENV("SCMINER_PIN_OPENCV") != nullptr;
    // The absolute value is the point: a shape matched with inverted
    // contrast is still that shape. SCMINER_PIN_SIGNED=1 measures what the
    // signed correlation alone would do.
    static const bool signedOnly = DEV_ENV("SCMINER_PIN_SIGNED") != nullptr;
    // SCMINER_PIN_VIEWS=1 says what each view scored on its own, in the
    // order pinViews built them: chroma, then bright. Without it there is
    // no way to tell a view that does not help from a view that is never
    // reached.
    static const bool traceViews = DEV_ENV("SCMINER_PIN_VIEWS") != nullptr;
    const bool prepared = cache->size() == templates.size();
    double best = -1.0;
    double bestCentred = -1.0;
    int viewIndex = -1;
    for (const auto& strip : strips) {
        viewIndex++;
        double bestHere = -1.0;
        if (strip.empty()) {
            if (traceViews) std::cerr << "  [pin] view " << viewIndex << ": empty" << std::endl;
            continue;
        }
        // Strip and template are the same size (normalisePin), so the
        // correlation is one dot product per template - the same number
        // cv::matchTemplate(TM_CCOEFF_NORMED) computes, without its setup,
        // which cost more than the arithmetic 36 times per strip.
        const Centred stripCentred = prepared ? centre(strip) : Centred();
        StripSums sums;
        const bool direct = prepared && !noDirect && strip.type() == CV_32F;
        if (direct) cv::integral(strip, sums.sum, sums.sqsum, CV_64F, CV_64F);
        for (size_t i = 0; i < templates.size(); i++) {
            const cv::Mat& t = templates[i];
            if (strip.rows < t.rows || strip.cols < t.cols) continue;
            double c = 0.0;
            if (prepared && strip.rows == t.rows && strip.cols == t.cols &&
                (*cache)[i].values.size() == stripCentred.values.size()) {
                const Centred& tc = (*cache)[i];
                if (stripCentred.norm > 1e-9 && tc.norm > 1e-9) {
                    double dot = 0.0;
                    const float* a = stripCentred.values.data();
                    const float* b = tc.values.data();
                    const size_t n = tc.values.size();
                    for (size_t k = 0; k < n; k++) dot += static_cast<double>(a[k]) * b[k];
                    c = dot / (stripCentred.norm * tc.norm);
                }
            } else if (direct && (*cache)[i].values.size() == (size_t)t.rows * t.cols) {
                double mid = 0.0;
                c = directCoeffNormed(strip, sums, t.rows, t.cols, (*cache)[i], &mid);
                bestCentred = (std::max)(bestCentred, signedOnly ? mid : std::abs(mid));
            } else {
                // Bigger strip than template: the correlation map has one
                // entry per alignment and the BEST of them is the answer.
                // Element (0,0) alone would judge only the one alignment
                // the crop happened to produce: the same map pin, one
                // second apart and indistinguishable by eye, scored 88 %
                // and then 51 % purely on where the strip had been cut.
                // Both polarities count, as everywhere in this function.
                cv::Mat corr;
                cv::matchTemplate(strip, t, corr, cv::TM_CCOEFF_NORMED);
                double lo = 0.0, hi = 0.0;
                cv::minMaxLoc(corr, &lo, &hi);
                c = (std::abs(lo) > std::abs(hi)) ? lo : hi;
                // The middle of the map is the alignment the crop gave -
                // what the score was before sliding was allowed
                const double mid = corr.at<float>(corr.rows / 2, corr.cols / 2);
                bestCentred = (std::max)(bestCentred, signedOnly ? mid : std::abs(mid));
            }
            const double score = signedOnly ? c : std::abs(c);
            best = (std::max)(best, score);
            bestHere = (std::max)(bestHere, score);
            // Same size as the template means there was nothing to slide
            if (strip.rows == t.rows && strip.cols == t.cols) {
                bestCentred = (std::max)(bestCentred, score);
            }
        }
        if (traceViews) {
            std::cerr << "  [pin] view " << viewIndex << ": "
                      << static_cast<int>(bestHere * 100) << " %" << std::endl;
        }
    }
    if (centred) *centred = bestCentred;
    return best;
}


// The templates travel inside the exe as well: one RCDATA blob
// (resources/glyphs.bin, packed by resources/pack_glyphs.py) so that a
// hand-out needs no folder of digit pictures beside it - a folder that told
// everyone at a glance what the program does. A glyphs folder beside the exe
// still wins when it is there: that is how templates get harvested and tried.
namespace {
struct EmbeddedFile {
    std::string name;
    const unsigned char* data = nullptr;
    size_t size = 0;
};

std::vector<EmbeddedFile> embeddedBlob(const char* name);
std::vector<EmbeddedFile> embeddedGlyphs() { return embeddedBlob("GLYPHS"); }
std::vector<EmbeddedFile> embeddedMfdGlyphs() { return embeddedBlob("GLYPHS_MFD"); }

std::vector<EmbeddedFile> embeddedBlob(const char* name) {
    // The packed set the program carries (resources/glyphs.bin, "SGLY"): a
    // resource on Windows, assembled in on Linux - see EmbeddedData.cpp
    std::vector<EmbeddedFile> found;
    const unsigned char* p = nullptr;
    size_t total = 0;
    if (!embeddedResource(name, &p, &total)) return found;
    if (!p || total < 8 || std::memcmp(p, "SGLY", 4) != 0) return found;
    size_t pos = 4;
    auto u16 = [&]() { const uint16_t v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8)); pos += 2; return v; };
    auto u32 = [&]() {
        const uint32_t v = static_cast<uint32_t>(p[pos]) | (static_cast<uint32_t>(p[pos + 1]) << 8) |
                           (static_cast<uint32_t>(p[pos + 2]) << 16) | (static_cast<uint32_t>(p[pos + 3]) << 24);
        pos += 4;
        return v;
    };
    const uint32_t count = u32();
    for (uint32_t i = 0; i < count && pos + 6 <= total; i++) {
        const uint16_t nameLen = u16();
        if (pos + nameLen + 4 > total) break;
        std::string name(reinterpret_cast<const char*>(p + pos), nameLen);
        pos += nameLen;
        const uint32_t size = u32();
        if (pos + size > total) break;
        found.push_back({name, p + pos, size});
        pos += size;
    }
    return found;
}
}  // namespace

bool GlyphReader::addTemplate(const std::string& name, const cv::Mat& gray, const std::string& file) {
    if (name.rfind("pin_", 0) == 0) {
        cv::Scalar mean, stddev;
        cv::meanStdDev(gray, mean, stddev);
        if (stddev[0] < MIN_PIN_STDDEV) {
            std::cerr << "[glyphs] pin template " << name << " has no contrast (std "
                      << stddev[0] << ") - skipped" << std::endl;
            return false;
        }
        const std::string token = loadingMfd_ ? "" : familyToken(name);
        if (!token.empty()) {
            g_rawFamilyPins[token].push_back(normalisePin(gray));
            return true;
        }
        auto& pins = loadingMfd_ ? mfdPinTemplates_ : pinTemplates_;
        auto& cache = loadingMfd_ ? g_mfdPinCentred : g_pinCentred;
        pins.push_back(normalisePin(gray));
        cache.push_back(centre(pins.back()));
        return true;
    }

    if (name.size() < 3 || !std::isdigit(static_cast<unsigned char>(name[0])) || name[1] != '_') return false;

    cv::Mat image = gray;
    if (image.cols != GLYPH_W || image.rows != GLYPH_H) {
        cv::resize(image, image, cv::Size(GLYPH_W, GLYPH_H), 0, 0, cv::INTER_AREA);
    }

    // "_a052" encodes the original aspect ratio 0.52
    double aspect = 0.6;
    const size_t pos = name.rfind("_a");
    if (pos != std::string::npos) {
        aspect = std::atoi(name.c_str() + pos + 2) / 100.0;
    }

    Template t;
    t.file = file;
    t.digit = name[0] - '0';
    image.convertTo(t.bitmap, CV_32F, 1.0 / 255.0);
    cv::GaussianBlur(t.bitmap, t.bitmap, cv::Size(3, 3), 0.8);   // same blur as the probe
    t.aspect = aspect;
    prepareTemplate(t);
    const std::string token = loadingMfd_ ? "" : familyToken(name);
    if (!token.empty()) rawFamilies_[token].push_back(std::move(t));
    else templates_.push_back(std::move(t));
    return true;
}

std::string GlyphReader::sceneFamilyOf(const std::string& token) {
    static const std::map<std::string, std::string> SCENE = {
        {"MB", "B"}, {"GB", "B"}, {"PB", "B"},
        {"MO", "O"}, {"GO", "O"}, {"PG", "O"},
        {"PN", "N"},
        {"ML", "W"}, {"PW", "W"}, {"PL", "W"}, {"GL", "W"}};
    const auto it = SCENE.find(token);
    return it == SCENE.end() ? "" : it->second;
}

void GlyphReader::buildFamilyPools() {
    familyPools_.clear();
    familyPairModels_.clear();
    g_familyPins.clear();
    g_familyPinCentred.clear();
    for (const auto& [token, raw] : rawFamilies_) {
        const std::string scene = sceneFamilyOf(token);
        if (scene.empty()) continue;   // a token nobody has named: not scored anywhere
        auto& pool = familyPools_[scene];
        if (pool.empty()) pool = templates_;   // the legacy set first
        pool.insert(pool.end(), raw.begin(), raw.end());
    }
    for (const auto& [token, raw] : g_rawFamilyPins) {
        const std::string scene = sceneFamilyOf(token);
        if (scene.empty()) continue;
        auto& pins = g_familyPins[scene];
        if (pins.empty()) pins = pinTemplates_;
        pins.insert(pins.end(), raw.begin(), raw.end());
    }
    // A family with digits but no pins of its own still gets the legacy pins
    for (const auto& [scene, pool] : familyPools_) {
        if (g_familyPins[scene].empty()) g_familyPins[scene] = pinTemplates_;
        auto& cache = g_familyPinCentred[scene];
        cache.clear();
        for (const auto& pin : g_familyPins[scene]) cache.push_back(centre(pin));
        buildPairModels(pool, familyPairModels_[scene]);
    }
}

const std::vector<GlyphReader::Template>* GlyphReader::familyPool(const std::string& family) const {
    const auto it = familyPools_.find(family);
    return it == familyPools_.end() ? nullptr : &it->second;
}

int GlyphReader::loadTemplates(const std::string& dir) {
    templates_.clear();
    g_pinCentred.clear();
    pinTemplates_.clear();
    g_mfdPinCentred.clear();
    mfdPinTemplates_.clear();
    rawFamilies_.clear();
    g_rawFamilyPins.clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".png") continue;
        cv::Mat image = cv::imread(entry.path().string(), cv::IMREAD_GRAYSCALE);
        if (image.empty()) continue;
        addTemplate(name, image, entry.path().string());
    }
    if (templates_.empty() && pinTemplates_.empty()) {
        for (const auto& f : embeddedGlyphs()) {
            cv::Mat raw(1, static_cast<int>(f.size), CV_8U, const_cast<unsigned char*>(f.data));
            cv::Mat image = cv::imdecode(raw, cv::IMREAD_GRAYSCALE);
            if (!image.empty()) addTemplate(f.name, image, "<embedded>/" + f.name);
        }
        embeddedTemplates_ = !templates_.empty();
    } else {
        embeddedTemplates_ = false;
    }
    loadMfdTemplates(dir + "_mfd");
    buildPairModels(templates_, pairModels_);
    buildFamilyPools();
    return static_cast<int>(templates_.size());
}

int GlyphReader::loadMfdTemplates(const std::string& dir) {
    // addTemplate fills templates_, so the canopy set steps aside while the
    // panel set is read in, and comes back afterwards. Pins are skipped:
    // the pin is the same on both and belongs to the canopy set.
    std::vector<Template> canopy = std::move(templates_);
    templates_.clear();
    loadingMfd_ = true;   // pins land in the panel set for the duration
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".png") continue;
        cv::Mat image = cv::imread(entry.path().string(), cv::IMREAD_GRAYSCALE);
        if (image.empty()) continue;
        addTemplate(name, image, entry.path().string());
    }
    if (templates_.empty() && mfdPinTemplates_.empty()) {
        for (const auto& f : embeddedMfdGlyphs()) {
            cv::Mat raw(1, static_cast<int>(f.size), CV_8U, const_cast<unsigned char*>(f.data));
            cv::Mat image = cv::imdecode(raw, cv::IMREAD_GRAYSCALE);
            if (!image.empty()) addTemplate(f.name, image, "<embedded-mfd>/" + f.name);
        }
    }
    loadingMfd_ = false;
    mfdTemplates_ = std::move(templates_);
    templates_ = std::move(canopy);
    buildPairModels(mfdTemplates_, mfdPairModels_);
    return static_cast<int>(mfdTemplates_.size());
}

namespace {

// Everything extractGlyphs and markerLikely share: the colour channels and
// the plate
using ProfClock = std::chrono::steady_clock;
inline double profMs(ProfClock::time_point since) {
    return std::chrono::duration<double, std::milli>(ProfClock::now() - since).count();
}

struct Scene {
    cv::Mat bgr;
    cv::Mat white, bright, chroma, whiteness, green;
    cv::Rect plate;
    cv::Mat roi;
    bool valid = false;
    double scale = 1.0;   // magnification applied before analysis
    int pad = 0;          // margin added around the crop before that
    bool wholeCrop = false; // the crop itself stands in for a plate nothing found
    bool lifted = false;    // the plate was dark and got opened up
    bool forcedLift = false; // ...because the caller insisted (a panel retry)
    double gain = 1.0;      // by this factor - 1 when it was not
    // The plate's own brightness as captured, lift taken back out: the
    // mean of the bright channel over the plate mask (digits punched out)
    // divided by the gain. This, not the crop's exposure, says which font
    // drew the marker: the Prospector's plate is light, the Drake's is dark
    // wherever it sits - inside the radar screen or out on the canopy, over
    // black space or a lit planet. Judged by the crop's p90 instead, a
    // Drake canopy marker over a bright planet went to the canopy set and a
    // Prospector's over dark sky would go to the panel's.
    double plateLuma = 0.0;
    double plateStd = 0.0;  // texture inside the plate: a translucent plate shows what is behind it
    bool panel = false;     // plateLuma under PANEL_PLATE_LUMA
    bool hinted = false;    // the region came from the caller's row hint
};

// The strip as the matcher wants to see it: how colourful it is, and how
// bright, both normalised to the template grid.
// SCMINER_LUMA_ONLY=1: judge everything by brightness alone - no saturation
// view of the pin, no neutral/whiteness/green channel for the digits. A
// measurement switch, to answer whether the "how white is it" criterion
// carries any weight, not a mode for playing.
static bool lumaOnly() {
    static const bool on = DEV_ENV("SCMINER_LUMA_ONLY") != nullptr;
    return on;
}

// How far the pin may sit from where the crop put it, in cells of the
// normalised grid. Two is about a fifth of the pin's width - enough for the
// wobble a probe box introduces, far too little to find a pin that is not
// there. It is not free: the best of more alignments is also a higher
// number for noise.
constexpr int PIN_SLIDE = 2;

std::vector<cv::Mat> pinViews(const Scene& scene, const cv::Rect& strip) {
    // SCMINER_PIN_DUMP=<dir> writes the strip the matcher is about to judge,
    // as it comes out of the frame and as each view sees it. Reasoning about
    // a correlation without looking at what went into it wastes more time
    // than the dump costs.
    static const char* const dumpDir = DEV_ENV("SCMINER_PIN_DUMP");
    if (dumpDir && !scene.bgr.empty()) {
        static int shot = 0;
        const std::string stem = std::string(dumpDir) + "/pin" + std::to_string(shot++);
        std::error_code ec;
        std::filesystem::create_directories(dumpDir, ec);
        cv::Mat big;
        cv::resize(scene.bgr(strip), big, cv::Size(), 6.0, 6.0, cv::INTER_NEAREST);
        cv::imwrite(stem + "_bgr.png", big);
        cv::resize(scene.chroma(strip), big, cv::Size(), 6.0, 6.0, cv::INTER_NEAREST);
        cv::imwrite(stem + "_chroma.png", big);
        cv::resize(scene.bright(strip), big, cv::Size(), 6.0, 6.0, cv::INTER_NEAREST);
        cv::imwrite(stem + "_bright.png", big);
        // ...and the whole scene once, with the strip drawn on it, so the
        // question "is it even looking in the right place" is answerable
        cv::Mat marked = scene.bgr.clone();
        cv::rectangle(marked, strip, cv::Scalar(0, 0, 255), 1);
        cv::imwrite(stem + "_where.png", marked);
    }
    std::vector<cv::Mat> views;
    // SCMINER_PIN_SLIDE=0 pins the match back to the single alignment the
    // crop produces, to measure what the sliding is worth.
    static const int slide = DEV_ENV("SCMINER_PIN_SLIDE")
        ? std::atoi(DEV_ENV("SCMINER_PIN_SLIDE")) : PIN_SLIDE;
    const cv::Rect room = grownStrip(strip, scene.chroma.size(), slide);
    const cv::Rect use = room.empty() ? strip : room;
    const int margin = room.empty() ? 0 : slide;
    if (!lumaOnly()) views.push_back(normalisePin(scene.chroma(use), margin));
    if (!scene.bright.empty()) views.push_back(normalisePin(scene.bright(use), margin));
    return views;
}

// Where a well-exposed marker's brightest pixels sit, and how far a dark crop
// may be pulled towards it. Four is enough to bring a Drake instrument panel
// up to the canopy markers this reader was built on, and short of amplifying
// an almost black crop into noise.
// The anchor is the 90th percentile, NOT the brightest pixels: the Golem
// crop's 99th percentile is 247, because the digits themselves are white -
// by that measure it is perfectly exposed. It is everything BELOW them that
// is dark. p90 is 66 there against 191 on the canopy marker that reads, and
// pulling 66 up to 190 is the same factor of three that made the identical
// crop readable by hand.
constexpr double LIFT_ANCHOR = 0.90;
constexpr int LIFT_TARGET = 190;
constexpr double LIFT_MAX_GAIN = 4.0;
// Above this the crop is exposed well enough and nothing is done at all
constexpr int LIFT_SKIP_ABOVE = 170;
// The gain a forced lift applies. The anchor cannot be trusted where a dark
// marker has bright neighbours - the locator's probe box round a Golem
// 10,800 measured p90 240 because of the orange text above the plate, was
// judged a canopy crop and read nothing. When the canopy reading finds no
// row at all, the crop is lifted by the factor that made the panel markers
// readable by hand and read once more as a panel. Only after a failure, so
// it costs nothing on a frame that reads.
constexpr double LIFT_FORCED_GAIN = 3.0;


// Below this plate brightness (as captured) the marker is the Drake's dark
// plate and is read with the panel's digits and pins. SCMINER_PANEL_LUMA
// to try another bar.
static const double PANEL_PLATE_LUMA = DEV_ENV("SCMINER_PANEL_LUMA")
    ? std::atof(DEV_ENV("SCMINER_PANEL_LUMA")) : 120.0;

// Opens up a crop that is too dark for any channel to threshold.
//
// On the Drake Golem the signature is drawn INSIDE the ship's radar screen,
// and there the plate is barely brighter than the screen behind it:
// R=58 G=34 B=10, against R=184 G=148 B=57 for the canopy marker this
// reader was built on. Unlifted, every channel is flat, the threshold lands
// at 0 and the row is thrown away before the pin or the digits are looked
// at; lifted, the same crop gives the plate, the pin at 79 % and four
// digits. What is missing is exposure, not a template.
//
// A percentile rather than the maximum, so one specular pixel cannot set
// the scale for the whole crop.
//
// It is a no-op where it is not wanted: a canopy marker is already bright
// enough to land above LIFT_SKIP_ABOVE, and the crop comes back untouched.
// The fixtures in the self-test are what says so, not this comment.
cv::Mat liftExposure(const cv::Mat& bgr, bool* lifted, bool force = false,
                     double* gainOut = nullptr) {
    if (bgr.empty() || bgr.channels() < 3) return bgr;
    cv::Mat grey;
    cv::cvtColor(bgr, grey, cv::COLOR_BGR2GRAY);

    int hist[256] = {0};
    for (int y = 0; y < grey.rows; y++) {
        const uchar* row = grey.ptr<uchar>(y);
        for (int x = 0; x < grey.cols; x++) hist[row[x]]++;
    }
    const long long total = (long long)grey.rows * grey.cols;
    if (total <= 0) return bgr;

    auto percentile = [&](double fraction) {
        const long long want = (long long)(total * fraction);
        long long seen = 0;
        for (int i = 0; i < 256; i++) {
            seen += hist[i];
            if (seen >= want) return i;
        }
        return 255;
    };
    const int anchor = percentile(LIFT_ANCHOR);
    if (!force && anchor >= LIFT_SKIP_ABOVE) return bgr;   // already exposed
    const double gain = force ? LIFT_FORCED_GAIN : (std::min)(LIFT_MAX_GAIN,
                                   (double)LIFT_TARGET / (std::max)(1, anchor));
    if (!force && gain <= 1.05) return bgr;      // nothing worth doing

    // A plain gain, with no floor taken off first. Subtracting the floor was
    // tried and is worse here: it pulls the plate back down towards the
    // screen it has to stand out from, and the plate is the thing that has
    // to become bright.
    cv::Mat out;
    bgr.convertTo(out, -1, gain, 0.0);
    if (gainOut) *gainOut = gain;
    if (lifted) *lifted = true;
    return out;
}

Scene analyseScene(const cv::Mat& input, bool wholePlate, bool forceLift = false,
                   const cv::Rect* rowHint = nullptr, const Scene* from = nullptr) {
    ReaderClock clock(wholePlate ? "analyseScene (whole)" : "analyseScene");
    Scene scene;
    if (input.empty() || input.rows < 8 || input.cols < 16) return scene;

    // The retry over the same crop: the picture and its channels are the
    // ones the first look made (same lift, same margin, same doubling), so
    // they are taken over and only the plate is decided afresh. Nothing
    // writes into them.
    if (from && !from->bgr.empty() && from->forcedLift == forceLift) {
        scene.bgr = from->bgr;
        scene.lifted = from->lifted;
        scene.gain = from->gain;
        scene.forcedLift = from->forcedLift;
        scene.pad = from->pad;
        scene.scale = from->scale;
        scene.green = from->green;
        scene.white = from->white;
        scene.bright = from->bright;
        scene.chroma = from->chroma;
        scene.whiteness = from->whiteness;
    } else {
    scene.forcedLift = forceLift;
    if (input.channels() == 4) cv::cvtColor(input, scene.bgr, cv::COLOR_BGRA2BGR);
    else if (input.channels() == 1) cv::cvtColor(input, scene.bgr, cv::COLOR_GRAY2BGR);
    else scene.bgr = input;

    // The input may be a VIEW into a bigger image - in the locator it is a
    // candidate cut out of the search area - so everything below works on a
    // copy. Writing back into a shared buffer corrupts the neighbouring
    // candidates and blinds the whole scan. The copy is also what makes the
    // margin below black: copyMakeBorder on a view takes the border from the
    // picture round the view (without the copy, 33 of 413 scan frames read
    // differently).
    scene.bgr = scene.bgr.clone();

    // ...and before any channel is built from it, make sure there is
    // something to build from. A marker on a lit instrument panel is
    // dark on dark; every channel below assumes it is not.
    scene.bgr = liftExposure(scene.bgr, &scene.lifted, forceLift, &scene.gain);

    // Small markers are doubled before anything looks at them. A marker seen
    // at distance is 20 px tall with digits of 8 or 9, and at that size the
    // chromatic fringe the game draws around every stroke is as wide as the
    // stroke itself: no channel separates the digits from their own halo,
    // every threshold learnt on the crop lands inside the fringe, and the
    // row comes out as nothing. Doubling adds no information, but it spreads
    // the fringe over more pixels relative to the stroke, lifts the digits
    // over the absolute size floors, and halves what the plate erosion below
    // takes from a plate that is only 20 px tall. Everything reported back is
    // scaled down again by `scale`.
    //
    // EVERY crop gets a margin, whatever its size, and the margin is BLACK.
    // A glow edge or a compass tick can connect the digits to the border of
    // the picture from anywhere, and the digit stage drops every shape that
    // touches the edge as scene leaking in. On a 108x43 crop reading 16,780
    // with the marker well inside it, seven channels produce no digit without
    // the margin and all five with it.
    //
    // Black, not a replicated rim: a replicated rim continues the plate
    // outward, the plate blob the thresholds are learnt on grows, and a clean
    // 16,960 reads as 16,900 at full confidence - exactly the wrong answer
    // this reader must never give. Black has no chroma and no brightness, so
    // it is never plate and never digit.
    //
    // For diagnosis the behaviour can be forced: SCMINER_PAD=small|none|
    // replicate, SCMINER_UPSCALE=0. Left unset, the defaults below apply.
    const int originalRows = scene.bgr.rows;
    const bool small = originalRows < SMALL_MARKER_ROWS;
    static const char* padMode = DEV_ENV("SCMINER_PAD");
    static const bool noUpscale =
        DEV_ENV("SCMINER_UPSCALE") && std::string(DEV_ENV("SCMINER_UPSCALE")) == "0";
    const bool padSmallOnly = padMode && std::string(padMode) == "small";
    const bool padNone = padMode && std::string(padMode) == "none";
    const bool padReplicate = padMode && std::string(padMode) == "replicate";

    scene.pad = 0;
    if (!padNone && (!padSmallOnly || small)) {
        scene.pad = (std::max)(8, originalRows / 3);
        if (padReplicate) {
            cv::copyMakeBorder(scene.bgr, scene.bgr, scene.pad, scene.pad, scene.pad, scene.pad,
                               cv::BORDER_REPLICATE);
        } else {
            cv::copyMakeBorder(scene.bgr, scene.bgr, scene.pad, scene.pad, scene.pad, scene.pad,
                               cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        }
    }

    scene.scale = 1.0;
    if (small && !noUpscale) {
        cv::resize(scene.bgr, scene.bgr, cv::Size(), 2.0, 2.0, cv::INTER_LINEAR);
        scene.scale = 2.0;
    }

    std::vector<cv::Mat> planes;
    cv::split(scene.bgr, planes);

    scene.green = planes[1];
    scene.white = cv::min(planes[0], cv::min(planes[1], planes[2]));
    scene.bright = cv::max(planes[0], cv::max(planes[1], planes[2]));
    scene.chroma = scene.bright - scene.white;

    // "Whiteness" = 2 * min - max: high only for pixels that are bright AND
    // neutral. The coloured plate scores low, so do the chromatic fringes the
    // game renders around the digits; the digit cores score high.
    // (A scalar pass computing all five at once is slower than these
    // vectorised calls.)
    {
        cv::Mat w16, b16;
        scene.white.convertTo(w16, CV_16S);
        scene.bright.convertTo(b16, CV_16S);
        cv::Mat diff = 2 * w16 - b16;
        diff.convertTo(scene.whiteness, CV_8U);   // negatives saturate to 0
    }
    }   // else: made afresh

    auto wholeCropAsPlate = [&]() {
        if (scene.bgr.cols > MARKER_CROP_MAX_COLS || scene.bgr.rows > MARKER_CROP_MAX_ROWS) return;
        // The crop as it was handed in - the black margin around it is not
        // part of the picture and must not pull the learnt thresholds down
        const int rim = static_cast<int>(std::lround(scene.pad * scene.scale));
        cv::Rect inner(rim, rim, scene.bgr.cols - 2 * rim, scene.bgr.rows - 2 * rim);
        inner &= cv::Rect(0, 0, scene.bgr.cols, scene.bgr.rows);
        if (inner.width < 12 || inner.height < 6) return;
        scene.plate = inner;
        scene.roi = cv::Mat::zeros(scene.bgr.size(), CV_8U);
        scene.roi(inner).setTo(255);
        scene.valid = true;
        scene.wholeCrop = true;
        scene.plateLuma = cv::mean(scene.bright, scene.roi)[0] / scene.gain;
        { cv::Scalar mu, sd; cv::meanStdDev(scene.bright, mu, sd, scene.roi); scene.plateStd = sd[0] / scene.gain; }
        scene.panel = scene.plateLuma < PANEL_PLATE_LUMA;
    };
    // Asked to skip the plate search altogether: the caller's first pass over
    // a plate the colour test DID find produced nothing. At night the plate
    // can be olive over black at brightness 58 and chroma 24 - under both
    // floors - and the only coloured blob left is the yellow pin, which the
    // closing welds to the digits' fringes into a 67x23 "plate" wide enough
    // to pass the shape test and too small to hold the digits. No floor tells
    // that plate from the ground behind it; the crop is marker-sized, so the
    // crop is the plate and the pin and the digit channels decide, exactly as
    // for a plate the sun has bleached.
    if (wholePlate) { wholeCropAsPlate(); return scene; }

    // The caller knows where the digits are: the locator found their row
    // before it cut this crop. Then the region the thresholds are learnt
    // over is that row with the plate around it - not the largest coloured
    // patch in the crop, which on the Drake's dark translucent plate is
    // whatever bright thing stands beside it: an asteroid at the plate's
    // edge, or the panel's own orange readout.
    if (rowHint && rowHint->width > 0 && rowHint->height > 0) {
        const double h = rowHint->height;
        cv::Rect region(static_cast<int>(std::lround(rowHint->x - h * HINT_SIDE)),
                        static_cast<int>(std::lround(rowHint->y - h * HINT_ABOVE)),
                        static_cast<int>(std::lround(rowHint->width + 2.0 * h * HINT_SIDE)),
                        static_cast<int>(std::lround(rowHint->height + h * (HINT_ABOVE + HINT_BELOW))));
        // Into the padded, possibly doubled picture
        region.x = static_cast<int>(std::lround((region.x + scene.pad) * scene.scale));
        region.y = static_cast<int>(std::lround((region.y + scene.pad) * scene.scale));
        region.width = static_cast<int>(std::lround(region.width * scene.scale));
        region.height = static_cast<int>(std::lround(region.height * scene.scale));
        region &= cv::Rect(0, 0, scene.bgr.cols, scene.bgr.rows);
        if (region.width >= 12 && region.height >= 6) {
            scene.plate = region;
            scene.roi = cv::Mat::zeros(scene.bgr.size(), CV_8U);
            scene.roi(region).setTo(255);
            scene.valid = true;
            scene.wholeCrop = false;
            scene.hinted = true;
            scene.plateLuma = cv::mean(scene.bright, scene.roi)[0] / scene.gain;
            { cv::Scalar mu, sd; cv::meanStdDev(scene.bright, mu, sd, scene.roi); scene.plateStd = sd[0] / scene.gain; }
            scene.panel = scene.plateLuma < PANEL_PLATE_LUMA;
            return scene;
        }
    }

    // The plate: the largest coloured blob once the digit holes are bridged
    cv::Mat plateMask(scene.bgr.size(), CV_8U);
    for (int y = 0; y < plateMask.rows; y++) {
        const uchar* c = scene.chroma.ptr<uchar>(y);
        const uchar* b = scene.bright.ptr<uchar>(y);
        uchar* m = plateMask.ptr<uchar>(y);
        for (int x = 0; x < plateMask.cols; x++) m[x] = (c[x] >= PLATE_CHROMA && b[x] >= PLATE_BRIGHTNESS) ? 255 : 0;
    }
    const int closeW = (std::max)(5, scene.bgr.rows / 3);
    const int closeH = (std::max)(3, scene.bgr.rows / 8);
    cv::morphologyEx(plateMask, plateMask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(closeW, closeH)));

    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(plateMask, labels, stats, centroids);
    int plateIndex = -1;
    int plateArea = 0;
    for (int i = 1; i < count; i++) {
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area > plateArea) {
            plateArea = area;
            plateIndex = i;
        }
    }
    // No coloured plate stands out. A plate bleached by the sun, or washed to
    // grey by the ship's lamp, has no chroma left to be found by - and then
    // nothing downstream can work, not because the digits are unreadable but
    // because the region they have to be found in is empty. Measured: a
    // sun-blown 12,000 produced nothing in any channel for exactly this
    // reason. The crop is small enough to BE a marker (the locator cut it, or
    // a hand did), so the whole of it becomes the plate and the digit
    // channels and the pin decide.
    if (plateIndex < 0) { wholeCropAsPlate(); return scene; }

    scene.plate = cv::Rect(stats.at<int>(plateIndex, cv::CC_STAT_LEFT),
                           stats.at<int>(plateIndex, cv::CC_STAT_TOP),
                           stats.at<int>(plateIndex, cv::CC_STAT_WIDTH),
                           stats.at<int>(plateIndex, cv::CC_STAT_HEIGHT));
    if (scene.plate.width < 12 || scene.plate.height < 6) { wholeCropAsPlate(); return scene; }
    // A plate is WIDE - a row of digits and a pin sit on it. What the chroma
    // mask finds on a distant marker (~10 px digits) is the pin alone: the
    // half-transparent plate behind the digits has washed out below the
    // chroma floor, and the yellow pin is the only coloured blob left. A blob
    // no wider than twice its height is not the plate, so the whole crop
    // takes its place, as for a bleached one.
    if (scene.plate.width < 2 * scene.plate.height) { wholeCropAsPlate(); return scene; }

    // Only the plate itself counts, not its bounding box: the box corners
    // hold whatever bright scene surrounds the marker. Shrinking it by a
    // pixel also drops the outer glow edge.
    scene.roi = (labels == plateIndex);
    cv::erode(scene.roi, scene.roi, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
    scene.valid = true;
    scene.plateLuma = cv::mean(scene.bright, scene.roi)[0] / scene.gain;
    { cv::Scalar mu, sd; cv::meanStdDev(scene.bright, mu, sd, scene.roi); scene.plateStd = sd[0] / scene.gain; }
    scene.panel = scene.plateLuma < PANEL_PLATE_LUMA;
    return scene;
}

} // namespace

// One digit row a channel produced, with everything the caller needs to
// judge it against the rows the other channels produced
struct RowCandidate {
    std::vector<GlyphReader::Glyph> glyphs;
    const char* channel = "";
    int threshold = 0;
    double pinScore = 0.0;
    bool separator = false;
    bool pinSlid = false;   // the pin only cleared the bar once it could slide
    cv::Mat pinStrip;       // the chroma strip the pin was scored in, PIN_H x PIN_W
    double digitChroma = 0.0;   // mean chroma of the digit strokes
    double pinChroma = 0.0;     // 90th percentile of chroma in the pin strip
    double commaGap = 0.0;      // widest gap between neighbours, in row heights
    bool panelFont = false;     // scored against the panel set and its pins
    std::string family;         // the template family of the plate's colour, "" = legacy only
    // The scene the glyph boxes are in: padded, then magnified - and, for a
    // row of the doubled pass, cut from a picture that was the crop doubled
    // (sceneOuter 2). Each row carries its own.
    double sceneScale = 1.0;
    int scenePad = 0;
    double sceneOuter = 1.0;
};

// Which template family a scene belongs to, off its plate: washed white
// under the lamp, dark at night, blue without a helmet, orange or gold
// with one. Grey and anything in between is the legacy set alone.
static const double LAMP_PLATE_LUMA = DEV_ENV("SCMINER_LAMP_LUMA")
    ? std::atof(DEV_ENV("SCMINER_LAMP_LUMA")) : 235.0;
static const double NIGHT_PLATE_LUMA = DEV_ENV("SCMINER_NIGHT_LUMA")
    ? std::atof(DEV_ENV("SCMINER_NIGHT_LUMA")) : 90.0;
std::string plateFamily(const Scene& scene) {
    if (!scene.valid || scene.plate.width <= 0 || scene.plate.height <= 0) return "";
    const cv::Rect plate = scene.plate & cv::Rect(0, 0, scene.bgr.cols, scene.bgr.rows);
    if (plate.area() <= 0) return "";
    if (scene.plateLuma >= LAMP_PLATE_LUMA) return "W";
    if (scene.plateLuma < NIGHT_PLATE_LUMA) return "N";
    const cv::Scalar m = cv::mean(scene.bgr(plate));
    const double b = m[0], g = m[1], r = m[2];
    const double chroma = (std::max)({b, g, r}) - (std::min)({b, g, r});
    if (chroma < 25.0) return "";
    if (b >= r && b >= g) return "B";
    if (r >= b && r >= g) return "O";
    return "";
}

// The widest gap between neighbouring glyphs, in row heights
double widestGapRatio(const std::vector<GlyphReader::Glyph>& glyphs) {
    int widest = 0, tallest = 1;
    for (size_t i = 0; i < glyphs.size(); i++) {
        tallest = (std::max)(tallest, glyphs[i].box.height);
        if (i > 0) widest = (std::max)(widest, glyphs[i].box.x - glyphs[i - 1].box.br().x);
    }
    return static_cast<double>(widest) / tallest;
}

// Every channel's row that passes the row test and (when required) the pin.
//
// Which channel separates the digits depends on the light: in sunlight
// everything is bright and only neutrality tells the digits from the pale
// plate; at night the chromatic fringes thin the neutral cores to slivers and
// plain brightness works better. The first channel to pass is not the one to
// trust: a peeled channel that squeaks past the pin with thin, eroded strokes
// can beat a plain channel that reads the same digits cleanly (on an 8,480:
// green+ 78 %, under the bar, where white gave 99 %). So every passing row is
// handed back, and the reader picks the one whose digits match best.
// Which pin set a scene is judged with: a plate that had to be opened up is
// an instrument panel, and the panel's pins score it - with no panel pins
// loaded the canopy's do.
static const std::vector<cv::Mat>& pinsFor(const Scene& scene) {
    return (scene.panel && !GlyphReader::mfdPinTemplates_.empty())
        ? GlyphReader::mfdPinTemplates_ : GlyphReader::pinTemplates_;
}
static const std::vector<Centred>* pinCacheFor(const Scene& scene) {
    return (scene.panel && !GlyphReader::mfdPinTemplates_.empty())
        ? &g_mfdPinCentred : &g_pinCentred;
}

std::vector<RowCandidate> collectRows(const Scene& scene, bool requirePin, double minPinScore,
                                      const std::vector<cv::Mat>& pinTemplates,
                                      const std::vector<Centred>* pinCache) {
    std::vector<RowCandidate> rows;
    if (!scene.valid) return rows;

    // On a pale sunlit plate even the plate itself is nearly white, so the
    // one thing left that marks the digits is the absence of colour
    cv::Mat neutral(scene.chroma.size(), CV_8U);
    for (int y = 0; y < neutral.rows; y++) {
        const uchar* c = scene.chroma.ptr<uchar>(y);
        uchar* n = neutral.ptr<uchar>(y);
        for (int x = 0; x < neutral.cols; x++) n[x] = static_cast<uchar>(255 - c[x]);
    }

    // fixed < 0 means: learn the threshold from the plate (Otsu); peel means
    // learn it a second time within the bright class only, which strips the
    // glow off the digits when the first split lands between plate and glow
    struct Channel { const char* name; const cv::Mat* image; double fixed; bool peel; };
    // Green sits in the middle of the chromatic aberration the game renders
    // (red and blue are shifted a pixel apart), so at night it keeps the
    // strokes whole where min(R,G,B) leaves only slivers
    const Channel channels[] = {
        {"neutral", &neutral, 255.0 - DIGIT_MAX_CHROMA, false},
        {"whiteness", &scene.whiteness, -1.0, false},
        {"green", &scene.green, -1.0, false}, {"green+", &scene.green, -1.0, true},
        {"white", &scene.white, -1.0, false}, {"white+", &scene.white, -1.0, true},
        {"bright", &scene.bright, -1.0, false},
        // Pure white on a pale plate: under a lamp without a helmet the
        // plate is (245,252,235) and the digits (255,255,255) - twenty
        // levels apart in min(R,G,B), which Otsu cannot split. A fixed bar
        // just under saturation can.
        {"white!", &scene.white, 248.0, false}
    };

    // SCMINER_GLYPH_TRACE=1 reports every channel attempt
    static const bool traceChannels = DEV_ENV("SCMINER_GLYPH_TRACE") != nullptr;

    // Several channels cut the same row, and then the strip beside it, the
    // pin's colour and its score are the same too: judged once a strip and
    // template set (33 pin scores a signature scan, 8 ms of it).
    struct PinJudged {
        cv::Rect strip;
        const std::vector<cv::Mat>* pins;
        double score, centred, pinChroma;
        cv::Mat stripImage;
    };
    std::vector<PinJudged> judged;

    for (const auto& channel : channels) {
        if (lumaOnly() && std::string(channel.name) != "bright") continue;
        ReaderClock* thrClock = new ReaderClock("  rows: learnThreshold");
        double threshold = channel.fixed >= 0.0
            ? channel.fixed : learnThreshold(*channel.image, scene.roi, scene.plate);
        if (channel.peel && threshold >= 0.0) {
            threshold = learnThreshold(*channel.image, scene.roi, scene.plate, threshold);
        }
        delete thrClock;
        if (channel.fixed < 0.0 && threshold < MIN_DIGIT_CONTRAST) {
            if (traceChannels) {
                std::cerr << "  [glyph] " << channel.name << ": threshold " << threshold
                          << " too low" << std::endl;
            }
            continue;
        }

        // SCMINER_CHANNEL_DUMP=<folder> writes what each channel cut, which is
        // the only way to see why a scene no eye has trouble with produces
        // nothing: the picture says at once whether the strokes came out
        // whole, hollow, or welded to the plate.
        static const char* dumpDir = DEV_ENV("SCMINER_CHANNEL_DUMP");
        if (dumpDir && *dumpDir) {
            cv::Mat mask = (*channel.image > threshold) & scene.roi;
            std::error_code dec;
            std::filesystem::create_directories(dumpDir, dec);
            cv::imwrite(std::string(dumpDir) + "/chan_" + channel.name + ".png", mask);
        }

        ReaderClock* segClock = new ReaderClock("  rows: segmentChannel");
        auto found = segmentChannel(*channel.image, scene.roi, scene.plate, threshold, scene.chroma);
        delete segClock;
        ReaderClock* trimClock = new ReaderClock("  rows: trim + baseline rows");
        trimToRow(found);
        if (traceChannels) {
            std::cerr << "  [glyph] " << channel.name << ": thr " << static_cast<int>(threshold)
                      << ", " << found.size() << " shapes:";
            for (const auto& g : found) {
                std::cerr << " " << g.box.x << "," << g.box.y << " " << g.box.width << "x" << g.box.height
                          << "f" << static_cast<int>(g.fill * 100) << "a" << static_cast<int>(g.aspect * 100);
            }
            std::cerr << (plausibleRow(found) ? "  -> row" : "  -> rejected, splitting by baseline")
                      << std::endl;
        }

        // One candidate when the find is a row already. Only when it is not
        // does the find get split into baselines, and at most MAX_BASELINE_ROWS
        // of those are tried, tallest first, so a cluttered crop cannot turn
        // into unbounded pin scoring.
        std::vector<std::vector<GlyphReader::Glyph>> candidates;
        if (plausibleRow(found)) {
            candidates.push_back(std::move(found));
        } else {
            for (auto& group : rowsByBaseline(found)) {
                if ((int)candidates.size() >= MAX_BASELINE_ROWS) break;
                trimToRow(group);
                if (!plausibleRow(group)) continue;
                if (traceChannels) {
                    std::cerr << "  [glyph]   baseline row at y " << group.front().box.y
                              << ", " << group.size() << " shapes" << std::endl;
                }
                candidates.push_back(std::move(group));
            }
        }

        delete trimClock;
        for (auto& rowGlyphs : candidates) {
            ReaderClock rowClock("  rows: per row (separator, chroma, pin)");
            const bool separator = hasThousandsSeparator(*channel.image, threshold, rowGlyphs);

            // The signature marker is the only HUD element with the map pin
            // beside its digits. Text labels in the same font (STRONG,
            // UNKNOWN) have nothing there, and without this check their
            // letters read as digits - confidently and wrongly.
            //
            // Each candidate row gets its OWN strip: letting one row's pin vouch
            // for every row in the scene lets a clipped "16,000" back in as 6,000,
            // because the row that lost its first digit lost the pin beside it too.
            double score = 0.0;
            double centred = -1.0;
            cv::Mat stripImage;
            double digitChroma = 0.0, pinChroma = 0.0;
            // Which font this row is set in - and so which pins vouch for it
            // and which digits it is scored against. The comma tells, and
            // only the comma: a Prospector's 18,780 on a dim plate (luma under
            // 120, digits 8 px) reads 68,840 when a dark plate is taken for the
            // Drake's and scored against the panel set. The Drake's comma gap is
            // 0.85 and up on the dark radar screen as much as over a lit planet,
            // so brightness adds nothing the gap does not say.
            const double gapRatio = widestGapRatio(rowGlyphs);
            const bool panelFont = fontForced() == 1 ? true
                                 : fontForced() == 2 ? false
                                 : gapRatio >= FONT_GAP_SPLIT;
            const bool haveMfdPins = !GlyphReader::mfdPinTemplates_.empty();
            // ...and, off the plate's colour, which family of the canopy
            // set - its pins and digits come with it
            const std::string family = panelFont ? std::string() : plateFamily(scene);
            const auto famPins = family.empty() ? g_familyPins.end() : g_familyPins.find(family);
            const bool haveFamilyPins = famPins != g_familyPins.end() && !famPins->second.empty();
            const std::vector<cv::Mat>& pinsHere =
                (panelFont && haveMfdPins) ? GlyphReader::mfdPinTemplates_
                                           : (haveFamilyPins ? famPins->second : GlyphReader::pinTemplates_);
            const std::vector<Centred>* cacheHere =
                (panelFont && haveMfdPins) ? &g_mfdPinCentred
                                           : (haveFamilyPins ? &g_familyPinCentred[family] : &g_pinCentred);
            (void)pinTemplates;
            (void)pinCache;
            {
                // The strokes' colour: chroma over the pixels the channel cut
                // as digit, inside each glyph's box
                double sum = 0.0;
                int n = 0;
                int level = 0;
                bool all = false;
                const bool varies = greaterLevel8u(threshold, &level, &all);
                for (const auto& g : rowGlyphs) {
                    const cv::Rect box = g.box & cv::Rect(0, 0, scene.chroma.cols, scene.chroma.rows);
                    if (box.area() <= 0) continue;
                    // The mean over the box's digit pixels, formed as
                    // cv::mean forms it (the sum times one over the count),
                    // times the count, so it matches cv::mean bit for bit
                    long long chromaSum = 0;
                    int count = 0;
                    for (int y = box.y; y < box.br().y; y++) {
                        const uchar* c = channel.image->ptr<uchar>(y);
                        const uchar* k = scene.chroma.ptr<uchar>(y);
                        for (int x = box.x; x < box.br().x; x++) {
                            if (varies ? c[x] > level : all) {
                                chromaSum += k[x];
                                count++;
                            }
                        }
                    }
                    const double m0 = count > 0 ? static_cast<double>(chromaSum) * (1.0 / count) : 0.0;
                    sum += m0 * count;
                    n += count;
                }
                digitChroma = n > 0 ? sum / n : 0.0;
            }
            if (requirePin) {
                // A strip cut short by the edge of the capture cannot be
                // judged; whatever the capture shows there, it is not the
                // whole pin
                const cv::Rect strip = pinStripFor(rowGlyphs, scene.chroma.size());
                const double rowHeight = rowGlyphs.front().box.height;
                const PinJudged* seen = nullptr;
                for (const auto& j : judged) {
                    if (j.strip == strip && j.pins == &pinsHere) { seen = &j; break; }
                }
                if (seen) {
                    score = seen->score;
                    centred = seen->centred;
                    pinChroma = seen->pinChroma;
                    stripImage = seen->stripImage;
                } else if (strip.width >= rowHeight * 1.6 && strip.height >= 6 && !pinsHere.empty()) {
                    const auto pinStart = ProfClock::now();
                    cv::resize(scene.chroma(strip), stripImage,
                               cv::Size(GlyphReader::PIN_W, GlyphReader::PIN_H), 0, 0, cv::INTER_AREA);
                    {
                        // The pin's colour: the strip's brightest chroma, as a
                        // 90th percentile so a stray pixel does not set it
                        std::vector<unsigned char> values;
                        values.reserve(static_cast<size_t>(strip.area()));
                        const cv::Mat part = scene.chroma(strip);
                        for (int y = 0; y < part.rows; y++) {
                            const unsigned char* row = part.ptr<unsigned char>(y);
                            values.insert(values.end(), row, row + part.cols);
                        }
                        if (!values.empty()) {
                            std::nth_element(values.begin(), values.begin() + values.size() * 9 / 10, values.end());
                            pinChroma = values[values.size() * 9 / 10];
                        }
                    }
                    ReaderClock pinClock("    pin: views + score");
                    score = pinScore(pinViews(scene, strip), pinsHere, &centred, cacheHere);
                    GlyphReader::profile().pinMs += profMs(pinStart);
                    GlyphReader::profile().pins++;
                    judged.push_back({strip, &pinsHere, score, centred, pinChroma, stripImage});
                }
                if (traceChannels) {
                    std::cerr << "  [glyph]   pin score " << static_cast<int>(score * 100)
                              << "% in " << strip.x << "," << strip.y << " " << strip.width
                              << "x" << strip.height << ", chroma digits " << static_cast<int>(digitChroma)
                              << " pin " << static_cast<int>(pinChroma) << ", gap " << static_cast<int>(gapRatio * 100)
                              << " -> " << (panelFont ? "panel" : "canopy") << " font"
                              << (family.empty() ? "" : " family " + family) << std::endl;
                }
                if (score < minPinScore) continue;
                // White digits beside a coloured pin, or it is not a marker.
                // A cockpit readout - "785.5" on the Drake's instrument
                // panel, orange like the letters beside it - otherwise reads
                // 7,855 with a pin of 88 % (really a "CS" label): its chroma is
                // 184-187 for the digits and 162-186 for the "pin". Over 56 Golem
                // frames, 36 Prospector frames, two LAMP clips and the fixtures,
                // real markers' digit chroma is at most 0.31 of the pin's - 67
                // against 218 under LAMP, 0-38 against 76-255 otherwise; one small
                // panel marker under amber glow reaches 0.73 (see the bar).
                if (pinChroma > 0.0 && digitChroma > pinChroma * MAX_DIGIT_CHROMA_SHARE) {
                    if (traceChannels) {
                        std::cerr << "  [glyph]   digits as coloured as the pin - not a marker" << std::endl;
                    }
                    continue;
                }
            }

            RowCandidate row;
            row.glyphs = std::move(rowGlyphs);
            row.sceneScale = scene.scale;
            row.scenePad = scene.pad;
            row.channel = channel.name;
            row.threshold = static_cast<int>(threshold);
            row.pinScore = score;
            row.pinStrip = stripImage;
            row.digitChroma = digitChroma;
            row.pinChroma = pinChroma;
            row.commaGap = gapRatio;
            row.panelFont = panelFont;
            row.family = family;
            row.separator = separator;
            // Rescued, not recognised: at the alignment the crop gave, this
            // pin was under the bar the reading is judged by
            row.pinSlid = requirePin && centred >= 0.0 &&
                          centred < GlyphReader::PIN_MIN_NCC &&
                          score >= GlyphReader::PIN_MIN_NCC;
            rows.push_back(std::move(row));
        }
    }

    // Measured and rejected: admitting every row of a scene once ANY row's
    // strip matched the pin (the pin belongs to the marker, not the channel)
    // recovers nothing and lets clipped "16,000" readings back in as 6,000:
    // the row that lost its first digit lost the pin beside it too, which is
    // exactly what the per-row strip detects.
    return rows;
}

GlyphReader::Profile& GlyphReader::profile() {
    static Profile p;
    return p;
}

std::vector<GlyphReader::Glyph> GlyphReader::extractGlyphs(const cv::Mat& input, Trace* trace,
                                                           bool requirePin, double minPinScore,
                                                           const cv::Rect* rowHint) {
    ReaderClock clock("probe: extractGlyphs (all)");
    std::vector<Glyph> glyphs;
    const bool wantPicture = trace && trace->wantPicture;
    if (trace) {
        *trace = Trace();
        trace->wantPicture = wantPicture;
    }
    struct Clocked {
        ProfClock::time_point start;
        ~Clocked() { profile().extractMs += profMs(start); profile().extracts++; }
    } clocked{ProfClock::now()};

    const auto analyseStart = ProfClock::now();
    Scene scene = analyseScene(input, false, false, rowHint);
    profile().probeAnalyseMs += profMs(analyseStart);
    if (trace) {
        trace->plate = scene.plate;
        trace->scale = scene.scale;
        trace->pad = scene.pad;
        trace->plateLuma = scene.plateLuma;
        trace->plateStd = scene.plateStd;
    }
    if (!scene.valid) return glyphs;

    // Every channel that yields a pinned row; the first is the most specific
    // one to pass, which is what callers that only ask "is there a marker"
    // (the locator, the pin harvest) want. The reader itself asks for all of
    // them and keeps the best - see read().
    const auto rowsStart = ProfClock::now();
    auto rows = collectRows(scene, requirePin, minPinScore, pinsFor(scene), pinCacheFor(scene));
    profile().probeRowsMs += profMs(rowsStart);
    // Nothing on the plate the colour test found? Then perhaps it found the
    // wrong thing - see analyseScene. One more pass over the whole crop; a
    // crop too big to be a marker is refused there and this stays empty.
    // Every failing pass gets the retry, even when the colour plate covers
    // most of the crop: the plate's mask is the eroded blob with the digits
    // punched out of it, the retry's is the plain rectangle, and the
    // thresholds learnt over the two differ (a 48,000 under LAMP whose colour
    // plate is the whole crop reads only on the retry). Cost: 10-20 ms a
    // frame.
    if (rows.empty() && !scene.wholeCrop) {
        const auto wholeStart = ProfClock::now();
        struct WholeClock {
            ProfClock::time_point start;
            ~WholeClock() { GlyphReader::profile().probeWholeMs += profMs(start); }
        } wholeClock{wholeStart};
        Scene whole = analyseScene(input, true, false, nullptr, &scene);
        if (whole.valid) {
            rows = collectRows(whole, requirePin, minPinScore, pinsFor(whole), pinCacheFor(whole));
            if (!rows.empty()) {
                scene = std::move(whole);
                if (trace) {
                    trace->plate = scene.plate;
                    trace->scale = scene.scale;
                    trace->pad = scene.pad;
                }
            }
        }
    }
    // Still nothing, and the crop was taken for a canopy one: try it as a
    // panel, lifted by force and judged with the panel's pins. This is
    // what gets a dark marker with bright neighbours through the probe.
    if (rows.empty() && !scene.panel && !GlyphReader::mfdPinTemplates_.empty()) {
        Scene panel = analyseScene(input, false, true, rowHint);
        if (panel.valid) {
            rows = collectRows(panel, requirePin, minPinScore, pinsFor(panel), pinCacheFor(panel));
            if (!rows.empty()) {
                scene = std::move(panel);
                if (trace) {
                    trace->plate = scene.plate;
                    trace->scale = scene.scale;
                    trace->pad = scene.pad;
                }
            }
        }
    }
    if (!rows.empty()) {
        const RowCandidate& chosen = rows.front();
        if (trace) {
            trace->channel = chosen.channel;
            trace->threshold = chosen.threshold;
            trace->pinScore = chosen.pinScore;
            trace->separator = chosen.separator;
        }
        glyphs = chosen.glyphs;
    }

    // The picture is for the glyph tool's eyes; the locator, which traces
    // every probe, never looks at it, and drawing it at four times the
    // crop cost a few milliseconds a frame
    if (trace && trace->wantPicture) {
        cv::Mat picture;
        cv::resize(scene.bgr, picture, cv::Size(), 4.0, 4.0, cv::INTER_NEAREST);
        auto scaled = [](const cv::Rect& r) {
            return cv::Rect(r.x * 4, r.y * 4, r.width * 4, r.height * 4);
        };
        cv::rectangle(picture, scaled(scene.plate), cv::Scalar(0, 255, 0), 1);
        for (const auto& g : glyphs) {
            cv::rectangle(picture, scaled(g.box), cv::Scalar(0, 0, 255), 1);
        }
        trace->picture = picture;
    }
    return glyphs;
}

double GlyphReader::pinScoreForRow(const cv::Mat& chroma, const cv::Mat& bright, const cv::Rect& row) {
    if (pinTemplates_.empty() || row.height < 4) return 0.0;
    // The same strip pinStripFor lays beside a row of glyphs, from the box
    const double h = row.height;
    cv::Rect strip(static_cast<int>(std::lround(row.x - 2.3 * h)),
                   static_cast<int>(std::lround(row.y - 0.6 * h)),
                   static_cast<int>(std::lround(2.15 * h)),
                   static_cast<int>(std::lround(row.height + 0.95 * h)));
    strip &= cv::Rect(0, 0, chroma.cols, chroma.rows);
    if (strip.width < h * 1.6 || strip.height < 6) return 0.0;
    std::vector<cv::Mat> views;
    if (!lumaOnly()) views.push_back(normalisePin(chroma(strip)));
    views.push_back(normalisePin(bright(strip)));
    return pinScore(views, pinTemplates_);
}

bool GlyphReader::markerLikely(const cv::Mat& input) {
    if (pinTemplates_.empty()) return false;
    Scene scene = analyseScene(input, false);
    if (!scene.valid) return false;
    const cv::Rect strip = pinStripForPlate(scene.plate, scene.chroma.size());
    if (strip.width < 4 || strip.height < 6) return false;
    return pinScore(pinViews(scene, strip), pinsFor(scene), nullptr, pinCacheFor(scene)) >= PIN_MIN_NCC;
}

bool GlyphReader::harvestPin(const cv::Mat& input, const std::string& dir, const std::string& tag,
                             const std::string& expected, const cv::Rect* rowHint) const {
    Scene scene = analyseScene(input, false, false, rowHint);
    if (!scene.valid) return false;
    std::vector<Glyph> row;
    if (!expected.empty()) {
        Reading reading;
        const auto value = read(input, &reading, {}, nullptr, {}, rowHint);
        if (!value || std::to_string(*value) != expected || reading.glyphs.size() != expected.size()) {
            return false;
        }
        // The strip as the pin check saw it, in the scene the row was read in
        if (reading.pinStrip.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        cv::Scalar mean, stddev;
        cv::meanStdDev(reading.pinStrip, mean, stddev);
        if (stddev[0] < MIN_PIN_STDDEV) return false;
        return cv::imwrite((std::filesystem::path(dir) / ("pin_" + tag + ".png")).string(), reading.pinStrip);
    } else {
        row = extractGlyphs(input, nullptr, false);
    }
    if (row.size() < 4) return false;
    const cv::Rect strip = pinStripFor(row, scene.chroma.size());
    if (strip.width < 4 || strip.height < 6) return false;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    cv::Mat image;
    cv::resize(scene.chroma(strip), image, cv::Size(PIN_W, PIN_H), 0, 0, cv::INTER_AREA);
    cv::Scalar mean, stddev;
    cv::meanStdDev(image, mean, stddev);
    if (stddev[0] < MIN_PIN_STDDEV) return false;   // a blank strip is not a pin
    return cv::imwrite((std::filesystem::path(dir) / ("pin_" + tag + ".png")).string(), image);
}

double GlyphReader::ambiguityLead() { return AMBIGUITY_LEAD; }

bool GlyphReader::ambiguousValue(const Reading& reading, const std::function<bool(int)>& known, double lead) {
    if (lead <= 0.0 || !known || reading.value < 0 || reading.repaired) return false;
    if (reading.digits.size() != reading.runnerUp.size() || reading.digits.size() != reading.leads.size()) return false;
    for (size_t k = 0; k < reading.digits.size(); k++) {
        if (reading.runnerUp[k] < 0 || reading.leads[k] > lead) continue;
        if (k == 0 && reading.runnerUp[k] == 0) continue;   // no value starts with a zero
        int v = 0;
        for (size_t j = 0; j < reading.digits.size(); j++) {
            v = v * 10 + (j == k ? reading.runnerUp[k] : reading.digits[j]);
        }
        if (v != reading.value && known(v)) return true;
    }
    return false;
}

void GlyphReader::prepareTemplate(Template& t) {
    cv::Mat bitmap = t.bitmap;
    if (bitmap.type() != CV_32F) bitmap.convertTo(bitmap, CV_32F);
    const int n = bitmap.rows * bitmap.cols;
    t.centred.assign(n, 0.0f);
    double mean = 0.0;
    for (int y = 0; y < bitmap.rows; y++) {
        const float* row = bitmap.ptr<float>(y);
        for (int x = 0; x < bitmap.cols; x++) mean += row[x];
    }
    mean /= (std::max)(1, n);
    double squares = 0.0;
    int i = 0;
    for (int y = 0; y < bitmap.rows; y++) {
        const float* row = bitmap.ptr<float>(y);
        for (int x = 0; x < bitmap.cols; x++, i++) {
            const double c = row[x] - mean;
            t.centred[i] = static_cast<float>(c);
            squares += c * c;
        }
    }
    t.norm = std::sqrt(squares);
}

void GlyphReader::scoreDigits(const Glyph& glyph, double scores[10]) const {
    // A slight blur makes the comparison tolerant to one-pixel shifts and to
    // the stroke width, which varies with the glow; normalised correlation
    // then ignores the contrast differences between renderings
    cv::Mat probe;
    cv::GaussianBlur(glyph.bitmap, probe, cv::Size(3, 3), 0.8);
    if (probe.type() != CV_32F) probe.convertTo(probe, CV_32F);

    // The correlation cv::matchTemplate(TM_CCOEFF_NORMED) computes for two
    // pictures of the same size, done by hand: the probe centred once, every
    // template centred at load, one dot product each. Same numbers - but a
    // matchTemplate call carries allocation and setup that outweigh 240
    // multiplications a hundredfold, and this runs 310 times per glyph (with
    // matchTemplate, 500 ms of a 700 ms frame went into classifying).
    const int n = probe.rows * probe.cols;
    std::vector<float> centred(static_cast<size_t>(n));
    double mean = 0.0;
    for (int y = 0; y < probe.rows; y++) {
        const float* row = probe.ptr<float>(y);
        for (int x = 0; x < probe.cols; x++) mean += row[x];
    }
    mean /= (std::max)(1, n);
    double squares = 0.0;
    {
        int i = 0;
        for (int y = 0; y < probe.rows; y++) {
            const float* row = probe.ptr<float>(y);
            for (int x = 0; x < probe.cols; x++, i++) {
                const double c = row[x] - mean;
                centred[i] = static_cast<float>(c);
                squares += c * c;
            }
        }
    }
    const double probeNorm = std::sqrt(squares);

    std::fill(scores, scores + 10, -1.0);
    for (const auto& t : (pool_ ? *pool_ : templates_)) {
        double ncc = 0.0;
        if (static_cast<int>(t.centred.size()) == n) {
            if (probeNorm > 1e-9 && t.norm > 1e-9) {
                double dot = 0.0;
                const float* a = centred.data();
                const float* b = t.centred.data();
                for (int i = 0; i < n; i++) dot += static_cast<double>(a[i]) * b[i];
                ncc = dot / (probeNorm * t.norm);
            }
        } else {
            // A template of another size (none today) still gets the slow path
            cv::Mat corr;
            cv::matchTemplate(probe, t.bitmap, corr, cv::TM_CCOEFF_NORMED);
            ncc = corr.at<float>(0, 0);
        }
        // The proportions are what tell a 1 from everything else
        const double aspectGap = std::abs(std::log((glyph.aspect + 0.05) / (t.aspect + 0.05)));
        const double score = 0.5 * (ncc + 1.0) - (std::min)(0.3, aspectGap * 0.5);
        scores[t.digit] = (std::max)(scores[t.digit], score);
    }

    // Two digits within PAIR_TIE of each other: the correlation has run out
    // of resolution. At 12 px an 8 with a faint bar and a 0 with glow
    // across its middle score alike, because every pixel the two shapes
    // share outvotes the few that differ. The pair's discriminant weighs
    // only those few, and when it is sure the winner leads by at least
    // PAIR_LEAD of its confidence, so the lead bar downstream sees a
    // decision rather than a coin toss. Without it, 16,840 reads 16,040
    // at 93 to 91 and 68,000 reads 68,080 at 95 to 90.
    const std::vector<PairModel>* modelsFor = &pairModels_;
    if (pool_ == &mfdTemplates_) modelsFor = &mfdPairModels_;
    else if (pool_ && pool_ != &templates_) {
        for (const auto& [scene, pool] : familyPools_) {
            if (&pool == pool_) { modelsFor = &familyPairModels_.at(scene); break; }
        }
    }
    const std::vector<PairModel>& models = *modelsFor;
    if (!models.empty()) {
        int first = -1, second = -1;
        for (int d = 0; d < 10; d++) {
            if (first < 0 || scores[d] > scores[first]) { second = first; first = d; }
            else if (second < 0 || scores[d] > scores[second]) second = d;
        }
        if (first >= 0 && second >= 0 && scores[second] > -1.0 &&
            scores[first] - scores[second] < PAIR_TIE) {
            const int a = (std::min)(first, second), b = (std::max)(first, second);
            const PairModel& model = models[a * 10 + b];
            if (model.valid) {
                const double c = projectPair(model, probe);
                if (std::abs(c) >= PAIR_MIN_CONFIDENCE) {
                    const int winner = c > 0.0 ? b : a;
                    const int loser = winner == a ? b : a;
                    const double top = scores[first];
                    const double lead = (std::max)(scores[first] - scores[second],
                                                   PAIR_LEAD * (std::min)(std::abs(c), 1.0));
                    static const bool tracePairs = DEV_ENV("SCMINER_GLYPH_TRACE") != nullptr;
                    if (tracePairs) {
                        std::cerr << "  [pair] " << first << ":" << static_cast<int>(scores[first] * 100)
                                  << " vs " << second << ":" << static_cast<int>(scores[second] * 100)
                                  << " model " << (c > 0.0 ? "+" : "") << static_cast<int>(c * 100)
                                  << " -> " << winner << (winner != first ? " (overruled)" : "") << std::endl;
                    }
                    scores[winner] = top;
                    scores[loser] = top - lead;
                }
            }
        }
    }
}

int GlyphReader::templateCount(bool panel) const {
    return static_cast<int>((panel ? mfdTemplates_ : templates_).size());
}

double GlyphReader::projectPair(const PairModel& model, const cv::Mat& probe) {
    const int n = probe.rows * probe.cols;
    if (static_cast<int>(model.weight.size()) != n || model.halfGap <= 0.0f) return 0.0;
    double p = 0.0;
    int i = 0;
    for (int y = 0; y < probe.rows; y++) {
        const float* row = probe.ptr<float>(y);
        for (int x = 0; x < probe.cols; x++, i++) p += model.weight[i] * (row[x] - model.middle[i]);
    }
    return p / model.halfGap;
}

void GlyphReader::buildPairModels(const std::vector<Template>& pool, std::vector<PairModel>& models) {
    models.clear();
    if (pool.empty()) return;
    const int n = pool.front().bitmap.rows * pool.front().bitmap.cols;
    std::vector<int> count(10, 0);
    std::vector<std::vector<double>> sum(10, std::vector<double>(n, 0.0));
    std::vector<std::vector<double>> squares(10, std::vector<double>(n, 0.0));
    for (const auto& t : pool) {
        if (t.bitmap.type() != CV_32F || t.bitmap.rows * t.bitmap.cols != n) continue;
        count[t.digit]++;
        int i = 0;
        for (int y = 0; y < t.bitmap.rows; y++) {
            const float* row = t.bitmap.ptr<float>(y);
            for (int x = 0; x < t.bitmap.cols; x++, i++) {
                sum[t.digit][i] += row[x];
                squares[t.digit][i] += static_cast<double>(row[x]) * row[x];
            }
        }
    }
    models.assign(100, PairModel());
    for (int a = 0; a < 10; a++) {
        for (int b = a + 1; b < 10; b++) {
            if (count[a] < PAIR_MIN_TEMPLATES || count[b] < PAIR_MIN_TEMPLATES) continue;
            PairModel m;
            m.weight.resize(n);
            m.middle.resize(n);
            double gap = 0.0;
            for (int i = 0; i < n; i++) {
                const double meanA = sum[a][i] / count[a], meanB = sum[b][i] / count[b];
                const double varA = squares[a][i] / count[a] - meanA * meanA;
                const double varB = squares[b][i] / count[b] - meanB * meanB;
                const double w = (meanB - meanA) / (varA + varB + PAIR_VARIANCE_FLOOR);
                m.weight[i] = static_cast<float>(w);
                m.middle[i] = static_cast<float>(0.5 * (meanA + meanB));
                gap += w * (meanB - meanA);
            }
            m.halfGap = static_cast<float>(0.5 * gap);
            if (m.halfGap <= 1e-6f) continue;
            // A model that cannot sort its own teachers is no model: the two
            // digits are then not told apart this way and the correlation
            // keeps the last word
            int right = 0, total = 0;
            for (const auto& t : pool) {
                if (t.digit != a && t.digit != b) continue;
                if (t.bitmap.type() != CV_32F || t.bitmap.rows * t.bitmap.cols != n) continue;
                total++;
                if ((projectPair(m, t.bitmap) > 0.0) == (t.digit == b)) right++;
            }
            m.valid = total > 0 && right >= 0.9 * total;
            models[a * 10 + b] = std::move(m);
        }
    }
}

int GlyphReader::classify(const Glyph& glyph, double* similarity, double* margin,
                          int* runnerUpOut, bool panel) const {
    struct PoolGuard {
        const std::vector<Template>*& slot;
        const std::vector<Template>* before;
        ~PoolGuard() { slot = before; }
    } poolGuard{pool_, pool_};
    if (panel && !mfdTemplates_.empty()) pool_ = &mfdTemplates_;
    double best[10];
    scoreDigits(glyph, best);

    int bestDigit = -1;
    int secondDigit = -1;
    double bestScore = -1.0;
    double runnerUp = -1.0;
    for (int d = 0; d < 10; d++) {
        if (best[d] > bestScore) {
            runnerUp = bestScore;
            secondDigit = bestDigit;
            bestScore = best[d];
            bestDigit = d;
        } else if (best[d] > runnerUp) {
            runnerUp = best[d];
            secondDigit = d;
        }
    }
    if (runnerUpOut) *runnerUpOut = secondDigit;
    if (similarity) *similarity = (std::max)(0.0, bestScore);
    if (margin) {
        *margin = (bestDigit < 0 || runnerUp < 0.0) ? 0.0 : (std::max)(0.0, bestScore - runnerUp);
    }
    return bestDigit;
}

// Every remaining failure this reader had was a SEGMENTATION failure. Once
// the digits are apart, the templates read them at 99 %; what goes wrong is
// deciding where one digit ends and the next begins. The cutter looks for
// valleys between strokes, and those are exactly what glare, glow and a sun
// behind the marker fill in - "12,000" then arrives as one 37x15 lump, the
// cutter guesses four pieces, and the pieces read 4394.
//
// So this does not cut at all. The HUD font has a fixed pitch and the format
// is known - four to six digits with a comma before the last three - so the
// digit positions follow from the row's extent alone. Windows are laid over
// the row at that pitch, every window is scored against every digit template
// once, and then the question is put the other way round: of the few hundred
// values a signature can show, which one's digits fit these windows best?
// There is no per-digit decision anywhere; the whole string is judged at once
// against the whole table, which is far harder to fool than five separate
// coin tosses.
std::optional<int> GlyphReader::readByGrid(const cv::Mat& bgr, const std::vector<int>& candidates,
                                           const std::function<bool(int)>& accept,
                                           Reading* reading, bool panelFont) const {
    if (candidates.empty() || templates_.empty()) return std::nullopt;
    // Pins and digits of the font the caller names, not guessed from the
    // plate's brightness: a Prospector's 20,975 on a dim plate scores only
    // 56-63 % against the Drake's pins, under the bar, and never reads.
    const bool usePanel = panelFont && !mfdTemplates_.empty() && !mfdPinTemplates_.empty();
    const std::vector<cv::Mat>& gridPins = usePanel ? mfdPinTemplates_ : pinTemplates_;
    const std::vector<Centred>* gridPinCache = usePanel ? &g_mfdPinCentred : &g_pinCentred;
    struct PoolGuard {
        const std::vector<Template>*& slot;
        const std::vector<Template>* before;
        ~PoolGuard() { slot = before; }
    } poolGuard{pool_, pool_};
    pool_ = usePanel ? &mfdTemplates_ : nullptr;
    static const bool trace = DEV_ENV("SCMINER_GLYPH_TRACE") != nullptr;
    const Scene scene = analyseScene(bgr, false);
    if (!scene.valid) {
        if (trace) std::cerr << "  [grid] no plate" << std::endl;
        return std::nullopt;
    }

    // The table, grouped by digit count, digits spelt out once
    std::array<std::vector<std::pair<int, std::array<int, 6>>>, 7> byLength;
    for (int v : candidates) {
        if (v < 1000 || v > 999999) continue;
        int n = 0;
        for (int t = v; t > 0; t /= 10) n++;
        std::array<int, 6> digits{};
        int t = v;
        for (int i = n - 1; i >= 0; i--) { digits[i] = t % 10; t /= 10; }
        byLength[n].push_back({v, digits});
    }

    // The channels the row can be lifted from, most reliable first. The mask
    // is what the windows are cut from, so it decides what a "digit" looks
    // like here - the same masks the cutter uses.
    // Each mask is cut in one pass, and the box round its set pixels
    // (empty when there are none) is where its components are labelled
    struct Channel { cv::Mat mask; const char* name; cv::Rect box; };
    std::vector<Channel> channels;
    auto maskWhere = [&](const cv::Mat& image, const std::function<bool(uchar)>& pick, const char* name) {
        Channel ch;
        ch.name = name;
        ch.mask = cv::Mat::zeros(image.size(), CV_8U);
        int minX = image.cols, maxX = -1, minY = image.rows, maxY = -1;
        for (int y = 0; y < image.rows; y++) {
            const uchar* v = image.ptr<uchar>(y);
            const uchar* r = scene.roi.ptr<uchar>(y);
            uchar* m = ch.mask.ptr<uchar>(y);
            int first = -1, last = -1;
            for (int x = 0; x < image.cols; x++) {
                if (!r[x] || !pick(v[x])) continue;
                m[x] = 255;
                if (first < 0) first = x;
                last = x;
            }
            if (first < 0) continue;
            minX = (std::min)(minX, first);
            maxX = (std::max)(maxX, last);
            minY = (std::min)(minY, y);
            maxY = y;
        }
        if (maxX >= 0) ch.box = cv::Rect(minX & ~1, minY & ~1, maxX - (minX & ~1) + 1, maxY - (minY & ~1) + 1);
        channels.push_back(std::move(ch));
    };
    if (!lumaOnly()) maskWhere(scene.chroma, [](uchar v) { return v <= 18; }, "neutral");
    auto addOtsu = [&](const cv::Mat& image, const char* name) {
        // Otsu over the whole picture, as cv::threshold learnt it here
        int h[256] = {0};
        for (int y = 0; y < image.rows; y++) {
            const uchar* v = image.ptr<uchar>(y);
            for (int x = 0; x < image.cols; x++) h[v[x]]++;
        }
        const double level = otsuLevel(h, image.rows * image.cols);
        if (level >= MIN_DIGIT_CONTRAST) maskWhere(image, [level](uchar v) { return v > level; }, name);
    };
    if (!lumaOnly()) addOtsu(scene.whiteness, "whiteness");
    addOtsu(scene.bright, "bright");
    if (!lumaOnly()) addOtsu(scene.green, "green");
    maskWhere(scene.white, [](uchar v) { return v >= 248; }, "white!");   // see the row channels

    // The pin beside a row, judged once a strip: the channels' rows often
    // come out alike
    struct PinJudged { cv::Rect strip; double pin, centred; };
    std::vector<PinJudged> judged;

    double bestMean = -1.0, bestWeakest = 0.0, bestLead = 0.0, bestPin = 0.0;
    bool bestSlid = false;   // the winning grid row needed the pin to slide
    int bestValue = -1, bestCount = 0;
    const char* bestHow = "";

    for (const auto& channel : channels) {
        // The row: the digit-sized shapes right of the pin, taken together
        if (channel.box.empty()) continue;   // nothing set: no shapes, no row
        cv::Mat labels, stats, centroids;
        const int count = cv::connectedComponentsWithStats(channel.mask(channel.box), labels, stats, centroids);
        std::vector<cv::Rect> pieces;
        int tallest = 0;
        for (int i = 1; i < count; i++) {
            const cv::Rect box(stats.at<int>(i, cv::CC_STAT_LEFT) + channel.box.x,
                               stats.at<int>(i, cv::CC_STAT_TOP) + channel.box.y,
                               stats.at<int>(i, cv::CC_STAT_WIDTH), stats.at<int>(i, cv::CC_STAT_HEIGHT));
            if (box.height < MIN_GLYPH_HEIGHT) continue;
            if (box.x + box.width / 2 < scene.plate.x + scene.plate.width * PIN_ZONE) continue;
            pieces.push_back(box);
            tallest = (std::max)(tallest, box.height);
        }
        if (tallest < MIN_GLYPH_HEIGHT) continue;
        cv::Rect row;
        bool any = false;
        for (const auto& box : pieces) {
            if (box.height < tallest * MIN_RELATIVE_HEIGHT) continue;
            row = any ? (row | box) : box;
            any = true;
        }
        if (!any || row.width < row.height) {
            if (trace) std::cerr << "  [grid] " << channel.name << ": no row" << std::endl;
            continue;
        }
        const int h = row.height;

        // Only a row with the map pin beside it is a signature; without this
        // the grid would happily read any line of text into a table value
        cv::Rect strip(static_cast<int>(std::lround(row.x - 2.3 * h)),
                       static_cast<int>(std::lround(row.y - 0.6 * h)),
                       static_cast<int>(std::lround(2.15 * h)),
                       static_cast<int>(std::lround(h + 0.95 * h)));
        strip &= cv::Rect(0, 0, scene.chroma.cols, scene.chroma.rows);
        if (strip.width < 4 || strip.height < 6) continue;
        double pinCentred = -1.0;
        double pin = 0.0;
        bool pinSeen = false;
        for (const auto& j : judged) {
            if (j.strip == strip) {
                pin = j.pin;
                pinCentred = j.centred;
                pinSeen = true;
                break;
            }
        }
        if (!pinSeen) {
            pin = pinScore(pinViews(scene, strip), gridPins, &pinCentred, gridPinCache);
            judged.push_back({strip, pin, pinCentred});
        }
        const bool pinWasSlid = pinCentred >= 0.0 && pinCentred < GRID_MIN_PIN &&
                                pin >= GRID_MIN_PIN;
        if (pin < PIN_MIN_NCC) {
            if (trace) {
                std::cerr << "  [grid] " << channel.name << ": row " << row.x << "," << row.y << " "
                          << row.width << "x" << row.height << " but pin " << static_cast<int>(pin * 100)
                          << "%" << std::endl;
            }
            continue;
        }

        for (int n = 4; n <= 6; n++) {
            if (byLength[n].empty()) continue;
            // The comma takes a slot of its own between the thousands and the
            // hundreds; its width is not known exactly, so a few are tried
            for (double commaShare : {0.25, 0.4, 0.55}) {
                const double comma = commaShare * h;
                const double advance = (row.width - comma) / n;
                // A digit of this font is roughly half as wide as it is tall,
                // plus its spacing. A pitch outside this is not n digits.
                if (advance < 0.40 * h || advance > 0.95 * h) continue;

                std::vector<std::array<double, 10>> scores(n);
                bool complete = true;
                for (int i = 0; i < n && complete; i++) {
                    const double x = row.x + i * advance + (i >= n - 3 ? comma : 0.0);
                    cv::Rect window(static_cast<int>(std::lround(x)), row.y,
                                    (std::max)(1, static_cast<int>(std::lround(advance))), h);
                    window &= cv::Rect(0, 0, channel.mask.cols, channel.mask.rows);
                    if (window.width < 2) { complete = false; break; }
                    cv::Mat crop = channel.mask(window);
                    const cv::Rect tight = cv::boundingRect(crop);
                    if (tight.width < 1 || tight.height < MIN_GLYPH_HEIGHT) { complete = false; break; }
                    cv::Mat part = crop(tight);
                    Glyph glyph;
                    glyph.bitmap = normaliseGlyph(part);
                    glyph.box = cv::Rect(window.x + tight.x, window.y + tight.y, tight.width, tight.height);
                    glyph.aspect = static_cast<double>(tight.width) / tight.height;
                    glyph.fill = cv::countNonZero(part) / static_cast<double>(tight.area());
                    scoreDigits(glyph, scores[i].data());
                }
                if (!complete) continue;

                // Which value explains these windows best - and by how much
                double top = -1.0, second = -1.0, topWeakest = 0.0;
                int topValue = -1;
                for (const auto& [value, digits] : byLength[n]) {
                    double sum = 0.0, weak = 1.0;
                    for (int i = 0; i < n; i++) {
                        const double sc = scores[i][digits[i]];
                        sum += sc;
                        weak = (std::min)(weak, sc);
                    }
                    if (sum > top) { second = top; top = sum; topValue = value; topWeakest = weak; }
                    else if (sum > second) second = sum;
                }
                if (topValue < 0) continue;
                const double mean = top / n;
                const double lead = (second < 0.0 ? top : top - second) / n;
                if (trace) {
                    std::cerr << "  [grid] " << channel.name << " n=" << n << " comma " << commaShare
                              << ": " << topValue << " mean " << static_cast<int>(mean * 100)
                              << "% weakest " << static_cast<int>(topWeakest * 100)
                              << "% lead " << static_cast<int>(lead * 100) << "%" << std::endl;
                }
                if (mean > bestMean) {
                    bestMean = mean; bestWeakest = topWeakest; bestLead = lead; bestPin = pin;
                    bestValue = topValue; bestCount = n; bestHow = channel.name;
                    bestSlid = pinWasSlid;
                }
            }
        }
    }

    if (bestValue < 0) return std::nullopt;
    if (trace) {
        std::cerr << "  [grid] best " << bestValue << " via " << bestHow << " (mean "
                  << static_cast<int>(bestMean * 100) << "%, weakest "
                  << static_cast<int>(bestWeakest * 100) << "%, lead "
                  << static_cast<int>(bestLead * 100) << "%)" << std::endl;
    }
    if (bestMean * 100.0 < GRID_MIN_MEAN || bestWeakest * 100.0 < GRID_MIN_WEAKEST ||
        bestLead * 100.0 < GRID_MIN_LEAD) {
        return std::nullopt;
    }
    if (accept && !accept(bestValue)) return std::nullopt;

    if (reading) {
        reading->value = bestValue;
        reading->glyphCount = bestCount;
        reading->markerLikely = true;
        reading->weakest = bestWeakest * 100.0;
        reading->mean = bestMean * 100.0;
        reading->minMargin = bestLead * 100.0;
        reading->repaired = true;   // a reconstruction
        reading->grid = true;       // judged by the grid's own bar in goodEnough()
        reading->pinScore = bestPin;
        reading->pinSlid = bestSlid;
    }
    return bestValue;
}

// See REFERENCE_VIEWPORT_HEIGHT in the header
static double g_hudScale = 1.0;

void GlyphReader::setHudScale(double scale) {
    g_hudScale = (scale > 0.2 && scale < 5.0) ? scale : 1.0;
}

double GlyphReader::hudScale() { return g_hudScale; }

// Measured (see Application::hudScalePercent_): with the scale on, the
// reader does better on the crop as captured than on one resampled to the
// reference - its glyphs are scaled into the template grid anyway
static bool g_readNative = true;

void GlyphReader::setReadAtNativeScale(bool native) { g_readNative = native; }

bool GlyphReader::readAtNativeScale() { return g_readNative; }

cv::Rect GlyphReader::fromReferenceScale(const cv::Rect& r, double scale) {
    // A crop a pixel too generous still holds the whole marker; one a pixel
    // too tight clips a stroke
    const int x = static_cast<int>(std::floor(r.x * scale));
    const int y = static_cast<int>(std::floor(r.y * scale));
    const int x2 = static_cast<int>(std::ceil((r.x + r.width) * scale));
    const int y2 = static_cast<int>(std::ceil((r.y + r.height) * scale));
    return cv::Rect(x, y, x2 - x, y2 - y);
}

// The picture as the reader was measured on: shrunk (or grown) by the HUD
// scale so a 4K marker looks like a 1440p one. Area interpolation when
// shrinking - it averages the pixels a stroke covers instead of dropping
// some of them - and bilinear when growing, which measured better than
// bicubic for the doubling of small digits below.
static cv::Mat toReferenceScale(const cv::Mat& image, double scale) {
    if (image.empty() || std::abs(scale - 1.0) < 0.01) return image;
    cv::Mat scaled;
    cv::resize(image, scaled, cv::Size(), 1.0 / scale, 1.0 / scale,
               scale > 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    return scaled;
}

// The row hint is what the locator saw, and it is usually right. When it
// is wrong it is wrong in one particular way: the window it opens around
// the row - a height and a half either side - catches the line of HUD text
// below or above, the shapes then straddle two baselines, and what the
// reader ends up scoring is neither row whole (a Golem 12,000 reads
// 211,641 hinted, and 12,000 at weakest 88 % with no hint).
//
// So a hinted read that comes back with nothing gets one more pass with
// the hint dropped. It costs nothing on a crop that reads - and a crop
// that does not read has already cost its probe.
std::optional<int> GlyphReader::read(const cv::Mat& input, Reading* reading,
                                     const std::function<bool(int)>& accept,
                                     const std::vector<int>* candidates,
                                     const std::function<bool(int)>& acceptPlain,
                                     const cv::Rect* rowHint, bool secondChance) const {
    Reading first;
    auto value = readOnce(input, &first, accept, candidates, acceptPlain, rowHint);
    // "Solid" is what the callers ask of a reading before they show it: a
    // row the bars pass and a value the table knows. Anything less is worth
    // a second look, because that is exactly where a hint that caught the
    // wrong baseline ends up - with a row that reads SOMETHING, badly.
    auto solid = [&](const std::optional<int>& v, const Reading& r) {
        return v.has_value() && !r.unknown && goodEnough(r);
    };
    const bool haveHint = rowHint && rowHint->width > 0 && rowHint->height > 0;
    // Only where the caller asked for it. The second pass costs as much as
    // the first, and a frame carries a dozen candidates - paying it on all
    // of them doubled the scan (measured: 265 ms to 524 on the MOLE set).
    // The callers hand it to the few candidates nearest the aim, which is
    // where the marker is.
    static const bool noSecond = DEV_ENV("SCMINER_NO_SECOND") != nullptr;
    if (secondChance && !noSecond && haveHint && !solid(value, first)) {
        Reading second;
        auto again = readOnce(input, &second, accept, candidates, acceptPlain, nullptr);
        static const bool traceRetry = DEV_ENV("SCMINER_GLYPH_TRACE") != nullptr;
        if (traceRetry) {
            std::cerr << "  [glyph] hint dropped, second pass -> "
                      << (again ? std::to_string(*again) : std::string("nothing"))
                      << (solid(again, second) ? " (solid)" : "") << std::endl;
        }
        if (solid(again, second)) {
            if (reading) *reading = second;
            return again;
        }
    }
    if (reading) *reading = first;
    return value;
}

std::optional<int> GlyphReader::readOnce(const cv::Mat& input, Reading* reading,
                                     const std::function<bool(int)>& accept,
                                     const std::vector<int>* candidates,
                                     const std::function<bool(int)>& acceptPlain,
                                     const cv::Rect* rowHint) const {
    if (reading) *reading = Reading();
    if (templates_.empty()) return std::nullopt;

    // Brought to the scale the templates were cut at - see
    // REFERENCE_VIEWPORT_HEIGHT. Everything below works on `bgr`; only the
    // plate handed back is mapped into the caller's pixels again.
    const double hud = g_readNative ? 1.0 : g_hudScale;
    const cv::Mat bgr = toReferenceScale(input, hud);
    // The row hint in the same pixels as `bgr`
    cv::Rect hintRef;
    const cv::Rect* hint = nullptr;
    if (rowHint && rowHint->width > 0 && rowHint->height > 0) {
        hintRef = *rowHint;
        if (std::abs(hud - 1.0) >= 0.01) {
            hintRef = cv::Rect(static_cast<int>(std::lround(rowHint->x / hud)),
                               static_cast<int>(std::lround(rowHint->y / hud)),
                               static_cast<int>(std::lround(rowHint->width / hud)),
                               static_cast<int>(std::lround(rowHint->height / hud)));
        }
        hint = &hintRef;
    }

    const auto analyseStart = ProfClock::now();
    Scene scene = analyseScene(bgr, false, false, hint);
    profile().analyseMs += profMs(analyseStart);
    profile().reads++;

    // Which font. A plate that had to be opened up is an instrument panel,
    // and the panel's digits are scored against the panel's own set - the
    // canopy set reads a panel 10,800 as 20,800, with the runner-up level
    // with the winner. Canopy markers sit at p90 191-231, panel markers at
    // 56-151, so the lift itself is the switch. With no panel set loaded
    // this is a no-op.
    struct PoolGuard {
        const std::vector<Template>*& slot;
        ~PoolGuard() { slot = nullptr; }
    } poolGuard{pool_};
    const bool panel = scene.panel && !mfdTemplates_.empty();
    pool_ = panel ? &mfdTemplates_ : nullptr;
    if (reading) reading->mfd = panel;

    // Every pinned row a scene yields, at its own size and - for small
    // digits - doubled. The doubled picture, and the channels made from it,
    // serve every pass of this read that doubles: the whole-crop retry
    // decides its plate afresh but looks at the same pixels.
    cv::Mat doubledImage;
    std::optional<Scene> doubledSeen;
    auto gather = [&](const Scene& base) {
        const auto rowsStart = ProfClock::now();
        std::vector<RowCandidate> found = collectRows(base, true, PIN_MIN_NCC, pinsFor(base), pinCacheFor(base));
        profile().rowsMs += profMs(rowsStart);

        // Small digits get a second look at double size, and the rows from
        // both sizes then compete like any others. The doubling in
        // analyseScene keys on the CROP being small; what matters is the
        // DIGITS. A 108x43 crop of a distant marker carries 11 px digits, and
        // at 11 px the loops of a 6 and an 8 close up and both read as 0
        // (10,700 at 1x; doubled, 16,780 at 80 %). Digits of 12-14 px read
        // at 99 % without any of this, which is where the limit sits.
        //
        // Judged per ROW, by the smallest row's tallest glyph. Judged over
        // every glyph of every row, one row of 50 px glow chunks (the plate's
        // own pieces in the bright channel) says "nothing small here" and the
        // 10 px digits beside it never get their second look.
        int smallestRow = INT_MAX;
        for (const auto& row : found) {
            int rowTall = 0;
            for (const auto& g : row.glyphs) rowTall = (std::max)(rowTall, g.box.height);
            smallestRow = (std::min)(smallestRow, rowTall);
        }
        // "<=": the capture that needed this had its tallest digit at exactly 12 px
        if (base.scale == 1.0 && (found.empty() || smallestRow <= SMALL_GLYPH_PX)) {
            const auto doubledStart = ProfClock::now();
            ReaderClock doubledClock("read: doubled pass");
            // Bilinear, not bicubic: measured on the capture that needed
            // this, bilinear doubling read at 81 %, bicubic at 80 - and the
            // bar is 80. Bicubic's ringing sharpens the fringe along with
            // the stroke.
            if (doubledImage.empty()) cv::resize(bgr, doubledImage, cv::Size(), 2.0, 2.0, cv::INTER_LINEAR);
            cv::Rect hintDoubled;
            if (hint) hintDoubled = cv::Rect(hint->x * 2, hint->y * 2, hint->width * 2, hint->height * 2);
            const Scene bigger = analyseScene(doubledImage, base.wholeCrop, base.lifted,
                                              hint && !base.wholeCrop ? &hintDoubled : nullptr,
                                              doubledSeen ? &*doubledSeen : nullptr);
            if (!doubledSeen) doubledSeen = bigger;
            auto more = collectRows(bigger, true, PIN_MIN_NCC, pinsFor(bigger), pinCacheFor(bigger));
            for (auto& row : more) {
                row.sceneOuter = 2.0;
                found.push_back(std::move(row));
            }
            profile().doubledMs += profMs(doubledStart);
        }
        return found;
    };

    std::vector<RowCandidate> rows = gather(scene);
    // Nothing on the plate the colour test found: one more pass with the
    // whole crop as the plate - see analyseScene for the night marker that
    // needed it. Refused there for a crop too big to be a marker.
    if (rows.empty() && !scene.wholeCrop) {
        const auto wholeStart = ProfClock::now();
        Scene whole = analyseScene(bgr, true, false, nullptr, &scene);
        if (whole.valid) {
            rows = gather(whole);
            if (!rows.empty()) scene = std::move(whole);
        }
        profile().wholeMs += profMs(wholeStart);
    }
    // Still nothing, and the crop was taken for a canopy one: read it once
    // as a panel - lifted by force, panel pins, panel digits. See the
    // same retry in extractGlyphs for the marker that needed it.
    if (rows.empty() && !scene.panel && !mfdTemplates_.empty()) {
        Scene panel = analyseScene(bgr, false, true, hint);
        if (panel.valid) {
            rows = gather(panel);
            if (!rows.empty()) {
                scene = std::move(panel);
                pool_ = &mfdTemplates_;
                if (reading) reading->mfd = true;
            }
        }
    }

    if (reading && scene.valid) {
        // Back into the caller's coordinates - the picture was padded and
        // possibly doubled before anything looked at it
        reading->plate = cv::Rect(
            static_cast<int>(std::lround(scene.plate.x / scene.scale)) - scene.pad,
            static_cast<int>(std::lround(scene.plate.y / scene.scale)) - scene.pad,
            static_cast<int>(std::lround(scene.plate.width / scene.scale)),
            static_cast<int>(std::lround(scene.plate.height / scene.scale)));
        if (std::abs(hud - 1.0) >= 0.01) reading->plate = fromReferenceScale(reading->plate, hud);
    }
    if (reading) {
        reading->glyphCount = rows.empty() ? 0 : static_cast<int>(rows.front().glyphs.size());
        // Only a digit row with the pin beside it proves a marker; the pin
        // alone at the plate edge also fires on text glow, so it is not used
        reading->markerLikely = !rows.empty();
        reading->separator = !rows.empty() && rows.front().separator;
    }
    if (rows.empty()) {
        // No usable row from the cutter. The grid does not need one - it
        // finds the row's extent for itself.
        if (candidates && !candidates->empty()) {
            const auto gridStart = ProfClock::now();
            ReaderClock gridClock("read: grid");
            // No row to tell the font by: the panel set only when forced
            auto grid = readByGrid(bgr, *candidates, accept, reading, fontForced() == 1);
            profile().gridMs += profMs(gridStart);
            profile().grids++;
            if (grid) return grid;
        }
        return std::nullopt;
    }

    // Every row classified. Full per-digit scores are kept, so an unsure
    // glyph can be reconsidered against more than just its runner-up: at
    // this size the right digit is sometimes only third.
    struct Evaluated {
        const RowCandidate* row = nullptr;
        std::vector<std::array<double, 10>> scores;
        std::vector<int> best;
        std::vector<double> margins;
        double weakest = 1.0;
        double mean = 0.0;
        int strong = 0;   // glyphs read at MIN_WEAKEST or better
    };
    std::vector<Evaluated> evals;
    for (const auto& row : rows) {
        Evaluated e;
        e.row = &row;
        // Scored against the set of the font the row is set in
        pool_ = (row.panelFont && !mfdTemplates_.empty()) ? &mfdTemplates_ : familyPool(row.family);
        const size_t n = row.glyphs.size();
        e.scores.resize(n);
        e.best.assign(n, -1);
        e.margins.assign(n, 1.0);
        double total = 0.0;
        bool usable = true;
        for (size_t i = 0; i < n; i++) {
            scoreDigits(row.glyphs[i], e.scores[i].data());
            int bestDigit = -1;
            double bestScore = -1.0, runnerUp = -1.0;
            for (int d = 0; d < 10; d++) {
                if (e.scores[i][d] > bestScore) {
                    runnerUp = bestScore;
                    bestScore = e.scores[i][d];
                    bestDigit = d;
                } else if (e.scores[i][d] > runnerUp) {
                    runnerUp = e.scores[i][d];
                }
            }
            if (bestDigit < 0) { usable = false; break; }
            e.best[i] = bestDigit;
            e.margins[i] = (std::max)(0.0, bestScore - runnerUp);
            e.weakest = (std::min)(e.weakest, bestScore);
            if (bestScore * 100.0 >= MIN_WEAKEST) e.strong++;
            total += bestScore;
        }
        if (!usable || n == 0) continue;
        e.mean = total / n;
        // What every candidate row read, before the sort below decides which
        // one the caller is told about. Without this the only visible answer
        // is the winner, and a frame that reads nothing says nothing about
        // the five rows that were in the running.
        static const bool traceRows = DEV_ENV("SCMINER_GLYPH_TRACE") != nullptr;
        if (traceRows) {
            std::cerr << "  [glyph]   row from " << row.channel << ": ";
            for (size_t i = 0; i < n; i++) std::cerr << e.best[i];
            std::cerr << "  (" << n << " glyphs, weakest " << static_cast<int>(e.weakest * 100)
                      << "%, mean " << static_cast<int>(e.mean * 100) << "%, strong " << e.strong
                      << ", height " << row.glyphs.front().box.height << ")" << std::endl;
        }
        evals.push_back(std::move(e));
    }
    if (evals.empty()) return std::nullopt;

    // Most digits read with confidence first, then best digits. Not the
    // first channel to pass: a peeled channel with thin, eroded strokes can
    // beat a plain one that reads the same digits at 99 %. Not the mean
    // alone: a FRAGMENT beats the whole row (a Drake 68,000 whose neutral
    // row lost its 6 reads 8,000 at mean 97, the white+ row all five at 95,
    // both table values). Not plain glyph counts: an 8,540 whose 5 one
    // channel splits into two slivers goes first as a five-shape row and
    // reads nothing. A sliver is not a digit read with confidence; a sixth
    // digit is.
    std::stable_sort(evals.begin(), evals.end(), [](const Evaluated& a, const Evaluated& b) {
        if (a.strong != b.strong) return a.strong > b.strong;
        if (a.mean != b.mean) return a.mean > b.mean;
        return a.weakest > b.weakest;
    });

    // Resolves one run of a row's glyphs into a value the caller accepts,
    // trying the digits the classifier nearly chose wherever it hesitated.
    // Returns -1 when nothing in reach is a real signature.
    // stage 0: the run as read, if the table (or the sums) accept it.
    // stage 1: the run as read, if it is clean and the table lacks it.
    // stage 2: one hesitant digit swapped towards the table.
    // The rows are asked in that order - every row at stage 0 before any row
    // at stage 1 - so that a five-digit row read plainly beats a four-digit
    // fragment of it that happens to be clean (measured: "5100" clean
    // beside "51,000" plain, "2130" beside "21,350"), and a clean row beats a
    // repair from any row (measured: "70,000" clean beside "78,000" repaired).
    auto resolveRun = [&](const Evaluated& e, const std::vector<size_t>& run,
                          bool* usedAlternative, bool wholeRow, bool* unknownOut, int stage) -> int {
        std::vector<int> digits;
        digits.reserve(run.size());
        for (size_t i : run) digits.push_back(e.best[i]);

        // No signature starts with a zero. A leading 0 is a misread digit, and
        // assembling past it shortens the number: "63,000" with its 6 taken for
        // a 0 becomes 3,000 - a table value.
        auto assemble = [&]() {
            if (digits.empty() || digits.front() == 0) return -1;
            int v = 0;
            for (int d : digits) v = v * 10 + d;
            return v;
        };

        const int plain = assemble();
        if (usedAlternative) *usedAlternative = false;
        if (plain >= 0 && (!accept || accept(plain))) return plain;
        // Exactly as read, the whole row: the rarer kind of value counts too.
        // A trimmed row does not get this - a row missing a digit that lands
        // on a sum is the fragment problem all over again.
        if (plain >= 0 && wholeRow && acceptPlain && acceptPlain(plain)) return plain;
        if (stage == 0) return -1;

        // Read cleanly and still unknown: the table is short, not the
        // reading - see CLEAN_SCORE. Handed back as read, never repaired.
        if (plain >= 0 && wholeRow && accept) {
            bool clean = true;
            for (size_t i : run) {
                if (e.scores[i][e.best[i]] < CLEAN_SCORE || e.margins[i] < CLEAN_MARGIN) {
                    clean = false;
                    break;
                }
            }
            if (clean) {
                if (unknownOut) *unknownOut = true;
                return plain;
            }
        }
        if (stage == 1) return -1;
        // No table to repair towards (the harvest reads without one), and an
        // empty std::function must not be called: a row starting with a 0 and
        // carrying one hesitant glyph would reach explore(), which calls accept(),
        // and the process would die in std::bad_function_call with its stdout
        // unflushed - no output, no message, one value silently missing.
        if (!accept) return -1;

        std::vector<size_t> unsure;
        for (size_t k = 0; k < run.size(); k++) {
            if (e.margins[run[k]] < AMBIGUOUS_MARGIN) unsure.push_back(k);
        }
        std::sort(unsure.begin(), unsure.end(),
                  [&](size_t x, size_t y) { return e.margins[run[x]] < e.margins[run[y]]; });
        if (unsure.size() > MAX_AMBIGUOUS) unsure.resize(MAX_AMBIGUOUS);
        if (unsure.empty()) return -1;

        // Per unsure position, the digits that were genuinely in contention
        std::vector<std::vector<int>> options(unsure.size());
        for (size_t k = 0; k < unsure.size(); k++) {
            const size_t i = run[unsure[k]];
            options[k].push_back(e.best[i]);
            for (int d = 0; d < 10; d++) {
                if (d == e.best[i]) continue;
                if (e.scores[i][e.best[i]] - e.scores[i][d] <= AMBIGUOUS_MARGIN * 0.5) {
                    options[k].push_back(d);
                }
            }
        }

        double bestTotal = -1.0;
        int bestValue = -1;
        std::set<int> reachable;   // every table value within one swap
        std::function<void(size_t, double, int)> explore = [&](size_t k, double sum, int changes) {
            if (k == unsure.size()) {
                const int candidate = assemble();
                if (candidate >= 0 && candidate != plain && accept(candidate)) {
                    reachable.insert(candidate);
                    if (sum > bestTotal) {
                        bestTotal = sum;
                        bestValue = candidate;
                    }
                }
                return;
            }
            for (int d : options[k]) {
                // One hesitant glyph may be settled by the table. Two or more
                // is the table inventing a number: "1 6 9 6 0" reaches 18,000 by
                // three swaps, at 92 %.
                const int swapped = (d != e.best[run[unsure[k]]]) ? 1 : 0;
                if (changes + swapped > 1) continue;
                digits[unsure[k]] = d;
                explore(k + 1, sum + e.scores[run[unsure[k]]][d], changes + swapped);
            }
            digits[unsure[k]] = e.best[run[unsure[k]]];
        };
        explore(0, 0.0, 0);
        // Two different table values one swap away - 16,900 and 16,960 from
        // the same picture - cannot be told apart by the picture, and the
        // higher score was a coin toss. No value is the honest answer.
        if (reachable.size() > 1) bestValue = -1;

        if (bestValue >= 0 && usedAlternative) *usedAlternative = true;
        return bestValue;
    };

    // The best row whose digits resolve to something in the table wins. A
    // row that classifies beautifully but spells no signature is not a
    // reading - the next row is asked before that one is handed back as a
    // plain number.
    const Evaluated* chosen = nullptr;
    std::vector<size_t> usedRun;
    int value = -1;
    bool usedAlternative = false;
    bool trimmed = false;
    bool unknownValue = false;
    for (int stage = 0; stage < 3 && !chosen; stage++)
    for (const Evaluated& e : evals) {
        std::vector<size_t> all(e.row->glyphs.size());
        for (size_t i = 0; i < all.size(); i++) all[i] = i;

        bool alt = false;
        bool unk = false;
        int v = resolveRun(e, all, &alt, true, &unk, stage);
        std::vector<size_t> run = all;
        bool trim = false;

        // Still nothing? The glow can leave a stray shape at either end of
        // the row - a comma the split turned into a digit, or a piece of the
        // pin - and the run without it is then the real signature. Only the
        // table decides, so this cannot invent a value out of nothing.
        //
        // ...but only a shape that does not LOOK like one more digit may go:
        // one much shorter than its neighbour (a comma the split turned into
        // a digit) or one standing apart from the row (a piece of the pin).
        // A full-height glyph in line with the others is a digit, and a row
        // without its first digit is not a shorter signature: every multiple
        // of 3,000 and 4,000 is a ground deposit, so a dropped leading digit
        // lands on a table value almost every time ("63,000" without its 6
        // resolves to 3,000, "16,000" without its 1 to 6,000).
        auto stray = [&](size_t index, size_t neighbour) {
            const cv::Rect& g = e.row->glyphs[index].box;
            const cv::Rect& n = e.row->glyphs[neighbour].box;
            const bool shorter = g.height < 0.75 * n.height;
            const int gap = index < neighbour ? n.x - (g.x + g.width) : g.x - (n.x + n.width);
            const bool apart = gap > 0.6 * n.height;
            return shorter || apart;
        };
        if (v < 0 && stage == 2 && accept && all.size() > 4) {
            if (stray(all.front(), all[1])) {
                std::vector<size_t> withoutFirst(all.begin() + 1, all.end());
                v = resolveRun(e, withoutFirst, &alt, false, nullptr, 2);
                run = withoutFirst;
            }
            if (v < 0 && stray(all.back(), all[all.size() - 2])) {
                std::vector<size_t> withoutLast(all.begin(), all.end() - 1);
                v = resolveRun(e, withoutLast, &alt, false, nullptr, 2);
                run = withoutLast;
            }
            if (v >= 0) { alt = true; trim = true; }
        }
        if (v >= 0) {
            chosen = &e;
            usedRun = run;
            value = v;
            usedAlternative = alt;
            trimmed = trim;
            unknownValue = unk;
            break;
        }
    }
    if (!chosen) {
        // Nothing the caller recognises anywhere - the best-scoring row is
        // handed back as a plain reading so the caller can reject it on its
        // own terms (and the grid gets its turn first, below)
        chosen = &evals.front();
        usedRun.resize(chosen->row->glyphs.size());
        for (size_t i = 0; i < usedRun.size(); i++) usedRun[i] = i;
    }

    if (reading) {
        reading->glyphCount = static_cast<int>(chosen->row->glyphs.size());
        reading->separator = chosen->row->separator;
        reading->pinSlid = chosen->row->pinSlid;
        reading->mfd = chosen->row->panelFont && !mfdTemplates_.empty();
        // Only the glyphs the value was built from, and what the classifier
        // made of each: a stray the run shed is not a digit to learn from
        reading->glyphs.clear();
        reading->digits.clear();
        reading->runnerUp.clear();
        reading->leads.clear();
        for (size_t i : usedRun) {
            reading->glyphs.push_back(chosen->row->glyphs[i]);
            reading->digits.push_back(chosen->best[i]);
            int second = -1;
            for (int d = 0; d < 10; d++) {
                if (d == chosen->best[i]) continue;
                if (second < 0 || chosen->scores[i][d] > chosen->scores[i][second]) second = d;
            }
            reading->runnerUp.push_back(second);
            reading->leads.push_back(second < 0 ? 1.0 : chosen->scores[i][chosen->best[i]] - chosen->scores[i][second]);
        }
        // The whole row, strays included, in the caller's pixels - where
        // the second reader looks (see TextLineReader)
        cv::Rect row;
        for (const auto& g : chosen->row->glyphs) row = row.area() > 0 ? (row | g.box) : g.box;
        if (row.area() > 0) {
            const RowCandidate& r = *chosen->row;
            auto back = [&](double v) { return (v / r.sceneScale - r.scenePad) / r.sceneOuter; };
            const int x0 = static_cast<int>(std::floor(back(row.x)));
            const int y0 = static_cast<int>(std::floor(back(row.y)));
            const int x1 = static_cast<int>(std::ceil(back(row.br().x)));
            const int y1 = static_cast<int>(std::ceil(back(row.br().y)));
            reading->digitBox = cv::Rect(x0, y0, x1 - x0, y1 - y0);
            if (std::abs(hud - 1.0) >= 0.01) reading->digitBox = fromReferenceScale(reading->digitBox, hud);
        }
    }

    // Score only the glyphs the value was actually built from - a stray
    // shape that was discarded must not drag the confidence down with it
    double usedWeakest = 1.0;
    double usedTotal = 0.0;
    double usedMargin = 1.0;
    for (size_t i : usedRun) {
        usedWeakest = (std::min)(usedWeakest, chosen->scores[i][chosen->best[i]]);
        usedMargin = (std::min)(usedMargin, chosen->margins[i]);
        usedTotal += chosen->scores[i][chosen->best[i]];
    }
    const double usedMean = usedTotal / usedRun.size();
    const bool cutterRepaired = usedAlternative || trimmed;
    static const bool traceRead = DEV_ENV("SCMINER_GLYPH_TRACE") != nullptr;
    if (traceRead) {
        std::cerr << "  [read] " << chosen->row->channel << " (" << chosen->row->glyphs.size()
                  << " glyphs, mean " << static_cast<int>(chosen->mean * 100) << "%) -> " << value
                  << " weakest " << static_cast<int>(usedWeakest * 100) << "% mean "
                  << static_cast<int>(usedMean * 100) << "%" << (cutterRepaired ? " repaired" : "")
                  << std::endl;
        // Every glyph: what it read, how well, and what came second
        std::cerr << "  [read]  ";
        for (size_t i = 0; i < chosen->row->glyphs.size(); i++) {
            int second = -1;
            double secondScore = -1.0;
            for (int d = 0; d < 10; d++) {
                if (d == chosen->best[i]) continue;
                if (chosen->scores[i][d] > secondScore) { secondScore = chosen->scores[i][d]; second = d; }
            }
            std::cerr << " " << chosen->best[i] << ":" << static_cast<int>(chosen->scores[i][chosen->best[i]] * 100)
                      << "/" << second << ":" << static_cast<int>(secondScore * 100);
        }
        std::cerr << std::endl;
    }

    // The cutter's reading is weak: either nothing in the table was reachable
    // from any row, or what was reached, was reached on shaky glyphs. From
    // the caller's side both are the same thing - a value that will not be
    // shown. Before handing that back, the grid gets the row.
    //
    // The second case is the one that matters: a sun-blown 12,000 can come
    // through the cutter as 16600 at weakest 75 - a real table value, and
    // wrong. A weak reading is not a finding; it is a question, and the grid
    // is better placed to answer it, because it judges the whole string
    // against the whole table instead of one bloated glyph at a time.
    const bool weak = value < 0 ||
                      usedWeakest * 100.0 < (cutterRepaired ? MIN_WEAKEST_REPAIRED : MIN_WEAKEST) ||
                      usedMean * 100.0 < (cutterRepaired ? MIN_MEAN_REPAIRED : MIN_MEAN);
    if (weak && candidates && !candidates->empty()) {
        // The grid scores against pool_, and the loop above leaves pool_ on
        // whatever font the LAST row was judged to be - a junk row with a wide
        // gap would have the grid read a Prospector marker against the panel
        // set. The grid gets the chosen row's font, or the scene's.
        if (auto grid = readByGrid(bgr, *candidates, accept, reading, chosen && chosen->row->panelFont)) return grid;
    }

    if (value < 0) {
        // The plain reading handed back for the caller to judge - unless it
        // starts with a zero, which no signature does
        int plain = 0;
        for (size_t i : usedRun) plain = plain * 10 + chosen->best[i];
        if (!usedRun.empty() && chosen->best[usedRun.front()] != 0) value = plain;
    }
    // A value the table lacks is still bound by what every signature is: a
    // multiple of 5, none under 1,500, none over the largest published. An
    // unknown is held to 2,000 at the low end.
    if (unknownValue && value >= 0 && (value % 5 != 0 || value < 2000 || value > MAX_VALID_SIGNATURE)) {
        value = -1;
        unknownValue = false;
    }
    // ...and has to be read better than a table value, not merely as well:
    // the table is what normally vouches for a reading, and here nothing
    // does. Junk rows beside the marker read 41,610, 53,520, 24,860 at
    // weakest 80-89, lead 1-9, pins of 65-76 % that had to slide; real
    // unknowns read like real values do.
    if (unknownValue && value >= 0 && chosen &&
        (usedWeakest * 100.0 < 90.0 || usedMean * 100.0 < 94.0 || usedMargin * 100.0 < 5.0 ||
         chosen->row->pinScore < 0.75 || chosen->row->pinSlid)) {
        value = -1;
        unknownValue = false;
    }

    if (reading) {
        reading->value = value;
        reading->weakest = usedWeakest * 100.0;
        reading->mean = usedMean * 100.0;
        reading->minMargin = usedMargin * 100.0;
        reading->repaired = cutterRepaired;
        reading->unknown = unknownValue;
        reading->pinScore = chosen ? chosen->row->pinScore : (rows.empty() ? 0.0 : rows.front().pinScore);
        reading->pinStrip = chosen ? chosen->row->pinStrip : cv::Mat();
        reading->digitChroma = chosen ? chosen->row->digitChroma : 0.0;
        reading->pinChroma = chosen ? chosen->row->pinChroma : 0.0;
        reading->commaGap = 0.0;
        if (chosen) {
            int widest = 0, tallest = 1;
            const auto& g = chosen->row->glyphs;
            for (size_t i = 0; i < g.size(); i++) {
                tallest = (std::max)(tallest, g[i].box.height);
                if (i > 0) widest = (std::max)(widest, g[i].box.x - g[i - 1].box.br().x);
            }
            reading->commaGap = static_cast<double>(widest) / tallest;
        }
    }
    return value;
}

int GlyphReader::pruneTemplates(bool deleteFiles) {
    std::vector<int> perDigit(10, 0);
    for (const auto& t : templates_) perDigit[t.digit]++;

    std::vector<bool> suspect(templates_.size(), false);
    for (size_t i = 0; i < templates_.size(); i++) {
        // A digit with hardly any examples cannot be judged this way
        if (perDigit[templates_[i].digit] < 3) continue;
        double bestScore = -1.0;
        int bestDigit = -1;
        for (size_t j = 0; j < templates_.size(); j++) {
            if (i == j) continue;
            cv::Mat corr;
            cv::matchTemplate(templates_[i].bitmap, templates_[j].bitmap, corr, cv::TM_CCOEFF_NORMED);
            const double ncc = corr.at<float>(0, 0);
            const double aspectGap = std::abs(std::log((templates_[i].aspect + 0.05) / (templates_[j].aspect + 0.05)));
            const double score = 0.5 * (ncc + 1.0) - (std::min)(0.3, aspectGap * 0.5);
            if (score > bestScore) {
                bestScore = score;
                bestDigit = templates_[j].digit;
            }
        }
        if (bestDigit != templates_[i].digit) {
            suspect[i] = true;
            std::cout << "  suspect " << std::filesystem::path(templates_[i].file).filename().string()
                      << " looks like " << bestDigit << std::endl;
        }
    }

    std::vector<Template> kept;
    int removed = 0;
    for (size_t i = 0; i < templates_.size(); i++) {
        if (!suspect[i]) {
            kept.push_back(templates_[i]);
            continue;
        }
        removed++;
        if (deleteFiles) {
            std::error_code ec;
            std::filesystem::remove(templates_[i].file, ec);
        }
    }
    templates_ = std::move(kept);
    return removed;
}

bool GlyphReader::saveTemplate(const Glyph& glyph, int digit,
                               const std::string& dir, const std::string& tag) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    cv::Mat image;
    glyph.bitmap.convertTo(image, CV_8U, 255.0);
    const std::string name = std::to_string(digit) + "_" + tag + "_a" +
                             std::to_string(static_cast<int>(std::lround(glyph.aspect * 100))) + ".png";
    return cv::imwrite((std::filesystem::path(dir) / name).string(), image);
}

} // namespace SCMiner
