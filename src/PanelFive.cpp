#include "PanelFive.h"

#include "DevSwitch.h"
#include "MiningHardware.h"
#include "PanelFiveShapes.h"
#include "RockContents.h"
#include "ScanPanel.h"
#include "TextLineReader.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace SCMiner {

namespace {

// Places at size 1, from the labels' left edge and the MASS label's top.
// Size 1 is the panel at 1440 lines as the Prospector showed it on Nyx I at
// 13:19:46 on 15 November 2026, its rows 27 pixels apart.
constexpr double ROW_PITCH = 27.0;           // MASS, RESISTANCE, INSTABILITY
constexpr double ORE_TOP = -27.0;            // the ore's name
constexpr double CAP = 12.0;                 // a label's and a figure's height
constexpr double FIGURES_END = 233.5;        // the figures end here, right-aligned
constexpr double DELTA_END = 173.5;          // ...and the laser's modifiers here
constexpr double VOLUME_TOP = 123.0;         // COMPOSITION's line: the volume and SCU
constexpr double LIST_TOP = 205.0;           // the list's first line
constexpr double LIST_PITCH = 21.3;
constexpr double QUALITY_END = 229.5;        // a line's quality ends here
constexpr double NAME_FROM = 70.0;           // ...and its name stands between these
constexpr double NAME_TO = 192.0;
constexpr double SHARE_ENDS_FROM = 40.0;     // a share's per cent sign ends between these:
constexpr double SHARE_ENDS_TO = 66.0;       // "8.57%" at 46, "100.00%" at 61 to 62 (17 November 2026)
constexpr int COLUMN_W = 100;                // the label column's shape, from 3 before
constexpr int COLUMN_H = 72;                 // the labels and 3 above MASS
constexpr double COLUMN_INSET = 3.0;
constexpr int GLYPH_H = 16;                  // a glyph's cell: 2 above the figures' top,
constexpr int GLYPH_ABOVE = 2;               // their 12, and 2 under them
constexpr int CELL_W = 16;                   // ...as harvested, ending where the glyph ends
constexpr double WORD_GAP = 4.5;             // wider than this between two pieces: two words
constexpr int LINE_BORDER = 6;               // nothing round a line's ink

// Where a panel's parts stand, in units from the labels' left edge and the
// MASS label's top. The list's places are from its shares' left edge.
struct Layout {
    double figuresEnd, deltaEnd;     // the figures end here, and the laser's modifiers
    double volumeTop;                // COMPOSITION's line: the volume and SCU
    double listLeft;                 // the shares' left edge
    double volumeFrom, scuEnd;       // the volume's line from here; SCU ends here
    double listTop, listPitch;
    double listEnd;                  // a list line's lettering ends by here
    double qualityEnd, nameFrom, nameTo;
    double shareEndsFrom, shareEndsTo;   // a share's per cent sign ends between these, from the shares' edge
};
// The panel beside the reticle: Prospector, Golem, the MOLE's pilot seat
constexpr Layout BESIDE{FIGURES_END, DELTA_END, VOLUME_TOP, 0.0, 128.0, FIGURES_END, LIST_TOP, LIST_PITCH,
                        FIGURES_END, QUALITY_END, NAME_FROM, NAME_TO, SHARE_ENDS_FROM, SHARE_ENDS_TO};
// The MOLE turret's: one row under the reticle - its laser table, the
// panel's labels with their figures, and to their right COMPOSITION and the
// list. Measured on the app's own snapshots of 17 November 2026 08:55:14 and
// 08:55:17 (5120x1440; the column found at size 1.096, labels from x 2388,
// MASS's top at y 1162 and 1175): the figures end at 236.3 on all three
// rows, a marker after the mass from 246.4, the modifier -35% at 176.1; the
// ore's name above MASS as beside the reticle; COMPOSITION's line from
// -67.5; the shares from x 323.9 to 326.6, the list's lines from 16.4 and
// 21.0 apart. From the shares' left edge (322.5) the names stand from 79 to
// 201 (INERT MATERIALS), the qualities end at 235 to 237 and SCU at 242 to
// 243. Its lettering is 4 to 8 per cent wider for its labels' size than
// beside the reticle (14.57% ends at 60.7 there, 53.4 beside it), so its
// shares end from 44 to 73; the lines measure their glyphs by their own
// height.
constexpr Layout TURRET{236.3, 176.1, -67.0, 322.5, 128.0, 241.0, 16.4, 21.0, 245.0, 233.0, 74.0, 205.0, 44.0, 73.0};

// The turret's row stands under the reticle: its labels at cy + 0.31h on
// both snapshots; the panel beside it has them from cy - 0.14h (Golem) to
// cy (Prospector, MOLE pilot seat) on the recording and the app's pictures
bool turretRow(const cv::Point& labels, const cv::Size& frame) {
    return labels.y > frame.height / 2.0 + 0.15 * frame.height;
}
const Layout& layoutOf(const cv::Point& labels, const cv::Size& frame) {
    return turretRow(labels, frame) ? TURRET : BESIDE;
}

cv::Mat bgrOf(const cv::Mat& frame) {
    if (frame.channels() == 4) {
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        return bgr;
    }
    return frame;
}

// What stands brighter than the ground around it, in the brightest channel:
// the lettering in any of its colours, white, yellow, red or green. The
// ground is an opening over a square wider than a numeral, so a numeral's
// body is kept whole, and a filled bar goes with the ground.
cv::Mat inkOf(const cv::Mat& bgr, double s) {
    std::vector<cv::Mat> planes;
    cv::split(bgr, planes);
    const cv::Mat high = cv::max(cv::max(planes[0], planes[1]), planes[2]);
    const int k = (std::max)(9, (int)std::lround(15 * s) | 1);
    cv::Mat ground, ink, out;
    cv::morphologyEx(high, ground, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k, k)));
    cv::subtract(high, ground, ink);
    ink.convertTo(out, CV_32F);
    return out;
}

const FiveShape* shapeOf(char glyph) {
    for (int i = 0; i < FIVE_SHAPE_COUNT; i++) {
        if (FIVE_SHAPES[i].glyph == glyph && FIVE_SHAPES[i].width > 0) return &FIVE_SHAPES[i];
    }
    return nullptr;
}

// The label column - MASS:, RESISTANCE:, INSTABILITY: - as a picture at size
// 1: the generated one, or for the bench one cut off a frame
// (SCMINER_FIVE_SEED=<frame>|<labels x>|<MASS top>)
const cv::Mat& columnShape() {
    static const cv::Mat shape = [] {
        if (const char* seed = DEV_ENV("SCMINER_FIVE_SEED")) {
            std::string spec(seed);
            const size_t a = spec.find('|'), b = spec.rfind('|');
            if (a != std::string::npos && b != a) {
                const cv::Mat frame = cv::imread(spec.substr(0, a), cv::IMREAD_COLOR);
                const int x = std::atoi(spec.substr(a + 1, b - a - 1).c_str());
                const int y = std::atoi(spec.substr(b + 1).c_str());
                const cv::Rect cut(x - (int)COLUMN_INSET, y - (int)COLUMN_INSET, COLUMN_W, COLUMN_H);
                const cv::Rect around = cut + cv::Point(-20, -20) + cv::Size(40, 40);
                if (!frame.empty() && (around & cv::Rect(0, 0, frame.cols, frame.rows)) == around) {
                    return cv::Mat(inkOf(frame(around), 1.0)(cv::Rect(20, 20, COLUMN_W, COLUMN_H))).clone();
                }
            }
            std::cerr << "[five] seed not usable: " << spec << std::endl;
        }
        if (const FiveShape* s = shapeOf('L')) {
            return cv::Mat(s->height, s->width, CV_32F, const_cast<float*>(s->grey)).clone();
        }
        return cv::Mat();
    }();
    return shape;
}

