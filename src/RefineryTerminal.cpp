// The REFINEMENT CENTER read off the screen - see RefineryTerminal.h.
#include "RefineryTerminal.h"

#include "MiningHardware.h"
#include "RefineryData.h"
#include "RefiningData.h"
#include "TextLineReader.h"
#include "DevSwitch.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace SCMiner {

namespace {

// The light a screen is seen in. The game's exposure, or a brightness set
// otherwise, makes everything darker or lighter, and the reader goes by how
// light things are. So the screen's white is measured - the lettering and
// the bars, the middle of the brightest hundredth of what has little colour
// (under three tenths of its brightest channel) - and the picture is scaled
// so that white stands at the reference, about 248 in its brightest channel.
// Colour is left out because the brightest of a whole picture are orange
// lamps and stripes (245,154,99 at Levski). All channels get the same gain:
// stretched channel by channel, the grey bars can turn blue and be lost.
// The gain stays within 0.85-1.8; one within 0.05 of 1 is not applied, and
// a white under 100 is left alone.
struct Light {
    double white = 248;   // the screen's white, in its brightest channel
    double gain = 1.0;
    cv::Mat table;        // 1 x 256: a value in the picture -> as recorded
};

constexpr double RECORDED_WHITE = 248;

// (every step-th pixel of every step-th row looked at)
Light lightOf(const cv::Mat& bgr, int step) {
    Light light;
    int top[256] = {0};
    long neutrals = 0;
    for (int y = 0; y < bgr.rows; y += step) {
        const cv::Vec3b* row = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; x += step) {
            const cv::Vec3b& p = row[x];
            const int hi = (std::max)({p[0], p[1], p[2]}), lo = (std::min)({p[0], p[1], p[2]});
            if (hi < 60 || (hi - lo) * 10 > hi * 3) continue;
            top[hi]++;
            neutrals++;
        }
    }
    if (neutrals >= 100) {
        // the middle of the brightest hundredth: the 99.5th of them
        long seen = 0;
        for (int v = 255; v > 0; v--) {
            seen += top[v];
            if (seen * 200 >= neutrals) {
                light.white = v;
                break;
            }
        }
        // (a white under 100 is no lettering's but a ground's with
        // little colour, where there is no lettering to be seen)
        if (light.white >= 100) light.gain = std::clamp(RECORDED_WHITE / light.white, 0.85, 1.8);
    }
    if (std::abs(light.gain - 1.0) >= 0.05) {
        light.table.create(1, 256, CV_8U);
        for (int v = 0; v < 256; v++) light.table.at<uchar>(0, v) = cv::saturate_cast<uchar>(v * light.gain);
    }
    return light;
}

cv::Mat inRecordedLight(const cv::Mat& bgr, const Light& light) {
    if (light.table.empty()) return bgr;
    cv::Mat out;
    cv::LUT(bgr, light.table, out);
    return out;
}

// How the bands under the bars look, measured at Levski (5120x1440).
RefineryBar::Band bandOf(const cv::Scalar& bgr) {
    const double b = bgr[0], g = bgr[1], r = bgr[2];
    // SETUP: a dark red - 41,46,79 (BGR) at Levski
    if (r >= 50 && r >= g * 1.45 && r >= b * 1.45) return RefineryBar::Band::Setup;
    // PROCESSING: amber, red and green well over blue - 40,77,89
    if (r >= 60 && r >= b * 1.6 && g >= b * 1.5 && r <= g * 1.4) return RefineryBar::Band::Processing;
    // COMPLETED: not yet seen at Levski - taken to be the yellow-green of
    // Pyro's block (180,196,106); the panel's title has the last word
    if (g >= 60 && g >= r * 0.95 && g >= b * 1.4) return RefineryBar::Band::Completed;
    return RefineryBar::Band::None;
}

// Levski's bars: light grey over each work order, lettered in dark grey,
// the band of the order's state under it - 446x18 in 1440 lines. They are
// looked for row by row, so that nothing hangs on how large the panels
// stand: a bar is rows on end light over the same long stretch, the
// lettering's rows light over most of it, and dark above and below. A
// closing that runs the lettering together also runs neighbouring panels'
// bars together at 1.5x panel size (a 4K screen), and grows a frame's
// rules into a bar in brighter light.
void findGreyBars(const cv::Mat& small, double s, bool everyBar, std::vector<RefineryBar>& found) {
    const double H = small.rows;
    static const bool trace = DEV_ENV("SCMINER_REFINERY_TRACE") != nullptr;
    // light grey: bright in every channel, with little colour - under a
    // quarter of the brightest channel, where a tint over the screen leaves
    // the grey
    cv::Mat light(small.size(), CV_8U);
    for (int y = 0; y < small.rows; y++) {
        const cv::Vec3b* row = small.ptr<cv::Vec3b>(y);
        uchar* out = light.ptr<uchar>(y);
        for (int x = 0; x < small.cols; x++) {
            const int hi = (std::max)({row[x][0], row[x][1], row[x][2]});
            const int lo = (std::min)({row[x][0], row[x][1], row[x][2]});
            out[x] = lo > 120 && hi - lo < (std::max)(40, hi / 4);
        }
    }
    struct Stretch {
        int x0, x1;
    };
    struct Open {
        Stretch first;       // where it began
        int x0, x1;          // the widest of its full rows
        int y0, rows;
        double cover;        // summed over its rows
        bool matched;
    };
    const int shortest = (std::max)(8, static_cast<int>(std::lround(H * 0.12)));
    std::vector<Open> open;
    std::vector<int> prefix(small.cols + 1, 0);
    const auto close = [&](const Open& o) {
        const int w = o.x1 - o.x0 + 1, h = o.rows;
        if (trace && h >= 2) {
            std::printf("      grey bar? %d,%d %dx%d (half) cover %.2f\n", o.x0, o.y0, w, h, o.cover / h);
        }
        // long for its height, as a bar is (25 times; the HUD's thin rules
        // are 50 times and more), and light over most of it
        if (h < 2 || h > H * 0.04 || w > H * 0.9) return;
        if (w < h * 12 || w > h * 40) return;
        if (o.cover / h < 0.7) return;
        // The band beneath it, between the panel's title and its number
        const int by0 = o.y0 + h + static_cast<int>(std::lround(h * 0.6));
        const int by1 = o.y0 + h + static_cast<int>(std::lround(h * 2.0));
        const int bx0 = o.x0 + static_cast<int>(std::lround(w * 0.38));
        const int bx1 = o.x0 + static_cast<int>(std::lround(w * 0.56));
        if (by1 >= small.rows || bx1 <= bx0 || by1 <= by0) return;
        const cv::Scalar band = cv::mean(small(cv::Rect(bx0, by0, bx1 - bx0, by1 - by0)));
        RefineryBar bar;
        bar.band = bandOf(band);
        bar.bandColour = band;
        bar.bar = cv::Rect(static_cast<int>(std::lround(o.x0 / s)), static_cast<int>(std::lround(o.y0 / s)),
                           static_cast<int>(std::lround(w / s)), static_cast<int>(std::lround(h / s)));
        if (bar.band == RefineryBar::Band::None && !everyBar) return;
        found.push_back(bar);
    };
    std::vector<Stretch> stretches;
    for (int y = 0; y <= small.rows; y++) {
        stretches.clear();
        if (y < small.rows) {
            const uchar* row = light.ptr<uchar>(y);
            for (int x = 0; x < small.cols; x++) prefix[x + 1] = prefix[x] + row[x];
            // the row's light stretches, over gaps of a pixel or two
            int start = -1, lastLit = -10;
            for (int x = 0; x <= small.cols; x++) {
                const bool lit = x < small.cols && row[x];
                if (lit) {
                    if (start < 0 || x - lastLit > 3) {
                        if (start >= 0 && lastLit - start + 1 >= shortest) stretches.push_back({start, lastLit});
                        start = x;
                    }
                    lastLit = x;
                }
            }
            if (start >= 0 && lastLit - start + 1 >= shortest) stretches.push_back({start, lastLit});
        }
        const auto overlap = [](int a0, int a1, int b0, int b1) { return (std::min)(a1, b1) - (std::max)(a0, b0) + 1; };
        std::vector<bool> used(stretches.size(), false);
        std::vector<Open> still;
        for (Open& o : open) {
            o.matched = false;
            if (y < small.rows) {
                // the row's stretch over the same: of much the same length and
                // lying over it, or longer and lying over all of it - the bar's
                // first row has its corners rounded off, and on a look a little
                // aslant the first rows are short of either end - but not
                // twice as long, run into a neighbour's. The longest sets where
                // the bar is looked at.
                const int length = o.first.x1 - o.first.x0 + 1;
                for (size_t i = 0; i < stretches.size(); i++) {
                    const int other = stretches[i].x1 - stretches[i].x0 + 1;
                    const int common = overlap(o.first.x0, o.first.x1, stretches[i].x0, stretches[i].x1);
                    const bool same = std::abs(other - length) * 100 <= length * 15 &&
                                      common * 100 >= (std::min)(length, other) * 80;
                    const bool longer = other > length && other * 10 <= length * 16 && common * 100 >= length * 80;
                    if (!same && !longer) continue;
                    used[i] = true;
                    o.matched = true;
                    o.x0 = (std::min)(o.x0, stretches[i].x0);
                    o.x1 = (std::max)(o.x1, stretches[i].x1);
                    if (other > length) o.first = stretches[i];
                    break;
                }
                // a row of the lettering: light over most of the stretch
                const double cover = (prefix[o.first.x1 + 1] - prefix[o.first.x0]) /
                                     static_cast<double>(o.first.x1 - o.first.x0 + 1);
                if (o.matched || cover >= 0.6) {
                    o.rows++;
                    o.cover += o.matched ? (std::max)(cover, 0.9) : cover;
                    still.push_back(o);
                    continue;
                }
            }
            close(o);
        }
        open = still;
        for (size_t i = 0; i < stretches.size(); i++) {
            if (used[i]) continue;
            // (a stretch inside a bar already open is a row of it)
            bool inside = false;
            for (const Open& o : open) {
                if (overlap(o.x0, o.x1, stretches[i].x0, stretches[i].x1) * 2 > stretches[i].x1 - stretches[i].x0 + 1) inside = true;
            }
            if (inside) continue;
            open.push_back(Open{stretches[i], stretches[i].x0, stretches[i].x1, y, 1, 1.0, true});
        }
    }
}

