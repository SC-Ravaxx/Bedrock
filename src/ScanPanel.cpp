#include "ScanPanel.h"
#include "DevSwitch.h"
#include "PanelGlyphs.h"
#include "PanelFace.h"
#include "PanelShapes.h"
#include "PanelDigits.h"
#include "MiningData.h"
#include "TextLineReader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <functional>
#include <map>

namespace SCMiner {
namespace {

// The reference the panel's pixel measurements were taken at. The game
// scales its HUD with the height of the picture, so every search runs on a
// copy resized to this height and the answer is scaled back.
constexpr int REFERENCE_HEIGHT = 1080;

// Measured off tests/scanpanel at 1080p: the panel is 218 wide, and the rule
// under the heading spans nearly all of it.
constexpr int RULE_MIN_W = 120;
constexpr int RULE_MAX_W = 420;

cv::Mat asBgr(const cv::Mat& frame);
cv::Mat brightStructure(const cv::Mat& bgr);

}  // namespace

// Reads one candidate box in place; see readScanPanel for why there is more
// than one candidate.
void readOnePanel(const cv::Mat& frame, ScanPanel& panel, OCREngine& ocr, GlyphReader* glyphs);

namespace {

// For the bench: what each stage of a look costs, added up until asked for
// (SCMINER_PANELSTAGES=1)
namespace {
std::map<std::string, std::pair<double, int>> g_stages;
const bool g_stagesOn = DEV_ENV("SCMINER_PANELSTAGES") != nullptr;
struct StageClock {
    const char* name;
    std::chrono::steady_clock::time_point start;
    explicit StageClock(const char* n) : name(n), start(std::chrono::steady_clock::now()) {}
    ~StageClock() {
        if (!g_stagesOn) return;
        auto& slot = g_stages[name];
        slot.first += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        slot.second++;
    }
};
}  // namespace

}  // namespace

void reportPanelStages(std::ostream& out, int looks) {
    for (const auto& kv : g_stages) {
        out << "    " << std::left << std::setw(28) << kv.first << std::right << std::fixed << std::setprecision(1)
            << std::setw(8) << kv.second.first / (std::max)(1, looks) << " ms a look  (" << kv.second.second << " calls, "
            << std::setprecision(2) << kv.second.first / (std::max)(1, kv.second.second) << " ms each)" << std::endl;
    }
    g_stages.clear();
}

namespace {
cv::Mat asBgr(const cv::Mat& frame) {
    StageClock clock("asBgr");
    if (frame.channels() == 4) {
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        return bgr;
    }
    return frame;
}

// What the panel is drawn in, whatever colour that is: a thin bright line on a
// darker ground. A top-hat keeps small bright structures and throws away the
// background level, so no absolute brightness is assumed. The signature
// locator works the same way, since colour alone is not reliable in the field.
cv::Mat brightStructure(const cv::Mat& bgr) {
    std::vector<cv::Mat> planes;
    cv::split(bgr, planes);
    const cv::Mat high = cv::max(planes[0], cv::max(planes[1], planes[2]));

    // taller than a rule, shorter than the panel: a rule survives, a lit
    // rock face does not
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(1, 15));
    cv::Mat tophat;
    cv::morphologyEx(high, tophat, cv::MORPH_TOPHAT, kernel);

    cv::Mat cut;
    cv::threshold(tophat, cut, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    return cut;
}

// How many rows of text sit between two rules, inside their own width.
//
// This is what tells the panel from a coincidence. On an ultrawide screen
// the POWER MANAGEMENT bar and the line under RESULTS can pass as top and
// bottom rule: both are long, both start at the same column, and the gap
// between them fits. A panel has its figures between its rules - MASS,
// RES, INST, the ore's name - and that empty stretch of cockpit has none.
int textRowsBetween(const cv::Mat& cut, const PanelRule& a, const PanelRule& b) {
    const int x0 = (std::max)(0, (std::min)(a.x0, b.x0));
    const int x1 = (std::min)(cut.cols, (std::max)(a.x1, b.x1));
    if (x1 - x0 < 20 || b.y - a.y < 12) return 0;
    const int need = (std::max)(3, (x1 - x0) / 20);   // a few glyphs' worth
    // Bands closer together than this are one line of text: a capital and
    // the x-height below it break the run, and counting those separately
    // turned five lines into eight.
    const int JOIN = 5;
    int rows = 0;
    int dark = JOIN + 1;
    for (int y = a.y + 4; y < b.y - 4 && y < cut.rows; y++) {
        if (cv::countNonZero(cut.row(y).colRange(x0, x1)) >= need) {
            if (dark > JOIN) rows++;
            dark = 0;
        } else {
            dark++;
        }
    }
    return rows;
}

}  // namespace

std::vector<PanelRule> findHorizontalRules(const cv::Mat& frame, int minWidth, int maxWidth) {
    const cv::Mat bgr = asBgr(frame);
    const cv::Mat cut = brightStructure(bgr);

    // A run of lit pixels one row tall and at least minWidth long. Opening
    // with a long flat kernel leaves exactly that and erases text, which is
    // the point: every glyph in the panel is narrower than the kernel.
    const cv::Mat flat = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(minWidth, 1));
    cv::Mat rules;
    cv::morphologyEx(cut, rules, cv::MORPH_OPEN, flat);