cv::Mat sized(const cv::Mat& shape, double s) {
    if (std::abs(s - 1.0) < 0.005) return shape;
    cv::Mat out;
    cv::resize(shape, out,
               cv::Size((std::max)(3, (int)std::lround(shape.cols * s)), (std::max)(3, (int)std::lround(shape.rows * s))),
               0, 0, s < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    return out;
}

struct Column {
    cv::Point labels{-1, -1};   // the labels' left edge and the MASS label's top
    double scale = 0;
    double score = -1;
};

// Where the HUD stands: about the middle of the screen and to its right, sized
// by the screen's height (as ScanPanel's band, with room for 5.0's panel)
cv::Rect bandOf(const cv::Size& frame) {
    const double h = frame.height, cx = frame.width / 2.0, cy = frame.height / 2.0;
    const cv::Rect band((int)std::lround(cx - 0.05 * h), (int)std::lround(cy - 0.45 * h), (int)std::lround(1.05 * h),
                        (int)std::lround(0.85 * h));
    return band & cv::Rect(0, 0, frame.width, frame.height);
}

// ...and where the MOLE turret's row has its labels, under the reticle and
// left of the band: from x 2387 (cx - 0.12h) and y 1164-1176 (cy + 0.31h) on
// the snapshots of 17 November 2026, with room round them
cv::Rect turretBandOf(const cv::Size& frame) {
    const double h = frame.height, cx = frame.width / 2.0, cy = frame.height / 2.0;
    const cv::Rect part((int)std::lround(cx - 0.22 * h), (int)std::lround(cy + 0.17 * h), (int)std::lround(0.30 * h),
                        (int)std::lround(0.30 * h));
    return part & cv::Rect(0, 0, frame.width, frame.height);
}

// The label column over a part of the frame's ink, at the sizes given:
// coarse at half size, then about the best place at full size
Column findColumn(const cv::Mat& ink, const cv::Rect& part, const std::vector<double>& scales) {
    Column best;
    const cv::Mat& shape = columnShape();
    if (shape.empty() || part.width < 60 || part.height < 60) return best;
    constexpr double COARSE = 0.5;
    cv::Mat half;
    cv::resize(ink(part), half, cv::Size(), COARSE, COARSE, cv::INTER_AREA);
    double coarseScale = 0;
    cv::Point coarseAt;
    double coarseScore = -1;
    for (double s : scales) {
        const cv::Mat templ = sized(shape, s * COARSE);
        if (templ.cols >= half.cols || templ.rows >= half.rows) continue;
        cv::Mat result;
        cv::matchTemplate(half, templ, result, cv::TM_CCOEFF_NORMED);
        double score = 0;
        cv::Point at;
        cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
        if (score > coarseScore) {
            coarseScore = score;
            coarseScale = s;
            coarseAt = cv::Point((int)std::lround(at.x / COARSE), (int)std::lround(at.y / COARSE)) + part.tl();
        }
    }
    if (coarseScore < 0) return best;
    const int slack = 5;
    for (double f : {0.97, 0.985, 1.0, 1.015, 1.03}) {
        const double s = coarseScale * f;
        const cv::Mat templ = sized(shape, s);
        const cv::Rect window =
            cv::Rect(coarseAt.x - slack, coarseAt.y - slack, templ.cols + 2 * slack, templ.rows + 2 * slack) &
            cv::Rect(0, 0, ink.cols, ink.rows);
        if (window.width <= templ.cols || window.height <= templ.rows) continue;
        cv::Mat result;
        cv::matchTemplate(ink(window), templ, result, cv::TM_CCOEFF_NORMED);
        double score = 0;
        cv::Point at;
        cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
        if (score > best.score) {
            best.score = score;
            best.scale = s;
            best.labels = window.tl() + at + cv::Point((int)std::lround(COLUMN_INSET * s), (int)std::lround(COLUMN_INSET * s));
        }
    }
    return best;
}

// One line of the panel brought to size 1: its ink, the lettering's top and
// bottom, and its pieces of ink in words, left to right. A piece is what is
// lit and joined; pieces over one another are one (a per cent sign is
// three). The words only say where the lettering is - each figure is then
// read glyph by glyph off the line's ink.
struct Line {
    cv::Mat ink;                     // CV_32F, size 1
    cv::Point2d origin;              // where its (0, 0) stands in the frame
    double s = 1;
    int top = -1, bottom = -1;
    double most = 0;
    std::vector<std::vector<cv::Rect>> words;
    cv::Rect inFrame(const cv::Rect& r) const {
        return cv::Rect((int)std::lround(origin.x + r.x * s), (int)std::lround(origin.y + r.y * s),
                        (std::max)(1, (int)std::lround(r.width * s)), (std::max)(1, (int)std::lround(r.height * s)));
    }
    int right(size_t word) const { return words[word].back().x + words[word].back().width; }
    int left(size_t word) const { return words[word].front().x; }
    double unit(double frameX) const { return (frameX - origin.x) / s; }
};

Line lineAt(const cv::Mat& ink, const cv::Rect& wanted, double s, double floor) {
    Line line;
    line.s = s;
    const cv::Rect area = wanted & cv::Rect(0, 0, ink.cols, ink.rows);
    if (area.width < 8 || area.height < 8) return line;
    line.origin = cv::Point2d(area.x, area.y);
    if (std::abs(s - 1.0) < 0.004) {
        line.ink = ink(area).clone();
    } else {
        cv::resize(ink(area), line.ink, cv::Size((int)std::lround(area.width / s), (int)std::lround(area.height / s)), 0,
                   0, s > 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    }
    // a border of nothing round it, so that a glyph's cell near the edge is whole
    cv::copyMakeBorder(line.ink, line.ink, LINE_BORDER, LINE_BORDER, LINE_BORDER, LINE_BORDER, cv::BORDER_CONSTANT, 0);
    line.origin -= cv::Point2d(LINE_BORDER * s, LINE_BORDER * s);
    cv::minMaxLoc(line.ink, nullptr, &line.most);
    if (line.most < floor) return line;
    cv::Mat on;
    cv::threshold(line.ink, on, 0.4 * line.most, 255, cv::THRESH_BINARY);
    on.convertTo(on, CV_8U);
    cv::Mat labels, stats, centres;
    const int count = cv::connectedComponentsWithStats(on, labels, stats, centres, 8, CV_32S);
    std::vector<cv::Rect> pieces;
    for (int i = 1; i < count; i++) {
        const cv::Rect r(stats.at<int>(i, cv::CC_STAT_LEFT), stats.at<int>(i, cv::CC_STAT_TOP),
                         stats.at<int>(i, cv::CC_STAT_WIDTH), stats.at<int>(i, cv::CC_STAT_HEIGHT));
        if (stats.at<int>(i, cv::CC_STAT_AREA) < 2) continue;
        if (r.height > 17 || r.width > 60) continue;   // a bar or a rule, no letter
        pieces.push_back(r);
    }
    std::vector<int> tops, bottoms;
    for (const cv::Rect& r : pieces) {
        if (r.height >= 8) {
            tops.push_back(r.y);
            bottoms.push_back(r.y + r.height);
        }
    }
    if (tops.empty()) return line;
    std::sort(tops.begin(), tops.end());
    std::sort(bottoms.begin(), bottoms.end());
    line.top = tops[tops.size() / 2];
    line.bottom = bottoms[bottoms.size() / 2];
    std::vector<cv::Rect> mine;
    for (const cv::Rect& r : pieces) {
        const double mid = r.y + r.height / 2.0;
        if (mid >= line.top - 2 && mid <= line.bottom + 2) mine.push_back(r);
    }
    std::sort(mine.begin(), mine.end(), [](const cv::Rect& a, const cv::Rect& b) { return a.x < b.x; });
    std::vector<cv::Rect> glyphs;
    for (const cv::Rect& r : mine) {
        if (!glyphs.empty()) {
            cv::Rect& last = glyphs.back();
            const int overlap = (std::min)(last.x + last.width, r.x + r.width) - (std::max)(last.x, r.x);
            if (overlap >= 2 || overlap >= (std::min)(last.width, r.width) / 2 + 1) {
                last |= r;
                continue;
            }
        }
        glyphs.push_back(r);
    }
    for (const cv::Rect& r : glyphs) {
        if (!line.words.empty() && r.x - (line.words.back().back().x + line.words.back().back().width) <= WORD_GAP) {
            line.words.back().push_back(r);
        } else {
            line.words.push_back({r});
        }
    }
    return line;
}

// Where a word's lettering ends: without a last glyph much shorter than the
// lettering - the small triangle 5.0 sets after the figure it points at. On
// the recording of 17 November 2026 it stands 8 to 9 units tall against the
// numerals' 13, 6 to 7 wide, its left 8 to 9 units after the figure on the
// Prospector and the MOLE, and on the Golem (larger numerals) 2 to 6 - near
// enough there to join the figure's word. A dot never ends a figure.
int textRight(const Line& line, size_t word) {
    const std::vector<cv::Rect>& w = line.words[word];
    const int tall = line.bottom - line.top;
    if (w.size() >= 2 && tall > 0 && w.back().height < 0.75 * tall) return w[w.size() - 2].x + w[w.size() - 2].width;
    return line.right(word);
}

// The word of a line whose lettering ends at `end` (in the line's units), up
// to `before` short of it or `after` past it; -1 for none. `whole`: the
// word's own end, with whatever short glyph it ends in.
int wordEndingAt(const Line& line, double end, double before = 3.5, double after = 3.5, bool whole = false) {
    for (int w = (int)line.words.size() - 1; w >= 0; w--) {
        const int right = whole ? line.right((size_t)w) : textRight(line, (size_t)w);
        if (right >= end - before && right <= end + after) return w;
        if (right < end - before) break;
    }
    return -1;
}

// The glyphs the reader knows: GLYPH_H tall and as wide as the glyph with the
// space before it, the glyph against the right edge
struct Known {
    char c;
    cv::Mat cell;    // CV_32F, size 1
};
const std::vector<Known>& knownGlyphs() {
    static const std::vector<Known> known = [] {
        std::vector<Known> out;
        // For the bench: the glyphs from a file instead (SCMINER_FIVE_SHAPES),
        // a line each: glyph width height and the values, row by row
        if (const char* from = DEV_ENV("SCMINER_FIVE_SHAPES")) {
            std::ifstream in(from);
            std::string glyph;
            int w = 0, h = 0;
            while (in >> glyph >> w >> h) {
                cv::Mat cell(h, w, CV_32F);
                for (int k = 0; k < w * h; k++) in >> cell.at<float>(k / w, k % w);
                if (in && h == GLYPH_H && w >= 2 && glyph.size() == 1) out.push_back({glyph[0], cell});
            }
            std::cerr << "[five] " << out.size() << " glyphs from " << from << std::endl;
            return out;
        }
        for (int i = 0; i < FIVE_SHAPE_COUNT; i++) {
            const FiveShape& s = FIVE_SHAPES[i];
            if (s.glyph == 'L' || s.height != GLYPH_H || s.width < 2) continue;
            out.push_back({s.glyph, cv::Mat(s.height, s.width, CV_32F, const_cast<float*>(s.grey)).clone()});
        }
        return out;
    }();
    return known;
}

// How alike two glyphs' shapes are as the matching sees them: b's best score
// on a clean cut of a - a's shape on blank ground, its right edge where the
// place ends - over the shifts the matching allows there (slackLeft to the
// left, slackRight to the right, a pixel up or down). Taken at one alignment,
// right edges together, the shifts raised the other glyph's score and not
// its likeness: a clean cut of a 6 stood (1 - shifted) / (2 (1 - aligned))
// from halfway to an 8, under the 0.5 every clean cut stands now, and on the
// app's pictures of 17 November 2026 the figures' margins came to 0.06-0.14
// in the median. Worked out once for every pair and slack up to 3.
constexpr int MOST_SLACK = 3;
double shiftedLikeness(const cv::Mat& a, const cv::Mat& b, int slackLeft, int slackRight) {
    const int right = (std::max)(a.cols, b.cols + slackLeft);
    double ground = 0;
    cv::minMaxLoc(a, &ground);
    cv::Mat clean(GLYPH_H + 2, right + slackRight, CV_32F, cv::Scalar(ground));
    a.copyTo(clean(cv::Rect(right - a.cols, 1, a.cols, GLYPH_H)));
    cv::Mat result;
    cv::matchTemplate(clean(cv::Rect(right - b.cols - slackLeft, 0, b.cols + slackLeft + slackRight, GLYPH_H + 2)), b,
                      result, cv::TM_CCOEFF_NORMED);
    double best = 0;
    cv::minMaxLoc(result, nullptr, &best);
    return best;
}
double likeness(const Known& a, const Known& b, int slackLeft, int slackRight) {
    const std::vector<Known>& known = knownGlyphs();
    const size_t n = known.size();
    static const std::vector<double> table = [&known, n] {
        std::vector<double> t((MOST_SLACK + 1) * (MOST_SLACK + 1) * n * n, 1.0);
        for (int l = 0; l <= MOST_SLACK; l++) {
            for (int r = 0; r <= MOST_SLACK; r++) {
                for (size_t i = 0; i < n; i++) {
                    for (size_t j = 0; j < n; j++) {
                        t[((size_t)(l * (MOST_SLACK + 1) + r) * n + i) * n + j] =
                            shiftedLikeness(known[i].cell, known[j].cell, l, r);
                    }
                }
            }
        }
        return t;
    }();
    const size_t i = (size_t)(&a - known.data()), j = (size_t)(&b - known.data());
    if (slackLeft < 0 || slackRight < 0 || slackLeft > MOST_SLACK || slackRight > MOST_SLACK || i >= n || j >= n) {
        return shiftedLikeness(a.cell, b.cell, (std::max)(slackLeft, 0), (std::max)(slackRight, 0));
    }
    return table[((size_t)(slackLeft * (MOST_SLACK + 1) + slackRight) * n + i) * n + j];
}

// A figure's format from its right end: slots of the glyphs allowed there,
// each so many times
struct Slot {
    const char* glyphs;
    int least, most;
};
using Format = std::vector<Slot>;
const char* const DIGITS = "0123456789";
const Format WHOLE{{DIGITS, 1, 7}};
const Format QUALITY{{DIGITS, 1, 4}};
const Format PERCENT{{"%", 1, 1}, {DIGITS, 1, 3}};
const Format DELTA{{"%", 1, 1}, {DIGITS, 1, 3}, {"+-", 1, 1}};
const Format TWO_PLACES{{DIGITS, 2, 2}, {".", 1, 1}, {DIGITS, 1, 5}};
const Format VOLUME{{"m", 0, 1}, {DIGITS, 2, 2}, {".", 1, 1}, {DIGITS, 1, 3}};
const Format SHARE{{"%", 1, 1}, {DIGITS, 2, 2}, {".", 1, 1}, {DIGITS, 1, 3}};
const Format SCU_WORD{{"U", 1, 1}, {"C", 1, 1}, {"S", 1, 1}};

struct Reading {
    std::string text;
    bool sure = false;
    double least = 1;        // the worst glyph's match
    double margin = 0.5;     // ...and how far the least sure glyph stands from halfway to another
    double zeroEight = 9;    // ...and how far the zero/eight test held at its closest (countersOf), 9 for none
    int left = 0;            // where the figure begins, in the line
    bool cut = false;        // it ended at its leftmost place with ink still going on
    std::vector<cv::Rect> boxes;   // each glyph's cell, in the line
};

// The slashed zero against the eight, where the glyph was found: how dark the
// lighter of its two counters is, against the glyph's own range. An eight
// shows both counters dark, one over its waist and one under it; the zero's
// slash cuts its counter in two, and at this size the glow fills at least one
// half. Its waist does not tell them apart: the outer columns at mid-height
// hold 1.00 of their ink above and below in eights and 1.01 in zeros. On the
// recording of 15 November 2026: eights 0.53 in the median and nine in ten
// under 0.65, zeros 0.83 and three in four over 0.72, and the five eights
// misread as zeros that were checked by eye 0.58 to 0.72.
double countersOf(const cv::Mat& ink, const cv::Rect& box) {
    const int w = 9;   // a numeral's advance
    const cv::Rect r(box.x + box.width - w, box.y, w, GLYPH_H);
    if (box.width < w || (r & cv::Rect(0, 0, ink.cols, ink.rows)) != r) return 0.7;
    const cv::Mat cell = ink(r);
    double lo = 0, hi = 0;
    cv::minMaxLoc(cell, &lo, &hi);
    if (hi - lo < 1e-6) return 0.7;
    double upper = 0, lower = 0;
    cv::minMaxLoc(cell(cv::Rect(2, 4, 4, 3)), &upper);
    cv::minMaxLoc(cell(cv::Rect(2, 8, 5, 4)), &lower);
    return ((std::max)(upper, lower) - lo) / (hi - lo);
}
constexpr double ZERO_COUNTERS = 0.80;    // a zero from here up
constexpr double EIGHT_COUNTERS = 0.65;   // an eight to here
// A tone-mapped frame (an HDR desktop, the app's own picture: see
// setPanelToneMapped) shows the zeros' counters darker. At the truth's
// places on the app's 55 pictures of 17 November 2026: zeros 0.79 in the
// median (113), eights 0.51 (156) and 0.76 at the most; on the recording of
// the same day zeros 0.84 (312), eights 0.56 (124) and up to 0.84. There a
// zero counts from 0.77 up, over every eight the app's pictures showed. The
// eights keep their limit: the app's zeros reach down to 0.22, and a limit
// under all of them would leave hardly an eight.
constexpr double ZERO_COUNTERS_TONE_MAPPED = 0.77;
double zeroCounters() {
    return panelToneMapped() ? ZERO_COUNTERS_TONE_MAPPED : ZERO_COUNTERS;
}

// Whether a text read backwards fills its format: every slot at least as
// often as it must be, none more often than it may
bool fills(const std::string& backwards, const Format& format) {
    if (backwards.empty()) return false;
    size_t k = 0;
    int had = 0;
    for (char c : backwards) {
        while (k < format.size() && !(std::strchr(format[k].glyphs, c) && had < format[k].most)) {
            if (had < format[k].least) return false;
            k++;
            had = 0;
        }
        if (k == format.size()) return false;
        had++;
    }
    if (had < format[k].least) return false;
    for (size_t j = k + 1; j < format.size(); j++) {
        if (format[j].least > 0) return false;
    }
    return true;
}

// A figure read from its right end, glyph by glyph: at each place every glyph
// its format allows there is matched a pixel or two either way and the best
// taken. It ends where the format is whole and no ink follows, or where the
// next glyph would begin left of `leftmost` - and then it is not sure: the
// ink runs on, and what it is was not read. `right` is where its last glyph
// ends in the line.
Reading slide(const Line& line, int right, const Format& format, int firstSlack, double sureScore, double sureMargin,
              int leftmost = -100000) {
    Reading out;
    if (line.ink.empty() || line.top < 0 || knownGlyphs().empty()) return out;
    const int baseTop = line.top - GLYPH_ABOVE;
    size_t slot = 0;
    int inSlot = 0;
    int x = right;
    bool first = true;
    std::string backwards;
    auto blankBefore = [&](int at) {
        const cv::Rect r =
            cv::Rect(at - 5, line.top, 4, line.bottom - line.top) & cv::Rect(0, 0, line.ink.cols, line.ink.rows);
        if (r.width <= 0 || r.height <= 0) return true;
        double most = 0;
        cv::minMaxLoc(line.ink(r), nullptr, &most);
        return most < 0.3 * line.most;
    };
    for (int guard = 0; guard < 16; guard++) {
        // what may stand here: more of this slot, or of the slots after it
        // as far as the first that must have one
        std::string allowed;
        bool mayEnd = true;
        for (size_t k = slot; k < format.size(); k++) {
            const int had = k == slot ? inSlot : 0;
            if (had < format[k].most) allowed += format[k].glyphs;
            if (had < format[k].least) {
                mayEnd = false;
                break;
            }
        }
        if (allowed.empty()) break;
        if (mayEnd && !first && blankBefore(x)) break;
        double best = -2;
        char bestC = 0;
        cv::Rect bestBox;
        std::vector<std::pair<const Known*, double>> tried;
        const int slackLeft = first ? firstSlack : 2, slackRight = first ? firstSlack : 1;
        // every shape of every glyph allowed here - a glyph can have several,
        // as the recording and the app's own picture draw it
        const Known* mine = nullptr;
        for (const Known& k : knownGlyphs()) {
            if (allowed.find(k.c) == std::string::npos) continue;
            const cv::Rect window(x - k.cell.cols - slackLeft, baseTop - 1, k.cell.cols + slackLeft + slackRight,
                                  GLYPH_H + 2);
            if ((window & cv::Rect(0, 0, line.ink.cols, line.ink.rows)) != window) continue;
            cv::Mat result;
            cv::matchTemplate(line.ink(window), k.cell, result, cv::TM_CCOEFF_NORMED);
            double score = 0;
            cv::Point at;
            cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
            tried.push_back({&k, score});
            if (score > best) {
                best = score;
                bestC = k.c;
                mine = &k;
                bestBox = cv::Rect(window.x + at.x, window.y + at.y, k.cell.cols, GLYPH_H);
            }
        }
        if (!bestC) break;
        // nothing like a glyph where the format could end: the figure ends
        if (mayEnd && best < 0.5) break;
        // ...and nothing is read left of where the figure can begin. A
        // glyph's likeness there ends the figure unsure: on the MOLE's
        // picture of 17 November 2026 08:54:15 a blurred 44.92% stood 6 left
        // of the list's edge and was read 4.92%, sure.
        if (bestBox.x < leftmost) {
            out.cut = true;
            break;
        }
        // how much nearer its own shape the glyph is than each other glyph's
        for (const auto& [other, score] : tried) {
            if (other->c == bestC) continue;
            const double apart = 2.0 * (1.0 - likeness(*mine, *other, slackLeft, slackRight));
            if (apart > 1e-6) out.margin = (std::min)(out.margin, (best - score) / apart);
        }
        // a zero or an eight where either may stand: their counters say which
        if ((bestC == '0' || bestC == '8') && allowed.find('0') != std::string::npos &&
            allowed.find('8') != std::string::npos) {
            const double c = countersOf(line.ink, bestBox);
            out.zeroEight = (std::min)(out.zeroEight, bestC == '0' ? c - zeroCounters() : EIGHT_COUNTERS - c);
        }
        backwards += bestC;
        out.boxes.push_back(bestBox);
        out.least = (std::min)(out.least, best);
        x = bestBox.x;
        first = false;
        while (slot < format.size()) {
            if (std::strchr(format[slot].glyphs, bestC) && inSlot < format[slot].most) {
                inSlot++;
                break;
            }
            slot++;
            inSlot = 0;
        }
    }
    out.text.assign(backwards.rbegin(), backwards.rend());
    std::reverse(out.boxes.begin(), out.boxes.end());
    out.left = x;
    out.sure = fills(backwards, format) && out.least >= sureScore && out.margin >= sureMargin &&
               out.zeroEight >= 0 && !out.cut;
    return out;
}

// The bench's own record of what was cut: every glyph of every figure with the
// figure's text where it is known (SCMINER_FIVE_HARVEST=<csv>,
// SCMINER_FIVE_TRUTH=<csv of file,ore,mass,resdelta,res,instdelta,inst,scu,list>)
struct Harvest {
    std::ofstream out;
    std::map<std::string, std::vector<std::string>> truth;
    std::string file;   // the frame being read
};
Harvest* harvest() {
    static Harvest* h = []() -> Harvest* {
        const char* to = DEV_ENV("SCMINER_FIVE_HARVEST");
        if (!to) return nullptr;
        Harvest* made = new Harvest;
        made->out.open(to, std::ios::app);
        if (const char* t = DEV_ENV("SCMINER_FIVE_TRUTH")) {
            std::ifstream in(t);
            std::string row;
            std::getline(in, row);
            while (std::getline(in, row)) {
                if (!row.empty() && row.back() == '\r') row.pop_back();
                std::vector<std::string> f;
                std::stringstream ss(row);
                std::string cell;
                while (std::getline(ss, cell, ',')) f.push_back(cell);
                if (!f.empty()) made->truth[f[0]] = f;
            }
        }
        return made;
    }();
    return h;
}

// ...and a picture of the panel with what was cut, four times its size, for
// looking at (SCMINER_FIVE_DRAW=<folder>)
struct Drawing {
    cv::Mat picture;     // the panel's part of the frame
    cv::Point origin;
    std::vector<std::pair<cv::Rect, cv::Scalar>> boxes;
};
Drawing g_drawing;
std::string g_file;      // the frame being read, for the bench
std::string g_listWords; // the list's lines as cut, for the bench (SCMINER_FIVE_WORDS)
Drawing* drawing() {
    static const bool on = DEV_ENV("SCMINER_FIVE_DRAW") != nullptr;
    return on && !g_drawing.picture.empty() ? &g_drawing : nullptr;
}
void startDrawing(const cv::Mat& bgr, const cv::Rect& part) {
    g_drawing.picture.release();
    g_drawing.boxes.clear();
    if (!DEV_ENV("SCMINER_FIVE_DRAW")) return;
    const cv::Rect safe = part & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (safe.area() <= 0) return;
    g_drawing.picture = bgr(safe).clone();
    g_drawing.origin = safe.tl();
}
void drawBox(const cv::Rect& r, const cv::Scalar& colour) {
    if (Drawing* d = drawing()) d->boxes.push_back({r - d->origin, colour});
}
void endDrawing() {
    const char* dir = DEV_ENV("SCMINER_FIVE_DRAW");
    if (!dir || g_drawing.picture.empty()) return;
    cv::Mat big;
    cv::resize(g_drawing.picture, big, cv::Size(), 4, 4, cv::INTER_NEAREST);
    for (const auto& [r, colour] : g_drawing.boxes) {
        cv::rectangle(big, cv::Rect(r.x * 4, r.y * 4, r.width * 4, r.height * 4), colour, 1);
    }
    cv::imwrite(std::string(dir) + "/" + (g_file.empty() ? std::string("frame") : g_file) + ".png", big);
    g_drawing.picture.release();
}

std::string truthOf(int column) {
    Harvest* h = harvest();
    if (!h) return "";
    const auto it = h->truth.find(h->file);
    if (it == h->truth.end() || (int)it->second.size() <= column) return "";
    return it->second[(size_t)column];
}

// The bench's harvest by the truth: a figure's glyphs placed from its right
// end one by one, each where its own shape matches best a pixel or two
// either way. The reading is not asked, so a glyph the shapes misread is cut
// where it stands. One line per glyph: file,field,index,glyph,score,right,
// top, then its cell, CELL_W wide ending where the glyph ends and GLYPH_H
// tall from where it matched.
void harvestForced(const char* field, const Line& line, int right, const std::string& truth, int firstSlack) {
    Harvest* h = harvest();
    if (!h || truth.empty() || line.ink.empty() || line.top < 0) return;
    const int baseTop = line.top - GLYPH_ABOVE;
    std::vector<std::pair<cv::Rect, double>> placed(truth.size());
    int x = right;
    for (size_t i = truth.size(); i-- > 0;) {
        const bool first = i + 1 == truth.size();
        const int slackLeft = first ? firstSlack : 2, slackRight = first ? firstSlack : 1;
        double best = -2;
        cv::Rect bestBox;
        for (const Known& k : knownGlyphs()) {
            if (k.c != truth[i]) continue;
            const cv::Rect window(x - k.cell.cols - slackLeft, baseTop - 1, k.cell.cols + slackLeft + slackRight,
                                  GLYPH_H + 2);
            if ((window & cv::Rect(0, 0, line.ink.cols, line.ink.rows)) != window) continue;
            cv::Mat result;
            cv::matchTemplate(line.ink(window), k.cell, result, cv::TM_CCOEFF_NORMED);
            double score = 0;
            cv::Point at;
            cv::minMaxLoc(result, nullptr, &score, nullptr, &at);
            if (score > best) {
                best = score;
                bestBox = cv::Rect(window.x + at.x, window.y + at.y, k.cell.cols, GLYPH_H);
            }
        }
        if (best < -1) return;   // no shape for it, or off the line
        placed[i] = {bestBox, best};
        x = bestBox.x;
    }
    for (size_t i = 0; i < truth.size(); i++) {
        const cv::Rect& b = placed[i].first;
        const cv::Rect cell(b.x + b.width - CELL_W, b.y, CELL_W, GLYPH_H);
        if ((cell & cv::Rect(0, 0, line.ink.cols, line.ink.rows)) != cell) continue;
        h->out << h->file << "," << field << "," << i << "," << truth[i] << "," << placed[i].second << ","
               << b.x + b.width << "," << b.y - line.top;
        for (int k = 0; k < CELL_W * GLYPH_H; k++) h->out << "," << line.ink(cell).at<float>(k / CELL_W, k % CELL_W);
        h->out << "\n";
    }
    h->out << std::flush;
}

// A figure as read, drawn: green where sure
void drawRead(const Line& line, const Reading& read) {
    for (const cv::Rect& b : read.boxes) drawBox(line.inFrame(b), read.sure ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255));
}

// How well a text fits a figure's glyphs, placed from its right end one by
// one as the harvest places them: the worst glyph's match, -1 where a glyph
// has no shape or falls off the line
double fitOf(const Line& line, int right, const std::string& text) {
    if (line.ink.empty() || line.top < 0 || text.empty()) return -1;
    const int baseTop = line.top - GLYPH_ABOVE;
    int x = right;
    double worst = 1;
    for (size_t i = text.size(); i-- > 0;) {
        const bool first = i + 1 == text.size();
        const int slackLeft = 2, slackRight = first ? 2 : 1;
        double best = -2;
        int at = x;
        for (const Known& k : knownGlyphs()) {
            if (k.c != text[i]) continue;
            const cv::Rect window(x - k.cell.cols - slackLeft, baseTop - 1, k.cell.cols + slackLeft + slackRight,
                                  GLYPH_H + 2);
            if ((window & cv::Rect(0, 0, line.ink.cols, line.ink.rows)) != window) continue;
            cv::Mat result;
            cv::matchTemplate(line.ink(window), k.cell, result, cv::TM_CCOEFF_NORMED);
            double score = 0;
            cv::Point p;
            cv::minMaxLoc(result, nullptr, &score, nullptr, &p);
            if (score > best) {
                best = score;
                at = window.x + p.x;
            }
        }
        if (best < -1) return -1;
        worst = (std::min)(worst, best);
        x = at;
    }
    return worst;
}

// Whether a figure is written in green (Positive, 64,224,49; about
// 207,254,117 in the recordings): its brightest pixels well greener than red
// and blue. White writing has the three alike.
bool greenOf(const cv::Mat& bgr, const cv::Rect& word) {
    const cv::Rect r = word & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (r.area() <= 0) return false;
    std::vector<cv::Vec3b> px;
    for (int y = r.y; y < r.y + r.height; y++) {
        for (int x = r.x; x < r.x + r.width; x++) px.push_back(bgr.at<cv::Vec3b>(y, x));
    }
    std::sort(px.begin(), px.end(), [](const cv::Vec3b& a, const cv::Vec3b& b) {
        return (std::max)({a[0], a[1], a[2]}) > (std::max)({b[0], b[1], b[2]});
    });
    const size_t n = (std::max)((size_t)1, px.size() * 3 / 10);
    double b = 0, g = 0, red = 0;
    for (size_t i = 0; i < n; i++) {
        b += px[i][0];
        g += px[i][1];
        red += px[i][2];
    }
    return (g - (std::max)(red, b)) / (double)n > 25.0;
}

double envOr(const char* name, double fallback) {
    const char* set = DEV_ENV(name);
    return set ? std::atof(set) : fallback;
}

// For the bench: a line's lettering top and bottom and its words, in units
// from the labels' left edge (SCMINER_FIVE_WORDS)
std::string wordsOf(const char* name, const Line& line, double labelsX, double s) {
    if (!DEV_ENV("SCMINER_FIVE_WORDS")) return "";
    char text[64];
    std::snprintf(text, sizeof(text), "; %s %d-%d", name, line.top, line.bottom);
    std::string out = text;
    for (size_t w = 0; w < line.words.size(); w++) {
        std::snprintf(text, sizeof(text), " %.1f-%.1f", (line.origin.x + line.left(w) * line.s - labelsX) / s,
                      (line.origin.x + line.right(w) * line.s - labelsX) / s);
        out += text;
    }
    return out;
}

}  // namespace