// How a candidate for Pyro's block is hatched, looked at in full. Each row's
// brightness - its strongest channel - is followed along against its own
// average over a stripe's width (a block is about 28 stripes wide), so that
// light falling across the block, or a ground lit between the stripes, does
// not count; a stripe is counted each time the row climbs from a third of
// its swing under that average to a third over it. A block crosses about 28
// stripes a row at any size - blur and a small picture take the swing out of
// them before they take the count; counted against the row's own dark and
// light, a blurred pale block at Pyro Gateway counted 9 - and the light it
// climbs to is the block's colour. The stripes lean to the right, so a row
// is like the one above it moved a pixel to the left.
struct Hatch {
    double crossings = 0;   // stripes a row
    double depth = 0;       // a row's dark under its light, of the light
    double lean = 0;        // how much better a row matches the one above moved over than not
    cv::Scalar colour;      // the light's colour (B, G, R)
};

Hatch hatchOf(const cv::Mat& bgr, const cv::Rect& r) {
    Hatch hatch;
    const cv::Mat block = bgr(r);
    if (block.cols < 8 || block.rows < 3) return hatch;
    const int n = block.cols;
    const int reach = (std::max)(1, n / 56);   // half a stripe's width each way
    std::vector<int> v(n), sorted(n), sum(n + 1, 0);
    std::vector<double> off(n), swings(n);
    std::vector<uchar> above(n), before(n);
    double rises = 0, depths = 0, sumB = 0, sumG = 0, sumR = 0, leaning = 0;
    long lit = 0;
    int rows = 0, pairs = 0;
    bool havePrevious = false;
    for (int y = 0; y < block.rows; y++) {
        const cv::Vec3b* p = block.ptr<cv::Vec3b>(y);
        for (int x = 0; x < n; x++) {
            v[x] = (std::max)({p[x][0], p[x][1], p[x][2]});
            sum[x + 1] = sum[x] + v[x];
        }
        sorted = v;
        std::nth_element(sorted.begin(), sorted.begin() + n / 10, sorted.end());
        const int low = sorted[n / 10];
        std::nth_element(sorted.begin(), sorted.begin() + n * 9 / 10, sorted.end());
        const int high = sorted[n * 9 / 10];
        rows++;
        if (high <= 0) continue;
        depths += (high - low) / static_cast<double>(high);
        for (int x = 0; x < n; x++) {
            const int from = (std::max)(0, x - reach), to = (std::min)(n, x + reach + 1);
            off[x] = v[x] - (sum[to] - sum[from]) / static_cast<double>(to - from);
            swings[x] = std::abs(off[x]);
        }
        std::nth_element(swings.begin(), swings.begin() + n * 9 / 10, swings.end());
        const double swing = swings[n * 9 / 10];
        if (swing < 4) {
            havePrevious = false;
            continue;
        }
        const double band = swing / 3;
        bool on = off[0] > band;
        for (int x = 0; x < n; x++) {
            if (!on && off[x] > band) {
                on = true;
                if (x > 0) rises++;
            } else if (on && off[x] < -band) {
                on = false;
            }
            above[x] = on;
            if (off[x] > band) {
                sumB += p[x][0];
                sumG += p[x][1];
                sumR += p[x][2];
                lit++;
            }
        }
        if (havePrevious) {
            int same = 0, moved = 0;
            for (int x = 1; x + 1 < n; x++) {
                same += above[x] == before[x];
                moved += above[x] == before[x + 1];
            }
            leaning += (moved - same) / static_cast<double>(n - 2);
            pairs++;
        }
        before = above;
        havePrevious = true;
    }
    if (rows == 0) return hatch;
    hatch.crossings = rises / rows;
    hatch.depth = depths / rows;
    hatch.lean = pairs > 0 ? leaning / pairs : 0;
    if (lit > 0) hatch.colour = cv::Scalar(sumB / lit, sumG / lit, sumR / lit);
    return hatch;
}

// Which of the three a block's colour is - red while an order is set up,
// orange while it runs, yellow-green once it is done - or -1 for a colour
// that is none of them: grey, white, blue
int blockKind(const cv::Scalar& colour) {
    const double b = colour[0], g = colour[1], r = colour[2];
    const double hi = (std::max)({b, g, r}), lo = (std::min)({b, g, r});
    if (hi < 40 || (hi - lo) < hi * 0.3) return -1;
    if (b > (std::max)(g, r) * 0.75) return -1;
    if (g >= r * 0.88) return 2;
    if (g >= r * 0.4) return 1;
    return 0;
}