    std::vector<std::vector<cv::Point>> blobs;
    cv::findContours(rules, blobs, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::vector<PanelRule> out;
    for (const auto& blob : blobs) {
        const cv::Rect r = cv::boundingRect(blob);
        if (r.height > 6) continue;                 // a block, not a rule
        if (r.width < minWidth || r.width > maxWidth) continue;
        out.push_back({r.y + r.height / 2, r.x, r.x + r.width});
    }
    std::sort(out.begin(), out.end(), [](const PanelRule& a, const PanelRule& b) {
        return a.y < b.y;
    });
    return out;
}

std::vector<ScanPanel> scanPanelCandidates(const cv::Mat& frame, int most) {
    std::vector<ScanPanel> out;
    if (frame.empty()) return out;

    const double scale = (double)REFERENCE_HEIGHT / (double)frame.rows;
    cv::Mat work;
    if (std::abs(scale - 1.0) < 0.02) {
        work = frame;
    } else {
        cv::resize(frame, work, cv::Size(), scale, scale,
                   scale < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
    }

    const std::vector<PanelRule> rules = findHorizontalRules(work, RULE_MIN_W, RULE_MAX_W);
    if (rules.size() < 2) {
        ScanPanel none;
        none.note = "rules seen: " + std::to_string(rules.size());
        out.push_back(none);
        return out;
    }
    const cv::Mat cut = brightStructure(asBgr(work));

    for (size_t i = 0; i < rules.size(); i++) {
        for (size_t j = i + 1; j < rules.size(); j++) {
            const PanelRule& a = rules[i];
            const PanelRule& b = rules[j];
            const int gap = b.y - a.y;
            if (gap < 60 || gap > 230) continue;
            const double widthAgreement =
                1.0 - std::abs(a.width() - b.width()) / (double)(std::max)(a.width(), b.width());
            const double edgeAgreement =
                1.0 - std::abs(a.x0 - b.x0) / (double)(std::max)(a.width(), b.width());
            if (widthAgreement < 0.85 || edgeAgreement < 0.90) continue;
            // A panel carries its figures between its rules: the ore, the
            // mass, the resistance, the instability. Three is the floor, and
            // the ceiling keeps out the composition list below, which has
            // more lines than the figures do.
            const int rows = textRowsBetween(cut, a, b);
            if (rows < 3 || rows > 8) continue;

            ScanPanel one;
            const int ruleW = (std::max)(a.width(), b.width());
            const int x0 = (std::max)(0, (std::min)(a.x0, b.x0) - (int)std::lround(ruleW * 0.18));
            const int x1 = (std::min)(work.cols,
                                      (std::max)(a.x1, b.x1) + (int)std::lround(ruleW * 0.22));
            const int top = (std::max)(0, a.y - 26);
            const int bottom = (std::min)(work.rows, b.y + 110);
            one.box = cv::Rect(x0, top, x1 - x0, bottom - top);
            one.headRule = a.y;
            one.bodyRule = b.y;
            one.found = true;
            one.confidence = (widthAgreement + edgeAgreement) / 2;
            one.note = "rules seen: " + std::to_string(rules.size()) + ", pair at " +
                       std::to_string((int)(one.confidence * 100)) + "% over " +
                       std::to_string(rows) + " rows of text";
            if (work.size() != frame.size()) {
                const double back = 1.0 / scale;
                one.box = cv::Rect((int)std::lround(one.box.x * back),
                                   (int)std::lround(one.box.y * back),
                                   (int)std::lround(one.box.width * back),
                                   (int)std::lround(one.box.height * back));
                one.headRule = (int)std::lround(one.headRule * back);
                one.bodyRule = (int)std::lround(one.bodyRule * back);
            }
            out.push_back(one);
        }
    }

    std::sort(out.begin(), out.end(), [](const ScanPanel& x, const ScanPanel& y) {
        return x.confidence > y.confidence;
    });
    if ((int)out.size() > most) out.resize(most);
    if (out.empty()) {
        ScanPanel none;
        none.note = "rules seen: " + std::to_string(rules.size()) + ", no pair held text";
        out.push_back(none);
    }
    return out;
}

cv::Mat hudStrokeMap(const cv::Mat& bgr, int radius) {
    const cv::Mat colour = asBgr(bgr);
    cv::Mat ground;
    cv::medianBlur(colour, ground, 2 * radius + 1);
    cv::Mat diff;
    cv::absdiff(colour, ground, diff);
    std::vector<cv::Mat> planes;
    cv::split(diff, planes);
    return cv::max(planes[0], cv::max(planes[1], planes[2]));
}

namespace {

// What stands out from its surroundings in any one colour, 0..255.
//
// Brightness alone - the brightest of the three channels - misses the HUD in
// front of a bright blue nebula: a yellow hairline is no brighter than that
// sky in its brightest channel, and white text barely. In the red channel the
// same line stands at twice the sky's level. So the vertical top-hat is taken
// in each channel and the strongest kept: a stroke counts if it is brighter
// than what is around it in any colour at all.
cv::Mat channelStructure(const cv::Mat& bgr, int tall) {
    std::vector<cv::Mat> planes;
    cv::split(bgr, planes);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(1, tall));
    cv::Mat best;
    for (const cv::Mat& plane : planes) {
        cv::Mat hat;
        cv::morphologyEx(plane, hat, cv::MORPH_TOPHAT, kernel);
        best = best.empty() ? hat : cv::max(best, hat);
    }
    return best;
}

}  // namespace

PanelInk panelInk(const cv::Mat& frame, const cv::Rect& box) {
    PanelInk out;
    const cv::Mat bgr = asBgr(frame);
    const int margin = (std::max)(8, box.height / 12);
    out.area = cv::Rect(box.x - margin, box.y - margin, box.width + 2 * margin,
                        box.height + 2 * margin) & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (out.area.width < 20 || out.area.height < 20) return out;

    // Taller than a numeral, so a numeral survives the top-hat whole
    const int tall = ((std::max)(15, (int)std::lround(box.height * 0.08))) | 1;
    static const int inkMode = DEV_ENV("SCMINER_INK") ? std::atoi(DEV_ENV("SCMINER_INK")) : 0;
    if (inkMode == 1) {
        std::vector<cv::Mat> planes;
        cv::split(bgr(out.area), planes);
        cv::Mat least = cv::min(cv::min(planes[0], planes[1]), planes[2]);
        const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(1, tall));
        cv::morphologyEx(least, out.structure, cv::MORPH_TOPHAT, kernel);
    } else {
        out.structure = channelStructure(bgr(out.area), tall);
    }
    cv::threshold(out.structure, out.ink, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    return out;
}

namespace {

struct InkPiece {
    int x0, y0, x1, y1, area;
};

// The pieces of ink in one row, joined where they stand over one another:
// the per cent sign is three pieces, a colon two
std::vector<InkPiece> inkPieces(const PanelInk& ink, const cv::Rect& local) {
    std::vector<InkPiece> pieces;
    cv::Mat labels, stats, centres;
    const int found = cv::connectedComponentsWithStats(ink.ink(local), labels, stats, centres, 8, CV_32S);
    for (int i = 1; i < found; i++) {
        const int a = stats.at<int>(i, cv::CC_STAT_AREA);
        if (a < 2) continue;
        const int x = stats.at<int>(i, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(i, cv::CC_STAT_TOP);
        pieces.push_back({x, y, x + stats.at<int>(i, cv::CC_STAT_WIDTH),
                          y + stats.at<int>(i, cv::CC_STAT_HEIGHT), a});
    }
    std::sort(pieces.begin(), pieces.end(), [](const InkPiece& a, const InkPiece& b) {
        return a.x0 < b.x0;
    });
    std::vector<InkPiece> merged;
    for (const InkPiece& p : pieces) {
        if (!merged.empty()) {
            InkPiece& last = merged.back();
            const int overlap = (std::min)(last.x1, p.x1) - (std::max)(last.x0, p.x0);
            const int narrower = (std::min)(last.x1 - last.x0, p.x1 - p.x0);
            if (overlap > 0 && overlap * 5 >= narrower * 2) {
                last.x0 = (std::min)(last.x0, p.x0);
                last.y0 = (std::min)(last.y0, p.y0);
                last.x1 = (std::max)(last.x1, p.x1);
                last.y1 = (std::max)(last.y1, p.y1);
                last.area += p.area;
                continue;
            }
        }
        merged.push_back(p);
    }
    return merged;
}

// Pieces of one word into glyphs. The height is the word's own: on some
// ships the labels are set larger than the figures, and measured against
// "MASS:" every numeral of 12805 came out too short to count.
std::vector<FaceGlyph> glyphsOf(const PanelInk& ink, const cv::Rect& local,
                                const std::vector<InkPiece>& word) {
    std::vector<FaceGlyph> out;
    std::vector<int> heights;
    for (const InkPiece& p : word) {
        if (p.area >= 6) heights.push_back(p.y1 - p.y0);
    }
    if (heights.empty()) return out;
    std::sort(heights.begin(), heights.end());
    const int numeral = heights[heights.size() * 7 / 10];
    if (numeral < 5) return out;
    std::vector<int> bottoms;
    for (const InkPiece& p : word) {
        if ((p.y1 - p.y0) * 10 >= numeral * 7) bottoms.push_back(p.y1);
    }
    std::sort(bottoms.begin(), bottoms.end());
    const int baseline = bottoms.empty() ? local.height : bottoms[bottoms.size() / 2];
    const int slack = (std::max)(2, numeral / 5);

    for (const InkPiece& p : word) {
        const int h = p.y1 - p.y0;
        FaceGlyph g;
        g.box = cv::Rect(ink.area.x + local.x + p.x0, ink.area.y + local.y + p.y0, p.x1 - p.x0, h);
        if (h * 10 >= numeral * 7) {
            if (std::abs(p.y1 - baseline) > slack) continue;   // off the line
            if (h * 10 > numeral * 14) continue;               // taller than any glyph
            const cv::Rect cut(p.x0, p.y0, p.x1 - p.x0, h);
            cv::Mat shape;
            cv::resize(ink.structure(local)(cut), shape, cv::Size(FACE_W, FACE_H), 0, 0,
                       cv::INTER_AREA);
            shape.convertTo(shape, CV_32F);
            shape -= cv::mean(shape)[0];
            const double length = cv::norm(shape);
            if (length < 1e-3) continue;
            g.shape = shape / length;
            out.push_back(g);
        } else if (h * 10 <= numeral * 4 && std::abs(p.y1 - baseline) <= slack &&
                   (p.x1 - p.x0) * 2 <= numeral) {
            g.stop = true;       // sits on the line and is tiny: a full stop
            out.push_back(g);
        }
    }
    return out;
}

}  // namespace

std::vector<FaceGlyph> faceGlyphs(const PanelInk& ink, const cv::Rect& row) {
    const cv::Rect r = row & ink.area;
    if (r.width < 4 || r.height < 4 || ink.ink.empty()) return {};
    const cv::Rect local(r.x - ink.area.x, r.y - ink.area.y, r.width, r.height);
    return glyphsOf(ink, local, inkPieces(ink, local));
}

// The words of one row, each cut into glyphs by its own measure
std::vector<std::vector<FaceGlyph>> faceWords(const PanelInk& ink, const cv::Rect& row) {
    std::vector<std::vector<FaceGlyph>> out;
    const cv::Rect r = row & ink.area;
    if (r.width < 4 || r.height < 4 || ink.ink.empty()) return out;
    const cv::Rect local(r.x - ink.area.x, r.y - ink.area.y, r.width, r.height);
    const std::vector<InkPiece> pieces = inkPieces(ink, local);
    std::vector<int> widths;
    for (const InkPiece& p : pieces) {
        if (p.area >= 6) widths.push_back(p.x1 - p.x0);
    }
    if (widths.empty()) return out;
    std::sort(widths.begin(), widths.end());
    const int wide = widths[widths.size() / 2];
    std::vector<InkPiece> word;
    auto flush = [&]() {
        if (word.empty()) return;
        std::vector<FaceGlyph> glyphs = glyphsOf(ink, local, word);
        if (!glyphs.empty()) out.push_back(glyphs);
        word.clear();
    };
    for (const InkPiece& p : pieces) {
        if (!word.empty() && p.x0 - word.back().x1 > wide * 9 / 10) flush();
        word.push_back(p);
    }
    flush();
    return out;
}

ScanPanel findScanPanel(const cv::Mat& frame) {
    return scanPanelCandidates(frame, 1).front();
}

namespace {

// The bands of text inside a box, top to bottom, in the box's own rows.
std::vector<cv::Range> textBands(const cv::Mat& cut, const cv::Rect& box) {
    std::vector<cv::Range> bands;
    const int need = (std::max)(3, box.width / 20);
    const int JOIN = 5;
    int start = -1, dark = JOIN + 1;
    for (int y = 0; y < box.height; y++) {
        const int row = box.y + y;
        if (row < 0 || row >= cut.rows) continue;
        const bool lit = cv::countNonZero(cut.row(row).colRange(box.x, box.x + box.width)) >= need;
        if (lit) {
            if (dark > JOIN) start = y;
            dark = 0;
        } else {
            if (dark == JOIN && start >= 0) {
                bands.push_back(cv::Range(start, y));
                start = -1;
            }
            dark++;
        }
    }
    if (start >= 0) bands.push_back(cv::Range(start, box.height));

    // A band far taller than the rest is two lines of text that ran together:
    // in weak light a row loses enough lit pixels to leave no clear gap under
    // it, and then the row below is read as part of it. That is how a
    // resistance came back as 434 % - the instability from the line beneath.
    // Split it down the middle, which is where the gap would have been.
    if (bands.size() >= 2) {
        std::vector<int> heights;
        for (const cv::Range& b : bands) heights.push_back(b.end - b.start);
        std::sort(heights.begin(), heights.end());
        const int middling = heights[heights.size() / 2];
        std::vector<cv::Range> split;
        for (const cv::Range& b : bands) {
            const int tall = b.end - b.start;
            if (middling > 0 && tall > middling * 9 / 5) {
                const int cut = b.start + tall / 2;
                split.push_back(cv::Range(b.start, cut));
                split.push_back(cv::Range(cut, b.end));
            } else {
                split.push_back(b);
            }
        }
        bands.swap(split);
    }
    return bands;
}

// One row of the panel, several ways, each blown up and turned dark-on-light
// - which is what Tesseract was trained on. Several, for two reasons:
//
// Brightness alone fails over a lit rock. The panel is drawn over whatever
// is behind it, and on a sunlit face the background is BRIGHTER than the
// glyphs - so a threshold learnt from brightness keeps the rock and throws
// the text away. What holds whatever the ground does is that the glyphs are
// white: neutral, and with no channel missing. The signature locator relies
// on the same property.
//
// And one rendering gives one answer, right or wrong. The same pixels
// thresholded the same way make the same mistake every time (a mass of
// 116,195 read as 118,195 on every look), and a median over repeats cannot
// mend that. Three renderings disagree about different digits, so the one
// they agree on is the one that is there.
std::vector<cv::Mat> forOcr(const cv::Mat& bgr, const cv::Rect& where) {
    std::vector<cv::Mat> out;
    cv::Rect safe = where & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (safe.width < 6 || safe.height < 6) return out;

    std::vector<cv::Mat> planes;
    cv::split(bgr(safe), planes);
    const cv::Mat high = cv::max(planes[0], cv::max(planes[1], planes[2]));
    const cv::Mat low = cv::min(planes[0], cv::min(planes[1], planes[2]));
    cv::Mat neutral = 255 - (high - low);

    // Four times, not two. The glyphs come off the screen about 20 px tall,
    // and a threshold at that size closes the gap in the HUD's 6 - which is
    // exactly how 116,195 was read as 118,195, over and over, by all three
    // renderings at once. Enlarging BEFORE the threshold keeps the gap open,
    // and cubic interpolation is what makes the enlarged stroke smooth
    // enough for the threshold to follow it.
    const double by = (std::max)(4.0, 68.0 / safe.height);
    std::vector<cv::Mat> loosePass;
    for (const cv::Mat& channel : {high, low, neutral}) {
        cv::Mat big;
        cv::resize(channel, big, cv::Size(), by, by, cv::INTER_CUBIC);

        cv::Mat loose;
        const double level =
            cv::threshold(big, loose, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);

        // ...and a tighter cut, on the core of the stroke alone. The HUD draws
        // its glyphs with a glow around them, and Otsu takes the glow with the
        // stroke: at four times the size the digits come out fat enough to
        // close the gap in a 6 and the tail of a 9, which is how 116,195 read
        // as 118,185 through every rendering at once. Raising the level past
        // Otsu leaves the core, and the core has the gaps.
        cv::Mat tight;
        cv::threshold(big, tight, level + (255.0 - level) * 0.35, 255, cv::THRESH_BINARY_INV);

        // Tight first. Where the two disagree the tight one is the one that
        // still has the gaps in its 6 and its 9, and the vote below takes the
        // first answer that two renderings share - so the order is the
        // preference.
        for (cv::Mat cut : {tight, loose}) {
            // Otsu splits whatever is in front of it, so on a row that is
            // mostly background it can come back inverted - the text black on
            // white is what Tesseract wants.
            if (cv::countNonZero(cut) < cut.total() / 2) cut = 255 - cut;
            cv::copyMakeBorder(cut, cut, 8, 8, 8, 8, cv::BORDER_CONSTANT, cv::Scalar(255));
            (cut.data == nullptr ? out : out).push_back(cut);
        }
    }
    (void)loosePass;
    return out;
}

// The digits of one row, read with the templates harvested from the game's
// own HUD font rather than with Tesseract.
//
// Tesseract is trained on print, and this is not print: the HUD's 6, 8 and 9
// share a shape that general OCR mixes up, which is how 116,195 came back as
// 118,185 twice in a row with three different renderings agreeing on the
// wrong answer. GlyphReader was built for exactly this font, for the
// signature marker - the panel is the same face at the same size.
//
// The count of decimals is not read; it does not have to be. The panel
// writes its figures to a fixed shape: the mass whole, the resistance whole,
// the instability and the volume to two places. So the digits are read and
// the point is put where the panel always puts it.
std::string digitsWithGlyphs(const cv::Mat& bgr, const cv::Rect& where, GlyphReader& reader) {
    cv::Rect safe = where & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (safe.width < 8 || safe.height < 8) return std::string();
    cv::Mat crop = bgr(safe).clone();
    // The reader wants glyphs about 20 px tall, which is what the marker
    // gives it; a panel row at 1440 is near that already, a smaller viewport
    // is not.
    const double by = 20.0 / (double)safe.height;
    if (by > 1.2 || by < 0.8) {
        cv::resize(crop, crop, cv::Size(), by, by,
                   by < 1 ? cv::INTER_AREA : cv::INTER_CUBIC);
    }

    std::vector<GlyphReader::Glyph> glyphs =
        GlyphReader::extractGlyphs(crop, nullptr, /*requirePin=*/false, 0.0, nullptr);
    if (glyphs.empty()) return std::string();
    std::sort(glyphs.begin(), glyphs.end(),
              [](const GlyphReader::Glyph& a, const GlyphReader::Glyph& b) {
                  return a.box.x < b.box.x;
              });

    std::string out;
    for (size_t i = 0; i < glyphs.size(); i++) {
        const GlyphReader::Glyph& glyph = glyphs[i];
        // A decimal point and the % sign are not digits: both are far shorter
        // than a numeral.
        if (glyph.box.height < 8) continue;
        double best = 0;
        int digit = reader.classify(glyph, &best, nullptr, nullptr, false);
        if (reader.templateCount(true) > 0) {
            double bestPanel = 0;
            const int panelDigit = reader.classify(glyph, &bestPanel, nullptr, nullptr, true);
            if (panelDigit >= 0 && bestPanel > best) { digit = panelDigit; best = bestPanel; }
        }
        if (digit < 0 || best < 0.80) {
            // Strict at the front, forgiving at the back. A figure whose
            // FIRST glyph is unsure is a figure that may be missing a digit,
            // and a mass short of a digit is a rock the player is told they
            // can break. After the figure comes the unit - "54.17 SCU" - and
            // stopping there is how the S is left out.
            if (out.empty()) return std::string();
            break;
        }
        out += (char)('0' + digit);
    }
    return out;
}

// Which digit one glyph is, by the HUD's own numerals, or -1 when unsure.
int panelDigitOf(const PanelGlyph& glyph, double* howSure) {
    int best = -1;
    double bestScore = -2, secondScore = -2;
    for (int t = 0; t < PANEL_TEMPLATE_COUNT; t++) {
        const cv::Mat shape(PANEL_GLYPH_H, PANEL_GLYPH_W, CV_8U,
                            const_cast<unsigned char*>(PANEL_TEMPLATES[t].pixels));
        cv::Mat asFloat;
        shape.convertTo(asFloat, CV_32F, 1.0 / 255.0);
        cv::Mat score;
        cv::matchTemplate(glyph.shape, asFloat, score, cv::TM_CCOEFF_NORMED);
        const double how = score.at<float>(0, 0);
        if (how > bestScore) {
            if (PANEL_TEMPLATES[t].digit != best) secondScore = bestScore;
            bestScore = how;
            best = PANEL_TEMPLATES[t].digit;
        } else if (PANEL_TEMPLATES[t].digit != best && how > secondScore) {
            secondScore = how;
        }
    }
    if (howSure) *howSure = bestScore - secondScore;
    return bestScore >= 0.70 ? best : -1;
}

// Tesseract's answer for a row, with its zeros and eights settled by the
// HUD's own numerals.
//
// It corrects rather than replaces, and that is the point. Reading a whole
// row from shapes needs the row cut into exactly as many pieces as it has
// numerals, which is a hard thing to get right in every light. Correcting
// needs only that the counts agree - and it is aimed at the one fault that
// every other fix left standing: the slashed zero, which Tesseract takes for
// an 8 about as often as not. 12,560 as 12,568. 40 % as 48. 0 % as 8.
//
// Anything else Tesseract says is left alone. The templates were harvested
// from three captures and cover nine of the ten digits; they are trusted for
// the pair they were built to tell apart and nothing more.
std::string settleZerosAndEights(const cv::Mat& frame, const cv::Rect& row,
                                 const std::string& asRead) {
    std::string digits;
    for (char c : asRead) {
        if (c >= '0' && c <= '9') digits += c;
    }
    if (digits.empty()) return asRead;
    if (digits.find('0') == std::string::npos && digits.find('8') == std::string::npos) {
        return asRead;   // nothing this can help with
    }

    // Lined up from the LEFT, and the count need only be big enough.
    //
    // The figure starts at the left of the crop and what follows it does not:
    // a per cent sign, the letters of SCU, a speck of background. The cutter
    // returns one or two extra pieces on nearly every row, at the end, so an
    // exact count would almost never let the correction run. The digits are
    // where the figure is.
    const std::vector<PanelGlyph> cut = cutPanelGlyphs(frame, row);
    if (cut.size() < digits.size()) return asRead;

    std::string fixed = asRead;
    size_t at = 0;
    for (size_t i = 0; i < digits.size(); i++) {
        while (at < fixed.size() && !(fixed[at] >= '0' && fixed[at] <= '9')) at++;
        if (at >= fixed.size()) break;
        if (digits[i] == '0' || digits[i] == '8') {
            double margin = 0;
            const int says = panelDigitOf(cut[i], &margin);
            // Only this pair, and only when the shape is clearly one of them
            if ((says == 0 || says == 8) && margin >= 0.05) {
                fixed[at] = (char)('0' + says);
            }
        }
        at++;
    }
    return fixed;
}

// "9521" / "40%" / "120.10" / "14.71 SCU" -> the number in it, or -1
float numberIn(const std::string& text) {
    std::string digits;
    for (char c : text) {
        if ((c >= '0' && c <= '9')) digits += c;
        else if (c == '.' && !digits.empty() && digits.find('.') == std::string::npos) digits += c;
        else if (!digits.empty()) break;   // the figure has ended
    }
    if (digits.empty() || digits == ".") return -1;
    return (float)std::atof(digits.c_str());
}

// Whether the figure in a row is printed in colour rather than white.
//
// The panel draws a resistance in white while nothing has changed it, in
// green once the laser on the rock has brought it down and in red once it has
// pushed it up. Which of the two does not matter here - either means the
// laser is already in the figure. Asked only of the brightest pixels, which
// are the digits: the glass behind them is the blue of space, and that has
// green in it too.
bool printedInColour(const cv::Mat& bgr, const cv::Rect& where) {
    const cv::Rect safe = where & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (safe.width < 4 || safe.height < 4) return false;
    const cv::Mat roi = bgr(safe);
    int brightest = 0;
    for (int y = 0; y < roi.rows; y++) {
        const cv::Vec3b* px = roi.ptr<cv::Vec3b>(y);
        for (int x = 0; x < roi.cols; x++) {
            const int top = (std::max)((int)px[x][0], (std::max)((int)px[x][1], (int)px[x][2]));
            if (top > brightest) brightest = top;
        }
    }
    if (brightest < 120) return false;
    int lit = 0, coloured = 0;
    for (int y = 0; y < roi.rows; y++) {
        const cv::Vec3b* px = roi.ptr<cv::Vec3b>(y);
        for (int x = 0; x < roi.cols; x++) {
            const int b = px[x][0], g = px[x][1], r = px[x][2];
            if ((std::max)(b, (std::max)(g, r)) * 10 < brightest * 7) continue;
            lit++;
            const bool green = g > r + 50 && g > b + 30;
            const bool red = r > g + 50 && r > b + 30;
            if (green || red) coloured++;
        }
    }
    return lit >= 12 && coloured * 2 > lit;
}

}  // namespace

// Which pair of rules is the panel is settled by reading it. A cockpit draws
// plenty of long horizontal lines and some of them pair up better than the
// panel's own do; what none of them has is a mass behind the word MASS.
std::vector<cv::Rect> panelValueRows(const cv::Mat& frame, const ScanPanel& panel) {
    std::vector<cv::Rect> out;
    if (!panel.found) return out;
    const cv::Mat bgr = asBgr(frame);
    const std::vector<cv::Range> all = textBands(brightStructure(bgr), panel.box);

    const int valueX = panel.box.x + panel.box.width * 33 / 100;
    const int valueW = panel.box.width - (valueX - panel.box.x);
    for (const cv::Range& b : all) {
        const int y = panel.box.y + b.start;
        if (y < panel.headRule || y >= panel.bodyRule) continue;   // between the rules
        out.push_back(cv::Rect(valueX, y - 2, valueW, b.end - b.start + 4));
    }
    return out;
}

// Cuts the glyphs out of one row of a panel.
//
// Blown up first, because the glyphs come off the screen about 20 px tall and
// every later step - the threshold, the connected components, the shape that
// is compared - works better with room. The three channels the rest of this
// file uses are ORed together, so a row is found whether it stands out by
// brightness, by neutrality or by whiteness: that is what carries it from a
// dark rock face to a sunlit ice field.
std::vector<PanelGlyph> cutPanelGlyphs(const cv::Mat& frame, const cv::Rect& row) {
    std::vector<PanelGlyph> out;
    const cv::Mat bgr = asBgr(frame);
    const cv::Rect safe = row & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (safe.width < 8 || safe.height < 6) return out;

    const double by = (std::max)(3.0, 60.0 / safe.height);
    cv::Mat big;
    cv::resize(bgr(safe), big, cv::Size(), by, by, cv::INTER_CUBIC);

    std::vector<cv::Mat> planes;
    cv::split(big, planes);
    const cv::Mat high = cv::max(planes[0], cv::max(planes[1], planes[2]));
    const cv::Mat low = cv::min(planes[0], cv::min(planes[1], planes[2]));
    const cv::Mat neutral = 255 - (high - low);

    cv::Mat lit = cv::Mat::zeros(big.size(), CV_8U);
    for (const cv::Mat& channel : {high, neutral, low}) {
        cv::Mat cut;
        cv::threshold(channel, cut, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        // a row is mostly ground, so the smaller half is the text
        if (cv::countNonZero(cut) > (int)cut.total() / 2) cut = 255 - cut;
        cv::bitwise_or(lit, cut, lit);
    }

    cv::Mat labels, stats, centres;
    const int found = cv::connectedComponentsWithStats(lit, labels, stats, centres, 8, CV_32S);

    // First pass: keep what could be part of a figure at all, and learn how
    // wide one numeral is from the ones that stand alone.
    //
    // The width is LEARNT rather than taken as a share of the height. A share
    // cuts a five-digit row into seven: the face is not as regular as a
    // formula, and the row itself says what a numeral looks like better than
    // any constant can.
    struct Piece { int x, y, w, h; };
    std::vector<Piece> pieces;
    const int tall = (int)std::lround(12 * by);
    for (int i = 1; i < found; i++) {
        const int w = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        const int x = stats.at<int>(i, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(i, cv::CC_STAT_TOP);
        const int lit_ = stats.at<int>(i, cv::CC_STAT_AREA);
        if (h < tall * 0.6 || h > tall * 2.0) continue;   // not a numeral's height
        if (w < 2) continue;
        if (lit_ < w * h / 8) continue;                   // a wisp, not a stroke
        pieces.push_back({x, y, w, h});
    }
    if (pieces.empty()) return out;

    std::vector<int> singles;
    for (const Piece& one : pieces) {
        if (one.w <= one.h * 0.8) singles.push_back(one.w);
    }
    int oneWide = 0;
    if (!singles.empty()) {
        std::sort(singles.begin(), singles.end());
        oneWide = singles[singles.size() / 2];
    } else {
        int narrowest = pieces.front().w;
        for (const Piece& one : pieces) narrowest = (std::min)(narrowest, one.w);
        oneWide = narrowest;
    }
    if (oneWide < 2) return out;

    // Second pass: anything a good deal wider than one numeral is several
    // that touch, and this face is monospaced, so equal columns are right.
    for (const Piece& one : pieces) {
        int count = 1;
        if (one.w > oneWide * 3 / 2) {
            count = (int)std::lround((double)one.w / oneWide);
            if (count < 1) count = 1;
            if (count > 8) continue;
        }
        for (int k = 0; k < count; k++) {
            const int from = one.x + one.w * k / count;
            const int wide = one.w * (k + 1) / count - one.w * k / count;
            if (wide < 2) continue;
            cv::Mat piece = lit(cv::Rect(from, one.y, wide, one.h));
            cv::Mat shaped;
            cv::resize(piece, shaped, cv::Size(PANEL_GLYPH_W, PANEL_GLYPH_H), 0, 0,
                       cv::INTER_AREA);
            shaped.convertTo(shaped, CV_32F, 1.0 / 255.0);
            out.push_back({shaped, cv::Rect((int)(from / by), (int)(one.y / by),
                                            (int)(wide / by), (int)(one.h / by))});
        }
    }
    std::sort(out.begin(), out.end(), [](const PanelGlyph& a, const PanelGlyph& b) {
        return a.box.x < b.box.x;
    });
    return out;
}

// ---------------------------------------------------------------------------
// The figures in the HUD's own face.
//
// Tesseract is made for print and this is not print: numerals thirteen pixels
// tall, a zero with a stroke through it, a glow around every glyph. It reads
// mass, resistance and instability all right in about one frame of nine:
// "0 %" comes back as 8, 9 or 2, "0.00" not at all, 5,359 as 9,359. So every
// glyph is compared with numerals harvested from the HUD itself, and a glyph
// that is not clearly one of them is not guessed at.

namespace {

constexpr double FACE_MIN_SCORE = 0.70;
constexpr double FACE_MIN_MARGIN = 0.04;

char faceClassOf(const cv::Mat& shape, double& score, double& margin) {
    // Each glyph has several shapes in the table - a numeral looks a little
    // different over a dark sky and a bright one - so the margin that says
    // how sure the answer is lies between the best shape of the best glyph
    // and the best shape of any OTHER glyph, not the runner-up shape
    std::map<char, double> bestOf;
    for (int t = 0; t < PANEL_FACE_COUNT; t++) {
        const cv::Mat templ(FACE_H, FACE_W, CV_32F, const_cast<float*>(PANEL_FACE[t].shape));
        const double s = shape.dot(templ);
        auto it = bestOf.find(PANEL_FACE[t].glyph);
        if (it == bestOf.end() || s > it->second) bestOf[PANEL_FACE[t].glyph] = s;
    }
    char glyph = '?';
    double best = -2, second = -2;
    for (const auto& entry : bestOf) {
        if (entry.second > best) {
            second = best;
            best = entry.second;
            glyph = entry.first;
        } else if (entry.second > second) {
            second = entry.second;
        }
    }
    score = best;
    margin = best - second;
    return glyph;
}

std::string withoutLeadingStops(const std::string& text) {
    size_t at = 0;
    while (at < text.size() && text[at] == '.') at++;
    return text.substr(at);
}

// "12805" and nothing else
float faceWhole(const std::string& read) {
    const std::string t = withoutLeadingStops(read);
    if (t.empty() || t.size() > 7) return -1;
    for (char c : t) {
        if (c < '0' || c > '9') return -1;
    }
    return (float)std::atof(t.c_str());
}

// "84%" and nothing else
float facePercent(const std::string& read) {
    const std::string t = withoutLeadingStops(read);
    if (t.size() < 2 || t.size() > 4 || t.back() != '%') return -1;
    for (size_t i = 0; i + 1 < t.size(); i++) {
        if (t[i] < '0' || t[i] > '9') return -1;
    }
    return (float)std::atof(t.c_str());
}

// A volume read off the panel held against the rock's mass. Over 1,395
// readings of every seat recorded the rocks weighed 143 to 1,514 kg an SCU,
// so a volume outside 50 to 5,000 kg an SCU is none of this rock's - and a
// thousandth of it may be: under one SCU the panel writes thousandths,
// "364.01m SCU", which a reader that cannot tell the m (the face reader, the
// old one) reads as 364.01 SCU. `thousandths` says the m was seen, and only
// a thousandth is taken then; `orThousandths` lets a reading that did not see
// it be one where only that fits. -1 for neither; the volume as read where
// the mass is not known, unless it is in thousandths.
float volumeOfMass(float volume, float mass, bool thousandths, bool orThousandths) {
    if (volume <= 0) return -1;
    if (thousandths) volume /= 1000.0f;
    if (mass <= 0) return thousandths ? -1 : volume;
    auto fits = [&](float v) {
        const float perScu = mass / v;
        return perScu >= 50.0f && perScu <= 5000.0f;
    };
    if (fits(volume)) return volume;
    if (!thousandths && orThousandths && fits(volume / 1000.0f)) return volume / 1000.0f;
    return -1;
}

// "13.86" - with nothing after it when `exact`, and anything after it when
// not (the volume is followed by the letters of SCU)
float faceTwoPlaces(const std::string& read, bool exact) {
    const std::string t = withoutLeadingStops(read);
    const size_t stop = t.find('.');
    if (stop == std::string::npos || stop == 0 || stop > 6 || t.size() < stop + 3) return -1;
    for (size_t i = 0; i < stop + 3; i++) {
        if (i == stop) continue;
        if (t[i] < '0' || t[i] > '9') return -1;
    }
    if (exact && t.size() != stop + 3) return -1;
    return (float)std::atof(t.substr(0, stop + 3).c_str());
}

}  // namespace

std::string readFaceRow(const PanelInk& ink, const cv::Rect& row) {
    std::string out;
    for (const FaceGlyph& g : faceGlyphs(ink, row)) {
        if (g.stop) {
            out += '.';
            continue;
        }
        double score = 0, margin = 0;
        const char c = faceClassOf(g.shape, score, margin);
        out += (score >= FACE_MIN_SCORE && margin >= FACE_MIN_MARGIN) ? c : '?';
    }
    return out;
}

namespace {

// A run of glyphs with no wide gap in it: a label, a figure, a word.
struct FaceToken {
    int x0 = 0, x1 = 0;
    std::string text;
    cv::Rect box;
    std::vector<cv::Mat> shapes;      // one per glyph of text, empty for a stop
    std::vector<cv::Rect> boxes;      // one per glyph of text, in the frame
};

std::vector<FaceToken> faceTokens(const std::vector<std::vector<FaceGlyph>>& words) {
    std::vector<FaceToken> out;
    for (const std::vector<FaceGlyph>& word : words) {
        FaceToken t;
        t.x0 = word.front().box.x;
        t.box = word.front().box;
        for (const FaceGlyph& g : word) {
            char c = '.';
            if (!g.stop) {
                double score = 0, margin = 0;
                const char best = faceClassOf(g.shape, score, margin);
                c = (score >= FACE_MIN_SCORE && margin >= FACE_MIN_MARGIN) ? best : '?';
                if (DEV_ENV("SCMINER_FACE_TRACE")) {
                    std::cout << "      glyph at " << g.box.x << "," << g.box.y << " " << g.box.width << "x" << g.box.height << ": best " << best << " " << score << ", margin " << margin << std::endl;
                }
            }
            t.text += c;
            t.box |= g.box;
        }
        t.x1 = t.box.x + t.box.width;
        out.push_back(t);
    }
    return out;
}

bool isLabel(const FaceToken& t) {
    int unknown = 0;
    for (char c : t.text) {
        if (c == '?') unknown++;
    }
    return unknown >= 2;
}

}  // namespace

// A line of text in a region, and its words read in the face.
struct FaceLine {
    cv::Rect rect;                    // frame coordinates
    std::vector<FaceToken> tokens;
};

// The lines of a region, out of its own ink - built piece by piece rather
// than from where the ink is thick enough across the whole width. A band
// across the width runs two lines together wherever a speck of rock or the
// glow of a glyph bridges the gap between them, and then a numeral of one
// line is read as part of the next: 13.86 came back as 1.86 that way. A piece
// belongs to the line it overlaps, and nothing between two lines joins them.
namespace {

// One glyph cell of a word: its columns and rows in the word's mask
struct Cell {
    int x0, x1;
};

// What a stretch of columns of a word is, in the face - and how sure
char classifyCell(const PanelInk& ink, const cv::Mat& mask, const cv::Rect& wordInArea, int x0, int x1,
                  double& score, double& margin, cv::Rect& box, cv::Mat& shapeOut) {
    int top = mask.rows, bottom = -1;
    for (int y = 0; y < mask.rows; y++) {
        if (cv::countNonZero(mask.row(y).colRange(x0, x1)) > 0) {
            top = (std::min)(top, y);
            bottom = (std::max)(bottom, y);
        }
    }
    score = -2;
    margin = 0;
    if (bottom < top) return '?';
    box = cv::Rect(wordInArea.x + x0, wordInArea.y + top, x1 - x0, bottom - top + 1);
    cv::Mat shape;
    cv::resize(ink.structure(box), shape, cv::Size(FACE_W, FACE_H), 0, 0, cv::INTER_AREA);
    shape.convertTo(shape, CV_32F);
    shape -= cv::mean(shape)[0];
    const double length = cv::norm(shape);
    if (length < 1e-3) return '?';
    shape /= length;
    shapeOut = shape;
    return faceClassOf(shape, score, margin);
}

}  // namespace

std::vector<FaceLine> faceLinesOf(const PanelInk& ink) {
    std::vector<FaceLine> out;
    if (ink.ink.empty()) return out;
    cv::Mat labels, stats, centres;
    const int found = cv::connectedComponentsWithStats(ink.ink, labels, stats, centres, 8, CV_32S);
    const int tallest = (std::max)(12, ink.area.height / 8);

    struct Comp {
        int x0, y0, x1, y1, area, id;
    };
    std::vector<Comp> comps;
    for (int i = 1; i < found; i++) {
        const int a = stats.at<int>(i, cv::CC_STAT_AREA);
        const int w = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        if (a < 2 || h > tallest || w > ink.area.width / 2) continue;
        const int x = stats.at<int>(i, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(i, cv::CC_STAT_TOP);
        comps.push_back({x, y, x + w, y + h, a, i});
    }
    // Lines: the tall pieces decide where they are, the small ones join them
    std::sort(comps.begin(), comps.end(), [](const Comp& a, const Comp& b) {
        return (a.y1 - a.y0) > (b.y1 - b.y0);
    });
    struct Span {
        int y0, y1;
        std::vector<Comp> members;
    };
    std::vector<Span> spans;
    for (const Comp& c : comps) {
        const int h = c.y1 - c.y0;
        Span* home = nullptr;
        for (Span& sp : spans) {
            const int overlap = (std::min)(sp.y1, c.y1) - (std::max)(sp.y0, c.y0);
            if (overlap * 2 >= (std::min)(h, sp.y1 - sp.y0)) {
                home = &sp;
                break;
            }
        }
        if (!home) {
            if (h < 5) continue;
            spans.push_back({c.y0, c.y1, {}});
            home = &spans.back();
        }
        home->members.push_back(c);
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.y0 < b.y0; });

    for (Span& sp : spans) {
        std::vector<Comp>& members = sp.members;
        std::sort(members.begin(), members.end(), [](const Comp& a, const Comp& b) { return a.x0 < b.x0; });
        std::vector<int> heights;
        for (const Comp& c : members) {
            if (c.area >= 6) heights.push_back(c.y1 - c.y0);
        }
        if (heights.empty()) continue;
        std::sort(heights.begin(), heights.end());
        const int typical = heights[heights.size() * 7 / 10];
        if (typical < 5) continue;

        // Words: a gap of half a glyph's height between two pieces ends one.
        // Not against the pieces' widths: a colon's dots and the specks of a rock
        // are narrow, and a gap that small comes between the 0 and the per cent
        // sign of "0%"
        std::vector<std::vector<Comp>> words;
        int reach = -1000000;
        for (const Comp& c : members) {
            if (words.empty() || c.x0 - reach > (std::max)(3, typical / 2)) words.push_back({});
            words.back().push_back(c);
            reach = (std::max)(reach, c.x1);
        }

        FaceLine line;
        int lineTop = sp.y0, lineBottom = sp.y1;
        for (const std::vector<Comp>& word : words) {
            int x0 = word.front().x0, x1 = 0, y0 = 1000000, y1 = 0;
            for (const Comp& c : word) {
                x0 = (std::min)(x0, c.x0);
                x1 = (std::max)(x1, c.x1);
                y0 = (std::min)(y0, c.y0);
                y1 = (std::max)(y1, c.y1);
            }
            lineTop = (std::min)(lineTop, y0);
            lineBottom = (std::max)(lineBottom, y1);
            const cv::Rect wordBox(x0, y0, x1 - x0, y1 - y0);

            // The word's numeral height and its line, from its own pieces
            std::vector<int> hs;
            for (const Comp& c : word) {
                if (c.area >= 6) hs.push_back(c.y1 - c.y0);
            }
            if (hs.empty()) continue;
            std::sort(hs.begin(), hs.end());
            const int H = hs[hs.size() * 7 / 10];
            std::vector<int> bottoms;
            for (const Comp& c : word) {
                if ((c.y1 - c.y0) * 10 >= H * 7) bottoms.push_back(c.y1);
            }
            std::sort(bottoms.begin(), bottoms.end());
            const int baseline = bottoms.empty() ? y1 : bottoms[bottoms.size() / 2];

            // Full stops first, as pieces of their own. Left in, a stop sits
            // in the column next to the numeral before it and the two are
            // read as one glyph - the 3 of 13.86 came out that way, a glyph
            // taller than any numeral and like none of them. A stop is small
            // and square and sits on the line; the lower ring of a per cent
            // sign is too big to be one.
            std::vector<int> stops;
            std::map<int, cv::Rect> boxesAt;              // x, where in the frame
            cv::Mat mask = cv::Mat::zeros(wordBox.size(), CV_8U);
            for (const Comp& c : word) {
                const int h = c.y1 - c.y0, w = c.x1 - c.x0;
                if (h * 10 <= H * 3 + 10 && w * 10 <= H * 3 + 10 &&
                    std::abs(c.y1 - baseline) <= (std::max)(2, H / 5)) {
                    stops.push_back((c.x0 + c.x1) / 2 - wordBox.x);
                    boxesAt[stops.back()] = cv::Rect(c.x0 + ink.area.x, c.y0 + ink.area.y, w, h);
                    continue;
                }
                if (c.area * 20 < H * H) continue;        // a speck, not a stroke
                mask.setTo(255, labels(wordBox) == c.id);
            }
            std::vector<int> columns(wordBox.width);
            for (int x = 0; x < wordBox.width; x++) columns[x] = cv::countNonZero(mask.col(x));

            const double T = 0.72 * H;
            std::vector<Cell> runs;
            for (int x = 0; x < wordBox.width;) {
                if (columns[x] == 0) {
                    x++;
                    continue;
                }
                int e = x;
                while (e < wordBox.width && columns[e] > 0) e++;
                runs.push_back({x, e});
                x = e;
            }

            FaceToken token;
            token.x0 = wordBox.x + ink.area.x;
            bool first = true;
            std::vector<std::pair<int, char>> glyphs;      // x, what
            std::map<int, cv::Mat> shapesAt;              // x, its shape
            std::function<void(int, int, int)> emit = [&](int a, int b, int depth) {
                double score = 0, margin = 0;
                cv::Rect box;
                cv::Mat shape;
                const char best = classifyCell(ink, mask, wordBox, a, b, score, margin, box, shape);
                if (box.width <= 0) return;
                const bool sure = score >= FACE_MIN_SCORE && margin >= FACE_MIN_MARGIN;
                if (!sure && (b - a) > T * 1.35 && depth < 4) {
                    // Two glyphs that touch: cut where they are thinnest
                    const int from = a + (int)std::lround(T * 0.3);
                    const int to = b - (int)std::lround(T * 0.3);
                    int cut = -1, least = 1 << 30;
                    for (int x = from; x < to; x++) {
                        if (columns[x] < least) {
                            least = columns[x];
                            cut = x;
                        }
                    }
                    if (cut > a && cut < b) {
                        emit(a, cut, depth + 1);
                        emit(cut + (least == 0 ? 1 : 0), b, depth + 1);
                        return;
                    }
                }
                if (DEV_ENV("SCMINER_FACE_TRACE")) {
                    std::cout << "      glyph at " << box.x + ink.area.x << "," << box.y + ink.area.y << " "
                              << box.width << "x" << box.height << ": best " << best << " " << score
                              << ", margin " << margin << std::endl;
                }
                glyphs.push_back({a, sure ? best : '?'});
                shapesAt[a] = shape;
                const cv::Rect inFrame(box.x + ink.area.x, box.y + ink.area.y, box.width, box.height);
                boxesAt[a] = inFrame;
                token.box = first ? inFrame : (token.box | inFrame);
                first = false;
            };
            for (const Cell& r : runs) emit(r.x0, r.x1, 0);
            for (int x : stops) glyphs.push_back({x, '.'});
            std::sort(glyphs.begin(), glyphs.end(),
                      [](const std::pair<int, char>& a, const std::pair<int, char>& b) { return a.first < b.first; });
            for (const auto& g : glyphs) {
                token.text += g.second;
                token.shapes.push_back(g.second == '.' ? cv::Mat() : shapesAt[g.first]);
                token.boxes.push_back(boxesAt[g.first]);
            }
            if (first) continue;          // nothing but stops
            token.x1 = token.box.x + token.box.width;
            line.tokens.push_back(token);
        }
        if (line.tokens.empty()) continue;
        line.rect = cv::Rect(ink.area.x, ink.area.y + lineTop, ink.area.width, lineBottom - lineTop);
        out.push_back(line);
    }
    return out;
}

bool readFiguresIn(const cv::Mat& frame, const cv::Rect& region, ScanPanel& panel) {
    const PanelInk ink = panelInk(frame, region);
    if (ink.ink.empty()) return false;

    const std::vector<FaceLine> lines = faceLinesOf(ink);
    if (lines.size() < 3) return false;

    // The figures a line could hold of one kind: a word that reads as the
    // format, with its label somewhere to its left. The label is only asked to
    // be a word of three glyphs or more - what it reads as does not matter.
    // MASS: came back as ?4?5. over a bright rock, its A and S close enough to
    // a 4 and a 5, and specks of rock run into a label's left end and move it.
    struct Figure {
        const FaceToken* label;
        const FaceToken* value;
        float v;
    };
    auto figuresOf = [](const FaceLine& line, int kind) {
        std::vector<Figure> out;
        for (size_t k = 1; k < line.tokens.size(); k++) {
            const FaceToken& t = line.tokens[k];
            const float v = kind == 0 ? faceWhole(t.text) : kind == 1 ? facePercent(t.text)
                                                                    : faceTwoPlaces(t.text, true);
            if (v < 0) continue;
            const int H = (std::max)(6, t.box.height);
            for (size_t j = k; j-- > 0;) {
                const FaceToken& l = line.tokens[j];
                if (l.text.size() < 3) continue;
                if (l.x1 <= t.x0 && t.x0 - l.x1 <= H * 12) out.push_back({&l, &t, v});
                break;
            }
        }
        return out;
    };
    std::vector<std::vector<Figure>> figures[3];
    for (int kind = 0; kind < 3; kind++) {
        for (const FaceLine& line : lines) figures[kind].push_back(figuresOf(line, kind));
    }

    // Mass, then resistance, then instability, in that order down the panel -
    // but not always on consecutive lines: the charge arc beside the panel
    // writes OPTIMAL at the height of the gap between two of them, and noise
    // makes lines of its own. The three figures stand under one another, left
    // edges together, a line's pitch apart.
    for (size_t i = 0; i < lines.size(); i++) {
      for (const Figure& fm : figures[0][i]) {
        if (fm.v < 1) continue;
        for (size_t j = i + 1; j < lines.size() && j <= i + 3; j++) {
          for (const Figure& fr : figures[1][j]) {
            for (size_t q = j + 1; q < lines.size() && q <= j + 3; q++) {
              for (const Figure& fi : figures[2][q]) {
        const FaceToken& massT = *fm.value;
        const FaceToken& resT = *fr.value;
        const FaceToken& instT = *fi.value;
        const int H = (std::max)({6, massT.box.height, instT.box.height});
        const int glyph = (std::max)(4, H * 3 / 4);
        const int values = (std::max)({massT.x0, resT.x0, instT.x0}) -
                           (std::min)({massT.x0, resT.x0, instT.x0});
        if (values > glyph * 2) continue;
        const int b0 = massT.box.y + massT.box.height;
        const int b1 = resT.box.y + resT.box.height;
        const int b2 = instT.box.y + instT.box.height;
        const int p0 = b1 - b0, p1 = b2 - b1;
        if (p0 < H || p1 < H || p0 > H * 3 || p1 > H * 3) continue;
        if (std::abs(p0 - p1) * 10 > (std::max)(p0, p1) * 3) continue;
        const FaceLine& a = lines[i];
        const FaceLine& c = lines[q];
        // the labels' left edge: the middle one of the three, as noise
        // stretches a label to the left and never to the right
        int labelEdge = (std::max)({(std::min)(fm.label->x0, fr.label->x0),
                                    (std::min)((std::max)(fm.label->x0, fr.label->x0), fi.label->x0)});
        labelEdge = (std::max)(labelEdge, massT.x0 - H * 16);
        const FaceToken* massL = fm.label;
        const float mass = fm.v, res = fr.v, inst = fi.v;

        panel.found = true;
        panel.stats.mass = mass;
        panel.stats.resistance = res;
        panel.stats.instability = inst;
        panel.stats.resistanceAdjusted = printedInColour(asBgr(frame), resT.box);
        panel.box = cv::Rect(cv::Point(labelEdge, (std::min)(massL->box.y, massT.box.y)),
                             cv::Point((std::max)({massT.x1, resT.x1, instT.x1}), b2));
        panel.headRule = a.rect.y;
        panel.bodyRule = c.rect.y + c.rect.height;
        // the ore's line is the one above, from the labels' edge on
        if (i >= 1) {
            const FaceLine& above = lines[i - 1];
            if (a.rect.y - (above.rect.y + above.rect.height) < a.rect.height * 3) {
                panel.oreRow = cv::Rect(labelEdge - 2, above.rect.y,
                                        (std::max)(10, ink.area.x + ink.area.width - labelEdge), above.rect.height);
            }
        }
        // the volume is on one of the next lines: "COMP.  21.30 SCU"
        for (size_t k = q + 1; k < lines.size() && k < q + 5; k++) {
            for (size_t t = 1; t < lines[k].tokens.size(); t++) {
                const float read = faceTwoPlaces(lines[k].tokens[t].text, false);
                if (read < 0 || read >= 1000.0f) continue;
                // (in thousandths where the mass says so: it reads no m)
                const float scu = volumeOfMass(read, panel.stats.mass, false, true);
                if (scu > 0) {
                    panel.stats.scu = scu;
                    break;
                }
            }
            if (panel.stats.scu > 0) break;
        }
        panel.note += "; face \"" + massT.text + "\" \"" + resT.text + "\" \"" + instT.text + "\"";
        return true;
              }
            }
          }
        }
      }
    }
    return false;
}

std::vector<std::pair<cv::Rect, std::string>> faceLinesIn(const cv::Mat& frame, const cv::Rect& region) {
    std::vector<std::pair<cv::Rect, std::string>> out;
    for (const FaceLine& line : faceLinesOf(panelInk(frame, region))) {
        std::string text;
        for (const FaceToken& t : line.tokens) text += (text.empty() ? "" : " | ") + t.text;
        out.push_back({line.rect, text});
    }
    return out;
}

namespace {

// Long rules found without one level for the whole picture. The brightness
// structure the search uses is cut at a level Otsu picks over everything on
// screen, and in front of a bright nebula that level climbs past the panel's
// own hairlines. A vertical top-hat takes the ground away whatever its
// brightness, and a hairline is left standing at the same height over a dark
// sky or a bright one - so a fixed level holds, and the long horizontal
// opening keeps only what is long.
std::vector<PanelRule> steadyRules(const cv::Mat& work) {
    const cv::Mat tophat = channelStructure(asBgr(work), 15);
    cv::Mat cut = tophat > 28;
    cv::Mat rules;
    cv::morphologyEx(cut, rules, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(RULE_MIN_W, 1)));
    std::vector<std::vector<cv::Point>> blobs;
    cv::findContours(rules, blobs, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::vector<PanelRule> out;
    for (const auto& blob : blobs) {
        const cv::Rect r = cv::boundingRect(blob);
        if (r.height > 6 || r.width < RULE_MIN_W || r.width > RULE_MAX_W) continue;
        out.push_back({r.y + r.height / 2, r.x, r.x + r.width});
    }
    return out;
}

}  // namespace

std::vector<FaceLineInfo> faceLinesDetailed(const cv::Mat& frame, const cv::Rect& region) {
    std::vector<FaceLineInfo> out;
    for (const FaceLine& line : faceLinesOf(panelInk(frame, region))) {
        FaceLineInfo info;
        info.rect = line.rect;
        for (const FaceToken& t : line.tokens) info.tokens.push_back({t.text, t.box, t.shapes, t.boxes});
        out.push_back(info);
    }
    return out;
}

// Where to look: around every pair of rules, and around every long rule on
// its own. The figures do not have to sit between the right pair - they only
// have to be near a rule of the panel's own width, and the format finds them.
std::vector<cv::Rect> figureRegions(const cv::Mat& frame) {
    StageClock clock("  figureRegions");
    std::vector<cv::Rect> out;
    const cv::Rect whole(0, 0, frame.cols, frame.rows);
    auto add = [&](const cv::Rect& r) {
        const cv::Rect clipped = r & whole;
        if (clipped.width < 40 || clipped.height < 40) return;
        for (const cv::Rect& had : out) {
            const double shared = (double)(had & clipped).area();
            if (shared > 0.7 * (std::min)(had.area(), clipped.area())) return;
        }
        out.push_back(clipped);
    };
    for (const ScanPanel& c : scanPanelCandidates(frame, 8)) {
        if (!c.found) continue;
        const int w = c.box.width;
        add(cv::Rect(c.box.x - w / 10, c.box.y - c.box.height, w + w / 5, c.box.height * 2));
    }
    const double scale = (double)REFERENCE_HEIGHT / (double)frame.rows;
    cv::Mat work;
    if (std::abs(scale - 1.0) < 0.02) {
        work = frame;
    } else {
        cv::resize(frame, work, cv::Size(), scale, scale, scale < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
    }
    const double back = 1.0 / scale;
    std::vector<PanelRule> rules = findHorizontalRules(work, RULE_MIN_W, RULE_MAX_W);
    for (const PanelRule& r : steadyRules(work)) rules.push_back(r);
    for (const PanelRule& r : rules) {
        const int w = (int)std::lround(r.width() * back);
        const int x0 = (int)std::lround(r.x0 * back);
        const int y = (int)std::lround(r.y * back);
        add(cv::Rect(x0 - w * 3 / 10, y - w * 7 / 5, w * 8 / 5, w * 11 / 5));
        if (out.size() >= 24) break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The shape reader.
//
// The face reader cuts the panel into ink first and reads the ink, and over a
// bright rock the ink is half rock: the lines run together, a speck on a
// numeral makes it a stranger, a laser's trail joins a label to its figure.
// This one never cuts. The panel's labels are the same three words in every
// frame, so they are looked for as a picture - the averaged column of MASS:,
// RES: and INST: - in the darkest of the three channels, where white text
// stays white and a blue rock or an orange glow goes dark. Where the labels
// are, the figures are too: a fixed step to the right, a line apart. Each
// figure is then matched glyph by glyph against the averaged numerals at the
// places a numeral can be, and the figure's format says where it ends.
// Measured at 1440 lines, the size the shapes were cut at; everything scales
// with the labels.

namespace {

constexpr int SHAPE_LABEL_INSET = 3;      // the label shape starts this far before the labels
constexpr int SHAPE_VALUE_DX = 118;        // a figure's left edge, right of the labels' left edge
constexpr int SHAPE_ROW_PITCH = 25;
constexpr int SHAPE_FIRST_BOTTOM = 9;      // the mass figure's bottom, below the MASS label's top
constexpr int SHAPE_DIGIT_H = 11;
constexpr int SHAPE_ABOVE = 2;             // sky above a numeral in its shape
constexpr int SHAPE_ADVANCE = 9;           // numeral to numeral, centre to centre
constexpr int SHAPE_STOP_ADVANCE = 13;     // the same across a full stop
constexpr int SHAPE_PCT_ADVANCE = 11;      // last numeral to the per cent sign
constexpr int SHAPE_FIRST_CENTRE = 4;      // first glyph's centre, right of the figure's left edge
constexpr double SHAPE_SCU_LAST_CENTRE = 183.5;  // the volume's last numeral, right of the labels' edge
// ...and how much further left it stands under one SCU, where an m comes
// between it and SCU ("364.01m SCU"), at the panel's size: tried from 2 to 20
// (SCMINER_SCU_PROBE), the numerals read from 8.5 to 14.5 further left over
// 82 turret frames and from 4.5 to 15.5 over 4 cockpit ones (21 October 2026)
constexpr double SHAPE_M_SHIFT = 11.5;
constexpr int SHAPE_SCU_BOTTOM = 128;      // ...and its bottom, below the MASS label's top

// The numerals a panel is written in: which shapes, how tall, how far apart.
// The Prospector's HUD and the MOLE's pilot seat share one font; the MOLE's
// turrets write theirs wider and rounder, at almost the same places -
// measured off 3,241 glyphs (--turretharvest).
struct PanelFont {
    const char* numerals;      // the shapes of 0 to 9, then the per cent sign
    double digitH, above, advance, stopAdvance, pctAdvance, firstCentre;
    bool clustered;            // named by the clustered numerals (PanelDigits.h) or by its own averages
};
const PanelFont PROSPECTOR_FONT{"0123456789%", SHAPE_DIGIT_H, SHAPE_ABOVE, SHAPE_ADVANCE, SHAPE_STOP_ADVANCE,
                                SHAPE_PCT_ADVANCE, SHAPE_FIRST_CENTRE, true};
const PanelFont TURRET_FONT{"abcdefghijp", 11.0, 2.0, 8.8, 13.5, 11.3, 3.9, false};
// The Golem writes the turret's numerals, set four per cent tighter against
// its labels' size: measured off 233 glyphs in 26 frames
// (--turretharvest G 4)
const PanelFont GOLEM_FONT{"abcdefghijp", 10.8, 2.0, 8.45, 12.8, 10.9, 3.9, false};

// Where things sit on a panel, at size 1, from the labels' left edge and the
// MASS label's top. Two panels are known. The short one writes MASS: RES:
// INST: and COMP.; the long one writes its labels out - RESISTANCE:,
// INSTABILITY:, COMPOSITION - and spaces its rows a little wider (26 against
// 25), with the figures three pixels further right. The long panel is
// measured on a single 1080p picture, so it is less sure than the short one.
struct PanelLayout {
    char labels;                       // the label column's shape
    const char* name;
    double valueDx, rowPitch, firstBottom;
    double labelsCx, labelsCy;         // the centre of the label column's shape
    bool comp;                         // COMP. is there to pin the size by
    double compCx, compCy;
    double scuCx, scuCy;               // the centre of the word SCU
    double scuLastCentre, scuBottom;   // the volume's last numeral
    double valueShear = 0;             // how much further left each row's figure starts than the one above
    const PanelFont* font = nullptr;   // nullptr: the Prospector's
};
const PanelLayout SHORT_PANEL{'L', "short", SHAPE_VALUE_DX, SHAPE_ROW_PITCH, SHAPE_FIRST_BOTTOM, 21.0, 32.0,
                              true, 25.0, 125.5, 205.5, 122.5, SHAPE_SCU_LAST_CENTRE, SHAPE_SCU_BOTTOM};
const PanelLayout LONG_PANEL{'M', "long", 121.3, 26.05, 10.9, 56.0, 33.5,
                             false, 0.0, 0.0, 209.1, 128.1, 189.5, 133.6};
// The MOLE's turret: labels of their own (see SHAPE_TURRET_LABELS) at about 1.54
// of the size, figures in a font of their own. Its places were measured off
// the face reader's glyph boxes on 320 frames: the figures stand four units
// lower than the Prospector's (13 against 9) and each row starts 1.8 further
// left than the one above - 118.7, 116.5, 115.1 - the panel is slightly
// sheared. Its volume, in the same numerals under the EASY bar, ends 172.7
// right of the labels' edge and stands 129.8 below - off 305 of them.
const PanelLayout TURRET_PANEL{'T', "turret", 118.7, 24.7, 13.0, 21.0, 32.0,
                               true, 25.0, 125.5, 205.5, 122.5, 172.7, 129.8,
                               -1.8, &TURRET_FONT};
// ...and the same turret with the game's own labels written out (see
// SHAPE_TURRET_LONG_LABELS, 100 wide). Its places off the face reader's glyph
// boxes on 8 frames: the figures 115.8 right of the labels' edge and 2.2
// further left each row down, the rows 24.3 apart, the mass's bottom 10.9
// below the MASS label's top - two units higher than with the short labels,
// whose column shape is anchored a little differently. Not pinned: it writes
// COMPOSITION for COMP., and no place is known for it. Its volume ends 167.6
// right of the labels' edge and stands 125.5 below, off the same 8 frames.
const PanelLayout TURRET_LONG_PANEL{'W', "turret long", 115.8, 24.3, 10.9, 47.0, 32.0,
                                    false, 0.0, 0.0, 0.0, 0.0, 167.6, 125.5,
                                    -2.2, &TURRET_FONT};
// The Golem: amber labels of their own (see SHAPE_GOLEM_LABELS, 56 wide), the
// figures in the turret's numerals. Its places off the face reader's glyph
// boxes on 26 frames: the figures 114.5 right of the labels' edge in every
// row, the rows 24.5 apart, the mass's bottom 12.65 below the MASS label's
// top. Its size is not pinned - no SCU place is known - so its scale is the
// labels' own. Its HUD is drawn in the world and grows and shrinks with the
// view, from 1.15 to 1.8. Where its volume stands is not known: it is not
// read (0.0).
const PanelLayout GOLEM_PANEL{'G', "golem", 114.5, 24.5, 12.65, 25.0, 32.0,
                              false, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                              0.0, &GOLEM_FONT};
// ...and with the game's own labels written out (see SHAPE_GOLEM_LONG_LABELS,
// 112 wide). Its places measured against that column on 96 frames with white
// figures, the same way as for the short column: the figures 0.4 further
// left, the mass a unit higher, the rows 25.15 apart.
const PanelLayout GOLEM_LONG_PANEL{'H', "golem long", 114.1, 25.15, 11.7, 53.0, 32.0,
                                   false, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                                   0.0, &GOLEM_FONT};
// ...and its pilot seat: the short panel's places exactly, with narrow labels
// (see SHAPE_COCKPIT_LABELS). With the Prospector's column the search found
// the labels a row too high, 0.77, and read the ore's line as the mass.
const PanelLayout COCKPIT_PANEL{'K', "cockpit", SHAPE_VALUE_DX, SHAPE_ROW_PITCH, SHAPE_FIRST_BOTTOM, 21.0, 32.0,
                                true, 25.0, 125.5, 205.5, 122.5, SHAPE_SCU_LAST_CENTRE, SHAPE_SCU_BOTTOM};
// ...and the same seat with the game's own labels written out (see
// SHAPE_COCKPIT_LONG_LABELS, 76 wide). Measured on one snapshot with the
// panel at 0.937: the figures 107 px right of the labels' edge, the rows
// 23 px apart, the mass's bottom 9 px below the MASS label's top - 115.1,
// 24.7 and 9.7 at the size the shape was cut for. The figures are white and
// in the Prospector's numerals, as in the same seat with the short labels.
// Not pinned: it writes COMPOSITION for COMP., and no SCU place is known
// for it.
const PanelLayout COCKPIT_LONG_PANEL{'P', "cockpit long", 115.05, 24.73, 9.68, 38.0, 35.0,
                                     false, 0.0, 0.0, 0.0, 0.0, SHAPE_SCU_LAST_CENTRE, SHAPE_SCU_BOTTOM};

// ...and which of them a label column's shape stands for.
const PanelLayout& layoutOf(char glyph) {
    return glyph == 'T'   ? TURRET_PANEL
           : glyph == 'W' ? TURRET_LONG_PANEL
           : glyph == 'K' ? COCKPIT_PANEL
           : glyph == 'P' ? COCKPIT_LONG_PANEL
           : glyph == 'G' ? GOLEM_PANEL
           : glyph == 'H' ? GOLEM_LONG_PANEL
           : glyph == 'M' ? LONG_PANEL
                          : SHORT_PANEL;
}
constexpr float SHAPE_GLYPH_FLOOR = 0.45f; // below this there is no glyph at all

const PanelShape& shapeFor(char glyph) {
    for (int i = 0; i < PANEL_SHAPE_COUNT; i++) {
        if (PANEL_SHAPES[i].glyph == glyph) return PANEL_SHAPES[i];
    }
    return PANEL_SHAPES[0];
}

cv::Mat shapeSized(const PanelShape& shape, double scale) {
    const cv::Mat grey(shape.height, shape.width, CV_32F, const_cast<float*>(shape.grey));
    if (std::abs(scale - 1.0) < 0.005) return grey.clone();
    cv::Mat out;
    cv::resize(grey, out,
               cv::Size((std::max)(3, (int)std::lround(shape.width * scale)),
                        (std::max)(3, (int)std::lround(shape.height * scale))),
               0, 0, scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    return out;
}

// 0 darkest, 1 brightest, 2 green, 3 red, 4 red over blue (orange lettering),
// 5 white and nothing coloured (LAMP by day), 6 amber (the Golem's lettering,
// green over blue less red over green), 7 dark on bright (red figures on a
// ground LAMP by day has washed nearly white) - as float
cv::Mat channelGrey(const cv::Mat& bgr, int which) {
    std::vector<cv::Mat> planes;
    cv::split(bgr, planes);
    cv::Mat out;
    if (which == 0) out = cv::min(cv::min(planes[0], planes[1]), planes[2]);
    else if (which == 5) {
        // White and nothing coloured: the darkest channel less twice how
        // coloured the pixel is. LAMP by day washes the Prospector's view
        // pale blue - 207/219/240 behind labels of 247/253/255 - and the
        // labels stand out of that ground by 40 in the darkest channel; the
        // HUD itself is not tinted, and in this ink they stand out by 97.
        // Not the ink for everything: the 1080p test picture's lettering has
        // coloured fringes, and in this ink its labels matched 0.48 for 0.98.
        const cv::Mat least = cv::min(cv::min(planes[0], planes[1]), planes[2]);
        const cv::Mat most = cv::max(cv::max(planes[0], planes[1]), planes[2]);
        // least - 2 (most - least) = 3 least - 2 most, in one pass
        cv::Mat f;
        cv::addWeighted(least, 3.0, most, -2.0, 0.0, f, CV_32F);
        return cv::max(f, 0.0);
    }
    else if (which == 1) out = cv::max(cv::max(planes[0], planes[1]), planes[2]);
    else if (which == 4) cv::subtract(planes[2], planes[0], out);   // stops at 0
    else if (which == 7) {
        // Under LAMP by day the Golem's red instability stands on a ground
        // washed to 251/192/134, darker than it in every channel - 248/156/104
        // - and in none of the inks above brighter by enough to read. Turned
        // over, it is lettering on a ground like any other.
        cv::Mat g8, f;
        cv::cvtColor(bgr, g8, cv::COLOR_BGR2GRAY);
        g8.convertTo(f, CV_32F);
        return 255.0 - f;
    }
    else if (which == 6) {
        // The Golem writes its labels in amber - 255/218/128 by day, 255/202/87
        // under its own LAMP - on red rock (153/73/57) or an amber-lit
        // cockpit (81/52/20): as green over blue less red over green the
        // lettering is 63 to 64 and the ground 0 to 5, where red over blue
        // gives 129 against 91. White is 0 in it, and so is the MOLE's orange
        // (241/198/153): 45 less 43.
        cv::Mat f;
        cv::addWeighted(planes[1], 2.0, planes[2], -1.0, 0.0, f, CV_32F);
        cv::Mat b;
        planes[0].convertTo(b, CV_32F);
        f -= b;
        return cv::max(f, 0.0);
    }
    else out = planes[which == 2 ? 1 : 2];
    cv::Mat f;
    out.convertTo(f, CV_32F);
    return f;
}

// The ink over a region of the screen only - the band the HUD stands in and
// wherever a column was last seen - into a buffer the size of the screen
// that is kept from look to look. What is outside the region is stale and
// never read: the searches run over the band, the figures are read beside
// the labels. Over a whole 5,120 x 1,440 screen the darkest channel costs
// 14 to 22 ms a look and whiteness 50 to 68.
cv::Mat channelGreyIn(const cv::Mat& bgr, int which, const cv::Rect& region) {
    static cv::Mat buffers[8];
    cv::Mat& buffer = buffers[which & 7];
    if (buffer.size() != bgr.size() || buffer.type() != CV_32F) buffer = cv::Mat::zeros(bgr.size(), CV_32F);
    const cv::Rect r = region & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (r.area() > 0) channelGrey(bgr(r), which).copyTo(buffer(r));
    return buffer;
}

struct LabelColumn {
    cv::Point labels;        // the labels' left edge and the MASS label's top
    double scale = 0;
    double score = -1;
    char glyph = 'L';        // which label column's shape it was
    bool tracked = false;    // found where, at the size and in the shape, one was a moment ago
};

// Where a label column can stand. Over 67,000 looks - the Prospector's HUD
// and the MOLE's pilot seat and turret, at 1,080 to 2,160 lines - the
// column's left edge stands between 0.23 and 0.64 screen heights right of
// the centre, and its top between 0.23 above and 0.10 below it. The HUD is
// drawn about the centre and sized by the screen's height, so the band is
// too, with room to spare. Searched over a whole 5,120 x 1,440 screen
// instead, a look with no panel on it costs half a second, which the
// signatures wait for.
cv::Rect hudBand(const cv::Size& frame) {
    const double h = frame.height;
    const double cx = frame.width / 2.0, cy = frame.height / 2.0;
    const cv::Rect band((int)std::lround(cx - 0.05 * h), (int)std::lround(cy - 0.45 * h),
                        (int)std::lround(1.05 * h), (int)std::lround(0.85 * h));
    return band & cv::Rect(0, 0, frame.width, frame.height);
}

// ...but a HUD this has not seen may stand elsewhere, so one pass in twelve
// searches the whole screen. Decided as the pass begins, because that pass
// needs its ink over the whole screen where the others need it over the band.
// Over 5,120 x 1,440 the search over all sizes costs 1,070 ms; one pass in
// five would add 134 ms to every look with nothing on screen.
bool takeEverywhereTurn() {
    static int passes = 0;
    return ++passes % 12 == 0;
}

LabelColumn findLabelColumnOf(const cv::Mat& darkest, const std::vector<double>& scales, char glyph,
                              double* atGiven);

// Every shape of label column the ink can carry, the best one kept
LabelColumn findLabelColumn(const cv::Mat& darkest, const std::vector<double>& scales, double* atGiven = nullptr,
                            const std::string& glyphs = "L") {
    LabelColumn best;
    double bestAt = -1;
    for (char glyph : glyphs) {
        double at = -1;
        const LabelColumn one = findLabelColumnOf(darkest, scales, glyph, &at);
        if (one.score > best.score) {
            best = one;
            bestAt = at;
        }
    }
    if (atGiven) *atGiven = bestAt;
    return best;
}

LabelColumn findLabelColumnShape(const cv::Mat& darkest, const std::vector<double>& scales, const PanelShape& column,
                                 double* atGiven);

LabelColumn findLabelColumnOf(const cv::Mat& darkest, const std::vector<double>& scales, char glyph,
                              double* atGiven) {
    LabelColumn found = findLabelColumnShape(darkest, scales, shapeFor(glyph), atGiven);
    found.glyph = glyph;
    return found;
}

LabelColumn findLabelColumnShape(const cv::Mat& darkest, const std::vector<double>& scales, const PanelShape& column,
                                 double* atGiven) {
    if (atGiven) *atGiven = -1;
    LabelColumn best;
    best.glyph = column.glyph;
    const cv::Rect frame(0, 0, darkest.cols, darkest.rows);
    std::map<int, cv::Mat> shrunk;
    int bestKey = 100;
    for (double scale : scales) {
        // looked for at a size where the column is some twenty pixels wide
        int key = 100;
        // A column fourteen pixels wide still comes out where it is, and the
        // fine search at full size has six pixels of slack about the place.
        // At twenty, the Prospector's column below 0.83 of its size is
        // searched at full size - 141 ms a size on the 5,120-wide band, four
        // sizes in every wide search
        while (key > 25 && column.width * scale * (key / 2) / 100.0 >= 14.0) key /= 2;
        cv::Mat& small = shrunk[key];
        if (small.empty()) {
            StageClock clock("    column: resize");
            if (key == 100) small = darkest;
            else cv::resize(darkest, small, cv::Size(), key / 100.0, key / 100.0, cv::INTER_AREA);
        }
        const cv::Mat templ = shapeSized(column, scale * key / 100.0);
        if (small.cols <= templ.cols || small.rows <= templ.rows) continue;
        cv::Mat result;
        StageClock clock(key == 100 ? "    column: coarse match (full)" : "    column: coarse match (small)");
        cv::matchTemplate(small, templ, result, cv::TM_CCOEFF_NORMED);
        double score = 0;
        cv::Point at;
        cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
        if (score > best.score) {
            best.score = score;
            best.scale = scale;
            best.labels = cv::Point((int)std::lround(at.x * 100.0 / key), (int)std::lround(at.y * 100.0 / key));
            bestKey = key;
        }
    }
    if (best.score < 0) return best;

    // At full size, a few pixels either way and a few per cent either way -
    // further when only one size was asked for: the panel grows into place
    // when it comes up, and a size learned from those frames is some way off
    const int slack = (int)std::ceil(200.0 / bestKey) + 2;
    StageClock fineClock("    column: fine");
    LabelColumn fine;
    // ...and the further the search was brought down, the wider the sizes
    // about the one it picked: at a quarter of the size the Golem's column
    // matches a neighbouring size as well as its own, and the figures are cut
    // a per cent off
    const std::vector<double> steps = scales.size() == 1 || bestKey <= 25
        ? std::vector<double>{0.94, 0.96, 0.98, 1.0, 1.02, 1.04, 1.06}
        : std::vector<double>{0.97, 0.985, 1.0, 1.015, 1.03};
    for (double f : steps) {
        const double scale = best.scale * f;
        const cv::Mat templ = shapeSized(column, scale);
        const cv::Rect window = cv::Rect(best.labels.x - slack, best.labels.y - slack, templ.cols + 2 * slack,
                                         templ.rows + 2 * slack) & frame;
        if (window.width <= templ.cols || window.height <= templ.rows) continue;
        cv::Mat result;
        cv::matchTemplate(darkest(window), templ, result, cv::TM_CCOEFF_NORMED);
        double score = 0;
        cv::Point at;
        cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
        if (atGiven && std::abs(f - 1.0) < 1e-9) *atGiven = score;
        if (score > fine.score) {
            fine.score = score;
            fine.scale = scale;
            fine.labels = window.tl() + at;
        }
    }
    if (fine.score < 0) return best;
    const int inset = (int)std::lround(SHAPE_LABEL_INSET * fine.scale);
    fine.labels += cv::Point(inset, inset);
    fine.glyph = column.glyph;
    return fine;
}

// Where a template matches best inside a window, to a fraction of a pixel:
// the peak and a parabola through it and its neighbours, each way
bool peakIn(const cv::Mat& grey, const cv::Rect& window, const cv::Mat& templ, cv::Point2d& at, double& score) {
    const cv::Rect safe = window & cv::Rect(0, 0, grey.cols, grey.rows);
    if (safe.width <= templ.cols || safe.height <= templ.rows) return false;
    cv::Mat result;
    cv::matchTemplate(grey(safe), templ, result, cv::TM_CCOEFF_NORMED);
    cv::Point best;
    cv::minMaxLoc(result, nullptr, &score, nullptr, &best);
    auto offset = [](double before, double peak, double after) {
        const double curve = before - 2 * peak + after;
        return std::abs(curve) < 1e-9 ? 0.0 : (std::max)(-0.5, (std::min)(0.5, (before - after) / (2 * curve)));
    };
    double dx = 0, dy = 0;
    if (best.x > 0 && best.x + 1 < result.cols) {
        dx = offset(result.at<float>(best.y, best.x - 1), score, result.at<float>(best.y, best.x + 1));
    }
    if (best.y > 0 && best.y + 1 < result.rows) {
        dy = offset(result.at<float>(best.y - 1, best.x), score, result.at<float>(best.y + 1, best.x));
    }
    at = cv::Point2d(safe.x + best.x + dx, safe.y + best.y + dy);
    return true;
}

// The panel's size, pinned by two fixed words further down. The label column
// is only seventy pixels tall, and the size it matches best at wanders by two
// per cent from one look to the next at 1440 lines and by five at 1080 - four
// pixels off by the time the figures are reached. COMP. and SCU stand a fixed
// way from the labels, up to a hundred and eighty pixels: found where the
// labels' size puts them, the distances between the centres of the three
// give the size to a fraction of a per cent. False when either word is not
// found, or the three disagree, and the labels' own size stands.
bool pinPanelSize(const cv::Mat& darkest, LabelColumn& column, const PanelLayout& layout,
                  const cv::Mat* otherInk = nullptr) {
    const double s0 = column.scale;
    const PanelShape& labels = shapeFor(layout.labels);
    const int inset = (int)std::lround(SHAPE_LABEL_INSET * s0);
    // centres at size 1, from the labels' edge and the MASS label's top
    const double LABELS_CX = layout.labelsCx, LABELS_CY = layout.labelsCy;
    const double COMP_CX = layout.compCx, COMP_CY = layout.compCy;
    const double SCU_CX = layout.scuCx, SCU_CY = layout.scuCy;
    const cv::Point2d centre(column.labels.x - inset + labels.width * s0 / 2.0,
                             column.labels.y - inset + labels.height * s0 / 2.0);
    cv::Point2d found[2];
    const char words[2] = {'C', 'U'};
    const double wordCx[2] = {COMP_CX, SCU_CX}, wordCy[2] = {COMP_CY, SCU_CY};
    for (int w = layout.comp ? 0 : 1; w < 2; w++) {
        const cv::Mat templ = shapeSized(shapeFor(words[w]), s0);
        const double expectX = centre.x + (wordCx[w] - LABELS_CX) * s0;
        const double expectY = centre.y + (wordCy[w] - LABELS_CY) * s0;
        const int slack = (std::max)(4, (int)std::lround(0.07 * 190 * s0));
        const cv::Rect window((int)std::lround(expectX - templ.cols / 2.0) - slack,
                              (int)std::lround(expectY - templ.rows / 2.0) - slack,
                              templ.cols + 2 * slack, templ.rows + 2 * slack);
        cv::Point2d at;
        double score = -1;
        // "SCU" follows the figure in the figure's ink, which on the MOLE's
        // turret is white beside orange labels
        if (!peakIn(darkest, window, templ, at, score) || score < 0.60) {
            if (!otherInk || !peakIn(*otherInk, window, templ, at, score) || score < 0.60) return false;
        }
        found[w] = at + cv::Point2d(templ.cols / 2.0, templ.rows / 2.0);
    }
    const double sx = (found[1].x - centre.x) / (SCU_CX - LABELS_CX);
    const double syScu = (found[1].y - centre.y) / (SCU_CY - LABELS_CY);
    // without COMP. (the long panel) the size stands on SCU alone
    const double syComp = layout.comp ? (found[0].y - centre.y) / (COMP_CY - LABELS_CY) : syScu;
    const double lo = (std::min)({sx, syScu, syComp}), hi = (std::max)({sx, syScu, syComp});
    if (lo <= 0 || hi - lo > 0.03 * hi) return false;
    const double wx = SCU_CX - LABELS_CX, wyU = SCU_CY - LABELS_CY;
    const double wyC = layout.comp ? COMP_CY - LABELS_CY : 0.0;
    const double scale = (sx * wx + syScu * wyU + syComp * wyC) / (wx + wyU + wyC);
    if (std::abs(scale - s0) > 0.08 * s0) return false;
    column.scale = scale;
    column.labels = cv::Point((int)std::lround(centre.x - LABELS_CX * scale),
                              (int)std::lround(centre.y - LABELS_CY * scale));
    return true;
}

// A label column of another shape, looked for right where one was found: the
// short column matches the written-out labels too (0.74 on the test picture),
// and which panel it is shows in which of the two matches better.
LabelColumn labelColumnNear(const cv::Mat& darkest, const LabelColumn& near, char glyph) {
    LabelColumn best;
    const PanelShape& shape = shapeFor(glyph);
    const int inset = (int)std::lround(SHAPE_LABEL_INSET * near.scale);
    const cv::Point from = near.labels - cv::Point(inset, inset);
    const int slack = (std::max)(4, (int)std::lround(8 * near.scale));
    for (double f : {0.94, 0.96, 0.98, 1.0, 1.02, 1.04, 1.06}) {
        const double scale = near.scale * f;
        const cv::Mat templ = shapeSized(shape, scale);
        const cv::Rect window = cv::Rect(from.x - slack, from.y - slack, templ.cols + 2 * slack,
                                         templ.rows + 2 * slack) & cv::Rect(0, 0, darkest.cols, darkest.rows);
        if (window.width <= templ.cols || window.height <= templ.rows) continue;
        cv::Mat result;
        cv::matchTemplate(darkest(window), templ, result, cv::TM_CCOEFF_NORMED);
        double score = 0;
        cv::Point at;
        cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
        if (score > best.score) {
            const int own = (int)std::lround(SHAPE_LABEL_INSET * scale);
            best.score = score;
            best.scale = scale;
            best.labels = window.tl() + at + cv::Point(own, own);
        }
    }
    return best;
}

struct ShapeRow {
    std::string text;
    float worst = -2;        // the least-matched glyph
    float margin = -2;       // the least lead a glyph had over what else it could be
    float mean = 0;
    cv::Rect box;
    bool read = false;
    std::string trace;       // glyph:score/lead for each, for the tools
    std::string ended;       // why the figure ended where it did
    bool placed = false;     // glyphs all found and in the figure's format, sure or not
    float ground = 0;        // the line's ground...
    float lift = 0;          // ...and how far above it its glyphs typically stand
    std::vector<cv::Point2d> at; // each glyph's shape's top left, full stops left out
    float lead = 2;              // the least lead of a numeral over the next likeliest
    // for each numeral, in order, how near it came to each of 0 to 9
    std::vector<std::array<float, 10>> likeness;
};

// Which numeral the cut at `at` is: the nearest of the clustered shapes names
// it, and `likeness` gets the nearest of each numeral's shapes
// A glyph's cell cut out at the shapes' own size, from its top left to a
// fraction of a pixel. Cut at whole pixels, the same numeral came out half a
// pixel one way or the other from look to look, and at eleven pixels - eight
// at 1080 lines - half a pixel is most of what tells a 0 from an 8.
cv::Mat glyphCut(const cv::Mat& grey, const cv::Point2d& at, double scale, int w, int h) {
    const double right = at.x + w * scale, bottom = at.y + h * scale;
    if (at.x < 0 || at.y < 0 || right > grey.cols || bottom > grey.rows) return cv::Mat();
    const cv::Matx23d map(scale, 0, at.x + 0.5 * scale - 0.5, 0, scale, at.y + 0.5 * scale - 0.5);
    cv::Mat cut;
    cv::warpAffine(grey, cut, map, cv::Size(w, h), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_REPLICATE);
    return cut;
}

int nearestDigit(const cv::Mat& grey, const cv::Point2d& at, double scale, std::array<float, 10>& likeness,
                 float& lead) {
    constexpr int W = 9, H = 15;
    static_assert(sizeof(PANEL_DIGITS[0].grey) == sizeof(float) * W * H, "digit shapes are 9 x 15");
    likeness.fill(-2.0f);
    lead = 0;
    cv::Mat patch = glyphCut(grey, at, scale, W, H);
    if (patch.empty()) return -1;
    patch = patch.reshape(1, 1).clone();
    patch -= cv::mean(patch)[0];
    const double length = cv::norm(patch);
    if (length < 1e-6) return -1;
    patch /= length;
    const float* p = patch.ptr<float>(0);
    // For the bench only: SCMINER_DIGITS names a file of other shapes - ones
    // clustered from one recording, to be tried on the other
    static const std::vector<PanelDigitShape> fromFile = [] {
        std::vector<PanelDigitShape> shapes;
        const char* path = DEV_ENV("SCMINER_DIGITS");
        if (!path) return shapes;
        std::ifstream in(path, std::ios::binary);
        int count = 0;
        in.read((char*)&count, sizeof(count));
        for (int i = 0; i < count && in; i++) {
            PanelDigitShape shape{};
            in.read((char*)&shape.digit, sizeof(shape.digit));
            in.read((char*)shape.grey, sizeof(shape.grey));
            shapes.push_back(shape);
        }
        return shapes;
    }();
    // The shapes cut at the size nearest the panel's: full size, or brought
    // down to 1080 lines
    const bool reduced = scale < 0.875;
    const int shapeCount = !fromFile.empty() ? (int)fromFile.size() : reduced ? PANEL_DIGIT_SMALL_COUNT : PANEL_DIGIT_COUNT;
    const PanelDigitShape* shapes = !fromFile.empty() ? fromFile.data() : reduced ? PANEL_DIGITS_SMALL : PANEL_DIGITS;
    for (int i = 0; i < shapeCount; i++) {
        const float* e = shapes[i].grey;
        float dot = 0.0f;
        for (int d = 0; d < W * H; d++) dot += p[d] * e[d];
        float& best = likeness[shapes[i].digit];
        if (dot > best) best = dot;
    }
    int top = 0;
    for (int d = 1; d < 10; d++) {
        if (likeness[d] > likeness[top]) top = d;
    }
    float second = -2.0f;
    for (int d = 0; d < 10; d++) {
        if (d != top && likeness[d] > second) second = likeness[d];
    }
    lead = likeness[top] - second;
    return top;
}

// Which numeral the glyph placed at `at` is, by the font's own averaged
// numerals, for a font without clustered shapes of its own. The turret's 0, 6,
// 8 and 9 share one oval and differ only inside it - the 0's slash, the 6's
// and 9's open corners, the 8's waist - and matched over the whole cell the
// oval swamps that: 103.69 reads as 103.09. So each pixel counts by how much
// the ten numerals differ there, and the cell is cut at the numerals' own
// size to a fraction of a pixel, as the clustered shapes are.
int nearestMean(const cv::Mat& grey, const cv::Point2d& at, double scale, const PanelFont& font,
                std::array<float, 10>& likeness, float& lead) {
    struct Book {
        int w = 0, h = 0;
        std::vector<cv::Mat> numerals;   // weighted-centred, one row each
        std::vector<double> norms;
        cv::Mat weight;
        double weightSum = 0;
    };
    static std::map<const PanelFont*, Book> books;
    Book& book = books[&font];
    if (book.numerals.empty()) {
        const PanelShape& first = shapeFor(font.numerals[0]);
        book.w = first.width;
        book.h = first.height;
        cv::Mat stack(10, book.w * book.h, CV_32F);
        for (int d = 0; d < 10; d++) {
            const PanelShape& shape = shapeFor(font.numerals[d]);
            cv::Mat t = cv::Mat(shape.height, shape.width, CV_32F, const_cast<float*>(shape.grey)).clone().reshape(1, 1);
            t -= cv::mean(t)[0];
            t /= (std::max)(1e-6, cv::norm(t));
            t.copyTo(stack.row(d));
        }
        cv::Mat mean, spread;
        cv::reduce(stack, mean, 0, cv::REDUCE_AVG);
        cv::Mat apart = stack - cv::repeat(mean, 10, 1);
        cv::reduce(apart.mul(apart), spread, 0, cv::REDUCE_AVG);
        cv::sqrt(spread, book.weight);
        book.weight /= (std::max)(1e-9, cv::mean(book.weight)[0]);
        // no pixel counts for nothing: a speck of rock there must still cost
        book.weight = cv::max(book.weight, 0.25);
        book.weightSum = cv::sum(book.weight)[0];
        for (int d = 0; d < 10; d++) {
            cv::Mat t = stack.row(d).clone();
            t -= cv::sum(t.mul(book.weight))[0] / book.weightSum;
            book.norms.push_back(std::sqrt(cv::sum(t.mul(t).mul(book.weight))[0]));
            book.numerals.push_back(t);
        }
    }
    likeness.fill(-2.0f);
    lead = 0;
    cv::Mat patch = glyphCut(grey, at, scale, book.w, book.h);
    if (patch.empty()) return -1;
    patch = patch.reshape(1, 1).clone();
    patch -= cv::sum(patch.mul(book.weight))[0] / book.weightSum;
    const double length = std::sqrt(cv::sum(patch.mul(patch).mul(book.weight))[0]);
    if (length < 1e-6) return -1;
    const cv::Mat weighted = patch.mul(book.weight);
    for (int d = 0; d < 10; d++) {
        if (book.norms[d] < 1e-9) continue;
        likeness[d] = (float)(weighted.dot(book.numerals[d]) / (length * book.norms[d]));
    }
    int top = 0;
    for (int d = 1; d < 10; d++) {
        if (likeness[d] > likeness[top]) top = d;
    }
    float second = -2.0f;
    for (int d = 0; d < 10; d++) {
        if (d != top && likeness[d] > second) second = likeness[d];
    }
    lead = likeness[top] - second;
    return top;
}

// How much brighter than the line's own ground the brightest tenth of a
// patch is
float contrastIn(const cv::Mat& grey, const cv::Rect& where, float ground) {
    const cv::Rect safe = where & cv::Rect(0, 0, grey.cols, grey.rows);
    if (safe.area() <= 0) return 0.0f;
    std::vector<float> v;
    v.reserve(safe.area());
    for (int y = safe.y; y < safe.y + safe.height; y++) {
        const float* p = grey.ptr<float>(y);
        for (int x = safe.x; x < safe.x + safe.width; x++) v.push_back(p[x]);
    }
    const size_t k = v.size() * 9 / 10;
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k] - ground;
}

float medianIn(const cv::Mat& grey, const cv::Rect& where) {
    const cv::Rect safe = where & cv::Rect(0, 0, grey.cols, grey.rows);
    if (safe.area() <= 0) return 0.0f;
    std::vector<float> v;
    v.reserve(safe.area());
    for (int y = safe.y; y < safe.y + safe.height; y++) {
        const float* p = grey.ptr<float>(y);
        for (int x = safe.x; x < safe.x + safe.width; x++) v.push_back(p[x]);
    }
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// One figure, from its left edge and its bottom. format: 0 whole, 1 per
// cent, 2 two places
ShapeRow readShapeRow(const cv::Mat& grey, const cv::Point& leftBottom, double scale, int format,
                      const PanelFont& font = PROSPECTOR_FONT) {
    StageClock clock("  readShapeRow");
    ShapeRow row;
    static const std::string glyphs = "0123456789%";
    std::vector<cv::Mat> shapes;
    for (int k = 0; k < 11; k++) shapes.push_back(shapeSized(shapeFor(font.numerals[k]), scale));
    const int th = shapes[0].rows;
    const int digitH = (int)std::lround(font.digitH * scale);
    const int expectTop = leftBottom.y - digitH - (int)std::lround(font.above * scale);
    const int slackY = (std::max)(2, (int)std::lround(2 * scale));
    const cv::Rect band = cv::Rect(leftBottom.x - (int)std::lround(10 * scale), expectTop - slackY,
                                   (int)std::lround((font.advance * 9 + 34) * scale), th + 2 * slackY) &
                          cv::Rect(0, 0, grey.cols, grey.rows);
    if (band.height < th + 1 || band.width < shapes.back().cols * 3) return row;

    // for each glyph, its best match over the rows at each left edge
    std::vector<std::vector<float>> columns(shapes.size());
    std::vector<std::vector<int>> tops(shapes.size());
    std::vector<cv::Mat> results(shapes.size());
    for (size_t k = 0; k < shapes.size(); k++) {
        cv::Mat& result = results[k];
        cv::matchTemplate(grey(band), shapes[k], result, cv::TM_CCOEFF_NORMED);
        columns[k].assign(result.cols, -2.0f);
        tops[k].assign(result.cols, 0);
        for (int y = 0; y < result.rows; y++) {
            const float* r = result.ptr<float>(y);
            for (int x = 0; x < result.cols; x++) {
                if (r[x] > columns[k][x]) {
                    columns[k][x] = r[x];
                    tops[k][x] = y;
                }
            }
        }
    }
    const int slackX = (std::max)(2, (int)std::lround(2 * scale));
    // glyph k centred near cx (band coordinates): its best score and centre
    auto match = [&](size_t k, double cx, int slack, double& centre, cv::Point* where = nullptr) {
        const int w = shapes[k].cols;
        const int left = (int)std::lround(cx - w / 2.0);
        float best = -2.0f;
        for (int dx = -slack; dx <= slack; dx++) {
            const int x = left + dx;
            if (x < 0 || x >= (int)columns[k].size()) continue;
            if (columns[k][x] > best) {
                best = columns[k][x];
                centre = x + w / 2.0;
                if (where) *where = cv::Point(band.x + x, band.y + tops[k][x]);
            }
        }
        return best;
    };
    struct Option {
        char glyph = 0;
        float score = -2.0f, runnerUp = -2.0f;
        double centre = 0;
        bool afterStop = false;
        cv::Point at;
        size_t shape = 0;         // which shape placed it
    };
    // the peak of a shape's map at a place, to a fraction of a pixel
    auto finer = [&](const Option& o) {
        const cv::Mat& map = results[o.shape];
        const int x = o.at.x - band.x, y = o.at.y - band.y;
        auto offset = [](double before, double peak, double after) {
            const double curve = before - 2 * peak + after;
            return std::abs(curve) < 1e-9 ? 0.0 : (std::max)(-0.5, (std::min)(0.5, (before - after) / (2 * curve)));
        };
        double dx = 0, dy = 0;
        if (x > 0 && x + 1 < map.cols && y >= 0 && y < map.rows) {
            dx = offset(map.at<float>(y, x - 1), map.at<float>(y, x), map.at<float>(y, x + 1));
        }
        if (y > 0 && y + 1 < map.rows && x >= 0 && x < map.cols) {
            dy = offset(map.at<float>(y - 1, x), map.at<float>(y, x), map.at<float>(y + 1, x));
        }
        return cv::Point2d(o.at.x + dx, o.at.y + dy);
    };
    auto numeral = [&](double cx, int slack) {
        Option o;
        for (size_t k = 0; k < 10; k++) {
            double c = cx;
            cv::Point where;
            const float sc = match(k, cx, slack, c, &where);
            if (sc > o.score) {
                o.runnerUp = o.score;
                o.score = sc;
                o.glyph = glyphs[k];
                o.centre = c;
                o.at = where;
                o.shape = k;
            } else if (sc > o.runnerUp) {
                o.runnerUp = sc;
            }
        }
        return o;
    };

    const double first = leftBottom.x + font.firstCentre * scale - band.x;
    double last = first;
    bool stop = false, ended = false;
    int decimals = 0, count = 0;
    float worst = 2.0f, margin = 2.0f, total = 0.0f;
    // The line's ground, and how far above it the glyphs so far stood. Rock
    // behind the end of a figure still matches a numeral now and then - 7148
    // came back as 714864 - but it is never as bright as the figure.
    const float ground = medianIn(grey, band);
    std::vector<float> lifts;
    std::string trace;
    for (int n = 0; n < 9 && !ended; n++) {
        std::vector<Option> options;
        if (n == 0) {
            options.push_back(numeral(first, slackX + 1));
        } else {
            options.push_back(numeral(last + font.advance * scale, slackX));
            if (format == 2 && !stop) {
                Option o = numeral(last + font.stopAdvance * scale, slackX);
                o.afterStop = true;
                options.push_back(o);
            }
            if (format == 1) {
                Option o;
                double c = last + font.pctAdvance * scale;
                o.score = match(10, last + font.pctAdvance * scale, slackX, c, &o.at);
                o.shape = 10;
                o.glyph = '%';
                o.centre = c;
                options.push_back(o);
            }
        }
        size_t pick = 0;
        for (size_t i = 1; i < options.size(); i++) {
            if (options[i].score > options[pick].score) pick = i;
        }
        Option o = options[pick];
        const int wCell = o.glyph == '%' ? shapes[10].cols : shapes[0].cols;
        const cv::Rect cell(band.x + (int)std::lround(o.centre - wCell / 2.0),
                            expectTop + (int)std::lround(font.above * scale), wCell, digitH);
        const float lift = contrastIn(grey, cell, ground);
        if (!lifts.empty()) {
            std::vector<float> sorted = lifts;
            std::sort(sorted.begin(), sorted.end());
            const float typical = sorted[sorted.size() / 2];
            if (lift < typical * 0.55f) {
                row.ended = "faint";
                break;
            }
        }
        // Bright, but nothing a numeral matches: something is there that
        // cannot be read, and stopping short of it would make 5359 a 53
        if (o.score < SHAPE_GLYPH_FLOOR || (count > 0 && o.score < total / count - 0.20f)) {
            row.ended = lifts.empty() ? "floor" : "unread";
            if (!lifts.empty()) row.text += '?';
            break;
        }
        lifts.push_back(lift);
        // The shapes placed the glyph; the clustered numerals say which it is
        float digitLead = 2.0f;
        std::array<float, 10> likeness{};
        const cv::Point2d fine = finer(o);
        if (o.glyph != '%') {
            const int d = font.clustered ? nearestDigit(grey, fine, scale, likeness, digitLead)
                                         : nearestMean(grey, fine, scale, font, likeness, digitLead);
            if (d >= 0) o.glyph = (char)('0' + d);
            row.likeness.push_back(likeness);
            row.lead = (std::min)(row.lead, digitLead);
        }
        float rival = o.runnerUp;
        for (size_t i = 0; i < options.size(); i++) {
            if (i != pick) rival = (std::max)(rival, options[i].score);
        }
        char part[40];
        std::snprintf(part, sizeof(part), "%s%s%c%.2f/%.3f", trace.empty() ? "" : " ", o.afterStop ? "." : "",
                      o.glyph, o.score, o.glyph == '%' ? o.score - rival : digitLead);
        trace += part;
        if (o.afterStop) {
            row.text += '.';
            stop = true;
        }
        row.text += o.glyph;
        row.at.push_back(fine);
        worst = (std::min)(worst, o.score);
        if (o.glyph == '%' || o.afterStop) margin = (std::min)(margin, o.score - rival);
        total += o.score;
        count++;
        const int w = o.glyph == '%' ? shapes[10].cols : shapes[0].cols;
        const cv::Rect glyphBox(band.x + (int)std::lround(o.centre - w / 2.0), expectTop, w, th);
        row.box = row.box.width == 0 ? glyphBox : (row.box | glyphBox);
        last = o.centre;
        if (stop && o.glyph != '%') decimals++;
        if (o.glyph == '%') ended = true;
        if (format == 2 && decimals == 2) ended = true;
    }
    row.trace = trace;
    row.ground = ground;
    if (!lifts.empty()) {
        std::vector<float> sorted = lifts;
        std::sort(sorted.begin(), sorted.end());
        row.lift = sorted[sorted.size() / 2];
    }
    if (count == 0) return row;
    row.worst = worst;
    row.margin = margin;
    row.mean = total / count;
    static const float least = DEV_ENV("SCMINER_SHAPE_MIN") ? (float)std::atof(DEV_ENV("SCMINER_SHAPE_MIN")) : 0.70f;
    static const float lead = DEV_ENV("SCMINER_SHAPE_LEAD") ? (float)std::atof(DEV_ENV("SCMINER_SHAPE_LEAD")) : 0.005f;
    if (worst < least) return row;
    if (format == 0) row.placed = faceWhole(row.text) >= 0;
    if (format == 1) row.placed = facePercent(row.text) >= 0;
    if (format == 2) row.placed = faceTwoPlaces(row.text, true) >= 0;
    row.read = row.placed && row.lead >= lead;
    return row;
}

// Whether a figure's strokes are green or red rather than white. The HUD adds
// its light to what is behind it, and white added to anything is white: all
// three channels run up to the top. Green added to the blue of a bright sky
// is green and blue at the top and red well short of it - 155/250/240 on the
// savrilium rock, which is why asking for green over blue did not work. So:
// the brightest quarter of the box by all three channels together, which is
// the strokes whatever the ground, and how far the weakest channel of those
// falls short of the strongest.
float strokeWhiteness(const cv::Mat& bgr, const cv::Rect& where) {
    const cv::Rect safe = where & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (safe.width < 4 || safe.height < 4) return 1.0f;
    std::vector<std::pair<int, float>> px;          // brightness, weakest over strongest
    for (int y = safe.y; y < safe.y + safe.height; y++) {
        const cv::Vec3b* row = bgr.ptr<cv::Vec3b>(y);
        for (int x = safe.x; x < safe.x + safe.width; x++) {
            const int b = row[x][0], g = row[x][1], r = row[x][2];
            const int top = (std::max)({b, g, r}), least = (std::min)({b, g, r});
            px.push_back({b + g + r, top > 0 ? (float)least / top : 1.0f});
        }
    }
    std::sort(px.begin(), px.end(), [](const std::pair<int, float>& a, const std::pair<int, float>& c) {
        return a.first > c.first;
    });
    const size_t n = (std::max)((size_t)4, px.size() / 4);
    std::vector<float> ratios;
    for (size_t i = 0; i < n && i < px.size(); i++) ratios.push_back(px[i].second);
    std::sort(ratios.begin(), ratios.end());
    return ratios[ratios.size() / 2];
}

// A resistance may be green or red: read in every channel that could carry
// it, and the best-matched reading kept
ShapeRow readResistanceRow(const cv::Mat& bgr, const cv::Mat& darkest, const cv::Point& leftBottom, double scale,
                           cv::Mat* greyUsed, const PanelFont& font = PROSPECTOR_FONT,
                           const std::function<const cv::Mat&(int)>* inks = nullptr) {
    ShapeRow res;
    for (int which = 0; which < 4; which++) {
        // the reader's own inks about the panel when it has them: turned into
        // grey over the whole screen, three of them cost 60 ms a look
        const cv::Mat grey = which == 0 ? darkest : inks ? (*inks)(which) : channelGrey(bgr, which);
        ShapeRow r = readShapeRow(grey, leftBottom, scale, 1, font);
        const bool better = r.placed ? (!res.placed || r.mean > res.mean) : (!res.placed && res.text.empty());
        if (better) {
            res = r;
            if (greyUsed) *greyUsed = grey;
        }
    }
    return res;
}

const std::vector<double>& labelScalesFor(int rows) {
    static std::map<int, std::vector<double>> byRows;
    std::vector<double>& scales = byRows[rows];
    if (scales.empty()) {
        // up to 1.64: from the MOLE's turret the panel stands 1.44 times the
        // size it has in a Prospector on the same 5120 x 1440 screen
        for (double f : {0.70, 0.78, 0.86, 0.94, 1.0, 1.07, 1.16, 1.26, 1.38, 1.50, 1.64}) {
            scales.push_back(rows / 1440.0 * f);
        }
    }
    return scales;
}

}  // namespace

ShapeFound findShapeIn(const cv::Mat& frame, const cv::Rect& region, int ink, const PanelShape& shape,
                       const std::vector<double>& scales) {
    ShapeFound found;
    const cv::Mat bgr = asBgr(frame);
    const cv::Rect r = region & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (r.width < 8 || r.height < 8 || scales.empty()) return found;
    const cv::Mat grey = channelGrey(bgr(r), ink);
    const LabelColumn c = findLabelColumnShape(grey, scales, shape, nullptr);
    if (c.score < 0) return found;
    found.at = c.labels + r.tl();
    found.scale = c.scale;
    found.score = c.score;
    return found;
}

bool findPanelLabels(const cv::Mat& frame, cv::Point& labels, double& scale, double& score) {
    const cv::Mat darkest = channelGrey(asBgr(frame), 0);
    const LabelColumn column = findLabelColumn(darkest, labelScalesFor(frame.rows));
    labels = column.labels;
    scale = column.scale;
    score = column.score;
    return column.score >= 0.70;
}

// A label column's averaged shape, grown from one cut: the seed is looked for
// in every frame, and where it matches well enough the column is cut at the
// size it was found at and added in. Two passes, the second with the first's
// average. Returns the average at size 1, 12 to 242 like the shapes in
// PanelShapes.h, and lists what was found where.
cv::Mat bootstrapLabelShape(const cv::Mat& seed, const std::vector<std::string>& files, int ink, double minScore,
                            std::vector<std::tuple<std::string, double, double, int, int>>& found, int passes) {
    cv::Mat templ = seed.clone();
    for (int pass = 0; pass < passes; pass++) {
        found.clear();
        PanelShape shape{'?', templ.cols, templ.rows, templ.ptr<float>(0)};
        cv::Mat sum = cv::Mat::zeros(templ.size(), CV_32F);
        int count = 0;
        std::vector<double> scales;
        for (double k = 0.85; k <= 1.95; k *= 1.05) scales.push_back(k);
        for (const std::string& file : files) {
            const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
            if (frame.empty()) continue;
            const cv::Mat grey = channelGrey(asBgr(frame), ink);
            // SCMINER_BOOT_LEFT: the band mirrored to the left of the middle,
            // where the mining HUD lists the laser and its modules
            cv::Rect band = hudBand(grey.size());
            if (DEV_ENV("SCMINER_BOOT_LEFT")) {
                band.x = (std::max)(0, grey.cols - band.x - band.width);
                band &= cv::Rect(0, 0, grey.cols, grey.rows);
            }
            LabelColumn c = findLabelColumnShape(grey(band), scales, shape, nullptr);
            c.labels += band.tl();
            if (c.score < minScore) continue;
            const double sc = c.scale;
            const cv::Point2d at(c.labels.x - SHAPE_LABEL_INSET * sc, c.labels.y - SHAPE_LABEL_INSET * sc);
            cv::Mat cut = glyphCut(grey, at, sc, templ.cols, templ.rows);
            if (cut.empty()) continue;
            cv::Scalar mean, dev;
            cv::meanStdDev(cut, mean, dev);
            if (dev[0] < 1e-3) continue;
            sum += (cut - mean[0]) / dev[0];
            count++;
            found.emplace_back(file, c.score, sc, c.labels.x, c.labels.y);
        }
        if (count == 0) return cv::Mat();
        cv::Mat avg = sum / count;
        double lo, hi;
        cv::minMaxLoc(avg, &lo, &hi);
        templ = (avg - lo) * (230.0 / (std::max)(1e-6, hi - lo)) + 12.0;
        templ = templ.clone();
    }
    return templ;
}

void probePanelInks(const cv::Mat& frame, std::ostream& out) {
    const cv::Mat bgr = asBgr(frame);
    const double r = frame.rows / 1440.0;
    const bool all = DEV_ENV("SCMINER_PROBE_ALL") != nullptr;
    std::vector<double> scales;
    for (double k = all ? 0.60 : 0.80; k <= (all ? 1.80 : 1.35); k *= 1.05) scales.push_back(k * r);
    const cv::Scalar mean = cv::mean(bgr);
    out << "  channel means b " << mean[0] << " g " << mean[1] << " r " << mean[2] << std::endl;
    const cv::Mat darkest = channelGrey(bgr, 0);
    const cv::Mat brightest = channelGrey(bgr, 1);
    // white and nothing else: the darkest channel less how coloured the pixel is
    const cv::Mat whiteness = darkest - 2.0 * (brightest - darkest);
    auto topHat = [&](const cv::Mat& grey, int size) {
        cv::Mat hat;
        const int k = (std::max)(3, (int)std::lround(size * r)) | 1;
        cv::morphologyEx(grey, hat, cv::MORPH_TOPHAT, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k, k)));
        return hat;
    };
    const struct {
        const char* name;
        cv::Mat grey;
    } inks[] = {
        {"darkest", darkest},
        {"red over blue", channelGrey(bgr, 4)},
        {"whiteness", whiteness},
        {"darkest top-hat 7", topHat(darkest, 7)},
        {"darkest top-hat 11", topHat(darkest, 11)},
        {"whiteness top-hat 9", topHat(whiteness, 9)},
    };
    // SCMINER_PROBE_GLYPHS=K and SCMINER_PROBE_INK="red over blue" narrow the
    // probe to one column and one ink, which is what a run over a whole set
    // of frames wants
    const char* onlyGlyphs = DEV_ENV("SCMINER_PROBE_GLYPHS");
    const char* onlyInk = DEV_ENV("SCMINER_PROBE_INK");
    for (const auto& ink : inks) {
        if (onlyInk && *onlyInk && std::string(ink.name) != onlyInk) continue;
        for (char glyph : std::string(onlyGlyphs && *onlyGlyphs ? onlyGlyphs : (all ? "LTK" : "L"))) {
            const auto t0 = std::chrono::steady_clock::now();
            const LabelColumn c = findLabelColumn(ink.grey, scales, nullptr, std::string(1, glyph));
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            out << "  " << ink.name << " " << glyph << ": " << std::fixed << std::setprecision(3) << c.score << " at "
                << c.labels << " x" << std::setprecision(3) << c.scale / r << " (" << std::setprecision(0) << ms
                << " ms)" << std::endl;
        }
    }
}

bool shapeSamplesOf(const cv::Mat& frame, std::string texts[3], std::vector<std::vector<cv::Mat>> patches[3],
                    int jitter) {
    const cv::Mat bgr = asBgr(frame);
    const cv::Mat darkest = channelGrey(bgr, 0);
    static double known = 0;
    LabelColumn column;
    if (known > 0) column = findLabelColumn(darkest, {known});
    if (column.score < 0.70) column = findLabelColumn(darkest, labelScalesFor(frame.rows));
    if (column.score < 0.70) return false;
    known = column.scale;
    const double s = column.scale;
    const int left = column.labels.x + (int)std::lround(SHAPE_VALUE_DX * s);
    const cv::Size size((int)std::lround(PANEL_SHAPES[1].width * s), (int)std::lround(PANEL_SHAPES[1].height * s));
    for (int r = 0; r < 3; r++) {
        const cv::Point leftBottom(left, column.labels.y + (int)std::lround((SHAPE_FIRST_BOTTOM + SHAPE_ROW_PITCH * r) * s));
        cv::Mat grey = darkest;
        const ShapeRow row = r == 1 ? readResistanceRow(bgr, darkest, leftBottom, s, &grey)
                                    : readShapeRow(darkest, leftBottom, s, r == 0 ? 0 : 2);
        texts[r] = row.text;
        patches[r].clear();
        for (const cv::Point2d& p : row.at) {
            std::vector<cv::Mat> around;
            for (int dy = -jitter; dy <= jitter; dy++) {
                for (int dx = -jitter; dx <= jitter; dx++) {
                    const cv::Mat patch = glyphCut(grey, p + cv::Point2d(dx * 0.5, dy * 0.5), s,
                                                   PANEL_SHAPES[1].width, PANEL_SHAPES[1].height);
                    if (!patch.empty()) around.push_back(patch);
                }
            }
            patches[r].push_back(around);
        }
    }
    return true;
}

namespace {
bool g_panelSearchThrottled = false;
}

void throttlePanelSearch(bool throttled) {
    g_panelSearchThrottled = throttled;
}

namespace {
bool readPanelByShapeInk(const cv::Mat& frame, const cv::Mat& bgr, int ink, ScanPanel& panel);
}

// The lettering's own ink. The Prospector's HUD writes the panel in white,
// which the darkest of the three channels keeps and every coloured ground
// loses; the MOLE's writes it in orange, which the darkest channel loses
// together with the ground. Measured on the MOLE's labels over grey
// rock, in standard deviations of the ground: 1.1 in the darkest channel,
// 4.3 in the brightest and 7.3 in red over blue - rock is as red as it is
// blue, orange lettering is not. The Prospector's white labels: 27 in the
// darkest channel, nothing in red over blue.
//
// So the ink the last panel was found in is tried first, and when it finds
// no panel the other one is. Only these two: the brightest channel finds a
// white panel too, but reads its figures worse - it reads the 1080p test
// panel's 22,593 as 22,503 - and once it has found one it is the ink tried
// first. Red over blue never finds a white panel, the darkest channel never
// an orange one over rock, so neither can take the other's place.
// the ink tried first - the last panel's, or what preferPanelInk asked for
static int g_homeInk = 0;

bool readPanelByShape(const cv::Mat& frame, ScanPanel& panel) {
    const cv::Mat bgr = asBgr(frame);
    int& ink = g_homeInk;
    const bool sure = readPanelByShapeInk(frame, bgr, ink, panel);
    panel.note += " ink " + std::to_string(ink);
    panel.ink = ink;
    // Whiteness stands in for the darkest channel and is not a home: it reads
    // a clean white panel as well as the darkest channel does, but lettering
    // with coloured fringes - the 1080p test picture's - not at all. So while
    // it is the ink, every fourth look with a panel tries the darkest channel
    // again and goes back to it as soon as it reads.
    if (ink == 5 && (panel.found || panel.labels.x >= 0)) {
        static int homeTry = 0;
        if (++homeTry % 4 == 0) {
            ScanPanel home;
            const bool homeSure = readPanelByShapeInk(frame, bgr, 0, home);
            if (home.found) {
                home.note += " ink 0";
                home.ink = 0;
                ink = 0;
                panel = home;
                return homeSure;
            }
        }
    }
    if (panel.found) return sure;
    // Labels found and their figures not read is still a panel in this ink -
    // the MOLE turret's, whose figures go to the face reader - and the other
    // ink has nothing to add but its cost
    if (panel.labels.x >= 0) return sure;
    // ...and the other ink on every look while nothing is found: over the band
    // it costs the ink image and the column search, 7 ms and 2 to 10 ms on a
    // 5,120 x 1,440 screen (--paneltiming). Tried only every fourth look, the
    // MOLE's pilot-seat panel goes unnoticed for twenty seconds.
    // The inks a panel can be written in, one of the others tried a turn:
    // white in the darkest channel (0), the MOLE's orange in red over blue
    // (4), and white and nothing coloured (5) for LAMP by day, where the
    // Prospector's labels match 0.44 to 0.75 in the darkest channel and
    // 0.76 to 0.89 in this one. One other ink a look, so three cost what two do.
    // From the darkest channel the two others by turns; from either of the
    // others the darkest channel first, then the third: it is the ink of
    // every panel but the MOLE's and LAMP's, and the one to get back to.
    static int turn = 0;
    const bool second_ = (turn++ % 2) == 1;
    const int other = ink == 0 ? (second_ ? 5 : 4) : (second_ ? (ink == 4 ? 5 : 4) : 0);
    ScanPanel second;
    const bool secondSure = readPanelByShapeInk(frame, bgr, other, second);
    second.note += " ink " + std::to_string(other);
    second.ink = other;
    if (second.found || (second.labels.x >= 0 && panel.labels.x < 0)) {
        // a column found in another ink is followed on the next look too, read
        // or not: the turret's figures go to the face reader, and the quick
        // looks after labels without figures look in the ink that found them
        if (second.found || other != 0) ink = other;
        panel = second;
        return secondSure;
    }
    return sure;
}

namespace {
// The orange label columns, looked for in red over blue. Each comes in at a
// size of its own - the turret's at about 1.54, the pilot seat's at about
// 0.92 - so each is looked for only about that size; and where one was found,
// the next look searches about it at its size first, as the white search does.
// Its own memory, apart from the white search's: a turret whose figures the
// shapes cannot read would never teach that one where to look.
LabelColumn g_orangeLast;

// Where the last panel stood, whichever ink it was written in - see
// lastPanelSpot
PanelSpot g_panelSpot;

// How far under its bar a column is still tried where the last panel stood.
// In the pilot seat under Nyx's bright sky the written-out labels can match
// 0.57 to 0.62 for a minute while every numeral under them reads at 0.86 to
// 0.95, and half those looks fall under the twelve hundredths allowed
// anywhere else. What makes it a panel is still what is read under it - see
// readPanelByShapeInk. SCMINER_TRY_AT_SPOT for the bench.
double tryAtSpot() {
    static const double under = DEV_ENV("SCMINER_TRY_AT_SPOT") ? std::atof(DEV_ENV("SCMINER_TRY_AT_SPOT")) : 0.20;
    return under;
}

// The same column as the last panel's: the same shape, in the same ink,
// within sixty pixels of where it stood (at its size - the labels wandered
// thirty between one rock and the next) and within eight per cent of its size
bool atPanelSpot(const LabelColumn& column, int ink) {
    const PanelSpot& spot = g_panelSpot;
    if (!spot.known || column.score < 0 || column.glyph != spot.glyph) return false;
    if ((spot.ink == 6 ? 4 : spot.ink) != ink) return false;
    const double reach = 60 * spot.scale;
    return std::abs(column.labels.x - spot.labels.x) <= reach && std::abs(column.labels.y - spot.labels.y) <= reach &&
           std::abs(column.scale - spot.scale) <= spot.scale * 0.08;
}

// Looks since the face reader last read a panel - see the stand-in rule in
// readScanPanel
int g_sinceFaceRead = 1000;

// where the orange search will look first: about the column last seen
cv::Rect orangeNearRect(const cv::Size& size) {
    if (g_orangeLast.score <= 0) return cv::Rect();
    const double s = g_orangeLast.scale;
    return cv::Rect(g_orangeLast.labels.x - (int)std::lround(160 * s), g_orangeLast.labels.y - (int)std::lround(110 * s),
                    (int)std::lround(380 * s), (int)std::lround(300 * s)) & cv::Rect(0, 0, size.width, size.height);
}

// What an orange column has to match to be taken as one.
//
// The turret's and the Golem's are long - three words one under the other -
// and a real one matches 0.92 to 0.99, so 0.85 is a bar only a panel clears.
// The pilot seat's is the short column, MASS over RES. over INST., and the
// same panel matches it anywhere from 0.79 to 0.95 as the HUD slides over
// the rock behind it: on 400 cockpit frames, 0.85 took 62 of them and left
// the rest to the face reader, so a rock in front of the MOLE's pilot seat
// was read about once a minute. Fewer letters cannot match as hard, so the
// short column gets the lower bar - and nothing is taken on the column
// alone: the figures still have to be found and read where it says they
// are.
double orangeBar(char glyph) {
    // The pilot seat's written-out column, the shortest of them, matches its
    // own panel at 0.80 to 0.85 as the ground behind it changes, where the
    // best thing that is not a panel reaches 0.58 in the sizes this column is
    // looked for at (four pilot-seat snapshots and the turret, Golem and
    // Prospector benches). A bar between the two rather than on top of the
    // good readings; and a column alone still reads nothing - the figures
    // have to be found where it says they are.
    if (glyph == 'P') return 0.72;
    return glyph == 'K' ? 0.80 : 0.85;
}

// How well the short Golem column must match for the long one to be looked
// for in its place: the long labels match it at 0.65 and better
constexpr double GOLEM_LONG_NEAR = 0.55;

LabelColumn findOrangeColumn(const cv::Mat& ink, const cv::Mat& bgr, int rows, bool everywhere) {
    LabelColumn& last = g_orangeLast;
    static int looksSinceWide = 1000;
    constexpr double ORANGE_MIN = 0.85;
    // LAMP, the night vision, washes the MOLE's cockpit yellow, and the orange
    // labels stand out of the yellow ground by a third of what they stand out
    // of rock: they match 0.85 to 0.90 live, and under 0.85 in every look of a
    // recording whose colour is stored at half the resolution. Where one was
    // found a moment ago - there, at that size, in that shape - less is still
    // that column.
    constexpr double TRACKED_MIN = 0.75;
    const cv::Rect frame(0, 0, ink.cols, ink.rows);
    LabelColumn column;
    if (last.score > 0) {
        const double s = last.scale;
        const cv::Rect near = cv::Rect(last.labels.x - (int)std::lround(160 * s), last.labels.y - (int)std::lround(110 * s),
                                       (int)std::lround(380 * s), (int)std::lround(300 * s)) & frame;
        if (near.width > 60 && near.height > 80) {
            // the Golem's column in its own ink, amber
            const bool golem = last.glyph == 'G' || last.glyph == 'H';
            const cv::Mat nearInk = golem ? channelGrey(bgr(near), 6) : ink(near);
            column = findLabelColumn(nearInk, {s}, nullptr, std::string(1, last.glyph));
            column.labels += near.tl();
            column.tracked = column.score >= TRACKED_MIN;
        }
    }
    // On every try: the orange ink is tried in every other look while nothing
    // is found, so the labels are found within two looks of sitting down;
    // every second try makes the pilot seat's panel come in four looks later
    // on the bench. Each column is almost always at about one size - 1.53 to
    // 1.57 in the turrets, 0.92 to 0.95 in the pilot seat - which the fine
    // search about the best place widens by six per cent; the Golem's HUD is
    // drawn in the world and comes in anywhere from 1.15 to 1.8. The searches
    // run on the band brought down to where a column is twenty pixels wide,
    // so all of them together cost a look without a panel a few milliseconds.
    static int wideTries = 0;
    (void)wideTries;
    if (column.score < orangeBar(column.glyph) && (!g_panelSearchThrottled || ++looksSinceWide >= 1)) {
        looksSinceWide = 0;
        const double r = rows / 1440.0;
        const cv::Rect where = everywhere ? frame : hudBand(ink.size());
        // ...and all of the sizes on every try. One size in four tries is
        // 8 ms cheaper a look and costs half a minute: the pilot seat's panel
        // sits at 0.93 one look and 0.95 the next as the rock drifts, and
        // the one-size pass at 0.93 misses the other one.
        const bool allSizes = true;
        const std::vector<double> turretSizes =
            allSizes ? std::vector<double>{1.44 * r, 1.54 * r, 1.64 * r} : std::vector<double>{1.54 * r};
        const std::vector<double> cockpitSizes =
            allSizes ? std::vector<double>{0.86 * r, 0.92 * r, 0.98 * r, 1.05 * r} : std::vector<double>{0.93 * r};
        // the turret's labels short (a language pack's) or written out (the game's own)
        StageClock* tw = new StageClock(where.width == frame.width ? "  orange: T W (everywhere)" : "  orange: T W");
        LabelColumn turret = findLabelColumn(ink(where), turretSizes, nullptr, "TW");
        delete tw;
        // the pilot seat's labels short (a language pack's) or written out
        StageClock* kc = new StageClock("  orange: K P");
        LabelColumn cockpit = findLabelColumn(ink(where), cockpitSizes, nullptr, "KP");
        delete kc;
        turret.labels += where.tl();
        cockpit.labels += where.tl();
        if (turret.score > column.score) column = turret;
        if (cockpit.score > column.score) column = cockpit;
        // ...and the Golem's, in amber, at the sizes its HUD comes in
        // ...six per cent apart, as the fine search reaches three either way:
        // at 1.5 and 1.7 a column at 1.63 falls between them
        // - for the picture's height as it is now: sizes made once would stay at
        // the first height when the game changes resolution or moves to a taller
        // screen
        const std::vector<double> golemSizes{1.15 * r, 1.22 * r, 1.29 * r, 1.37 * r, 1.45 * r,
                                             1.54 * r, 1.63 * r, 1.73 * r, 1.83 * r};
        StageClock* amberClock = new StageClock("  orange: amber image");
        const cv::Mat amber = channelGrey(bgr(where), 6);
        delete amberClock;
        StageClock* gClock = new StageClock("  orange: G");
        LabelColumn golem = findLabelColumn(amber, golemSizes, nullptr, "G");
        delete gClock;
        // ...and with the game's own labels written out, where the short
        // column was found: MASS: and the first letters of the other two are
        // the same words, and the short column finds the long one at 0.65 to
        // 0.88. Searched over the band at every size beside the short one, the
        // long column - twice as wide - costs 30 to 50 ms a look with no Golem
        // on the screen.
        if (golem.score >= GOLEM_LONG_NEAR) {
            StageClock hClock("  orange: H near G");
            LabelColumn longer = labelColumnNear(amber, golem, 'H');
            longer.glyph = 'H';
            if (longer.score > golem.score) golem = longer;
        }
        golem.labels += where.tl();
        if (golem.score > column.score) column = golem;
    }
    if (column.score >= orangeBar(column.glyph) || column.tracked) last = column;
    return column;
}

bool readPanelByShapeInk(const cv::Mat& frame, const cv::Mat& bgr, int ink, ScanPanel& panel) {
    StageClock clockAll(ink == 0 ? "pass ink 0 (all)" : ink == 4 ? "pass ink 4 (all)" : "pass ink 5 (all)");
    const std::string glyphs = "L";

    // Only a panel read in full teaches the size: otherwise a column of three
    // words matched at a third of its size somewhere in the cockpit holds the
    // search at that size for every frame after it. At that size it is
    // looked for where it last was, then everywhere; the whole range of sizes
    // costs a third of a second on a 5120 x 1440 frame, and the app, which
    // looks every second and a half with nothing on screen most of the time,
    // tries that only every fourth look - every fortieth once it has read one.
    static double knownScale = 0;
    static double sureScale = 0;
    static cv::Point lastLabels(-1, -1);
    static int looksSinceWide = 1000;
    const bool everywhere = takeEverywhereTurn();
    const cv::Rect screen(0, 0, bgr.cols, bgr.rows);
    cv::Rect inkRegion = everywhere ? screen : hudBand(bgr.size());
    if (!everywhere) {
        if (ink != 4 && knownScale > 0 && lastLabels.x >= 0) {
            inkRegion |= cv::Rect(lastLabels.x - (int)std::lround(320 * knownScale),
                                  lastLabels.y - (int)std::lround(220 * knownScale),
                                  (int)std::lround(700 * knownScale), (int)std::lround(520 * knownScale)) & screen;
        }
        if (ink == 4) {
            const cv::Rect near = orangeNearRect(bgr.size());
            if (near.area() > 0) inkRegion |= near;
        }
    }
    const cv::Mat darkest = [&] {
        StageClock clock(ink == 0 ? "ink 0 image" : ink == 4 ? "ink 4 image" : "ink 5 image");
        return channelGreyIn(bgr, ink, inkRegion);
    }();
    // Three label columns are tried in orange, and the more shapes are tried
    // the likelier one of them lines up with something that is not a panel:
    // the cockpit's column found 0.76 in a turret frame and, being over 0.70,
    // kept the turret's own from being looked for at all. Real orange panels
    // matched 0.92 to 0.99.
    LabelColumn column;
    if (ink == 4) {
        StageClock clock("orange column search");
        column = findOrangeColumn(darkest, bgr, frame.rows, everywhere);
    }
    // (a tracked column's bar is never above its own: the pilot seat's is
    // 0.72, and a column tracked at 0.73 was refused by the 0.75 meant to
    // let a tracked turret column in)
    const double LABELS_MIN =
        ink != 4 ? 0.70 : (column.tracked ? (std::min)(0.75, orangeBar(column.glyph)) : orangeBar(column.glyph));
    // A column a little under its bar is tried all the same, and is a panel
    // only if the figures where it says they are read sure - every numeral
    // clear of the next likeliest. With the panel on screen the pilot seat's
    // column can stand at 0.63 to 0.71 for many looks running; the best column
    // at that spot with no panel there reached 0.58. Twelve hundredths under
    // the bar, then, and the reading decides; a column that does not read is
    // not reported as labels either, so the app neither waits on it nor
    // looks again at once. SCMINER_TRY_UNDER for the bench.
    static const double TRY_UNDER = DEV_ENV("SCMINER_TRY_UNDER") ? std::atof(DEV_ENV("SCMINER_TRY_UNDER")) : 0.12;
    // ...and further under it where the last panel stood (see tryAtSpot)
    const double LABELS_TRY = LABELS_MIN - (atPanelSpot(column, ink) ? tryAtSpot() : TRY_UNDER);
    // the searches over the whole screen at one size go over the HUD's band
    const cv::Rect band = hudBand(darkest.size());
    auto overBand = [&](const cv::Rect& where, const std::vector<double>& scales, double* at) {
        LabelColumn found = findLabelColumn(darkest(where), scales, at, glyphs);
        found.labels += where.tl();
        return found;
    };
    // At the size last read for sure the column matches a little better or
    // worse from one look to the next - 0.997 and 1.017 of it by turns on a
    // panel that was not moving - and the figures were placed by that noise.
    // So the size stays where it was unless another fits clearly better.
    double atKnown = -1;
    StageClock* whiteClock = ink != 4 ? new StageClock("white column search") : nullptr;
    if (ink != 4 && knownScale > 0 && lastLabels.x >= 0) {
        const cv::Rect near = cv::Rect(lastLabels.x - (int)std::lround(320 * knownScale),
                                       lastLabels.y - (int)std::lround(220 * knownScale),
                                       (int)std::lround(700 * knownScale), (int)std::lround(520 * knownScale)) &
                              cv::Rect(0, 0, darkest.cols, darkest.rows);
        if (near.width > 60 && near.height > 80) {
            column = findLabelColumn(darkest(near), {knownScale}, &atKnown, glyphs);
            column.labels += near.tl();
        }
    }
    if (ink != 4 && column.score < LABELS_MIN && knownScale > 0) {
        const LabelColumn whole = overBand(band, {knownScale}, &atKnown);
        if (whole.score > column.score) column = whole;
    }
    constexpr double CLEARLY = 0.05;
    if (ink != 4 && column.score >= LABELS_MIN && knownScale > 0 && atKnown >= LABELS_MIN &&
        column.score - atKnown < CLEARLY && std::abs(column.scale - knownScale) > 1e-9) {
        // the same search again at exactly the known size, for its position
        const cv::Mat templ = shapeSized(shapeFor(column.glyph), knownScale);
        const int inset = (int)std::lround(SHAPE_LABEL_INSET * column.scale);
        const cv::Point from = column.labels - cv::Point(inset, inset);
        const cv::Rect window = cv::Rect(from.x - 4, from.y - 4, templ.cols + 8, templ.rows + 8) &
                                cv::Rect(0, 0, darkest.cols, darkest.rows);
        if (window.width > templ.cols && window.height > templ.rows) {
            cv::Mat result;
            cv::matchTemplate(darkest(window), templ, result, cv::TM_CCOEFF_NORMED);
            double score = 0;
            cv::Point at;
            cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
            const int keep = (int)std::lround(SHAPE_LABEL_INSET * knownScale);
            column.labels = window.tl() + at + cv::Point(keep, keep);
            column.scale = knownScale;
            column.score = score;
        }
    }
    // A size once read for sure, or the one the frame's height gives, before
    // the whole range: the panel comes back at the size it went away at
    const double fallback = sureScale > 0 ? sureScale : frame.rows / 1440.0;
    if (ink != 4 && column.score < LABELS_MIN && std::abs(fallback - knownScale) > 0.01) {
        StageClock clock("  white: fallback size");
        const LabelColumn whole = overBand(band, {fallback}, nullptr);
        if (whole.score > column.score) column = whole;
    }
    // Once a panel has been read for sure its size is known, and the whole
    // range is only worth a look now and then - a change of seat or of field
    // of view; before that, every fourth look
    if (ink != 4 && column.score < LABELS_MIN &&
        (!g_panelSearchThrottled || ++looksSinceWide >= (sureScale > 0 ? 40 : 4))) {
        looksSinceWide = 0;
        const cv::Rect where = everywhere ? screen : band;
        StageClock clock(where.width == band.width ? "  white: wide (band)" : "  white: wide (everywhere)");
        const LabelColumn wide = overBand(where, labelScalesFor(frame.rows), nullptr);
        if (wide.score > column.score) column = wide;
    }
    delete whiteClock;
    // The size pinned by COMP. and SCU where they can be seen; a panel seen
    // for the first time, or at a size clearly off the last one, is still
    // growing in
    const double searched = column.scale;
    const PanelLayout* layout = column.glyph == 'T'   ? &TURRET_PANEL
                                : column.glyph == 'W' ? &TURRET_LONG_PANEL
                                : column.glyph == 'K' ? &COCKPIT_PANEL
                                : column.glyph == 'P' ? &COCKPIT_LONG_PANEL
                                : column.glyph == 'G' ? &GOLEM_PANEL
                                : column.glyph == 'H' ? &GOLEM_LONG_PANEL
                                                      : &SHORT_PANEL;
    if (column.score >= LABELS_MIN && column.glyph == 'L') {
        const LabelColumn longer = labelColumnNear(darkest, column, 'M');
        if (longer.score > column.score + 0.03) {
            column = longer;
            layout = &LONG_PANEL;
        }
    }
    // The figures need not be in the labels' ink. The MOLE's turret writes the
    // labels in orange, the mass in white, the resistance in green and the
    // instability in red - so a figure the labels' ink shows nothing of is
    // looked for in the other inks, the labels' own always first, so a white
    // panel reads as it would in its one ink.
    //
    // The other inks only about the panel, and only when asked for: turning
    // each into grey over a whole 5120x1440 screen costs a turret look 550 ms
    // before the face reader has even started.
    std::map<int, cv::Mat> otherInks;
    const cv::Rect panelArea = column.score < LABELS_TRY
        ? cv::Rect()
        : cv::Rect((int)std::lround(column.labels.x - 40 * column.scale), (int)std::lround(column.labels.y - 60 * column.scale),
                   (int)std::lround(300 * column.scale), (int)std::lround(220 * column.scale)) &
              cv::Rect(0, 0, darkest.cols, darkest.rows);
    auto inkImage = [&](int which) -> const cv::Mat& {
        if (which == ink) return darkest;
        auto it = otherInks.find(which);
        if (it == otherInks.end()) {
            cv::Mat full = cv::Mat::zeros(darkest.size(), CV_32F);
            if (panelArea.area() > 0) channelGrey(bgr(panelArea), which).copyTo(full(panelArea));
            it = otherInks.emplace(which, full).first;
        }
        return it->second;
    };
    // (a layout with no SCU place is not pinned: its scale is the labels' own)
    StageClock* pinClock = new StageClock("pin");
    bool pinned = column.score >= LABELS_MIN && layout->scuCx > 0 && pinPanelSize(darkest, column, *layout);
    if (!pinned && column.score >= LABELS_MIN && ink == 4 && layout->scuCx > 0) {
        pinned = pinPanelSize(darkest, column, *layout, &inkImage(1));
    }
    delete pinClock;
    panel.pinned = pinned;
    panel.resizing = knownScale <= 0 ||
                     (pinned ? std::abs(column.scale - knownScale) > knownScale * 0.025
                             : std::abs(searched - knownScale) > 1e-9);
    char note[160];
    std::snprintf(note, sizeof(note), "shape: labels %.2f at %d,%d x%.3f%s%s", column.score, column.labels.x,
                  column.labels.y, column.scale, pinned ? " pinned" : "", layout == &LONG_PANEL ? " long" : "");
    panel.note = note;
    const bool trial = column.score < LABELS_MIN;
    if (column.score < LABELS_TRY) return false;
    // Seen, whatever comes of the figures below
    panel.tryLabels = column.labels;
    panel.tryScale = column.scale;
    panel.tryGlyph = column.glyph;

    const double s = column.scale;
    const PanelFont& font = layout->font ? *layout->font : PROSPECTOR_FONT;
    // Whiteness finds the labels, but takes the waist out of a small 8: at
    // 1080 lines it read the savrilium's 365.81 as 365.01 in looks the
    // darkest channel had lost the labels in. Below 0.86 - numerals under ten
    // pixels - the figures are read in the darkest channel; LAMP by day at
    // 1440 lines stands at 0.93 and over, and reads in whiteness.
    const cv::Mat& rowInk = (ink == 5 && s < 0.86) ? inkImage(0) : darkest;
    const int left = column.labels.x + (int)std::lround(layout->valueDx * s);
    auto leftOf = [&](int r) { return left + (int)std::lround(layout->valueShear * r * s); };
    auto bottomOf = [&](int r) {
        return column.labels.y + (int)std::lround((layout->firstBottom + layout->rowPitch * r) * s);
    };
    auto readAnyInk = [&](const cv::Point& leftBottom, int format) {
        ShapeRow row = readShapeRow(rowInk, leftBottom, s, format, font);
        // The turret's figures: every ink, and the clearest reading kept, not the
        // first ink that reads at all: red over blue - where the turret's labels
        // are found - reads its red instability over red rock as ovals: 724.99
        // came back 724.09, the 9 a 0 by 0.006.
        if (!font.clustered) {
            // SCMINER_INKONLY for the bench: read in that one ink only
            static const int onlyInk = DEV_ENV("SCMINER_INKONLY") ? std::atoi(DEV_ENV("SCMINER_INKONLY")) : -1;
            for (int which : {0, 5, 1, 3, 2, 7}) {
                if (which == ink) continue;
                if (onlyInk >= 0 && which != onlyInk) continue;
                // dark on bright only for the instability, which is red: the
                // white figures turned over read a bright ground as figures
                if (which == 7 && format != 2) continue;
                ShapeRow other = readShapeRow(inkImage(which), leftBottom, s, format, font);
                if (!other.placed) continue;
                // ...by the least-matched glyph and the lead, not the mean: red
                // over blue is blurred on a recording, and read the Golem's 61.78
                // as 01.70 with a 1 at 0.74 and the rest at 0.8 to 0.93, where the
                // brightest channel had all four at 0.89 and better
                if (!row.placed || other.worst + other.lead > row.worst + row.lead) row = other;
            }
            return row;
        }
        if (row.placed && row.read) return row;
        // The other inks when the labels' own did not read sure. Under LAMP by
        // day the darkest channel finds the Prospector's labels at 0.81 to 0.90
        // but reads its figures at 0.60 to 0.78, below the floor. Whiteness (5)
        // is left out here: at 1080 lines it placed the savrilium's 365.81 as
        // 305.01 where no other ink placed anything.
        for (int which : {0, 4, 3, 2, 1}) {
            if (which == ink) continue;
            ShapeRow other = readShapeRow(inkImage(which), leftBottom, s, format, font);
            if (!other.placed) continue;
            // A figure placed in the labels' own ink stands, sure or not,
            // unless another ink's reading is sure AND clearer glyph for glyph:
            // whiteness read the savrilium's 365.81 as 365.01 with a lead of
            // its own where the darkest channel had it right without one, and
            // the looks that follow settle that
            if (row.placed && !(other.read && other.worst > row.worst + 0.03f)) continue;
            if (other.read) return other;
            if (!row.placed) row = other;
        }
        return row;
    };
    StageClock* rowsClock = new StageClock("rows");
    const ShapeRow mass = readAnyInk(cv::Point(leftOf(0), bottomOf(0)), 0);
    const std::function<const cv::Mat&(int)> panelInks = [&](int which) -> const cv::Mat& { return inkImage(which); };
    const ShapeRow res = readResistanceRow(bgr, rowInk, cv::Point(leftOf(1), bottomOf(1)), s, nullptr, font, &panelInks);
    const ShapeRow inst = readAnyInk(cv::Point(leftOf(2), bottomOf(2)), 2);
    delete rowsClock;

    panel.note += "; m [" + mass.trace + "] " + mass.ended + " r [" + res.trace + "] " + res.ended + " i [" +
                  inst.trace + "] " + inst.ended;
    const ShapeRow* rows[3] = {&mass, &res, &inst};
    for (int r = 0; r < 3; r++) {
        panel.figures[r].text = rows[r]->text;
        panel.figures[r].likeness = rows[r]->likeness;
        panel.figures[r].placed = rows[r]->placed;
    }
    const bool placed = mass.placed && res.placed && inst.placed;
    const bool sure = placed && mass.read && res.read && inst.read;
    // A column under its bar is a panel when what stands under it is: every
    // figure in its place and its format, and every numeral looking like one
    // of the font's - 0.80 and better, where a bright ground behind them (the
    // sun behind the figures) brings them to 0.55 to 0.70. Not "sure of every
    // numeral": whether one is an 8 or a 9 in a single look is what the
    // steadier settles over several, and asking it of one look throws away
    // looks at 0.66 to 0.68 whose numerals stand at 0.86 to 0.95. Anything
    // else is not a panel: nothing is learned from it, and no labels are
    // reported.
    static const float TRIAL_GLYPHS =
        DEV_ENV("SCMINER_TRIAL_GLYPHS") ? (float)std::atof(DEV_ENV("SCMINER_TRIAL_GLYPHS")) : 0.80f;
    const bool clean = placed && (std::min)({mass.worst, res.worst, inst.worst}) >= TRIAL_GLYPHS;
    if (trial && !sure && !clean) return false;
    // The size is learned from a look that was sure of its figures, or from
    // any placed look while none is known; labels found at a size where the
    // figures will not place three looks running mean the size is wrong.
    static int unplaced = 0;
    panel.labels = column.labels;
    panel.labelScale = column.scale;
    panel.labelGlyph = column.glyph;
    if (!placed) {
        if (++unplaced >= 3) {
            knownScale = sureScale;
            unplaced = 0;
        }
        return false;
    }
    unplaced = 0;
    // The size follows the panel as it grows; only a look at a size that held
    // is taken as the size it has for sure
    knownScale = column.scale;
    if (sure && !panel.resizing) sureScale = column.scale;
    lastLabels = column.labels;

    // Found: every figure in its place and its format. Whether this one look
    // is sure of every numeral is the return value; the steadier can make
    // use of the look either way.
    panel.found = true;
    g_panelSpot.known = true;
    g_panelSpot.labels = column.labels;
    g_panelSpot.scale = column.scale;
    g_panelSpot.glyph = column.glyph;
    // the Golem's columns are found in amber
    g_panelSpot.ink = column.glyph == 'G' || column.glyph == 'H' ? 6 : ink;
    // a column read under its bar is followed from here like one found over it
    if (ink == 4 && trial) g_orangeLast = column;
    panel.stats.mass = faceWhole(mass.text);
    panel.stats.resistance = facePercent(res.text);
    panel.stats.instability = faceTwoPlaces(inst.text, true);
    // Asked against the mass and the instability, which are always white, in
    // the same face and on much the same ground: at 1080 lines a white 0 %
    // on the turquoise glow of an ice rock fell to 0.72 on its own and read
    // as green. Green over blue is 0.6, over black a third.
    // With the Golem's labels written out, the E of RESISTANCE: stands at the
    // start of the figure's box - amber, its brightest a ratio of 0.6 - and
    // makes a white 7 % look coloured, the laser's. There the box starts past
    // it.
    cv::Rect resBox = res.box;
    if (layout == &GOLEM_LONG_PANEL) {
        const int pastE = column.labels.x + (int)std::lround(118 * s);
        const int right = resBox.x + resBox.width;
        if (pastE > resBox.x && right - pastE >= 4) resBox = cv::Rect(pastE, resBox.y, right - pastE, resBox.height);
    }
    const float resWhite = strokeWhiteness(bgr, resBox);
    const float refWhite = (std::max)(strokeWhiteness(bgr, mass.box), strokeWhiteness(bgr, inst.box));
    panel.stats.resistanceAdjusted = resWhite < 0.80f && resWhite < refWhite - 0.12f;
    panel.box = cv::Rect(column.labels, cv::Point((std::max)({mass.box.x + mass.box.width, res.box.x + res.box.width,
                                                              inst.box.x + inst.box.width}),
                                                  bottomOf(2)));
    panel.headRule = column.labels.y;
    panel.bodyRule = bottomOf(2);
    panel.oreRow = cv::Rect(column.labels.x - (int)std::lround(3 * s), column.labels.y - (int)std::lround(30 * s),
                            (int)std::lround(215 * s), (int)std::lround(17 * s)) &
                   cv::Rect(0, 0, frame.cols, frame.rows);

    // (the Golem's volume stands where it is not known, and is not read)
    if (layout->scuLastCentre <= 0) return sure;
    StageClock scuClock("scu");
    // The volume: "COMP.  21.30 SCU", right-aligned against SCU, so its last
    // numeral sits at the same place whatever its length. Tried from the
    // longest it can be down: started one numeral too far right, 21.30 reads
    // as a perfectly good 1.30, but started too far left the first cell is
    // empty and the reading fails.
    auto volumeEndingAt = [&](double lastCentre, ShapeRow& found) -> float {
        for (int whole = 3; whole >= 1; whole--) {
            const double firstCentre = lastCentre - font.advance - font.stopAdvance - font.advance * (whole - 1);
            const cv::Point leftBottom(column.labels.x + (int)std::lround((firstCentre - font.firstCentre) * s),
                                       column.labels.y + (int)std::lround(layout->scuBottom * s));
            // the volume is written in white on the MOLE too, beside orange labels
            const cv::Mat& scuInk = ink == 4 ? inkImage(0) : darkest;
            ShapeRow scu = readShapeRow(scuInk, leftBottom, s, 2, font);
            // ...in the turret's numerals: in each white ink, the clearest
            // reading kept, as for its figures (never dark on bright: the white
            // figures turned over read a bright ground as figures)
            if (!font.clustered) {
                for (int which : {5, 1}) {
                    ShapeRow other = readShapeRow(inkImage(which), leftBottom, s, 2, font);
                    if (!other.placed) continue;
                    if (!scu.placed || other.worst + other.lead > scu.worst + scu.lead) scu = other;
                }
            }
            if (!scu.placed || scu.text.find('.') != (size_t)whole) continue;
            // ...and a shorter reading only when nothing stands before it: a 5
            // too faint to match over the ice still lifts off the ground, and
            // without it 50.03 was 0.03
            const cv::Rect before((int)std::lround(column.labels.x + (firstCentre - font.advance - 4.5) * s),
                                  leftBottom.y - (int)std::lround(font.digitH * s),
                                  (int)std::lround(9 * s), (int)std::lround(font.digitH * s));
            if (whole < 3 && contrastIn(scuInk, before, scu.ground) >= scu.lift * 0.55f) return -1;
            found = scu;
            return faceTwoPlaces(scu.text, true);
        }
        return -1;
    };
    ShapeRow scu;
    const float read = volumeEndingAt(layout->scuLastCentre, scu);
    // ...and held against the mass: a volume no rock of this mass has is a
    // misreading, kept out
    const float volume = volumeOfMass(read, panel.stats.mass, false, false);
    if (volume > 0) {
        panel.stats.scu = volume;
        panel.note += " s [" + scu.trace + "]";
        return sure;
    }
    if (read > 0) return sure;
    // Under one SCU in thousandths, "364.01m SCU": the m stands between the
    // last numeral and SCU, so every numeral stands an m's width further
    // left. Taken only where the mass says it is a thousandth of an SCU.
    // (SCMINER_SCU_PROBE: every shift tried, for measuring one)
    if (DEV_ENV("SCMINER_SCU_PROBE")) {
        std::cout << "PROBE mass " << panel.stats.mass << " glyph " << layout->labels << " scale " << s << ":";
        for (int tenth = 20; tenth <= 200; tenth += 5) {
            ShapeRow at;
            const float v = volumeEndingAt(layout->scuLastCentre - tenth / 10.0, at);
            if (v > 0) std::cout << " " << tenth / 10.0 << "=" << at.text << "/" << at.worst << "/" << at.lead;
        }
        std::cout << std::endl;
    }
    ShapeRow milli;
    const float thousandths = volumeEndingAt(layout->scuLastCentre - SHAPE_M_SHIFT, milli);
    const float small = volumeOfMass(thousandths, panel.stats.mass, true, false);
    if (small > 0) {
        panel.stats.scu = small;
        panel.note += " s m [" + milli.trace + "]";
    }
    return sure;
}
}  // namespace

PanelSpot lastPanelSpot() {
    return g_panelSpot;
}

void setPanelSpot(const PanelSpot& spot) {
    g_panelSpot = spot;
}

void preferPanelInk(int ink) {
    // the Golem's amber column is found inside the orange pass
    g_homeInk = ink == 6 ? 4 : ink;
}

cv::Rect panelSpotWindow(const PanelSpot& spot, const cv::Size& frame) {
    if (!spot.known) return cv::Rect();
    const PanelShape& shape = shapeFor(spot.glyph);
    // The labels wander thirty pixels between one rock and the next in the
    // pilot seat, and further while the laser fires and the HUD sways: eighty
    // each way, at the panel's size
    const int room = (int)std::lround(80 * spot.scale);
    const int inset = (int)std::lround(SHAPE_LABEL_INSET * spot.scale);
    return cv::Rect(spot.labels.x - inset - room, spot.labels.y - inset - room,
                    (int)std::lround(shape.width * spot.scale) + 2 * room,
                    (int)std::lround(shape.height * spot.scale) + 2 * room) &
           cv::Rect(0, 0, frame.width, frame.height);
}

double glanceBar(const PanelSpot& spot) {
    if (!spot.known) return 2;
    // the column's own bar in its ink, less what the full look allows at the
    // spot (see tryAtSpot)
    return (spot.ink == 4 || spot.ink == 6 ? orangeBar(spot.glyph) : 0.70) - tryAtSpot();
}

double glanceAtPanelSpot(const PanelSpot& spot, const cv::Mat& window) {
    if (!spot.known || window.empty()) return -1;
    const cv::Mat ink = channelGrey(asBgr(window), spot.ink);
    // one size, and the fine search's six per cent either way about it
    const LabelColumn column = findLabelColumnOf(ink, {spot.scale}, spot.glyph, nullptr);
    return column.score;
}

// ---------------------------------------------------------------------------

void PanelSteadier::clear() {
    for (Figure& f : figures_) f = Figure();
    scus_.clear();
    ores_.clear();
    looks_ = 0;
    pinnedLooks_ = 0;
}


bool PanelSteadier::add(const ScanPanel& look, RockStats& rock) {
    // How sure one look has to be of a numeral to say the rock has changed,
    // how much an older look still counts, and how far ahead of the next
    // likeliest numeral the sums have to be. Leads between a 0 and an 8 in
    // one look run from nothing to 0.05; between numerals that do not look
    // alike they are 0.1 and more. With the numerals of one half of a test
    // run read against the other, at 0.01 no figure is given out wrong; at
    // 0.005 four are.
    static const float CHANGED = DEV_ENV("SCMINER_CHANGED") ? (float)std::atof(DEV_ENV("SCMINER_CHANGED")) : 0.06f;
    static const float FADE = DEV_ENV("SCMINER_FADE") ? (float)std::atof(DEV_ENV("SCMINER_FADE")) : 0.75f;
    static const float SETTLED = DEV_ENV("SCMINER_SETTLED") ? (float)std::atof(DEV_ENV("SCMINER_SETTLED")) : 0.01f;
    // for the bench: how many looks in a row a new size needs when COMP. and
    // SCU did not pin it (see below)
    static const int SIZELOOKS = DEV_ENV("SCMINER_SIZELOOKS") ? std::atoi(DEV_ENV("SCMINER_SIZELOOKS")) : 5;

    // The panel grows into place when it comes up, and an eight read at the
    // wrong size is a zero: an ice rock's panel can sit at 0.93 of its size
    // for three seconds and read 7148 as 7140 all that time. So a look at a
    // size other than the one the figures last settled at is left out until
    // five in a row find the new one. Four per cent either way is the same
    // size: the size a single look finds wanders by two, and a panel growing
    // in can be seven short.
    //
    // Except a look whose size COMP. and SCU pinned. The whole mining HUD
    // sways with the camera - from 1.02 to 1.20 while the laser fires - and
    // can come back from a firing run at 1.03 where it had settled at 1.13;
    // waiting out five looks there leaves rocks unread. Those looks are nearly
    // all pinned and the growing panel's are not: believing every size
    // outright shows 7140 for nine frames at 1080 lines.
    if (!look.found) return false;
    // What counts as the same size. The pilot seat's written-out labels give
    // their size to four per cent either way from one look to the next -
    // 0.92 to 1.00 over a rock that did not move - and nothing pins it: no
    // SCU place is known for them. At four per cent, the rock after one that
    // settled at 0.998 waited five looks at 0.94 - seven seconds. For them
    // eight per cent is the same size; for every other column, four.
    const double same = look.labelGlyph == 'P' ? 0.08 : 0.04;
    // ...and a size a quarter or more away is another HUD altogether - the
    // MOLE's turret's panel at 1.54 after its pilot seat's at 0.93 - not the
    // panel growing in or the HUD swaying: five looks there would keep the
    // turret's first rock off the view for three seconds and more
    if (settledScale_ > 0 && std::abs(look.labelScale - settledScale_) > settledScale_ * 0.25) {
        settledScale_ = look.labelScale;
    }
    if (settledScale_ > 0 && std::abs(look.labelScale - settledScale_) > settledScale_ * same && !look.pinned) {
        if (offScale_ > 0 && std::abs(look.labelScale - offScale_) <= offScale_ * same) {
            offLooks_++;
        } else {
            offScale_ = look.labelScale;
            offLooks_ = 1;
        }
        if (offLooks_ < SIZELOOKS) return false;
        settledScale_ = look.labelScale;
    }
    if (look.pinned) settledScale_ = look.labelScale;
    offLooks_ = 0;
    auto shapeOf = [](const PanelFigure& f, bool coloured) {
        std::string shape;
        for (char c : f.text) shape += (c >= '0' && c <= '9') ? 'D' : c;
        return shape + (coloured ? "c" : "");
    };
    auto best = [](const std::array<float, 10>& v, float& lead) {
        int top = 0;
        for (int d = 1; d < 10; d++) {
            if (v[d] > v[top]) top = d;
        }
        float second = -1e9f;
        for (int d = 0; d < 10; d++) {
            if (d != top && v[d] > second) second = v[d];
        }
        lead = v[top] - second;
        return top;
    };

    // Is this another rock? A figure of another shape, or a numeral this look
    // is sure of that the sums are sure is something else.
    // ...and looks at two sizes are not summed: the panel was still growing
    bool another = looks_ == 0 || std::abs(look.labelScale - stateScale_) > stateScale_ * same;
    for (int r = 0; r < 3 && !another; r++) {
        const PanelFigure& f = look.figures[r];
        const Figure& was = figures_[r];
        if (shapeOf(f, r == 1 && look.stats.resistanceAdjusted) != was.shape ||
            f.likeness.size() != was.sums.size()) {
            another = true;
            break;
        }
        for (size_t g = 0; g < f.likeness.size(); g++) {
            float nowLead = 0, thenLead = 0;
            const int now = best(f.likeness[g], nowLead);
            const int then = best(was.sums[g], thenLead);
            if (now != then && nowLead >= CHANGED && thenLead >= CHANGED * FADE) {
                another = true;
                break;
            }
        }
    }
    if (another) {
        clear();
        stateScale_ = look.labelScale;
    }

    if (look.pinned) pinnedLooks_++;
    for (int r = 0; r < 3; r++) {
        const PanelFigure& f = look.figures[r];
        Figure& fig = figures_[r];
        fig.shape = shapeOf(f, r == 1 && look.stats.resistanceAdjusted);
        fig.text = f.text;
        if (fig.sums.size() != f.likeness.size()) fig.sums.assign(f.likeness.size(), std::array<float, 10>{});
        for (size_t g = 0; g < f.likeness.size(); g++) {
            for (int d = 0; d < 10; d++) fig.sums[g][d] = fig.sums[g][d] * FADE + f.likeness[g][d];
        }
        fig.looks++;
    }
    looks_++;
    if (look.stats.scu > 0) scus_.push_back(look.stats.scu);
    if (!look.stats.ore.empty()) ores_.push_back(look.stats.ore);
    while (scus_.size() > 5) scus_.pop_front();
    while (ores_.size() > 5) ores_.pop_front();

    // Settled when every numeral is clear of the next likeliest - and never
    // on one look alone: when a scan comes up the figures count up from
    // nought, and a single look at 40 on its way to 31949 is sure of its 40
    // A look whose place COMP. and SCU confirmed may settle on its own; one
    // they could not - a blurred look in the middle of turning to the next
    // rock, labels found twenty pixels off - needs a second look to agree
    static const int LEAST_LOOKS = DEV_ENV("SCMINER_MINLOOKS") ? std::atoi(DEV_ENV("SCMINER_MINLOOKS")) : 1;
    if (looks_ < LEAST_LOOKS || (pinnedLooks_ == 0 && looks_ < 2)) return false;
    std::string texts[3];
    for (int r = 0; r < 3; r++) {
        const Figure& fig = figures_[r];
        size_t g = 0;
        for (char c : fig.text) {
            if (c < '0' || c > '9') {
                texts[r] += c;
                continue;
            }
            if (g >= fig.sums.size()) return false;
            float lead = 0;
            const int d = best(fig.sums[g++], lead);
            // Twice the lead while no look of this rock was pinned, on a HUD
            // whose COMP. and SCU can pin one: the Prospector's panel grows
            // in unpinned, and at 1080 lines the ICE rock's 7148 read 7140 in
            // two such looks, with a lead of 0.006 each - enough between them.
            // The pinned look that followed read the 8 at 0.013.
            // The turret's and the Golem's panels are never pinned; their
            // numerals, named by weighted shapes, lead by 0.03 and more.
            const bool pinnable = look.labelGlyph == 'L' || look.labelGlyph == 'M' || look.labelGlyph == 'K';
            if (lead < (pinnable && pinnedLooks_ == 0 ? 2 * SETTLED : SETTLED)) return false;
            texts[r] += (char)('0' + d);
        }
    }
    const float mass = faceWhole(texts[0]);
    const float res = facePercent(texts[1]);
    const float inst = faceTwoPlaces(texts[2], true);
    if (mass < 1 || res < 0 || inst < 0) return false;
    settledScale_ = look.labelScale;
    rock = RockStats();
    rock.mass = mass;
    rock.resistance = res;
    rock.instability = inst;
    rock.resistanceAdjusted = look.stats.resistanceAdjusted;
    if (!scus_.empty()) {
        std::vector<float> v(scus_.begin(), scus_.end());
        std::sort(v.begin(), v.end());
        rock.scu = v[v.size() / 2];
    }
    std::map<std::string, int> votes;
    for (const std::string& o : ores_) votes[o]++;
    int most = 0;
    for (const auto& v : votes) {
        if (v.second > most) {
            most = v.second;
            rock.ore = v.first;
        }
    }
    return true;
}

namespace {

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) row[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        int diagonal = row[0];
        row[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) {
            const int above = row[j];
            row[j] = (std::min)({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

}  // namespace

// What Tesseract made of the ore's line, put right against the minerals the
// game has: over the ice rock's bright face ICE (RAW) came back as JCE (RAW),
// (CE (RAW) and BCE (RAW) as often as not. The name is the nearest mineral
// within a third of its letters, written the way the rest of the app writes
// it; anything further off is not given out at all. The bracket after it
// is dropped.
std::string snapOreName(const std::string& read) {
    std::string name;
    const size_t open = read.rfind('(');
    for (size_t i = 0; i < (open == std::string::npos ? read.size() : open); i++) {
        const char c = (char)std::toupper((unsigned char)read[i]);
        if (c >= 'A' && c <= 'Z') name += c;
    }
    if (name.empty()) return "";
    const MineralSignature* best = nullptr;
    int bestDistance = 1000, ties = 0;
    for (const MineralSignature& m : MINERAL_SIGNATURES) {
        std::string upper;
        for (char c : m.name) upper += (char)std::toupper((unsigned char)c);
        const int d = editDistance(name, upper);
        if (d < bestDistance) {
            bestDistance = d;
            best = &m;
            ties = 0;
        } else if (d == bestDistance) {
            ties++;
        }
    }
    if (best && ties == 0 && bestDistance <= (std::max)(1, (int)best->name.size() / 3)) return best->name;
    // The second reader writes the panel's brackets its own way - "IRON ORE)"
    // with the opening one lost, "IRONCORE)" with it read as a C, and the
    // alphabet it was trained on has a full-width ) that is no bracket here
    // at all - so the word ORE runs into the name and nothing matches. No
    // mineral the game has ends in ORE or RAW, so that tail can go.
    std::string bare = name;
    for (const char* tail : {"ORE", "RAW"}) {
        const size_t at = bare.size() >= 6 ? bare.size() - 3 : std::string::npos;
        if (at != std::string::npos && bare.compare(at, 3, tail) == 0) {
            bare.resize(at);
            break;
        }
    }
    if (bare != name) {
        const std::string again = snapOreName(bare);
        if (!again.empty()) return again;
    }
    // ...and in the game's own English ice is RAW ICE, the word in front, on
    // the panel's heading and in its list alike (the MOLE's seat with the
    // long labels); the language pack that shortens the labels turns it
    // round to ICE (RAW). No mineral begins with RAW.
    if (name.size() >= 6 && name.compare(0, 3, "RAW") == 0) {
        const std::string again = snapOreName(name.substr(3));
        if (!again.empty()) return again;
    }
    // ...and the game itself shortens a long name on some HUDs - the Golem
    // writes HEPH (RAW) for Hephaestanite. Four letters name one mineral and
    // no other, so a name that begins one and only one is that one.
    if (bare.size() >= 4) {
        const MineralSignature* only = nullptr;
        for (const MineralSignature& m : MINERAL_SIGNATURES) {
            std::string upper;
            for (char c : m.name) {
                if (c >= 'a' && c <= 'z') upper += (char)(c - 'a' + 'A');
                else if (c >= 'A' && c <= 'Z') upper += c;
            }
            if (upper.size() < bare.size() || upper.compare(0, bare.size(), bare) != 0) continue;
            if (only && only->name != m.name) return "";   // begins two: no name
            if (!only) only = &m;
        }
        if (only) return only->name;
    }
    return "";
}

std::vector<FigureLines> figureLineCandidates(const cv::Mat& frame, const cv::Rect& region, int most) {
    std::vector<FigureLines> out;
    const std::vector<FaceLineInfo> lines = faceLinesDetailed(frame, region);
    if (lines.size() < 3) return out;
    // A line that could be a label and a figure: a word of three glyphs or
    // more, and a shorter word after it on the same line. Which is
    // readFiguresIn's own pairing with the one test left out that the MOLE's
    // turret fails - what the second word READS as.
    struct Pair {
        cv::Rect label, value;
    };
    auto pairsOf = [](const FaceLineInfo& line) {
        std::vector<Pair> out;
        for (size_t k = 1; k < line.tokens.size(); k++) {
            const cv::Rect& v = line.tokens[k].box;
            const size_t glyphs = line.tokens[k].boxes.size();
            if (glyphs < 1 || glyphs > 7) continue;
            const int H = (std::max)(6, v.height);
            for (size_t j = k; j-- > 0;) {
                const cv::Rect& l = line.tokens[j].box;
                if (line.tokens[j].boxes.size() < 3) continue;
                if (l.x + l.width <= v.x && v.x - (l.x + l.width) <= H * 12) out.push_back({l, v});
                break;
            }
        }
        return out;
    };
    std::vector<std::vector<Pair>> pairs;
    pairs.reserve(lines.size());
    for (const FaceLineInfo& line : lines) pairs.push_back(pairsOf(line));

    const cv::Rect screen(0, 0, frame.cols, frame.rows);
    for (size_t i = 0; i + 2 < lines.size(); i++) {
      for (const Pair& pm : pairs[i]) {
        for (size_t j = i + 1; j < lines.size() && j <= i + 3; j++) {
          for (const Pair& pr : pairs[j]) {
            for (size_t q = j + 1; q < lines.size() && q <= j + 3; q++) {
              for (const Pair& pi : pairs[q]) {
        // ...standing under one another, their figures' left edges together,
        // an even pitch apart: readFiguresIn's tests exactly
        const int H = (std::max)({6, pm.value.height, pi.value.height});
        const int glyph = (std::max)(4, H * 3 / 4);
        const int values = (std::max)({pm.value.x, pr.value.x, pi.value.x}) -
                           (std::min)({pm.value.x, pr.value.x, pi.value.x});
        if (values > glyph * 2) continue;
        const int b0 = pm.value.y + pm.value.height;
        const int b1 = pr.value.y + pr.value.height;
        const int b2 = pi.value.y + pi.value.height;
        const int p0 = b1 - b0, p1 = b2 - b1;
        if (p0 < H || p1 < H || p0 > H * 3 || p1 > H * 3) continue;
        if (std::abs(p0 - p1) * 10 > (std::max)(p0, p1) * 3) continue;
        // the labels' left edge: the middle of the three, as noise stretches
        // a label to the left and never to the right
        int left = (std::max)({(std::min)(pm.label.x, pr.label.x),
                               (std::min)((std::max)(pm.label.x, pr.label.x), pi.label.x)});
        left = (std::max)(left, pm.value.x - H * 16);
        const int right = (std::max)({pm.value.x + pm.value.width, pr.value.x + pr.value.width,
                                      pi.value.x + pi.value.width});
        if (right - left < H * 3) continue;
        // the line the second reader is given: the label and its figure, with
        // a little sky over and under so no stroke is cut off
        const int pad = (std::max)(2, H / 3);
        auto row = [&](const cv::Rect& v, const cv::Rect& l) {
            const int top = (std::min)(v.y, l.y) - pad;
            const int tall = (std::max)(v.y + v.height, l.y + l.height) + pad - top;
            return cv::Rect(left - pad, top, right + pad - (left - pad), tall) & screen;
        };
        FigureLines found;
        found.values = {pm.value, pr.value, pi.value};
        found.rows[1] = row(pm.value, pm.label);
        found.rows[2] = row(pr.value, pr.label);
        found.rows[3] = row(pi.value, pi.label);
        // the ore's name is the line above, when one stands close enough over
        // the mass
        found.rows[0] = cv::Rect(left - pad, found.rows[1].y - p0, found.rows[1].width, found.rows[1].height) & screen;
        if (i >= 1) {
            const cv::Rect& up = lines[i - 1].rect;
            if (found.rows[1].y - up.y <= p0 * 2 && found.rows[1].y > up.y) {
                found.rows[0] = cv::Rect(left - pad, up.y, (std::max)(right, up.x + up.width) + pad - (left - pad),
                                         up.height) & screen;
            }
        }
        found.off = std::abs(p0 - p1) / (double)H + values / (double)glyph;
        out.push_back(found);
              }
            }
          }
        }
      }
    }
    std::sort(out.begin(), out.end(), [](const FigureLines& x, const FigureLines& y) { return x.off < y.off; });
    // one block per place: several pairings of the same three lines are one panel
    std::vector<FigureLines> kept;
    for (const FigureLines& f : out) {
        bool seen = false;
        for (const FigureLines& had : kept) {
            if ((had.rows[1] & f.rows[1]).area() > 0 && (had.rows[3] & f.rows[3]).area() > 0) seen = true;
        }
        if (seen) continue;
        kept.push_back(f);
        if ((int)kept.size() >= most) break;
    }
    return kept;
}

std::vector<cv::Rect> panelFigureLines(const ScanPanel& panel, const cv::Size& frame) {
    std::vector<cv::Rect> rows;
    const bool trusted = panel.labels.x >= 0 && panel.labelScale > 0;
    const cv::Point at = trusted ? panel.labels : panel.tryLabels;
    const double s = trusted ? panel.labelScale : panel.tryScale;
    if (at.x < 0 || s <= 0) return rows;
    const PanelLayout& layout = layoutOf(trusted ? panel.labelGlyph : panel.tryGlyph);
    const PanelFont& font = layout.font ? *layout.font : PROSPECTOR_FONT;
    const double tall = font.digitH * s;
    const int pad = (std::max)(2, (int)std::lround(tall * 0.55));
    const cv::Rect screen(0, 0, frame.width, frame.height);
    for (int r = -1; r <= 2; r++) {
        // The ore's name is a word, and a long one runs past where the
        // figures end; the rows below it are sheared a little further left
        // with every row on the MOLE's turret.
        const double room = r < 0 ? 150.0 : 72.0;
        const double shear = layout.valueShear * (r < 0 ? 0 : r);
        const int left = at.x - (int)std::lround(SHAPE_LABEL_INSET * s);
        const int right = at.x + (int)std::lround((layout.valueDx + shear + room) * s);
        const int bottom = at.y + (int)std::lround((layout.firstBottom + layout.rowPitch * r) * s);
        rows.push_back(cv::Rect(left, bottom - (int)std::lround(tall) - pad, right - left,
                                (int)std::lround(tall) + 2 * pad) &
                       screen);
    }
    return rows;
}

namespace {

// A panel line as the second reader gives it back - "MASS: 173",
// "RESISTANCE:  56% ", "INSTABILITY:5.81" - split into the label it names
// itself with and the figure beside it.
//
// The label is the gate. The geometry that placed the row is only as sure as
// the label column's match, and a reader handed a figure alone has nothing to
// check itself against: a box half a row out reads "MASS7", which would pass
// as a mass of 7. So the line has to carry the word AND the colon the game
// writes after it.
bool labelledFigure(const std::string& text, const std::string& label, std::string& figure) {
    std::string tight;
    for (char c : text) {
        if (c != ' ') tight += (char)std::toupper((unsigned char)c);
    }
    // The same panel writes MASS: RES: INST: on one HUD and MASS:
    // RESISTANCE: INSTABILITY: on another, so the colon is asked to be near
    // the start of the line rather than just past the short label -
    // INSTABILITY: puts it at eleven.
    const size_t colon = tight.find_first_of(":;");
    if (colon == std::string::npos || colon == 0 || colon > 14) return false;
    if (tight.compare(0, label.size(), label) != 0) return false;
    figure = tight.substr(colon + 1);
    return !figure.empty();
}

// ...and the mass with the thousands separator the HUD sets on a big rock
float lineWhole(const std::string& figure) {
    std::string bare;
    for (char c : figure) {
        if (c != ',') bare += c;
    }
    return faceWhole(bare);
}

// How sure a line has to read to be taken. The word, the colon and the
// figure's format are the gate; this only keeps out a line the net itself
// was guessing at. The turret's own rows read at 0.86 to 0.96.
constexpr double LINE_SURE = 0.60;
constexpr double LINE_ORE_SURE = 0.70;   // a name has no format to be gated by
// How clear the shapes must have been about a numeral for the second reader
// to have to agree with it. See readPanelRows.
constexpr float SHAPES_CLEAR = 0.10f;

// The numerals of a figure as the shapes named them, left to right, each with
// its lead over the next likeliest numeral. Every glyph but the per cent sign
// left a likeness behind; the full stop and the '?' of a row that ran into
// something unreadable are not glyphs.
std::vector<std::pair<char, float>> shapeNumerals(const PanelFigure& figure) {
    std::vector<std::pair<char, float>> out;
    size_t k = 0;
    for (char c : figure.text) {
        if (c == '.' || c == '?' || c == '%') continue;
        if (k >= figure.likeness.size()) break;
        const std::array<float, 10>& l = figure.likeness[k++];
        int top = 0;
        for (int d = 1; d < 10; d++) {
            if (l[d] > l[top]) top = d;
        }
        float second = -1e9f;
        for (int d = 0; d < 10; d++) {
            if (d != top && l[d] > second) second = l[d];
        }
        out.push_back({(char)('0' + top), l[top] - second});
    }
    return out;
}

}  // namespace

std::string oreByLine(const cv::Mat& frame, const cv::Rect& row, TextLineReader& lines) {
    // The last name is kept while the row looks the same. Reading it costs as
    // much as a figure, and an ore's name does not change while the rock does
    // not - the Tesseract path keeps it the same way.
    static cv::Mat lastRow;
    static std::string lastOre;
    static int reused = 0;
    if (!lines.ready()) return "";
    const cv::Mat bgr = asBgr(frame);
    const cv::Rect safe = row & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (safe.width < 24 || safe.height < 8) return "";
    cv::Mat small;
    cv::cvtColor(bgr(safe), small, cv::COLOR_BGR2GRAY);
    cv::resize(small, small, cv::Size(128, 12), 0, 0, cv::INTER_AREA);
    small.convertTo(small, CV_32F);
    // A row that read as no name is kept for three looks, not eight: a name
    // lost to a glint should come back soon, but one that never reads - a
    // short form no mineral begins with - should not cost a line every look.
    if (!lastRow.empty() && reused < (lastOre.empty() ? 3 : 8) &&
        cv::norm(small, lastRow, cv::NORM_L1) / (double)small.total() < 6.0) {
        reused++;
        return lastOre;
    }
    const TextLineReader::Line name = lines.read(bgr(safe));
    lastOre = name.mean >= LINE_ORE_SURE ? snapOreName(name.text) : std::string();
    if (DEV_ENV("SCMINER_PANELLINES_TRACE")) {
        std::cerr << "  [lines] ore row " << safe << " read '" << name.text << "' mean " << name.mean << " -> '"
                  << lastOre << "'" << std::endl;
    }
    lastRow = small;
    reused = 0;
    return lastOre;
}

bool readPanelByLines(const cv::Mat& frame, TextLineReader& lines, ScanPanel& panel) {
    const std::vector<cv::Rect> found = panelFigureLines(panel, frame.size());
    if (found.size() != 4) return false;
    const std::array<cv::Rect, 4> rows{found[0], found[1], found[2], found[3]};
    // The figure half of each row, for the colour test: from where the layout
    // says the figure starts to the end of the row, the row's full height.
    //
    // Not the numerals' own height on their baseline, though that sounds
    // tighter: it turns a white 0 % on the Golem into "printed in colour" -
    // that HUD grows and shrinks with the view, the layout's baseline sits a
    // few pixels off, and a box as tall as the numerals then catches the red
    // instability under them. The full row brings the ground in instead, and
    // the mass is the control for that (see readPanelRows): on all 24 bench
    // panels for the line reader it gave the colour the other readers gave.
    // What it cannot tell is a green resistance over a sunlit red rock bright
    // enough to colour the mass's box too; none is on file.
    const bool trusted = panel.labels.x >= 0 && panel.labelScale > 0;
    const PanelLayout& layout = layoutOf(trusted ? panel.labelGlyph : panel.tryGlyph);
    const double s = trusted ? panel.labelScale : panel.tryScale;
    const int at = (trusted ? panel.labels.x : panel.tryLabels.x) + (int)std::lround(layout.valueDx * s);
    std::array<cv::Rect, 3> values{};
    for (int r = 0; r < 3; r++) {
        const int left = at + (int)std::lround(layout.valueShear * r * s);
        values[r] = cv::Rect(left, rows[r + 1].y, (std::max)(1, rows[r + 1].x + rows[r + 1].width - left),
                             rows[r + 1].height) &
                    cv::Rect(0, 0, frame.cols, frame.rows);
    }
    return readPanelRows(frame, lines, rows, values, panel);
}

bool readPanelRows(const cv::Mat& frame, TextLineReader& lines, const std::array<cv::Rect, 4>& rows,
                   const std::array<cv::Rect, 3>& values, ScanPanel& panel) {
    if (!lines.ready()) return false;
    for (size_t i = 1; i < rows.size(); i++) {
        if (rows[i].width < 24 || rows[i].height < 8) return false;
    }
    // A panel running off the side of the screen has its figures cut, and a
    // cut mass is still a whole number: a turret panel slides sideways with
    // the turret, and 16,754 cut after its third numeral reads as a clean
    // 167 - with the resistance and the instability, which stand a little
    // further left and are shorter, whole beside it. So a panel whose mass
    // row reaches the edge of the picture is not read by line; the looks
    // after it, as the panel comes back, will read it whole.
    if (rows[1].x <= 0 || rows[1].x + rows[1].width >= frame.cols) {
        panel.note += "; lines: the mass runs off the screen";
        return false;
    }
    StageClock clockAll("line reader");
    const cv::Mat bgr = asBgr(frame);
    double ms = 0;
    // The mass first and alone: a look at a column that is not a panel then
    // costs one line, not four.
    std::string lastText;
    auto figureOf = [&](int row, const char* label, std::string& figure) {
        const TextLineReader::Line line = lines.read(bgr(rows[row]));
        ms += line.ms;
        lastText = line.text;
        if (line.mean < LINE_SURE) return false;
        return labelledFigure(line.text, label, figure);
    };
    std::string figures[3];
    char note[200];
    if (!figureOf(1, "MASS", figures[0])) {
        std::snprintf(note, sizeof(note), "; lines: no mass in '%.40s', %.0f ms", lastText.c_str(), ms);
        panel.note += note;
        return false;
    }
    const float mass = lineWhole(figures[0]);
    float resistance = -1, instability = -1;
    if (mass > 0) {
        if (figureOf(2, "RES", figures[1])) resistance = facePercent(figures[1]);
        if (resistance >= 0 && figureOf(3, "INST", figures[2])) instability = faceTwoPlaces(figures[2], true);
    }
    if (mass <= 0 || resistance < 0 || resistance > 100 || instability <= 0) {
        std::snprintf(note, sizeof(note), "; lines: %.0f/%.0f/%.2f in '%.40s', %.0f ms", mass, resistance, instability,
                      lastText.c_str(), ms);
        panel.note += note;
        return false;
    }
    // Where the shapes looked at this same place and were clear about a
    // numeral of the MASS, the second reader has to have that numeral there
    // too. It is a general reader, and this HUD's zero is slashed: on the
    // Golem it read 9,570 as 9,578 where the shapes, cut from this very font,
    // had the 0 clear of the next likeliest by 0.12. The bar is 0.10 because
    // the shapes are not always right either - they read 39,245 as 30,245
    // with the 0 clear by 0.059, where the second reader had it right. On the
    // 24 bench panels for the line reader, 3 of them wrong, this turns away
    // one wrong reading and no right one.
    //
    // The mass only. It is the one figure every HUD prints in white, which
    // is what the shapes read well; the resistance and the instability are
    // green or red, and there the shapes are sure of the wrong numeral as
    // often as not - held to them as well, the Golem lost eight right
    // readings for two wrong ones (14.89 thrown out because the shapes were
    // sure by 0.35 of a 1 where the 8 stood).
    //
    // Where the shapes did not look at this place - the lines found by shape
    // on the screen - there is nothing to hold the reading to, and this lets
    // it through.
    for (int r = 0; r < 1; r++) {
        std::string mine;
        for (char c : figures[r]) {
            if (c >= '0' && c <= '9') mine += c;
        }
        const std::vector<std::pair<char, float>> theirs = shapeNumerals(panel.figures[r]);
        for (size_t k = 0; k < theirs.size() && k < mine.size(); k++) {
            if (theirs[k].second < SHAPES_CLEAR || theirs[k].first == mine[k]) continue;
            std::snprintf(note, sizeof(note), "; lines: %s '%.12s' has %c where the shapes are sure of %c (%.3f)",
                          r == 0 ? "mass" : r == 1 ? "resistance" : "instability", figures[r].c_str(), mine[k],
                          theirs[k].first, theirs[k].second);
            panel.note += note;
            return false;
        }
    }
    const std::string ore = oreByLine(frame, rows[0], lines);
    // The column is proven: the three labels, their colons and their three
    // formats stood where it said they would. A column only SEEN before is
    // now the panel's own, so the glance has a place to come back to.
    if (panel.labels.x < 0) {
        panel.labels = panel.tryLabels;
        panel.labelScale = panel.tryScale;
        panel.labelGlyph = panel.tryGlyph;
    }
    panel.stats.mass = mass;
    panel.stats.resistance = resistance;
    panel.stats.instability = instability;
    // Green or red rather than white: the laser has already had its say on
    // this resistance, and the view must not read it as the rock's own.
    if (values[1].width > 2 && values[1].height > 2) {
        // Over the figure itself, and nothing else. The three figures are one
        // size, so a value box taller than the mass's has caught something
        // that is not the figure, such as sunlit red rock that makes a white
        // figure read as printed in colour. Cut to the mass's height from the
        // BOTTOM, which is the line the numerals stand on.
        cv::Rect ink = values[1];
        const int tall = (std::max)(4, (std::min)(ink.height, values[0].height));
        ink.y = ink.y + ink.height - tall;
        ink.height = tall;
        // ...and the mass is the control: where the ground is bright enough
        // to colour anything, it colours the mass too.
        panel.stats.resistanceAdjusted = printedInColour(bgr, ink) && !printedInColour(bgr, values[0]);
        if (DEV_ENV("SCMINER_PANELLINES_TRACE")) {
            std::cerr << "  [lines] colour: mass " << values[0] << " " << printedInColour(bgr, values[0]) << "  res "
                      << values[1] << " -> " << ink << " " << printedInColour(bgr, ink) << std::endl;
        }
    }
    panel.stats.ore = ore;
    // The figures are the second reader's now, and what the shapes made of
    // the same place comes out of the panel. The app hands a panel whose mass
    // carries the shapes' likeness to the PanelSteadier - which would sum the
    // very reading that just failed, start again on every look, and after
    // sixteen of them take the rock off the screen. A line reading goes the
    // way a face reading goes.
    for (PanelFigure& figure : panel.figures) figure = PanelFigure();
    panel.found = true;
    panel.figuresRead = 3 + (ore.empty() ? 0 : 1);
    panel.confidence = 1.0;
    panel.box = rows[1] | rows[3];
    panel.oreRow = rows[0];
    std::snprintf(note, sizeof(note), "; lines: %.0f kg %.0f%% %.2f%s%s, %.0f ms", mass, resistance, instability,
                  ore.empty() ? "" : " ", ore.c_str(), ms);
    panel.note += note;
    return true;
}

ScanPanel readScanPanel(const cv::Mat& frame, OCREngine& ocr, GlyphReader* glyphs, TextLineReader* lines) {
    // SCMINER_FACE for the bench only: 0 the old reader, 1 the face alone,
    // 2 the face and the old reader behind it, 3 the shapes alone, 4 - the
    // default - the shapes, and the face behind them only while the shapes
    // have not found the labels for a long while
    static const int mode = [] {
        const char* set = DEV_ENV("SCMINER_FACE");
        return set ? std::atoi(set) : 4;
    }();
    // SCMINER_NO_PANELLINES for the bench: the second reader kept off the
    // panel altogether
    static const bool noLines = DEV_ENV("SCMINER_NO_PANELLINES") != nullptr;
    // ...and SCMINER_NO_FACEREAD, which puts the face reader's own reading
    // aside so that what structure and the second reader manage between them
    // can be measured on its own
    static const bool noFace = DEV_ENV("SCMINER_NO_FACEREAD") != nullptr;
    // The ore's name for a panel the second reader read: it names it itself
    // most of the time (0.92 on the turret's lettering), and where it did not
    // and Tesseract is there, Tesseract is asked. Without this a panel the
    // line reader won over the face reader can lose its ore name.
    auto nameTheOre = [&](ScanPanel& panel) {
        if (!panel.stats.ore.empty() || panel.oreRow.width <= 0) return;
        std::map<std::string, int> votes;
        for (const cv::Mat& shot : forOcr(asBgr(frame), panel.oreRow)) {
            const std::string ore = snapOreName(ocr.recognizeLine(shot, "ABCDEFGHIJKLMNOPQRSTUVWXYZ() -"));
            if (!ore.empty()) votes[ore]++;
        }
        int most = 0;
        for (const auto& v : votes) {
            if (v.second > most) {
                most = v.second;
                panel.stats.ore = v.first;
            }
        }
        if (panel.stats.ore.empty() && lines && !noLines) panel.stats.ore = oreByLine(frame, panel.oreRow, *lines);
        panel.figuresRead = 3 + (panel.stats.ore.empty() ? 0 : 1);
    };
    // where the face reader is to look, when the shapes found the labels
    cv::Rect faceNear;
    // ...and the shapes' own look, for when the face reader finds nothing
    ScanPanel shapeLook;
    if (mode >= 3) {
        ScanPanel shape;
        const bool sure = readPanelByShape(frame, shape);
        // The face reader stands in for a HUD whose labels the shapes do not
        // know - a patch that renames them, say - and not for a look that
        // merely missed: twenty looks without the labels, and even then only
        // every fourth look, as it costs a sixth of a second a time
        static int withoutLabels = 0;
        static int faceTurn = 0;
        withoutLabels = shape.labels.x >= 0 ? 0 : withoutLabels + 1;
        // Orange labels found and their figures not read - the MOLE's turret,
        // whose figures the shapes do not know yet - go to the face reader on
        // every look, never through the throttle below: with the labels found
        // the throttle holds it back, and throttled it reads that panel as
        // seldom as once in sixteen seconds
        const bool orangeUnread = shape.labels.x >= 0 && shape.ink == 4;
        // ...and only about those labels: reading every region of the whole
        // screen puts eight seconds between looks on the MOLE and reads figures
        // off other lines
        if (orangeUnread && shape.labelScale > 0) {
            const double s = shape.labelScale;
            faceNear = cv::Rect((int)std::lround(shape.labels.x - 60 * s), (int)std::lround(shape.labels.y - 80 * s),
                                (int)std::lround(330 * s), (int)std::lround(260 * s));
        }
        // The labels found and the figures beside them not read: the second
        // reader is given the rows.
        //
        // This is the MOLE's TURRET. The shapes know two fonts at the sizes
        // they were harvested at, and the turret's are neither faint nor small,
        // yet the shapes find that panel's labels and read none of its figures:
        // the mass ending at the floor, the resistance reading a 9 where a 5
        // stood. The second reader is sure of all three rows there, and of the
        // ore's name besides, which nothing else reads without Tesseract. It
        // comes after the shapes: it costs 50 to 60 ms a line, and the shapes
        // are quicker and surer of the numerals they do know.
        // SCMINER_NO_PANELLINES for the bench turns it off.
        if (!noLines && !shape.found && (shape.labels.x >= 0 || shape.tryLabels.x >= 0) && lines &&
            lines->ready() && readPanelByLines(frame, *lines, shape)) {
            nameTheOre(shape);
            return shape;
        }
        // ...and the shapes' own look, kept AFTER that try so that what the
        // line reader made of the rows is in the note either way
        shapeLook = shape;
        // How often the face reader stands in when the shapes find nothing
        // at all. In the MOLE's PILOT SEAT it is the only reader there is: the
        // shapes miss the short orange column over the rock. Looking over the
        // HUD's band alone, a look that stands in costs 180 to 220 ms, so it
        // comes in after four looks without labels and then every fourth look;
        // once it has read a panel it reads on every look until eight looks
        // have passed with nothing, so a rock in view is followed rather than
        // glimpsed.
        const bool faceHot = g_sinceFaceRead < 8;
        if (g_sinceFaceRead < 1000) g_sinceFaceRead++;
        if (mode == 4 && !shape.found && !orangeUnread && !faceHot &&
            (withoutLabels < 4 || ++faceTurn % 4 != 0)) {
            return shape;
        }
        StageClock faceClock("face reader (whole)");
        if (shape.found) {
            StageClock oreClock("ore name OCR");
            if (shape.oreRow.width > 0) {
                // Tesseract cost 60 to 90 ms a look, half of a look with a
                // panel on it, to read the same name again and again. The
                // line is kept small and grey from the last reading; while it
                // looks the same - the HUD sways, a pixel or two - the name
                // stands, and every eighth look reads it afresh regardless.
                static cv::Mat lastLine;
                static std::string lastOre;
                static int reused = 0;
                const cv::Rect lineRect = shape.oreRow & cv::Rect(0, 0, frame.cols, frame.rows);
                cv::Mat line;
                if (lineRect.width > 8 && lineRect.height > 4) {
                    cv::Mat grey;
                    cv::cvtColor(asBgr(frame)(lineRect), grey, cv::COLOR_BGR2GRAY);
                    cv::resize(grey, line, cv::Size(128, 12), 0, 0, cv::INTER_AREA);
                    line.convertTo(line, CV_32F);
                }
                bool same = false;
                if (!line.empty() && !lastLine.empty() && !lastOre.empty() && reused < 8) {
                    const double diff = cv::norm(line, lastLine, cv::NORM_L1) / (double)line.total();
                    same = diff < 6.0;
                }
                if (same) {
                    shape.stats.ore = lastOre;
                    reused++;
                } else {
                    std::map<std::string, int> votes;
                    for (const cv::Mat& shot : forOcr(asBgr(frame), shape.oreRow)) {
                        const std::string ore = snapOreName(ocr.recognizeLine(shot, "ABCDEFGHIJKLMNOPQRSTUVWXYZ() -"));
                        if (!ore.empty()) votes[ore]++;
                    }
                    int most = 0;
                    for (const auto& v : votes) {
                        if (v.second > most) {
                            most = v.second;
                            shape.stats.ore = v.first;
                        }
                    }
                    lastLine = line;
                    lastOre = shape.stats.ore;
                    reused = 0;
                }
            }
            // Tesseract's data is not in the download, so on a fresh install
            // Tesseract names no ore. The second reader IS in the exe and reads
            // that line at 0.92 on the turret's lettering, so where Tesseract said
            // nothing it is asked.
            if (shape.stats.ore.empty() && !noLines && lines && lines->ready()) {
                StageClock oreLineClock("ore name by line");
                shape.stats.ore = oreByLine(frame, shape.oreRow, *lines);
            }
            shape.figuresRead = sure ? 3 + (shape.stats.ore.empty() ? 0 : 1) : 0;
            shape.confidence = 1.0;
            return shape;
        }
        if (mode == 3) return shape;
    }
    if (mode >= 1) {
        // A reading the face reader made, finished: the ore's name is the one
        // thing the face cannot read - it is letters, and Tesseract reads
        // letters well enough
        auto finished = [&](ScanPanel& face) {
            // The face reader reports an ore row only where it found a line
            // of text close enough above the mass, and where its own left
            // edge sits right of the ink it measured it reports a row ten
            // pixels wide, which is no row (as on the turret's panel). Then the
            // row is where the panel's own pitch says it is: one row above the
            // mass, as wide as the box, so the panel still gets an ore name.
            if ((face.oreRow.width < 24 || face.oreRow.height < 8) && face.box.width > 8 && face.box.height > 12) {
                const int pitch = (std::max)(6, face.box.height * 2 / 5);
                face.oreRow = cv::Rect(face.box.x - 2, face.box.y - pitch, face.box.width, pitch) &
                              cv::Rect(0, 0, frame.cols, frame.rows);
            }
            if (face.oreRow.width > 0) {
                std::map<std::string, int> votes;
                for (const cv::Mat& shot : forOcr(asBgr(frame), face.oreRow)) {
                    // Snapped to a mineral the game has, as the shape reader's
                    // own path does. What Tesseract reads off a turret's lettering
                    // is not always a word ("BRUERGN - S U RN" for a rock's name);
                    // a name that is near nothing the game has is no name.
                    const std::string ore = snapOreName(ocr.recognizeLine(shot, "ABCDEFGHIJKLMNOPQRSTUVWXYZ() -"));
                    if (!ore.empty()) votes[ore]++;
                }
                int most = 0;
                for (const auto& v : votes) {
                    if (v.second > most) {
                        most = v.second;
                        face.stats.ore = v.first;
                    }
                }
            }
            // ...and the second reader behind Tesseract here too, as on
            // every other path: without it a turret panel the face reader
            // read came back with no ore name at all.
            if (face.stats.ore.empty() && !noLines && lines && lines->ready()) {
                face.stats.ore = oreByLine(frame, face.oreRow, *lines);
            }
            face.figuresRead = 3 + (face.stats.ore.empty() ? 0 : 1);
            face.confidence = 1.0;
            face.note = "face" + face.note + ", " + std::to_string(face.figuresRead) + " of 4 figures";
            if (face.found) g_sinceFaceRead = 0;   // a rock in view is followed, see above
        };
        // Labels the shapes found tell the face reader where the panel is, so
        // it reads that part of the screen straight away. The rules it would
        // otherwise look about are mostly not found in the MOLE's turrets: with
        // them alone the Helix turret's panel reads in 9 looks of 104, with the
        // labels found in every one. SCMINER_FACENEAR for the bench: 0 the rules
        // only, 1 - the default - the labels' part only, 2 the labels' part,
        // then the rules. On the turret 1 reads as often as 0 and 2 within a
        // tenth, at 200 to 270 ms a look against 310 to 380 for 0 and 2.
        static const int nearMode = [] {
            const char* set = DEV_ENV("SCMINER_FACENEAR");
            return set ? std::atoi(set) : 1;
        }();
        const cv::Rect nearPart = faceNear & cv::Rect(0, 0, frame.cols, frame.rows);
        if (nearMode >= 1 && nearPart.width > 40 && nearPart.height > 40) {
            ScanPanel face;
            if (!noFace && readFiguresIn(frame, nearPart, face)) {
                finished(face);
                return face;
            }
        }
        if (nearMode != 1 || nearPart.area() == 0) {
            // in the app's own mode over the band the HUD stands in, which is
            // a quarter of the screen
            const cv::Rect regionBand = mode == 4 ? hudBand(frame.size()) : cv::Rect(0, 0, frame.cols, frame.rows);
            for (cv::Rect region : figureRegions(frame(regionBand))) {
                region += regionBand.tl();
                if (faceNear.area() > 0 && (region & faceNear).area() == 0) continue;
                ScanPanel face;
                if (noFace || !readFiguresIn(frame, region, face)) continue;
                finished(face);
                return face;
            }
        }
        if (mode == 1) {
            ScanPanel none;
            none.note = "face: no figures in any region";
            return none;
        }
        // No figures the face reader could read: in the app's own mode the old
        // reader is not asked after it. It has nothing to add but its cost and
        // its misreadings - in the MOLE's turret it reads 139 / 0 % / 0.01 as
        // 139 / 8 % / 8.01, at up to two and a half seconds a look.
        // Nothing the face reader could READ - which in the MOLE's turret is
        // almost every look. The same regions are looked at once more for the
        // SHAPE of a panel, and the second reader is asked what those lines
        // say. It is the last thing tried and the dearest: at most
        // LINE_PROBES lines of one look are spent finding out, and each costs
        // 50 to 60 ms.
        if (!noLines && lines && lines->ready()) {
            StageClock clock("line reader (by shape)");
            // SCMINER_PANELLINES_TRACE for the bench: what shape each region
            // offered and what the reader made of it
            static const bool trace = DEV_ENV("SCMINER_PANELLINES_TRACE") != nullptr;
            constexpr int LINE_PROBES = 3;
            const cv::Rect band = mode == 4 ? hudBand(frame.size()) : cv::Rect(0, 0, frame.cols, frame.rows);
            // Every region's shapes first, and the best few of ALL of them
            // read - not the first region's. A region is a pair of rules and
            // there are dozens of them; the one the panel is whole in is not
            // the first, and in the turret the regions before it hold the SCAN
            // RESULTS heading, which reads as a line as well as any.
            std::vector<FigureLines> shapes;
            for (cv::Rect region : figureRegions(frame(band))) {
                region += band.tl();
                if (faceNear.area() > 0 && (region & faceNear).area() == 0) continue;
                for (const FigureLines& f : figureLineCandidates(frame, region, LINE_PROBES)) {
                    bool seen = false;
                    for (const FigureLines& had : shapes) {
                        if ((had.rows[1] & f.rows[1]).area() > 0 && (had.rows[3] & f.rows[3]).area() > 0) seen = true;
                    }
                    if (!seen) shapes.push_back(f);
                }
            }
            std::sort(shapes.begin(), shapes.end(),
                      [](const FigureLines& x, const FigureLines& y) { return x.off < y.off; });
            if (trace) std::cerr << "  [lines] " << shapes.size() << " shape(s) over the band" << std::endl;
            for (int k = 0; k < (int)shapes.size() && k < LINE_PROBES; k++) {
                ScanPanel found;
                const bool got = readPanelRows(frame, *lines, shapes[k].rows, shapes[k].values, found);
                if (trace) {
                    std::cerr << "  [lines] " << shapes[k].rows[1] << " off " << shapes[k].off
                              << (got ? " READ" : " -") << found.note << std::endl;
                }
                if (!got) continue;
                nameTheOre(found);
                found.note = shapeLook.note + "; by shape" + found.note;
                g_sinceFaceRead = 0;   // a rock in view is followed, as after a face reading
                return found;
            }
        }
        if (mode == 4) return shapeLook;
    }
    ScanPanel best;
    int bestTold = 0;
    for (ScanPanel& panel : scanPanelCandidates(frame)) {
        if (!panel.found) return panel;
        readOnePanel(frame, panel, ocr, glyphs);

        // One number is not a panel. The composition list pairs up as neatly
        // as the figures do and a line of it reads as a mass - 322, off "49.24
        // % SAVRILIUM (ORE) 322". What only the figures have is all four of
        // them at once: the ore, a mass worth calling a rock, a resistance and
        // an instability.
        int told = 0;
        if (!panel.stats.ore.empty()) told++;
        if (panel.stats.mass >= 100) told++;
        if (panel.stats.resistance >= 0) told++;
        if (panel.stats.instability >= 0) told++;
        // The mass is not required of a single look. It is the longest figure
        // and so the one the vote most often cannot settle, and the caller
        // reads the panel again every second and a half - a mass that failed
        // here comes in on the next look and the median picks it up.
        panel.figuresRead = told;
        if (told >= 3) {
            panel.note += ", " + std::to_string(told) + " of 4 figures";
            return panel;
        }
        if (told > bestTold) {
            bestTold = told;
            best = panel;
        }
    }
    return best;
}

void readOnePanel(const cv::Mat& frame, ScanPanel& panel, OCREngine& ocr, GlyphReader* glyphs) {
    if (!panel.found) return;

    const cv::Mat bgr = asBgr(frame);
    const cv::Mat cut = brightStructure(bgr);
    const std::vector<cv::Range> all = textBands(cut, panel.box);
    // Between the rules: the ore, the mass, the resistance, the instability.
    // Above the first rule is the heading, below the second the composition.
    std::vector<cv::Range> bands, after;
    for (const cv::Range& b : all) {
        const int y = panel.box.y + b.start;
        if (y < panel.headRule) continue;             // the heading
        if (y < panel.bodyRule) bands.push_back(b);
        else after.push_back(b);
    }
    if (bands.size() < 4) {
        panel.note += ", only " + std::to_string(bands.size()) + " rows between the rules";
        return;
    }

    // The value sits on the right of its label, in every row that has one.
    // The labels - MASS:, RES:, INST:, COMP. - end around a fifth of the way
    // across; the figures are set right. A crop from 42 % cuts into a
    // six-digit mass, so it starts at a third.
    const int valueX = panel.box.x + panel.box.width * 33 / 100;
    const int valueW = panel.box.width - (valueX - panel.box.x);

    // What the renderings agree on. Two out of three is enough; with three
    // different answers the row is not read at all, because a figure nothing
    // confirms is a guess, and a guess here becomes a rock the player is told
    // they can or cannot break.
    // decimals: 0 for a whole number, 2 for the ones the panel writes to two
    auto agreedNumber = [&](const cv::Rect& where, int decimals = 0,
                            bool wholeRow = false) -> float {
        if (wholeRow) return -1;
        const std::vector<cv::Mat> shots = forOcr(bgr, where);
        std::vector<float> answers;

        // The signature marker's reader gets a vote too, because it is
        // the only one of them that has seen this typeface. The HUD writes a
        // SLASHED zero, which general OCR reads as an 8 as often as not -
        // "40 %" came back as 48 and "120.10" as 120.18 through every
        // rendering at once, and no amount of thresholding mends a digit that
        // is being recognised as a different digit. It is a vote rather than
        // the answer: built for the signature marker's plate, it segments a
        // panel row less surely, so it has to be confirmed like the rest.
        if (glyphs) {
            const std::string digits = digitsWithGlyphs(bgr, where, *glyphs);
            if ((int)digits.size() > decimals) {
                std::string shaped = digits;
                if (decimals > 0) shaped.insert(shaped.size() - decimals, ".");
                answers.push_back((float)std::atof(shaped.c_str()));
            }
        }
        for (const cv::Mat& shot : shots) {
            // Every reading goes past the HUD's own numerals on its way in,
            // so a zero read as an eight is put right before it votes rather
            // than after six renderings have agreed on it.
            const std::string said = ocr.recognizeLine(shot, "0123456789.%");
            const float v = numberIn(settleZerosAndEights(bgr, where, said));
            if (v >= 0) answers.push_back(v);
        }
        // Three agreeing is the bar, and where nothing reaches it, two.
        //
        // Three alone is too strict in hard light. Over a bright ice field the
        // row still renders cleanly, but the seven readings scatter over the
        // last digit and none of them gathers three, so the figure would come
        // back as nothing and the view would not switch.
        //
        // What makes two safe enough is the caller: the panel is read every
        // second and a half and what is shown is the median of five, so a
        // single reading carried on two votes is one vote in a larger count,
        // not the answer. One rendering alone still says nothing.
        for (int need = 3; need >= 2; need--) {
            for (size_t i = 0; i < answers.size(); i++) {
                int votes = 0;
                for (size_t j = 0; j < answers.size(); j++) {
                    if (std::fabs(answers[i] - answers[j]) < 0.005f) votes++;
                }
                if (votes >= need) return answers[i];
            }
        }
        return -1;
    };

    // SCMINER_PANEL_DEBUG=<dir> writes every row the reader looks at, raw and
    // as each rendering hands it to Tesseract. Without seeing those, tuning a
    // reader is guessing at which of five stages lost the digit.
    const char* debugDir = DEV_ENV("SCMINER_PANEL_DEBUG");
    int debugRow = 0;

    auto valueOf = [&](size_t row, int decimals) -> float {
        if (row >= bands.size()) return -1;
        if (debugDir) {
            const cv::Rect whole(panel.box.x, panel.box.y + bands[row].start - 2,
                                 panel.box.width, bands[row].end - bands[row].start + 4);
            const cv::Rect safe = whole & cv::Rect(0, 0, bgr.cols, bgr.rows);
            if (safe.width > 4 && safe.height > 4) {
                const std::string tag = std::string(debugDir) + "/row" + std::to_string(debugRow);
                cv::Mat big;
                cv::resize(bgr(safe), big, cv::Size(), 4, 4, cv::INTER_NEAREST);
                cv::imwrite(tag + "_raw.png", big);
                int k = 0;
                for (const cv::Mat& shot : forOcr(bgr, safe)) {
                    cv::imwrite(tag + "_cut" + std::to_string(k++) + ".png", shot);
                }
            }
            debugRow++;
        }
        const cv::Rect right(valueX, panel.box.y + bands[row].start - 2, valueW,
                             bands[row].end - bands[row].start + 4);
        return agreedNumber(right, decimals);
    };

    // The first row between the rules is the ore, and it is a name.
    {
        const cv::Rect where(panel.box.x, panel.box.y + bands[0].start - 2, panel.box.width,
                             bands[0].end - bands[0].start + 4);
        std::map<std::string, int> votes;
        for (const cv::Mat& shot : forOcr(bgr, where)) {
            std::string ore = ocr.recognizeLine(shot, "ABCDEFGHIJKLMNOPQRSTUVWXYZ() -");
            while (!ore.empty() && (ore.back() == ' ' || ore.back() == '-')) ore.pop_back();
            if (ore.size() >= 3) votes[ore]++;
        }
        for (const auto& v : votes) {
            if (v.second >= 2) { panel.stats.ore = v.first; break; }
        }
        if (panel.stats.ore.empty() && votes.size() == 1) panel.stats.ore = votes.begin()->first;
    }

    // As the panel writes them: mass whole, resistance whole per cent,
    // instability to two places.
    //
    // And each inside what it can possibly be. A resistance of 434 % is not a
    // resistance - it is the instability on the line below, picked up because
    // two rows of faint text ran together into one band. A figure that cannot
    // be what it claims to be must be dropped rather than shown: it goes on to
    // pick a gadget, and at 434 % the app would recommend bringing down a
    // resistance that is not there.
    panel.stats.mass = valueOf(1, 0);
    if (panel.stats.mass > 0 && panel.stats.mass < 100) panel.stats.mass = -1;

    panel.stats.resistance = valueOf(2, 0);
    if (panel.stats.resistance > 100) panel.stats.resistance = -1;
    // ...and in which colour: white is the rock's own, green or red is after
    // the laser on it has had its say. See rockOwnResistance.
    if (panel.stats.resistance >= 0 && bands.size() > 2) {
        const cv::Rect right(valueX, panel.box.y + bands[2].start - 2, valueW,
                             bands[2].end - bands[2].start + 4);
        panel.stats.resistanceAdjusted = printedInColour(bgr, right);
    }

    panel.stats.instability = valueOf(3, 2);
    if (panel.stats.instability > 100000) panel.stats.instability = -1;

    // ...and the same figure twice over is two readings of one row, not two
    // rows: the bands ran together. Keep the one that is in range for its own
    // line and drop the other.
    if (panel.stats.resistance >= 0 && panel.stats.instability >= 0 &&
        std::fabs(panel.stats.resistance - panel.stats.instability) < 1.0f &&
        panel.stats.instability > 100) {
        panel.stats.resistance = -1;
    }

    // The volume - "COMP.  14.71 SCU" - is the row after the four figures,
    // and which side of the second rule it lands on depends on which rule was
    // found. The composition bar under COMP. is a long horizontal run of lit
    // pixels, so the search takes IT for the second rule as readily as the
    // thin line above the heading: then COMP. counts as a fifth row between
    // the rules and what sits below is the bar itself.
    //
    // So both sides are tried, the figures' side first, and a reading only
    // counts if it could be a rock's contents: a few dozen SCU. Read as
    // digits the bar comes back as 11,750.
    {
        std::vector<cv::Rect> candidates;
        for (size_t i = 4; i < bands.size(); i++) {
            candidates.push_back(cv::Rect(valueX, panel.box.y + bands[i].start - 2, valueW,
                                          bands[i].end - bands[i].start + 4));
        }
        for (size_t i = 0; i < after.size() && i < 2; i++) {
            candidates.push_back(cv::Rect(valueX, panel.box.y + after[i].start - 2, valueW,
                                          after[i].end - after[i].start + 4));
        }
        for (const cv::Rect& where : candidates) {
            const float read = agreedNumber(where, 2);
            if (read <= 0.05f || read >= 1000.0f) continue;
            const float scu = volumeOfMass(read, panel.stats.mass, false, true);
            if (scu > 0) {
                panel.stats.scu = scu;
                break;
            }
        }
    }

    panel.note += "; mass " + std::to_string((int)panel.stats.mass) +
                  ", res " + std::to_string((int)panel.stats.resistance) +
                  ", inst " + std::to_string(panel.stats.instability) +
                  ", " + std::to_string(panel.stats.scu) + " SCU" +
                  (panel.stats.ore.empty() ? "" : ", " + panel.stats.ore);
}

cv::Mat drawScanPanelSearch(const cv::Mat& frame, const ScanPanel& panel) {
    cv::Mat shown = asBgr(frame).clone();
    const double scale = (double)REFERENCE_HEIGHT / (double)frame.rows;
    cv::Mat work;
    if (std::abs(scale - 1.0) < 0.02) {
        work = asBgr(frame);
    } else {
        cv::resize(asBgr(frame), work, cv::Size(), scale, scale,
                   scale < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
    }
    for (const PanelRule& r : findHorizontalRules(work, RULE_MIN_W, RULE_MAX_W)) {
        const double back = 1.0 / scale;
        cv::line(shown, cv::Point((int)(r.x0 * back), (int)(r.y * back)),
                 cv::Point((int)(r.x1 * back), (int)(r.y * back)), cv::Scalar(0, 255, 255), 2);
    }
    if (panel.found) cv::rectangle(shown, panel.box, cv::Scalar(0, 0, 255), 3);
    return shown;
}

}  // namespace SCMiner