bool fiveWhole(const std::string& text, float& value) {
    if (text.empty() || text.size() > 7) return false;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
    }
    if (text.size() > 1 && text[0] == '0') return false;
    value = (float)std::atof(text.c_str());
    return true;
}

bool fivePercent(const std::string& text, float& value) {
    if (text.size() < 2 || text.back() != '%') return false;
    float v = 0;
    if (!fiveWhole(text.substr(0, text.size() - 1), v) || v > 100) return false;
    value = v;
    return true;
}

bool fiveDelta(const std::string& text, float& value) {
    if (text.size() < 3 || (text[0] != '+' && text[0] != '-')) return false;
    float v = 0;
    if (!fivePercent(text.substr(1), v)) return false;
    value = text[0] == '-' ? -v : v;
    return true;
}

bool fiveTwoPlaces(const std::string& text, float& value) {
    const size_t stop = text.find('.');
    if (stop == std::string::npos || stop == 0 || stop + 3 != text.size()) return false;
    float whole = 0;
    if (!fiveWhole(text.substr(0, stop), whole)) return false;
    for (size_t i = stop + 1; i < text.size(); i++) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    value = whole + (float)std::atoi(text.substr(stop + 1).c_str()) / 100.0f;
    return true;
}

bool fiveVolume(const std::string& text, float& scu) {
    if (!text.empty() && text.back() == 'm') {
        float milli = 0;
        if (!fiveTwoPlaces(text.substr(0, text.size() - 1), milli) || milli >= 1000.0f) return false;
        scu = milli / 1000.0f;
        return true;
    }
    return fiveTwoPlaces(text, scu);
}