// A block's edges, found in full where its hatching begins and ends. Blur
// and a small picture take the end stripes' colour into the dark round
// them, and the colour alone found a blurred block a seventh too narrow -
// and every field measured from it out of place. Across the stripes - down
// to the right - the brightness changes from pixel to pixel; along them, up
// to the right, it is still; on the ground round the block it is still both
// ways, and where the stripes end at the block's side it changes both ways
// alike, which blur spread outwards and made a blurred block wider. What the
// change across has over the change along is the hatching alone, and where
// it falls to half of what it is inside the block is its edge: blur spreads
// the fall, but leaves its half way where it was.
cv::Rect blockEdges(const cv::Mat& bgr, const cv::Rect& rough) {
    const int mx = (std::max)(4, rough.width / 2), my = (std::max)(3, rough.height / 2);
    const cv::Rect window =
        cv::Rect(rough.x - mx, rough.y - my, rough.width + 2 * mx, rough.height + 2 * my) & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (window.width < 8 || window.height < 4) return rough;
    cv::Mat v(window.size(), CV_16S);
    for (int y = 0; y < window.height; y++) {
        const cv::Vec3b* p = bgr.ptr<cv::Vec3b>(window.y + y) + window.x;
        short* out = v.ptr<short>(y);
        for (int x = 0; x < window.width; x++) out[x] = static_cast<short>((std::max)({p[x][0], p[x][1], p[x][2]}));
    }
    // the hatching's change, summed down the columns over the rough's rows
    // and along the rows over the rough's columns
    const int r0 = rough.y - window.y, r1 = r0 + rough.height;
    const int c0 = rough.x - window.x, c1 = c0 + rough.width;
    std::vector<double> columns(window.width, 0.0), rows(window.height, 0.0);
    for (int y = 1; y + 1 < window.height; y++) {
        const short* above = v.ptr<short>(y - 1);
        const short* below = v.ptr<short>(y + 1);
        for (int x = 1; x + 1 < window.width; x++) {
            const int across = std::abs(below[x + 1] - above[x - 1]);
            const int along = std::abs(above[x + 1] - below[x - 1]);
            const double change = (std::max)(0, across - along);
            if (y >= r0 && y < r1) columns[x] += change;
            if (x >= c0 && x < c1) rows[y] += change;
        }
    }
    // ...evened out over two stripes - a block is about 28 wide - which
    // leaves the half way of a fall where it is: column by column the change
    // came and went with the stripes at 1.5 times the size, under half on
    // every third column, and the block's edge was put inside it
    const auto evened = [](const std::vector<double>& profile, int reach) {
        std::vector<double> out(profile.size(), 0.0);
        double sum = 0;
        int count = 0;
        for (int i = 0; i < static_cast<int>(profile.size()) + reach; i++) {
            if (i < static_cast<int>(profile.size())) {
                sum += profile[i];
                count++;
            }
            if (i - 2 * reach - 1 >= 0) {
                sum -= profile[i - 2 * reach - 1];
                count--;
            }
            if (i - reach >= 0 && i - reach < static_cast<int>(out.size())) out[i - reach] = sum / count;
        }
        return out;
    };
    columns = evened(columns, (std::max)(1, rough.width / 28));
    rows = evened(rows, 1);
    // the inside's level: the middle of the rough's middle half
    const auto level = [](const std::vector<double>& profile, int from, int to) {
        if (to <= from) return 0.0;
        std::vector<double> part(profile.begin() + from, profile.begin() + to);
        std::nth_element(part.begin(), part.begin() + part.size() / 2, part.end());
        return part[part.size() / 2];
    };
    const double across = level(columns, c0 + rough.width / 4, c1 - rough.width / 4);
    const double down = level(rows, r0 + rough.height / 4, r1 - rough.height / 4);
    if (across <= 0 || down <= 0) return rough;
    // outwards from the middle to where the change falls under half
    int left = (c0 + c1) / 2, right = left, top = (r0 + r1) / 2, bottom = top;
    while (left > 1 && columns[left - 1] >= across / 2) left--;
    while (right < window.width - 2 && columns[right + 1] >= across / 2) right++;
    while (top > 0 && rows[top - 1] >= down / 2) top--;
    while (bottom < window.height - 1 && rows[bottom + 1] >= down / 2) bottom++;
    return cv::Rect(window.x + left, window.y + top, right - left + 1, bottom - top + 1);
}

// Pyro's rest stops dress the same panels another way: no grey bar over a
// work order but a hatched block at its top left - red stripes (RGB
// 127,32,42) while it is set up, orange (203,119,68) once it runs,
// yellow-green (180,196,106) once it is done, on near black - 181x31 in
// 1440 lines (0.126 and 0.0215 of the height). Pyro Gateway's (in Stanton)
// are the same blocks paler, on a ground lit between the stripes. Sand and
// a cockpit's orange light come in the same colours, so the block has to be
// a clean rectangle, and hatched rather than solid.
//
// The block is found by its stripes, not by its colour at half size: there
// a stripe and the dark beside it come out as their mean, and in warm light
// the ground between the stripes (98,74,54) passes for orange, joining the
// block with the header and the station's name. The stripes stand out (an
// orange of 190 against that brown of 100), so they are looked for in full,
// and a pixel of half size is marked where one passes through what it
// stands for; the marks, closed, are the block.
void findHatchedBlocks(const cv::Mat& bgr, const cv::Mat& small, double s, std::vector<RefineryBar>& found) {
    std::vector<cv::Mat> ch;
    cv::split(small, ch);
    const cv::Mat& b = ch[0];
    const cv::Mat& g = ch[1];
    const cv::Mat& r = ch[2];
    // Only where a warm colour stands at half size - red, orange, yellow,
    // yellow-green: blue well under the stronger of red and green - and next
    // to it, is looked at in full: no stripe passes anywhere else
    const cv::Mat redOrGreen = cv::max(r, g);
    cv::Mat warm = (redOrGreen > 50) & (b < redOrGreen * 0.85);
    cv::dilate(warm, warm, cv::Mat());
    // ...and of it only what stands out of what is round it: a stripe is
    // brighter than the ten pixels about it, where a lamp's warm glow on a
    // wall is as bright as its own surroundings and would otherwise run
    // into the block
    const cv::Mat brightest = cv::max(cv::max(b, g), r);
    cv::Mat around;
    cv::blur(brightest, around, cv::Size(5, 5));
    const int step = static_cast<int>(std::lround(1.0 / s));
    // each pixel in full: 1 for red ink, 2 for warm, 0 for neither
    cv::Mat ink = cv::Mat::zeros(bgr.size(), CV_8U);
    for (int y = 0; y < small.rows; y++) {
        const uchar* look = warm.ptr<uchar>(y);
        const uchar* surround = around.ptr<uchar>(y);
        const int fy1 = (std::min)(bgr.rows, (y + 1) * step);
        for (int x = 0; x < small.cols; x++) {
            if (!look[x]) continue;
            const int fx1 = (std::min)(bgr.cols, (x + 1) * step);
            for (int fy = y * step; fy < fy1; fy++) {
                const cv::Vec3b* row = bgr.ptr<cv::Vec3b>(fy);
                uchar* kinds = ink.ptr<uchar>(fy);
                for (int fx = x * step; fx < fx1; fx++) {
                    const int B = row[fx][0], G = row[fx][1], R = row[fx][2];
                    if ((std::max)({B, G, R}) * 100 < surround[x] * 112) continue;
                    // Red: 127,32,42 at Checkmate. And one ink for the rest,
                    // orange to yellow-green - the panel's title says which
                    // kind it is, and a tint or blur took the colour of a
                    // finished order's stripes between the two: orange
                    // 203,119,68, at Pyro Gateway as pale as 135,84,49, in
                    // warm light 189,123,81 and blurred there 154,99,73;
                    // yellow-green 180,196,106, paler 141,139,83 and washed
                    // out by a brighter gamma 169,167,116. Blue under 0.72 of
                    // the stronger of red and green, and red at least 0.7 of
                    // green - the green notice's (174,250,120) is not; the
                    // warm ground (98,74,54) stands out of nothing. Dim
                    // stripes count - red at two thirds of its light is 83 -
                    // as they stand out and go on.
                    const int redGreen = (std::max)(R, G);
                    if (R > 60 && R > 2 * G && 5 * R > 8 * B) {
                        kinds[fx] = 1;
                    } else if (redGreen > 70 && 100 * G > 42 * R && 10 * R >= 7 * G && 100 * B < 72 * redGreen) {
                        kinds[fx] = 2;
                    }
                }
            }
        }
    }
    // A pixel of a stripe is one the stripe goes on through, up to the right
    // and down to the left. Noise, a letter's edge and the rules round a
    // panel stand out too under a tint or in grain, but no stripe goes on
    // through them; marked, they would be closed into the block and lose it.
    // A pixel of half size is marked where one passes through what it stands
    // for.
    cv::Mat inks[2];
    for (cv::Mat& marked : inks) marked = cv::Mat::zeros(small.size(), CV_8U);
    for (int y = 0; y < small.rows; y++) {
        const uchar* look = warm.ptr<uchar>(y);
        uchar* marks[2] = {inks[0].ptr<uchar>(y), inks[1].ptr<uchar>(y)};
        const int fy1 = (std::min)(bgr.rows - 1, (y + 1) * step);
        for (int x = 0; x < small.cols; x++) {
            if (!look[x]) continue;
            const int fx1 = (std::min)(bgr.cols - 1, (x + 1) * step);
            for (int fy = (std::max)(1, y * step); fy < fy1; fy++) {
                const uchar* above = ink.ptr<uchar>(fy - 1);
                const uchar* here = ink.ptr<uchar>(fy);
                const uchar* below = ink.ptr<uchar>(fy + 1);
                for (int fx = (std::max)(1, x * step); fx < fx1; fx++) {
                    if (here[fx] && above[fx + 1] && below[fx - 1]) marks[here[fx] - 1][x] = 255;
                }
            }
        }
    }
    // Each colour's marks closed, and what they close into looked over
    // together - once over the picture, not three times. The brush bridges
    // the gaps between the stripes up to twice the recorded size.
    const double H = small.rows;
    const int brush = (std::max)(5, static_cast<int>(std::lround(H * 0.009))) | 1;
    const cv::Mat square = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(brush, brush));
    cv::Mat closed = cv::Mat::zeros(small.size(), CV_8U);
    for (cv::Mat& ink : inks) {
        if (cv::countNonZero(ink) == 0) continue;
        cv::morphologyEx(ink, ink, cv::MORPH_CLOSE, square);
        closed |= ink;
    }
    static const bool trace = DEV_ENV("SCMINER_REFINERY_TRACE") != nullptr;
    cv::Mat labels, stats, middles;
    const int n = cv::connectedComponentsWithStats(closed, labels, stats, middles, 8, CV_32S);
    for (int i = 1; i < n; i++) {
        const int x = stats.at<int>(i, cv::CC_STAT_LEFT), y = stats.at<int>(i, cv::CC_STAT_TOP);
        const int w = stats.at<int>(i, cv::CC_STAT_WIDTH), h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        if (trace && w >= H * 0.05 && h >= H * 0.008) {
            std::printf("      piece %d,%d %dx%d (half) fill %.2f\n", x, y, w, h,
                        stats.at<int>(i, cv::CC_STAT_AREA) / static_cast<double>(w * h));
        }
        // A block's shape, from 0.4 to 2.4 times the size recorded (0.126 x
        // 0.0215 of the height, 5.8 times as wide as high): the marks need
        // only come near it - grain added to them above and below, and blur
        // took the end stripes of a pale red block away - the block's own
        // edges must have it
        const auto blockShaped = [&](double bw, double bh) {
            return bw >= H * 0.05 && bw <= H * 0.30 && bh >= H * 0.008 && bh <= H * 0.055 && bw >= bh * 4 &&
                   bw <= bh * 9;
        };
        if (w < H * 0.04 || w > H * 0.36 || h < H * 0.006 || h > H * 0.07) continue;
        if (w < h * 2.5 || w > h * 12) continue;
        if (stats.at<int>(i, cv::CC_STAT_AREA) < 0.4 * w * h) continue;
        const cv::Rect full = cv::Rect(static_cast<int>(std::lround(x / s)), static_cast<int>(std::lround(y / s)),
                                       static_cast<int>(std::lround(w / s)), static_cast<int>(std::lround(h / s))) &
                              cv::Rect(0, 0, bgr.cols, bgr.rows);
        if (full.area() <= 0) continue;
        const cv::Rect edges = blockEdges(bgr, full);
        // ...hatched, the stripes leaning to the right, and in a block's
        // colour (every block recorded leant 0.06 to 0.44, in every light
        // and at every size tried; hatching in a MOLE's cockpit -0.13)
        const Hatch hatch = hatchOf(bgr, edges);
        const int kind = blockKind(hatch.colour);
        if (trace) {
            std::printf("        block %d,%d %dx%d (edges %d,%d %dx%d): crossings %.1f depth %.2f lean %.2f colour "
                        "%.0f,%.0f,%.0f kind %d\n",
                        full.x, full.y, full.width, full.height, edges.x, edges.y, edges.width, edges.height,
                        hatch.crossings, hatch.depth, hatch.lean, hatch.colour[2], hatch.colour[1], hatch.colour[0], kind);
        }
        // (the edges' width with their height, or with the marks': the
        // hatching thins out at the block's top and bottom, and a block a
        // little further off measured 145x16 by its edges and 144x26 by its
        // marks - only the width goes into the fields)
        if (!blockShaped(edges.width * s, edges.height * s) && !blockShaped(edges.width * s, h)) continue;
        if (hatch.crossings < 14 || hatch.crossings > 60 || hatch.depth < 0.15 || hatch.lean < 0.02 || kind < 0) continue;
        RefineryBar bar;
        bar.theme = RefineryBar::Theme::Pyro;
        bar.band = kind == 0 ? RefineryBar::Band::Setup
                   : kind == 1 ? RefineryBar::Band::Processing
                               : RefineryBar::Band::Completed;
        bar.bandColour = cv::mean(bgr(full));
        // Where the block is, from side to side: its marks, as every field
        // was measured from them - unless they are not a block's shape, or a
        // tenth and more off its edges, as when the end stripes lose their
        // colour or the marks take in what is next to the block (the edges
        // measure a block in a small picture a little wide, where the marks
        // are right)...
        const bool marksRight = blockShaped(w, h) && stats.at<int>(i, cv::CC_STAT_AREA) >= 0.6 * w * h &&
                                std::abs(edges.width - full.width) * 10 <= edges.width;
        const cv::Rect across = marksRight ? full : edges;
        // ...and up and down, the middle of its edges, and as tall as a
        // block is for its width (31 to 181): under a warmer light the marks
        // can take in a rule over the block and sit eight pixels too high,
        // while the middle of the edges holds in any light and at any size
        const int tall = static_cast<int>(std::lround(across.width * 31.0 / 181.0));
        const cv::Rect block(across.x, edges.y + edges.height / 2 - tall / 2, across.width, tall);
        // The bar it stands for: the block sits 8 px further in than
        // Levski's bar, and is 181 wide where the bar is 446
        const double k = block.width / 181.0;
        bar.bar = cv::Rect(static_cast<int>(std::lround(block.x - 8 * k)), block.y, static_cast<int>(std::lround(446 * k)),
                           block.height);
        found.push_back(bar);
    }
}

}  // namespace