bool fiveShare(const std::string& text, float& value) {
    if (text.size() < 5 || text.back() != '%') return false;
    float v = 0;
    if (!fiveTwoPlaces(text.substr(0, text.size() - 1), v) || v <= 0 || v > 100) return false;
    value = v;
    return true;
}

void fiveHarvestFile(const std::string& file) {
    g_file = file;
    if (Harvest* h = harvest()) h->file = file;
}

bool readPanelFive(const cv::Mat& frame, ScanPanel& panel, TextLineReader* lines) {
    if (frame.empty() || columnShape().empty()) return false;
    const cv::Mat bgr = bgrOf(frame);
    const double screen = frame.rows / 1440.0;
    const cv::Rect band = bandOf(frame.size());
    const cv::Rect turretBand = turretBandOf(frame.size());
    const cv::Mat ink = [&] {
        cv::Mat all = cv::Mat::zeros(bgr.size(), CV_32F);
        if (turretBand.area() > 0) inkOf(bgr(turretBand), screen).copyTo(all(turretBand));
        inkOf(bgr(band), screen).copyTo(all(band));
        return all;
    }();
    std::vector<double> scales;
    for (double f = 0.78; f <= 1.27; f += 0.06) scales.push_back(screen * f);
    // the panel beside the reticle, or the MOLE turret's row under it. The
    // turret's labels must match well: on the 3,600 frames of 15 November
    // 2026 (a Prospector, no turret) the row under the reticle matched them
    // at 0.30 to 0.67, the turret's own at 0.82 and 0.85 (17 November 2026)
    static const double turretBar = envOr("SCMINER_FIVE_TURRET_BAR", 0.75);
    const Column beside = findColumn(ink, band, scales);
    const Column underneath = findColumn(ink, turretBand, scales);
    // where the labels stand says the layout (layoutOf): a column in the
    // turret's row is one only at the turret's bar, whichever part found it
    auto fits = [&](const Column& c) { return !turretRow(c.labels, frame.size()) || c.score >= turretBar; };
    const Column column = fits(underneath) && (!fits(beside) || underneath.score > beside.score) ? underneath : beside;
    static const double bar = envOr("SCMINER_FIVE_BAR", 0.5);
    char note[320];
    std::snprintf(note, sizeof(note), "five: column %.2f at %d,%d x%.3f%s", column.score, column.labels.x, column.labels.y,
                  column.scale, turretRow(column.labels, frame.size()) ? " turret" : "");
    panel.note = note;
    if (column.score < bar || !fits(column)) return false;
    const double s = column.scale;
    const int L = column.labels.x, T = column.labels.y;
    const Layout& lay = layoutOf(column.labels, frame.size());
    auto X = [&](double u) { return (int)std::lround(L + u * s); };
    auto Y = [&](double u) { return (int)std::lround(T + u * s); };
    // a line of the panel at its place, from `from` to `to` right of the labels
    auto lineOf = [&](double top, double from, double to) {
        return lineAt(ink, cv::Rect(cv::Point(X(from), Y(top - 4)), cv::Point(X(to), Y(top + CAP + 4))), s, 12.0);
    };
    // the word of a line that ends at `end` right of the labels; -1 for none
    auto endingAt = [&](const Line& line, double end) { return wordEndingAt(line, line.unit(X(end))); };
    // A 5.0 panel, not 4.10's with the same labels: 4.10 writes its figures
    // after the labels, 5.0 at the panel's right edge, and there a word ends
    // on the MASS line - or the column is 5.0's beyond doubt, and the
    // panel is one whose mass is lost to the ground behind it. Here the
    // word's own end counts, a short last glyph and all: on the 3,600
    // frames of 15 November 2026, measured by its lettering alone
    // (textRight), 599 of the 694 with a panel passed and 81 of the 2,906
    // without one; by the word's own end 592 and 70 (the reader before 17
    // November 2026: 592 and 73).
    static const double sureColumn = envOr("SCMINER_FIVE_SURE_COLUMN", 0.85);
    const Line massLine = lineOf(0, 60, lay.figuresEnd + 8);
    if (wordEndingAt(massLine, massLine.unit(X(lay.figuresEnd)), 3.5, 3.5, true) < 0 && column.score < sureColumn) {
        panel.note += "; nothing at the right edge";
        return false;
    }
    startDrawing(bgr, cv::Rect(cv::Point(X(-10), Y((std::min)(-55.0, lay.volumeTop - 10))),
                               cv::Point(X(lay.listLeft + lay.scuEnd + 15), Y(lay.listTop + 6 * lay.listPitch))));
    drawBox(cv::Rect(L, T, (int)std::lround(95 * s), (int)std::lround((2 * ROW_PITCH + CAP) * s)), cv::Scalar(255, 255, 0));
    // the label column as found, at size 1, for averaging into its shape
    if (Harvest* h = harvest()) {
        const cv::Rect cut((int)std::lround(L - COLUMN_INSET * s), (int)std::lround(T - COLUMN_INSET * s),
                           (int)std::lround(COLUMN_W * s), (int)std::lround(COLUMN_H * s));
        if ((cut & cv::Rect(0, 0, ink.cols, ink.rows)) == cut) {
            cv::Mat one;
            cv::resize(ink(cut), one, cv::Size(COLUMN_W, COLUMN_H), 0, 0, cv::INTER_AREA);
            h->out << h->file << ",column,0,L," << column.score << "," << s;
            for (int k = 0; k < COLUMN_W * COLUMN_H; k++) h->out << "," << one.at<float>(k / COLUMN_W, k % COLUMN_W);
            h->out << "\n";
        }
    }

    // A glyph is taken where it matches at 0.80 or better and stands 0.3 of
    // the way from halfway to the likeliest other glyph: at 0.2 a slashed
    // zero passed for an 8 twice on the recording of 15 November 2026
    // (0.82 read 0.02, 20.38 read 28.38), at 0.3 nothing checked by eye did.
    static const double sureScore = envOr("SCMINER_FIVE_SCORE", 0.80);
    static const double sureMargin = envOr("SCMINER_FIVE_MARGIN", 0.3);
    struct Figure {
        Reading read;
        int word = -1;
    };
    auto figure = [&](const Line& line, double end, const Format& format) {
        Figure f;
        f.word = endingAt(line, end);
        if (f.word < 0) return f;
        f.read = slide(line, textRight(line, (size_t)f.word), format, 2, sureScore, sureMargin);
        drawRead(line, f.read);
        return f;
    };
    // The laser's modifier in front of a figure: none where nothing stands
    // left of the figure, read where one word does and ends where modifiers
    // end; anything else leaves the figure unknown, as its own value could
    // not be told
    auto modifier = [&](const Line& line, const Figure& value, Figure& delta) {
        if (!value.read.sure) return false;
        std::vector<int> before;
        for (size_t w = 0; w < line.words.size(); w++) {
            if (line.right(w) <= value.read.left + 1) before.push_back((int)w);
        }
        if (before.empty()) return true;
        if (before.size() != 1) return false;
        delta = figure(line, lay.deltaEnd, DELTA);
        return delta.word == before[0] && delta.read.sure;
    };
    const Line resLine = lineOf(ROW_PITCH, 100, lay.figuresEnd + 8);
    const Line instLine = lineOf(2 * ROW_PITCH, 100, lay.figuresEnd + 8);
    const Figure mass = figure(massLine, lay.figuresEnd, WHOLE);
    const Figure res = figure(resLine, lay.figuresEnd, PERCENT);
    const Figure inst = figure(instLine, lay.figuresEnd, TWO_PLACES);
    Figure resDelta, instDelta;
    const bool resOwn = modifier(resLine, res, resDelta);
    const bool instOwn = modifier(instLine, inst, instDelta);
    // the volume, left of SCU at the line's end. SCU is the line's last word
    // and can end a little further right than the figures: up to 239 on
    // the Prospector and the MOLE, to 241 on the Golem (17 November 2026)
    Figure volume, scuWord;
    const double volumeEnd = lay.listLeft + lay.scuEnd;
    const Line volumeLine = lineOf(lay.volumeTop, lay.listLeft + lay.volumeFrom, volumeEnd + 8);
    scuWord.word = wordEndingAt(volumeLine, volumeLine.unit(X(volumeEnd)), 3.5, 8.0);
    if (scuWord.word >= 0) {
        scuWord.read = slide(volumeLine, textRight(volumeLine, (size_t)scuWord.word), SCU_WORD, 2, sureScore, sureMargin);
        drawRead(volumeLine, scuWord.read);
        if (scuWord.read.sure && scuWord.read.text == "SCU") {
            // S's shape holds the space before it, so the volume ends where it begins
            volume.read = slide(volumeLine, scuWord.read.left, VOLUME, 3, sureScore, sureMargin);
            volume.word = scuWord.word;
            drawRead(volumeLine, volume.read);
        }
    }
    // the bench's harvest: each figure by the truth, glyph by glyph
    if (harvest()) {
        auto take = [&](const char* field, const Line& line, int word, const std::string& truth, int slack) {
            if (word >= 0) harvestForced(field, line, textRight(line, (size_t)word), truth, slack);
        };
        take("mass", massLine, endingAt(massLine, lay.figuresEnd), truthOf(2), 2);
        take("res", resLine, endingAt(resLine, lay.figuresEnd), truthOf(4), 2);
        take("resdelta", resLine, endingAt(resLine, lay.deltaEnd), truthOf(3), 2);
        take("inst", instLine, endingAt(instLine, lay.figuresEnd), truthOf(6), 2);
        take("instdelta", instLine, endingAt(instLine, lay.deltaEnd), truthOf(5), 2);
        if (!truthOf(7).empty()) take("scu", volumeLine, endingAt(volumeLine, volumeEnd), truthOf(7) + "SCU", 2);
        std::vector<std::string> parts;
        {
            std::stringstream ss(truthOf(8));
            std::string p;
            while (std::getline(ss, p, ';')) parts.push_back(p);
        }
        for (int row = 0; row < (int)parts.size(); row++) {
            const Line line =
                lineOf(lay.listTop + row * lay.listPitch, lay.listLeft - 8, lay.listLeft + lay.listEnd + 8);
            if (line.words.size() < 2) continue;
            // share|name|quality|colour
            std::vector<std::string> f;
            {
                std::stringstream ss(parts[(size_t)row]);
                std::string cell;
                while (std::getline(ss, cell, '|')) f.push_back(cell);
            }
            if (f.size() < 3) continue;
            if (std::abs(line.left(0) - line.unit(X(lay.listLeft))) <= 4) take("share", line, 0, f[0] + "%", 2);
            if (!f[2].empty()) take("quality", line, endingAt(line, lay.listLeft + lay.qualityEnd), f[2], 2);
        }
    }

    RockStats& r = panel.stats;
    r = RockStats();
    r.resistance = -1;
    r.instability = -1;
    r.deltasRead = true;
    float v = 0;
    if (mass.read.sure && fiveWhole(mass.read.text, v) && v > 0) r.mass = v;
    if (resOwn && fivePercent(res.read.text, v) &&
        (resDelta.word < 0 || fiveDelta(resDelta.read.text, r.resistanceDelta))) {
        r.resistance = v;
        r.resistanceAdjusted = r.resistanceDelta != 0;
    }
    if (instOwn && fiveTwoPlaces(inst.read.text, v) &&
        (instDelta.word < 0 || fiveDelta(instDelta.read.text, r.instabilityDelta))) {
        r.instability = v;
    }
    if (volume.read.sure && fiveVolume(volume.read.text, v) && v > 0) r.scu = v;
    if (r.resistance < 0) r.resistanceDelta = 0;
    if (r.instability < 0) r.instabilityDelta = 0;
    panel.found = r.mass > 0 && r.resistance >= 0 && r.instability >= 0;
    panel.figuresRead = (r.mass > 0) + (r.resistance >= 0) + (r.instability >= 0);
    auto shown = [](const Figure& f) {
        if (f.word < 0) return std::string("-");
        char text[64];
        std::snprintf(text, sizeof(text), "%s%s(%.2f/%.2f/%.2f)", f.read.text.c_str(), f.read.sure ? "" : "?",
                      f.read.least, f.read.margin, f.read.zeroEight);
        return std::string(text);
    };
    panel.note += "; read " + shown(mass) + " " + shown(resDelta) + "/" + shown(res) + " " + shown(instDelta) + "/" +
                  shown(inst) + " " + shown(volume);
    auto delta = [](float figure, float d) { return figure < 0 ? std::string("-") : std::to_string((int)std::lround(d)); };
    panel.note += "; delta r" + delta(r.resistance, r.resistanceDelta) + " i" + delta(r.instability, r.instabilityDelta);
    panel.note += wordsOf("mass", massLine, L, s) + wordsOf("res", resLine, L, s) + wordsOf("inst", instLine, L, s) +
                  wordsOf("vol", volumeLine, L, s);
    // For the bench: the list too, its qualities narrowed by the ore the
    // second reader names (SCMINER_FIVE_LIST)
    if (DEV_ENV("SCMINER_FIVE_LIST") && lines && lines->ready()) {
        ScanPanel placed = panel;
        placed.labels = column.labels;
        placed.labelScale = s;
        const std::string ore = oreByLine(frame, panel.oreRow.area() > 0 ? panel.oreRow
                                                   : cv::Rect(cv::Point(X(-4), Y(ORE_TOP - 3)),
                                                              cv::Point(X(lay.figuresEnd + 4), Y(ORE_TOP + CAP + 3))) &
                                                         cv::Rect(0, 0, frame.cols, frame.rows), *lines);
        const std::vector<int> values = fiveQualities(ore);
        panel.note += "; ore '" + ore + "' list";
        for (const FiveListLine& row : readFiveList(frame, placed, &values)) {
            // the name as the list's reader reads it (the inert rest is known
            // by it), spaces as '_'
            std::string name = row.name.area() > 0 ? lines->read(frame(row.name)).text : std::string();
            std::replace(name.begin(), name.end(), ' ', '_');
            panel.note += " " + row.shareText + (row.shareSure ? "" : "?") + "|" +
                          (row.byValues ? std::to_string(row.quality) + "v" : row.qualityText + (row.qualitySure ? "" : "?")) +
                          (row.dim ? "d" : "") + "|n=" + name;
        }
        panel.note += g_listWords;
    }
    panel.figures[0].text = mass.word >= 0 ? mass.read.text : std::string();
    panel.labels = column.labels;
    panel.labelScale = s;
    panel.labelGlyph = FIVE_PANEL;
    panel.confidence = column.score;
    panel.box = cv::Rect(cv::Point(X(-6), Y((std::min)(-50.0, lay.volumeTop - 6))),
                         cv::Point(X(lay.listLeft + lay.scuEnd + 12), Y(lay.listTop + 5 * lay.listPitch))) &
                cv::Rect(0, 0, frame.cols, frame.rows);
    panel.oreRow = cv::Rect(cv::Point(X(-4), Y(ORE_TOP - 3)), cv::Point(X(lay.figuresEnd + 4), Y(ORE_TOP + CAP + 3))) &
                   cv::Rect(0, 0, frame.cols, frame.rows);
    endDrawing();
    return true;
}