std::vector<RefineryBar> findRefineryBars(const cv::Mat& input, bool everyBar) {
    std::vector<RefineryBar> found;
    if (input.empty()) return found;
    cv::Mat raw;
    if (input.channels() == 4) {
        cv::cvtColor(input, raw, cv::COLOR_BGRA2BGR);
    } else if (input.channels() == 1) {
        return found;
    } else {
        raw = input;
    }
    // Half size where there is plenty: a bar stays 7-8 pixels tall at 1440
    const double s = raw.rows > 900 ? 0.5 : 1.0;
    cv::Mat rawSmall;
    if (s < 1.0) {
        cv::resize(raw, rawSmall, cv::Size(), s, s, cv::INTER_AREA);
    } else {
        rawSmall = raw;
    }
    // ...both in the light of the recordings
    const Light light = lightOf(rawSmall, 2);
    static const bool traceLight = DEV_ENV("SCMINER_REFINERY_LIGHT") != nullptr;
    if (traceLight) std::printf("LIGHT white %.0f gain %.2f\n", light.white, light.gain);
    const cv::Mat bgr = inRecordedLight(raw, light);
    const cv::Mat small = inRecordedLight(rawSmall, light);
    findGreyBars(small, s, everyBar, found);
    findHatchedBlocks(bgr, small, s, found);
    std::sort(found.begin(), found.end(), [](const RefineryBar& a, const RefineryBar& b) { return a.bar.x < b.bar.x; });
    return found;
}

// --------------------------------------------------------------------------
// Parsing what the reader gives back
// --------------------------------------------------------------------------

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Letters only, upper case: "IRON (ORE)" -> "IRONORE"
std::string letters(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalpha(static_cast<unsigned char>(c))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); j++) {
            cur[j] = (std::min)({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// The digits a reader of words gives for numerals it took for letters
char asDigit(char c) {
    switch (c) {
        case 'O': case 'o': case 'D': case 'Q': return '0';
        case 'I': case 'l': case '|': case 'i': case 'L': return '1';
        case 'Z': case 'z': return '2';
        case 'S': case 's': return '5';
        case 'B': return '8';
        case 'G': return '6';
        default: return c;
    }
}

}  // namespace

int parseRefineryCount(const std::string& input) {
    // What the reader adds at the edges - a column rule taken for a full
    // stop (".234"), a colon, a stray mark beyond the ASCII - is no part of
    // the number; inside it, anything but a figure makes it no number
    size_t from = 0, to = input.size();
    auto edge = [](char c) {
        return !std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80;
    };
    while (from < to && edge(input[from])) from++;
    while (to > from && edge(input[to - 1])) to--;
    std::string digits;
    for (size_t i = from; i < to; i++) {
        const char c = input[i];
        if (c == ' ' || c == ',') continue;
        const char d = asDigit(c);
        if (!std::isdigit(static_cast<unsigned char>(d))) return -1;   // "--" stands for nothing
        digits += d;
    }
    if (digits.empty() || digits.size() > 7) return -1;
    return std::stoi(digits);
}

double parseRefineryAmount(const std::string& text) {
    std::string t;
    for (char c : text) {
        if (c == ',' || c == ' ') continue;
        const char d = asDigit(c);
        if (std::isdigit(static_cast<unsigned char>(d)) || c == '.') t += std::isdigit(static_cast<unsigned char>(d)) ? d : c;
        else if (!t.empty()) break;   // "194.00aUEC": the unit ends the number
    }
    if (t.empty() || t == ".") return -1;
    if (std::count(t.begin(), t.end(), '.') > 1) return -1;
    try {
        return std::stod(t);
    } catch (...) {
        return -1;
    }
}

int parseRefineryTime(const std::string& text) {
    // "7m 22s", "1h 2m 3s", "45s", "2d 3h": numbers each followed by its unit
    int total = 0, value = -1, parts = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == ' ') continue;
        const char d = (c == 'm' || c == 's' || c == 'h' || c == 'd') ? c : asDigit(c);
        if (std::isdigit(static_cast<unsigned char>(d))) {
            value = (value < 0 ? 0 : value * 10) + (d - '0');
            if (value > 100000) return -1;
            continue;
        }
        if (value < 0) return -1;
        switch (d) {
            case 'd': total += value * 86400; break;
            case 'h': total += value * 3600; break;
            case 'm': total += value * 60; break;
            case 's': total += value; break;
            default: return -1;
        }
        value = -1;
        parts++;
    }
    if (value >= 0 || parts == 0) return -1;
    return total;
}

std::string refiningMethodIn(const std::string& text) {
    const std::string t = letters(text);
    if (t.size() < 4) return "";
    int best = 1 << 30;
    std::string name;
    for (const RefiningMethod& m : REFINING_METHODS) {
        const std::string want = letters(m.name);
        // A long name squeezed into the reader's line may come back cut short:
        // compare with as much of the name as was read
        const std::string head = want.substr(0, (std::min)(want.size(), t.size()));
        const int d = editDistance(t, t.size() < want.size() ? head : want);
        if (d < best) {
            best = d;
            name = m.name;
        }
    }
    return best <= (std::max)(1, static_cast<int>(t.size()) / 4) ? name : "";
}

std::string refineryMaterialIn(const std::string& text, bool* inert) {
    if (inert) *inert = false;
    const std::string t = letters(text);
    if (t.size() < 3) return "";
    if (editDistance(t, "INERTMATERIALS") <= 3 || editDistance(t, "NERTMATERIALS") <= 2 ||
        t.rfind("INERT", 0) == 0) {
        if (inert) *inert = true;
        return "";
    }
    int best = 1 << 30;
    std::string name;
    for (const char* material : REFINERY_MATERIALS) {
        const std::string full = letters(material);
        // "Iron (Ore)" as SETUP writes it, "IRON" as PROCESSING does
        std::string bare = material;
        const size_t paren = bare.find(" (");
        if (paren != std::string::npos) bare = bare.substr(0, paren);
        for (const std::string& want : {full, letters(bare)}) {
            const int d = editDistance(t, want);
            if (d < best) {
                best = d;
                name = material;
            }
        }
    }
    return best <= (std::max)(1, static_cast<int>(t.size()) / 5) ? name : "";
}