cv::Rect fiveContentsRegion(const ScanPanel& panel, const cv::Size& frame) {
    if (panel.labels.x < 0 || panel.labelScale <= 0) return cv::Rect();
    const double s = panel.labelScale;
    const Layout& lay = layoutOf(panel.labels, frame);
    const cv::Rect wanted(cv::Point((int)std::lround(panel.labels.x + (lay.listLeft - 12) * s),
                                    (int)std::lround(panel.labels.y + (lay.listTop - 8) * s)),
                          cv::Point((int)std::lround(panel.labels.x + (lay.listLeft + lay.listEnd + 12) * s),
                                    (int)std::lround(panel.labels.y + (lay.listTop + 7 * lay.listPitch) * s)));
    return wanted & cv::Rect(0, 0, frame.width, frame.height);
}

std::vector<FiveListLine> readFiveList(const cv::Mat& frame, const ScanPanel& panel, const std::vector<int>* qualities) {
    std::vector<FiveListLine> out;
    if (frame.empty() || panel.labels.x < 0 || panel.labelScale <= 0 || knownGlyphs().empty()) return out;
    const double s = panel.labelScale;
    const int L = panel.labels.x, T = panel.labels.y;
    const Layout& lay = layoutOf(panel.labels, frame.size());
    auto X = [&](double u) { return (int)std::lround(L + u * s); };
    auto Y = [&](double u) { return (int)std::lround(T + u * s); };
    // the list's ink, and its lines cut out of it
    const cv::Rect part = cv::Rect(cv::Point(X(lay.listLeft - 16), Y(lay.listTop - 12)),
                                   cv::Point(X(lay.listLeft + lay.listEnd + 16), Y(lay.listTop + 7.5 * lay.listPitch))) &
                          cv::Rect(0, 0, frame.cols, frame.rows);
    if (part.width < 100 * s || part.height < 2 * lay.listPitch * s) return out;
    const cv::Mat ink = inkOf(bgrOf(frame)(part), s);
    auto lineOf = [&](double top) {
        const cv::Rect r = cv::Rect(cv::Point(X(lay.listLeft - 8), Y(top - 4)),
                                    cv::Point(X(lay.listLeft + lay.listEnd + 8), Y(top + CAP + 4))) -
                           part.tl();
        return lineAt(ink, r, s, 12.0);
    };
    static const double sureScore = envOr("SCMINER_FIVE_SCORE", 0.80);
    static const double sureMargin = envOr("SCMINER_FIVE_MARGIN", 0.3);
    double brightest = 0;
    g_listWords.clear();
    for (int row = 0; row < 7; row++) {
        const Line line = lineOf(lay.listTop + row * lay.listPitch);
        g_listWords += wordsOf(("row" + std::to_string(row)).c_str(), line, X(lay.listLeft) - part.x, s);
        // A line of the list begins with its share at the labels' edge. Over a
        // bright rock the ground's ink can join the share's word on its left
        // (6 to 12 units on the MOLE, 17 November 2026), and the inert rest's
        // long name scrolls back and forth in its column until it touches
        // the share ("100.00%NERT MATERIALS" one word). So the share's word is
        // the first that reaches past the edge by 3 - a speck can stand
        // before it, and the faint inert share breaks into pieces as short as
        // 4 of 41.78% ending 5.5 after the edge (the MOLE's pilot seat, 17
        // November 2026 08:54) - beginning up to 16 units before the edge or
        // 8 after it (the Golem's shares begin 2 to 6 after it, the others' 0
        // to 4); nothing is read left of the edge less 4; and where the word
        // runs on past where shares end, or ends short of them, the share ends
        // at the glyph that reads best as one.
        const double edgeOfLine = line.unit(X(lay.listLeft) - part.x);
        auto shareWordOf = [&](const Line& l, double edge) {
            for (size_t w = 0; w < l.words.size(); w++) {
                if (l.right(w) < edge + 3) continue;   // a speck before it
                if (l.left(w) <= edge + 8 && l.left(w) >= edge - 16) return (int)w;
                break;
            }
            return -1;
        };
        // The inert rest's share is drawn faint beside its white name, and
        // the line's ink, cut at 0.4 of its brightest, leaves of it a few
        // specks (41.78% as 12-14 and 25-31 on the Golem, 17 November 2026):
        // so where the line has no share, the share's own stretch is looked
        // at alone - if a name follows it on the line.
        const Line* sl = &line;
        Line shareOnly;
        double edge = edgeOfLine;
        int sw = shareWordOf(line, edge);
        if (sw < 0) {
            shareOnly = lineAt(ink,
                               cv::Rect(cv::Point(X(lay.listLeft - 8), Y(lay.listTop + row * lay.listPitch - 4)),
                                        cv::Point(X(lay.listLeft + lay.shareEndsTo + 6),
                                                  Y(lay.listTop + row * lay.listPitch + CAP + 4))) -
                                   part.tl(),
                               s, 12.0);
            const double e = shareOnly.unit(X(lay.listLeft) - part.x);
            const int w = shareWordOf(shareOnly, e);
            // a name: a word of some length right of where shares end
            bool named = false;
            for (size_t k = 0; k < line.words.size(); k++) {
                named = named || (line.left(k) > edgeOfLine + lay.shareEndsTo && line.right(k) - line.left(k) >= 12);
            }
            if (w >= 0 && named) {
                sl = &shareOnly;
                sw = w;
                edge = e;
            }
        }
        const int leftmost = (int)std::floor(edge - 4);
        if (sw < 0 || (sl == &line && (size_t)sw + 1 == line.words.size() && line.right((size_t)sw) <= edge + lay.shareEndsTo))
            break;
        FiveListLine entry;
        int shareEnd = sl->right((size_t)sw);
        Reading share;
        if (shareEnd >= edge + lay.shareEndsFrom && shareEnd <= edge + lay.shareEndsTo) {
            share = slide(*sl, shareEnd, SHARE, 2, sureScore, sureMargin, leftmost);
        } else {
            // joined to the name, or broken by a gap: every glyph that ends
            // where shares end is tried, and every fourth unit there (a per
            // cent sign touching the name's first letter is one piece with it)
            std::vector<int> ends;
            for (size_t w = (size_t)sw; w < sl->words.size() && sl->left(w) < edge + lay.shareEndsTo; w++) {
                for (const cv::Rect& g : sl->words[w]) ends.push_back(g.x + g.width);
            }
            for (double u = lay.shareEndsFrom; u <= lay.shareEndsTo; u += 4) ends.push_back((int)std::lround(edge + u));
            double best = -2;
            for (const int end : ends) {
                if (end < edge + lay.shareEndsFrom || end > edge + lay.shareEndsTo) continue;
                const Reading r = slide(*sl, end, SHARE, 2, sureScore, sureMargin, leftmost);
                const double score = fills(std::string(r.text.rbegin(), r.text.rend()), SHARE) ? r.least : r.least - 1;
                if (score > best) {
                    best = score;
                    share = r;
                    shareEnd = r.boxes.empty() ? end : r.boxes.back().x + r.boxes.back().width;
                }
            }
        }
        entry.shareText = share.text;
        entry.shareSure =
            share.sure && share.left <= (std::max)(sl->left((size_t)sw), (int)std::lround(edge + 4)) + 2;
        float v = 0;
        if (entry.shareSure && fiveShare(share.text, v)) entry.share = v;
        else entry.shareSure = false;
        // the quality, the line's last word: it ends at 229 to 233 on the
        // Prospector and the MOLE and at 234 to 241 on the Golem's larger
        // numerals (17 November 2026); well right of the share
        const int q = wordEndingAt(line, line.unit(X(lay.listLeft + lay.qualityEnd) - part.x), 3.5, 12.0);
        if (q >= 0 && line.left((size_t)q) > edgeOfLine + lay.shareEndsTo) {
            const int qRight = textRight(line, (size_t)q);
            const Reading quality = slide(line, qRight, QUALITY, 2, sureScore, sureMargin);
            entry.qualityText = quality.text;
            entry.qualitySure = quality.sure && quality.left <= line.left((size_t)q) + 2;
            if (entry.qualitySure && fiveWhole(quality.text, v)) entry.quality = (int)v;
            else if (entry.qualitySure && quality.text == "0") entry.quality = 0;
            else entry.qualitySure = false;
            // Not sure numeral by numeral: a quality is one of the values the
            // rock's ores come in, and its colour narrows them - green from
            // 700, white from 300 to 699. Taken where exactly one of those
            // fits every glyph at 0.80 or better.
            if (!entry.qualitySure && qualities && !qualities->empty() && quality.boxes.size() >= 3) {
                const cv::Rect word = line.inFrame(cv::Rect(cv::Point(quality.left, line.top),
                                                            cv::Point(qRight, line.bottom))) +
                                      part.tl();
                const bool green = greenOf(bgrOf(frame), word);
                int fits = 0, chosen = -1;
                for (int value : *qualities) {
                    if (green ? value < 700 : (value < 300 || value >= 700)) continue;
                    if (fitOf(line, qRight, std::to_string(value)) >= sureScore) {
                        fits++;
                        chosen = value;
                    }
                }
                if (fits == 1) {
                    entry.quality = chosen;
                    entry.qualitySure = true;
                    entry.byValues = true;
                }
            }
        }
        // how bright the share is: the inert rest's line is drawn faint
        double most = 0;
        const cv::Rect shareBox = cv::Rect(cv::Point((std::max)(share.left, sl->left((size_t)sw)), sl->top),
                                           cv::Point(shareEnd, sl->bottom)) &
                                  cv::Rect(0, 0, sl->ink.cols, sl->ink.rows);
        if (shareBox.area() > 0) cv::minMaxLoc(sl->ink(shareBox), nullptr, &most);
        entry.bright = most;
        brightest = (std::max)(brightest, most);
        const double top = lay.listTop + row * lay.listPitch;
        entry.name = (cv::Rect(cv::Point(X(lay.listLeft + lay.nameFrom), Y(top - 3)),
                               cv::Point(X(lay.listLeft + lay.nameTo), Y(top + CAP + 3)))) &
                     cv::Rect(0, 0, frame.cols, frame.rows);
        out.push_back(entry);
        // The list ends where its shares add up to the rock (cut after the
        // second place: 100 less a hundredth a line, to 100 - as
        // readRockContents takes them): a line under it is the ground's ink
        double sum = 0;
        bool all = true;
        for (const FiveListLine& e : out) {
            float p = 0;
            if (fiveShare(e.shareText, p)) sum += p;
            else all = false;
        }
        if (all && sum > 100.0 - 0.01 * (double)out.size() - 0.006 && sum < 100.006) break;
    }
    for (FiveListLine& entry : out) entry.dim = entry.bright < 0.7 * brightest;
    return out;
}