bool refineryQualityFits(const std::string& material, int quality) {
    std::string ore = material;
    const size_t paren = ore.find(" (");
    if (paren != std::string::npos) ore = ore.substr(0, paren);
    if (ore == "Raw Ice") ore = "Ice";
    else if (ore == "Raw Silicon") ore = "Silicon";
    else if (ore == "Aluminum") ore = "Aluminium";
    for (const OreBands& b : ORE_BANDS) {
        if (ore != b.ore) continue;
        for (uint16_t v : b.bands) {
            if (v == quality) return true;
        }
        return false;
    }
    return true;
}

bool RefineryPanel::whole() const {
    if (rows.empty()) return false;
    for (const RefineryRow& row : rows) {
        if (!row.inert && row.material.empty()) return false;
        if (!row.inert && row.quality < 0) return false;
    }
    if (kind == RefineryBar::Band::Setup) return !method.empty() && cost >= 0 && seconds >= 0;
    if (kind == RefineryBar::Band::Processing) return order >= 0 && seconds >= 0;
    if (kind == RefineryBar::Band::Completed) return true;
    return false;
}

// ---- the panels' fields ------------------------------------------------
// Where everything stands under a work order's bar, in the pixels of the
// Levski recording (5120x1440, bar 446 wide), from the bar's top left
// corner; a bar of another width scales them all alike.
namespace {

constexpr double REF_BAR_W = 446.0;

struct Box {
    double x0, y0, x1, y1;
};

// SETUP
constexpr Box SETUP_ORDER{352, 26, 440, 52};        // the "0" of WORK ORDER 0
constexpr Box SETUP_MANIFEST{40, 134, 200, 164};    // IN MANIFEST 720
constexpr Box SETUP_TO_REFINE{258, 134, 420, 164};  // TO REFINE 484
constexpr Box SETUP_METHOD{4, 232, 300, 266};       // PYROMETRIC CHROMALYSIS
constexpr Box SETUP_ROWS{28, 340, 210, 686};        // the materials' names, where rows are looked for
constexpr double SETUP_NAME_X0 = 28, SETUP_NAME_X1 = 200;
constexpr double SETUP_QUALITY_X0 = 192, SETUP_QUALITY_X1 = 252;
constexpr double SETUP_QTY_X0 = 256, SETUP_QTY_X1 = 314;
constexpr double SETUP_YIELD_X0 = 318, SETUP_YIELD_X1 = 378;
constexpr Box SETUP_COST{170, 686, 398, 728};       // TOTAL COST 194.00 (aUEC beyond)
constexpr Box SETUP_TIME{190, 758, 446, 806};       // PROCESSING TIME 7m 22s
// ...where Pyro's dress puts them otherwise: the number under the block,
// beside WORK ORDER, and the bill and the time a little higher
constexpr Box PYRO_ORDER{104, 36, 176, 62};
// The panel's title: in Levski's band under the bar, at Pyro large and to the
// right of the block, on the block's own line
constexpr Box LEVSKI_TITLE{8, 24, 230, 54};
constexpr Box PYRO_TITLE{200, -6, 450, 38};
constexpr Box PYRO_COST{170, 664, 398, 704};
constexpr Box PYRO_TIME{190, 736, 446, 778};

// PROCESSING
constexpr Box PROC_ORDER{352, 26, 440, 52};
constexpr Box PROC_ROWS{38, 126, 210, 520};
constexpr double PROC_NAME_X0 = 38, PROC_NAME_X1 = 200;
constexpr double PROC_QUALITY_X0 = 214, PROC_QUALITY_X1 = 272;
constexpr double PROC_YIELD_X0 = 274, PROC_YIELD_X1 = 330;
constexpr double PROC_TODO_X0 = 332, PROC_TODO_X1 = 388;
constexpr double PROC_DONE_X0 = 390, PROC_DONE_X1 = 446;
constexpr Box PROC_YIELD{190, 526, 386, 572};       // YIELD 7.77 (cSCU beyond)

// COMPLETED: the rows have a quality and a yield only, further right
// (measured at Checkmate; Levski's is taken to sit alike)
constexpr double DONE_QUALITY_X0 = 262, DONE_QUALITY_X1 = 318;
constexpr double DONE_YIELD_X0 = 346, DONE_YIELD_X1 = 404;
constexpr Box PROC_TIME{190, 628, 446, 680};        // TIME REMAINING 7m 7s

// The station's name at the top left of the screen, over the first panel's
// bar to its left, clear of the header's rules and of REFINEMENT CENTER
constexpr Box STATION_NAME{-372, -80, 262, -34};


cv::Rect place(const RefineryBar& bar, const Box& b, const cv::Size& picture) {
    const double k = bar.bar.width / REF_BAR_W;
    const cv::Rect r(static_cast<int>(std::lround(bar.bar.x + b.x0 * k)), static_cast<int>(std::lround(bar.bar.y + b.y0 * k)),
                     static_cast<int>(std::lround((b.x1 - b.x0) * k)), static_cast<int>(std::lround((b.y1 - b.y0) * k)));
    return r & cv::Rect(0, 0, picture.width, picture.height);
}

std::string readText(const cv::Mat& bgr, const cv::Rect& where, TextLineReader& reader, const char* what = "") {
    if (where.width < 4 || where.height < 4) return "";
    const TextLineReader::Line line = reader.read(bgr(where));
    static const bool trace = DEV_ENV("SCMINER_REFINERY_TRACE") != nullptr;
    if (trace) {
        std::printf("      %s at %d,%d %dx%d: '%s' %.2f\n", what, where.x, where.y, where.width, where.height,
                    line.text.c_str(), line.mean);
    }
    return line.text;
}

// Where lettering stands out of a patch: the patch's middle is its ground,
// its brightest few pixels its lettering - three hundredths of a percent of
// it, between 6 and 30: one short word in a column of rows is a few tenths
// of a percent of it, and a lone "1" has fewer than 30 - and ink is what
// stands more than nine twentieths of the way up from the one to the other.
// A fixed level of 120 loses every material's line once the picture is
// blurred a little (the white of thin lettering falls to 150, and between
// the lines it rises); 256 for a patch with no lettering in it.
int inkLevel(const cv::Mat& grey) {
    int hist[256] = {0};
    for (int y = 0; y < grey.rows; y++) {
        const uchar* row = grey.ptr<uchar>(y);
        for (int x = 0; x < grey.cols; x++) hist[row[x]]++;
    }
    const long total = static_cast<long>(grey.total());
    int ground = 0;
    long seen = 0;
    for (int v = 0; v < 256; v++) {
        seen += hist[v];
        if (seen * 2 <= total) ground = v + 1;
    }
    const long few = std::clamp(total * 3 / 1000, 6L, 30L);
    int letters = 255;
    for (long brighter = 0; letters > 0; letters--) {
        brighter += hist[letters];
        if (brighter >= few) break;
    }
    if (letters - ground < 40) return 256;
    return ground + (letters - ground) * 9 / 20;
}

// The text lines of a column: runs of rows with light lettering in them
std::vector<std::pair<int, int>> textRows(const cv::Mat& bgr, const cv::Rect& column, double k) {
    std::vector<std::pair<int, int>> rows;
    if (column.width < 4 || column.height < 4) return rows;
    cv::Mat grey;
    cv::cvtColor(bgr(column), grey, cv::COLOR_BGR2GRAY);
    const int level = inkLevel(grey);
    const cv::Mat ink = grey >= level;
    static const bool trace = DEV_ENV("SCMINER_REFINERY_TRACE") != nullptr;
    if (trace) {
        std::printf("      rows in %d,%d %dx%d: ink from %d\n", column.x, column.y, column.width, column.height, level);
    }
    const int minRun = (std::max)(3, static_cast<int>(std::lround(6 * k)));
    const int minInk = (std::max)(2, static_cast<int>(std::lround(column.width * 0.02)));
    int start = -1;
    for (int y = 0; y <= ink.rows; y++) {
        const bool lit = y < ink.rows && cv::countNonZero(ink.row(y)) >= minInk;
        if (lit && start < 0) start = y;
        if (!lit && start >= 0) {
            if (y - start >= minRun) rows.push_back({column.y + start, column.y + y});
            start = -1;
        }
    }
    return rows;
}

cv::Rect cell(const RefineryBar& bar, double x0, double x1, int top, int bottom, double k, const cv::Size& picture) {
    const int pad = static_cast<int>(std::lround(5 * k));
    const cv::Rect r(static_cast<int>(std::lround(bar.bar.x + x0 * k)), top - pad,
                     static_cast<int>(std::lround((x1 - x0) * k)), bottom - top + 2 * pad);
    return r & cv::Rect(0, 0, picture.width, picture.height);
}

// A figure on a line of its own, looked for round where the layout puts it.
// A Pyro panel is measured by its block alone, and Pyro Gateway's terminal
// comes out 4 % wider in the picture than Checkmate's but only 2 % taller:
// its lower lines stand up to 20 px above their boxes. So the lines of
// lettering in a window round the box are tried, the nearest to it first,
// and the first that reads as the figure is it.
cv::Rect inkBox(const cv::Mat& bgr, const cv::Rect& where);
std::string readText(const cv::Mat& bgr, const cv::Rect& where, TextLineReader& reader, const char* what);
double figureNear(const cv::Mat& bgr, const RefineryBar& bar, const Box& expected, const cv::Size& size,
                  TextLineReader& reader, const char* what, double (*parse)(const std::string&)) {
    const double k = bar.bar.width / REF_BAR_W;
    const double slack = 34;
    const cv::Rect area = place(bar, Box{expected.x0, expected.y0 - slack, expected.x1, expected.y1 + slack}, size);
    const double centre = bar.bar.y + (expected.y0 + expected.y1) / 2 * k;
    std::vector<std::pair<int, int>> rows = textRows(bgr, area, k);
    std::sort(rows.begin(), rows.end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        return std::abs((a.first + a.second) / 2.0 - centre) < std::abs((b.first + b.second) / 2.0 - centre);
    });
    int tries = 0;
    for (const auto& [top, bottom] : rows) {
        if (bottom - top > 50 * k) continue;   // a box or a bar, not a line of figures
        if (++tries > 3) break;
        const int pad = static_cast<int>(std::lround(4 * k));
        const cv::Rect line =
            cv::Rect(area.x, top - pad, area.width, bottom - top + 2 * pad) & cv::Rect(0, 0, size.width, size.height);
        const double value = parse(readText(bgr, inkBox(bgr, line), reader, what));
        if (value >= 0) return value;
    }
    return -1;
}

// The bill and the yield so far have their cents: "02" off a section's rule
// is neither
double amountWithCents(const std::string& text) {
    return text.find('.') != std::string::npos ? parseRefineryAmount(text) : -1;
}

double timeFigure(const std::string& text) {
    return parseRefineryTime(text);
}

// A figure's cell narrowed to its lettering, with a little room round it:
// in the full width of DONE a lone "1" comes back as noise, and the
// column's rule beside "234" as a full stop in front of it
cv::Rect inkBox(const cv::Mat& bgr, const cv::Rect& where) {
    if (where.width < 4 || where.height < 4) return cv::Rect();
    cv::Mat grey;
    cv::cvtColor(bgr(where), grey, cv::COLOR_BGR2GRAY);
    std::vector<cv::Point> lit;
    cv::findNonZero(grey >= inkLevel(grey), lit);
    if (lit.size() < 3) return cv::Rect();
    cv::Rect box = cv::boundingRect(lit);
    const int mx = (std::max)(3, box.height / 2), my = (std::max)(2, box.height / 4);
    box.x += where.x - mx;
    box.y += where.y - my;
    box.width += 2 * mx;
    box.height += 2 * my;
    return box & cv::Rect(0, 0, bgr.cols, bgr.rows);
}


}  // namespace

// "WORK ORDER DELIVERED", the notice after COLLECT, in a box of its own over
// the list: green lettering (R 174, G 250, B 120 at Checkmate, a little
// darker at Pyro Gateway), greener than the blocks' yellow-green.
// The terminal floats - the player walks about in front of it with WASD - so
// the line is looked for by its colour anywhere: at half size, the letters
// run together into a line about a quarter of the picture's height long and
// a fiftieth high, and the two largest such lines are read.
bool findDeliveredNotice(const cv::Mat& input, TextLineReader& reader) {
    if (input.empty() || !reader.ready()) return false;
    cv::Mat raw;
    if (input.channels() == 4) {
        cv::cvtColor(input, raw, cv::COLOR_BGRA2BGR);
    } else {
        raw = input;
    }
    const double s = raw.rows > 900 ? 0.5 : 1.0;
    cv::Mat rawSmall;
    if (s < 1.0) {
        cv::resize(raw, rawSmall, cv::Size(), s, s, cv::INTER_AREA);
    } else {
        rawSmall = raw;
    }
    const Light light = lightOf(rawSmall, 2);
    const cv::Mat bgr = inRecordedLight(raw, light);
    const cv::Mat small = inRecordedLight(rawSmall, light);
    std::vector<cv::Mat> ch;
    cv::split(small, ch);
    cv::Mat mask = (ch[1] > 120) & (ch[1] > ch[2] * 1.2) & (ch[1] > ch[0] * 1.7);
    const double H = small.rows;
    // The letters are run together over the gaps between them and between
    // the words, which go with the letters' height - the middle height of
    // what is green and letter-shaped - and not with the picture's
    int join = (std::max)(3, static_cast<int>(std::lround(H * 0.012)));
    {
        cv::Mat labels, stats, centroids;
        const int n = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8, CV_32S);
        std::vector<int> heights;
        for (int i = 1; i < n; i++) {
            const int w = stats.at<int>(i, cv::CC_STAT_WIDTH), h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
            if (h >= 3 && w <= h * 2) heights.push_back(h);
        }
        if (heights.size() >= 5) {
            std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
            join = std::clamp(static_cast<int>(std::lround(heights[heights.size() / 2] * 1.2)), 3, 60);
        }
    }
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(join, 3)));
    cv::Mat labels, stats, centroids;
    const int n = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8, CV_32S);
    std::vector<cv::Rect> lines;
    for (int i = 1; i < n; i++) {
        const int w = stats.at<int>(i, cv::CC_STAT_WIDTH), h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        // a line a quarter of the picture's height long and a fiftieth high
        // where it was recorded, 0.4 to 2.4 times that here
        if (w < H * 0.09 || w > H * 0.8 || h < H * 0.005 || h > H * 0.08 || w < h * 7) continue;
        lines.push_back(cv::Rect(stats.at<int>(i, cv::CC_STAT_LEFT), stats.at<int>(i, cv::CC_STAT_TOP), w, h));
    }
    std::sort(lines.begin(), lines.end(), [](const cv::Rect& a, const cv::Rect& b) { return a.area() > b.area(); });
    static const bool trace = DEV_ENV("SCMINER_REFINERY_TRACE") != nullptr;
    for (size_t i = 0; i < lines.size() && i < 2; i++) {
        const cv::Rect& r = lines[i];
        const int pad = static_cast<int>(std::lround(r.height * 0.5));
        const cv::Rect full = cv::Rect(static_cast<int>(std::lround(r.x / s)) - pad, static_cast<int>(std::lround(r.y / s)) - pad,
                                       static_cast<int>(std::lround(r.width / s)) + 2 * pad,
                                       static_cast<int>(std::lround(r.height / s)) + 2 * pad) &
                              cv::Rect(0, 0, bgr.cols, bgr.rows);
        const std::string said = letters(readText(bgr, full, reader, "notice"));
        if (trace) std::printf("      notice? %d,%d %dx%d '%s'\n", full.x, full.y, full.width, full.height, said.c_str());
        if (said.find("DELIVERED") != std::string::npos || editDistance(said, "WORKORDERDELIVERED") <= 3) return true;
    }
    return false;
}