std::vector<int> fiveQualities(const std::string& rockOre) {
    auto same = [](const char* a, const std::string& b) {
        if (std::strlen(a) != b.size()) return false;
        for (size_t i = 0; i < b.size(); i++) {
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        }
        return true;
    };
    std::vector<int> out;
    for (int i = 0; i < ROCK_PART_COUNT; i++) {
        const RockPart& part = ROCK_PARTS[i];
        if (!same(part.rock, rockOre)) continue;
        for (int b = 0; b < ORE_BANDS_COUNT; b++) {
            if (!same(ORE_BANDS[b].ore, part.ore)) continue;
            for (int band = lowestQualityBand(part); band <= topQualityBand(part.qualityScale); band++) {
                out.push_back(ORE_BANDS[b].bands[band]);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

char zeroOrEight(const cv::Mat& cell) {
    const double c = countersOf(cell, cv::Rect(0, 0, cell.cols, cell.rows));
    return c >= zeroCounters() ? '0' : c <= EIGHT_COUNTERS ? '8' : 0;
}

int numeralsOff(const std::string& a, const std::string& b) {
    const size_t n = (std::max)(a.size(), b.size());
    int off = 0;
    for (size_t i = 0; i < n; i++) {
        const char x = i < a.size() ? a[a.size() - 1 - i] : 0;
        const char y = i < b.size() ? b[b.size() - 1 - i] : 0;
        if (x != y) off++;
    }
    return off;
}

cv::Rect fivePanelCut(const ScanPanel& panel, const cv::Size& frame) {
    if (panel.labels.x < 0 || panel.labelScale <= 0) return cv::Rect();
    const double s = panel.labelScale;
    const Layout& lay = layoutOf(panel.labels, frame);
    auto X = [&](double u) { return (int)std::lround(panel.labels.x + u * s); };
    auto Y = [&](double u) { return (int)std::lround(panel.labels.y + u * s); };
    // the turret's row is wide and low: its list stands right of the figures
    return cv::Rect(cv::Point(X(-60), Y((std::min)(-90.0, lay.volumeTop - 30))),
                    cv::Point(X(lay.listLeft + lay.scuEnd + 60), Y(lay.listTop + 8 * lay.listPitch))) &
           cv::Rect(0, 0, frame.width, frame.height);
}

}  // namespace SCMiner