RefineryScreen readRefineryScreen(const cv::Mat& input, const std::vector<RefineryBar>& bars, TextLineReader& reader,
                                  bool notice) {
    RefineryScreen screen;
    if (input.empty() || !reader.ready()) return screen;
    cv::Mat raw;
    if (input.channels() == 4) {
        cv::cvtColor(input, raw, cv::COLOR_BGRA2BGR);
    } else {
        raw = input;
    }
    const auto started = std::chrono::steady_clock::now();
    // in the light of the recordings, as the bars were found
    const cv::Mat bgr = inRecordedLight(raw, lightOf(raw, raw.rows > 900 ? 4 : 2));
    const cv::Size size = bgr.size();
    const RefineryBar* first = nullptr;   // the leftmost bar a panel was read under
    for (const RefineryBar& bar : bars) {
        RefineryPanel panel;
        panel.bar = bar.bar;
        const double k = bar.bar.width / REF_BAR_W;
        const bool pyro = bar.theme == RefineryBar::Theme::Pyro;
        // The panel's title first, one reading: SETUP or PROCESSING, in the
        // band under Levski's bar or large to the right of Pyro's block. What
        // has neither is no work order - an orange light in a MOLE's cockpit
        // can take the shape of Pyro's block (one picture in twenty) - and the
        // title, not the colour, says which kind it is.
        const std::string title = letters(readText(bgr, place(bar, pyro ? PYRO_TITLE : LEVSKI_TITLE, size), reader, "title"));
        if (title.find("SETUP") != std::string::npos || editDistance(title, "SETUP") <= 1) {
            panel.kind = RefineryBar::Band::Setup;
        } else if (title.find("PROCESSING") != std::string::npos || editDistance(title, "PROCESSING") <= 2) {
            panel.kind = RefineryBar::Band::Processing;
        } else if (title.find("COMPLETE") != std::string::npos || editDistance(title, "COMPLETED") <= 2) {
            panel.kind = RefineryBar::Band::Completed;
        } else {
            continue;
        }
        const bool setup = panel.kind == RefineryBar::Band::Setup;
        const bool done = panel.kind == RefineryBar::Band::Completed;
        panel.order = parseRefineryCount(
            readText(bgr, inkBox(bgr, place(bar, pyro ? PYRO_ORDER : setup ? SETUP_ORDER : PROC_ORDER, size)), reader, "order"));
        if (setup) {
            panel.manifest = parseRefineryCount(readText(bgr, place(bar, SETUP_MANIFEST, size), reader, "manifest"));
            panel.toRefine = parseRefineryCount(readText(bgr, place(bar, SETUP_TO_REFINE, size), reader, "to refine"));
            panel.methodText = readText(bgr, place(bar, SETUP_METHOD, size), reader, "method");
            panel.method = refiningMethodIn(panel.methodText);
            panel.cost = figureNear(bgr, bar, pyro ? PYRO_COST : SETUP_COST, size, reader, "cost", amountWithCents);
            panel.seconds =
                static_cast<int>(figureNear(bgr, bar, pyro ? PYRO_TIME : SETUP_TIME, size, reader, "time", timeFigure));
        } else if (done) {
            // what it came to, and no time any more
            panel.yieldSoFar = parseRefineryAmount(readText(bgr, place(bar, PROC_YIELD, size), reader, "yield"));
        } else {
            panel.yieldSoFar = figureNear(bgr, bar, PROC_YIELD, size, reader, "yield so far", amountWithCents);
            panel.seconds = static_cast<int>(figureNear(bgr, bar, PROC_TIME, size, reader, "remaining", timeFigure));
        }
        const cv::Rect rowsAt = place(bar, setup ? SETUP_ROWS : PROC_ROWS, size);
        for (const auto& [top, bottom] : textRows(bgr, rowsAt, k)) {
            RefineryRow row;
            auto at = [&](double x0, double x1) { return cell(bar, x0, x1, top, bottom, k, size); };
            auto figure = [&](double x0, double x1, const char* what) {
                return parseRefineryCount(readText(bgr, inkBox(bgr, at(x0, x1)), reader, what));
            };
            row.text = readText(bgr, setup ? at(SETUP_NAME_X0, SETUP_NAME_X1) : at(PROC_NAME_X0, PROC_NAME_X1), reader, "name");
            row.material = refineryMaterialIn(row.text, &row.inert);
            // A line that names no material is none of the table's: the
            // warning laid over the list as an order is confirmed, the
            // "WORK ORDER ACCEPTED" box over a panel beside it
            if (row.material.empty() && !row.inert) continue;
            row.quality = setup  ? figure(SETUP_QUALITY_X0, SETUP_QUALITY_X1, "quality")
                          : done ? figure(DONE_QUALITY_X0, DONE_QUALITY_X1, "quality")
                                 : figure(PROC_QUALITY_X0, PROC_QUALITY_X1, "quality");
            // ...and a quality its ore does not come in is a misreading - an
            // 874 taken for 674 would have stood as a part of its own
            if (!row.inert && row.quality >= 0 && !refineryQualityFits(row.material, row.quality)) continue;
            if (setup) {
                row.quantity = figure(SETUP_QTY_X0, SETUP_QTY_X1, "qty");
                row.yield = figure(SETUP_YIELD_X0, SETUP_YIELD_X1, "yield");
            } else if (done) {
                row.yield = figure(DONE_YIELD_X0, DONE_YIELD_X1, "yield");
            } else {
                row.yield = figure(PROC_YIELD_X0, PROC_YIELD_X1, "yield");
                row.toDo = figure(PROC_TODO_X0, PROC_TODO_X1, "todo");
                row.done = figure(PROC_DONE_X0, PROC_DONE_X1, "done");
            }
            panel.rows.push_back(row);
        }
        screen.panels.push_back(panel);
        if (!first || bar.bar.x < first->bar.x) first = &bar;
    }
    // The station's name, measured from the first panel: a bar no title was
    // read under may be anything
    if (first) {
        screen.stationText = readText(bgr, inkBox(bgr, place(*first, STATION_NAME, size)), reader, "station");
    }
    if (notice) screen.delivered = findDeliveredNotice(raw, reader);
    screen.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return screen;
}

}  // namespace SCMiner
