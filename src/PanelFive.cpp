#include "PanelFive.h"

#include "DevSwitch.h"
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
constexpr int COLUMN_W = 100;                // the label column's shape, from 3 before
constexpr int COLUMN_H = 72;                 // the labels and 3 above MASS
constexpr double COLUMN_INSET = 3.0;
constexpr int GLYPH_H = 16;                  // a glyph's cell: 2 above the figures' top,
constexpr int GLYPH_ABOVE = 2;               // their 12, and 2 under them
constexpr int CELL_W = 16;                   // ...as harvested, ending where the glyph ends
constexpr double WORD_GAP = 4.5;             // wider than this between two pieces: two words
constexpr int LINE_BORDER = 6;               // nothing round a line's ink

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

// The word of a line that ends at `end` (in the line's units); -1 for none
int wordEndingAt(const Line& line, double end) {
    for (int w = (int)line.words.size() - 1; w >= 0; w--) {
        if (std::abs(line.right((size_t)w) - end) <= 3.5) return w;
        if (line.right((size_t)w) < end - 3.5) break;
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
        for (int i = 0; i < FIVE_SHAPE_COUNT; i++) {
            const FiveShape& s = FIVE_SHAPES[i];
            if (s.glyph == 'L' || s.height != GLYPH_H || s.width < 2) continue;
            out.push_back({s.glyph, cv::Mat(s.height, s.width, CV_32F, const_cast<float*>(s.grey)).clone()});
        }
        return out;
    }();
    return known;
}

// How alike two glyphs' shapes are: their correlation over the part both
// cover, from their right edges; worked out once for every pair
double likeness(const Known& a, const Known& b) {
    static const std::vector<std::vector<double>> table = [] {
        const std::vector<Known>& known = knownGlyphs();
        std::vector<std::vector<double>> t(known.size(), std::vector<double>(known.size(), 1.0));
        for (size_t i = 0; i < known.size(); i++) {
            for (size_t j = 0; j < known.size(); j++) {
                const int w = (std::min)(known[i].cell.cols, known[j].cell.cols);
                cv::Mat x = known[i].cell(cv::Rect(known[i].cell.cols - w, 0, w, GLYPH_H)).clone();
                cv::Mat y = known[j].cell(cv::Rect(known[j].cell.cols - w, 0, w, GLYPH_H)).clone();
                x -= cv::mean(x)[0];
                y -= cv::mean(y)[0];
                const double n = cv::norm(x) * cv::norm(y);
                t[i][j] = n > 1e-9 ? x.dot(y) / n : 0;
            }
        }
        return t;
    }();
    const std::vector<Known>& known = knownGlyphs();
    const size_t i = (size_t)(&a - known.data()), j = (size_t)(&b - known.data());
    return i < table.size() && j < table.size() ? table[i][j] : 1.0;
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
    int left = 0;            // where the figure begins, in the line
    std::vector<cv::Rect> boxes;   // each glyph's cell, in the line
};

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
// taken. It ends where the format is whole and no ink follows. `right` is
// where its last glyph ends in the line.
Reading slide(const Line& line, int right, const Format& format, int firstSlack, double sureScore, double sureMargin) {
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
        // how much nearer its own shape the glyph is than each other glyph's
        for (const auto& [other, score] : tried) {
            if (other->c == bestC) continue;
            const double apart = 2.0 * (1.0 - likeness(*mine, *other));
            if (apart > 1e-6) out.margin = (std::min)(out.margin, (best - score) / apart);
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
    out.sure = fills(backwards, format) && out.least >= sureScore && out.margin >= sureMargin;
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

// A word's pieces for the harvest, named by the truth: where the pieces are
// fewer than the truth's glyphs - numerals the glow ran together - the widest
// is cut where it is thinnest until they are as many. Each cell is GLYPH_H
// tall and CELL_W wide, ending where its piece does.
void harvestWords(const char* field, const Line& line, int from, int to, const std::string& truth) {
    Harvest* h = harvest();
    if (!h || from < 0 || to < from || to >= (int)line.words.size() || truth.empty()) return;
    std::vector<cv::Rect> glyphs;
    for (int w = from; w <= to; w++) glyphs.insert(glyphs.end(), line.words[(size_t)w].begin(), line.words[(size_t)w].end());
    cv::Mat on;
    cv::threshold(line.ink, on, 0.4 * line.most, 1, cv::THRESH_BINARY);
    while (glyphs.size() < truth.size()) {
        size_t widest = 0;
        for (size_t i = 1; i < glyphs.size(); i++) {
            if (glyphs[i].width > glyphs[widest].width) widest = i;
        }
        const cv::Rect r = glyphs[widest];
        if (r.width < 8) break;
        int at = -1;
        double least = 1e9;
        for (int x = r.width * 3 / 10; x <= r.width * 7 / 10; x++) {
            const double sum = cv::sum(on(cv::Rect(r.x + x, r.y, 1, r.height)))[0];
            if (sum < least) {
                least = sum;
                at = x;
            }
        }
        if (at <= 0) break;
        glyphs[widest] = cv::Rect(r.x, r.y, at, r.height);
        glyphs.insert(glyphs.begin() + (long long)widest + 1, cv::Rect(r.x + at, r.y, r.width - at, r.height));
    }
    if (glyphs.size() != truth.size()) return;
    for (size_t i = 0; i < glyphs.size(); i++) {
        const int right = glyphs[i].x + glyphs[i].width;
        const cv::Rect cell(right - CELL_W, line.top - GLYPH_ABOVE, CELL_W, GLYPH_H);
        if ((cell & cv::Rect(0, 0, line.ink.cols, line.ink.rows)) != cell) continue;
        const int advance = i > 0 ? right - (glyphs[i - 1].x + glyphs[i - 1].width) : 0;
        h->out << h->file << "," << field << "," << i << "," << truth[i] << ",?," << advance << "," << glyphs[i].width;
        for (int k = 0; k < CELL_W * GLYPH_H; k++) h->out << "," << line.ink(cell).at<float>(k / CELL_W, k % CELL_W);
        h->out << "\n";
    }
    h->out << std::flush;
}

// A figure as read, drawn: green where sure
void drawRead(const Line& line, const Reading& read) {
    for (const cv::Rect& b : read.boxes) drawBox(line.inFrame(b), read.sure ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255));
}

// ...and for the harvest, a figure read as the truth has it: each glyph's
// cell where the reading put it (SCMINER_FIVE_SELF: only these)
void harvestRead(const char* field, const Line& line, const Reading& read, const std::string& truth) {
    Harvest* h = harvest();
    if (!h || truth.empty() || read.text != truth || read.boxes.size() != truth.size()) return;
    for (size_t i = 0; i < read.boxes.size(); i++) {
        const int right = read.boxes[i].x + read.boxes[i].width;
        const cv::Rect cell(right - CELL_W, line.top - GLYPH_ABOVE, CELL_W, GLYPH_H);
        if ((cell & cv::Rect(0, 0, line.ink.cols, line.ink.rows)) != cell) continue;
        h->out << h->file << "," << field << "," << i << "," << truth[i] << ",=," << read.boxes[i].width << ","
               << read.boxes[i].width;
        for (int k = 0; k < CELL_W * GLYPH_H; k++) h->out << "," << line.ink(cell).at<float>(k / CELL_W, k % CELL_W);
        h->out << "\n";
    }
    h->out << std::flush;
}

double envOr(const char* name, double fallback) {
    const char* set = DEV_ENV(name);
    return set ? std::atof(set) : fallback;
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
    (void)lines;
    if (frame.empty() || columnShape().empty()) return false;
    const cv::Mat bgr = bgrOf(frame);
    const double screen = frame.rows / 1440.0;
    const cv::Rect band = bandOf(frame.size());
    const cv::Mat ink = [&] {
        cv::Mat all = cv::Mat::zeros(bgr.size(), CV_32F);
        inkOf(bgr(band), screen).copyTo(all(band));
        return all;
    }();
    std::vector<double> scales;
    for (double f = 0.78; f <= 1.27; f += 0.06) scales.push_back(screen * f);
    const Column column = findColumn(ink, band, scales);
    static const double bar = envOr("SCMINER_FIVE_BAR", 0.5);
    char note[320];
    std::snprintf(note, sizeof(note), "five: column %.2f at %d,%d x%.3f", column.score, column.labels.x, column.labels.y,
                  column.scale);
    panel.note = note;
    if (column.score < bar) return false;
    const double s = column.scale;
    const int L = column.labels.x, T = column.labels.y;
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
    // on the MASS line
    // on the MASS line - or the column is 5.0's beyond doubt, and the
    // panel is one whose mass is lost to the ground behind it
    static const double sureColumn = envOr("SCMINER_FIVE_SURE_COLUMN", 0.85);
    const Line massLine = lineOf(0, 60, FIGURES_END + 8);
    if (endingAt(massLine, FIGURES_END) < 0 && column.score < sureColumn) {
        panel.note += "; nothing at the right edge";
        return false;
    }
    startDrawing(bgr, cv::Rect(cv::Point(X(-10), Y(-55)), cv::Point(X(FIGURES_END + 15), Y(LIST_TOP + 6 * LIST_PITCH))));
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
        f.read = slide(line, line.right((size_t)f.word), format, 2, sureScore, sureMargin);
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
        delta = figure(line, DELTA_END, DELTA);
        return delta.word == before[0] && delta.read.sure;
    };
    const Line resLine = lineOf(ROW_PITCH, 100, FIGURES_END + 8);
    const Line instLine = lineOf(2 * ROW_PITCH, 100, FIGURES_END + 8);
    const Figure mass = figure(massLine, FIGURES_END, WHOLE);
    const Figure res = figure(resLine, FIGURES_END, PERCENT);
    const Figure inst = figure(instLine, FIGURES_END, TWO_PLACES);
    Figure resDelta, instDelta;
    const bool resOwn = modifier(resLine, res, resDelta);
    const bool instOwn = modifier(instLine, inst, instDelta);
    // the volume, left of SCU at the line's end
    Figure volume, scuWord;
    const Line volumeLine = lineOf(VOLUME_TOP, 128, FIGURES_END + 8);
    scuWord.word = endingAt(volumeLine, FIGURES_END);
    if (scuWord.word >= 0) {
        scuWord.read = slide(volumeLine, volumeLine.right((size_t)scuWord.word), SCU_WORD, 2, sureScore, sureMargin);
        drawRead(volumeLine, scuWord.read);
        if (scuWord.read.sure && scuWord.read.text == "SCU") {
            // S's shape holds the space before it, so the volume ends where it begins
            volume.read = slide(volumeLine, scuWord.read.left, VOLUME, 3, sureScore, sureMargin);
            volume.word = scuWord.word;
            drawRead(volumeLine, volume.read);
        }
    }
    // the bench's harvest: each figure's word by the truth, cut by its pieces
    // or, with SCMINER_FIVE_SELF, as read
    if (harvest()) {
        static const bool self = DEV_ENV("SCMINER_FIVE_SELF") != nullptr;
        auto take = [&](const char* field, const Line& line, int word, const Format& format, const std::string& truth) {
            if (word < 0) return;
            if (!self) {
                harvestWords(field, line, word, word, truth);
                return;
            }
            const Reading r = slide(line, line.right((size_t)word), format, 2, sureScore, sureMargin);
            harvestRead(field, line, r, truth);
        };
        take("mass", massLine, endingAt(massLine, FIGURES_END), WHOLE, truthOf(2));
        take("res", resLine, endingAt(resLine, FIGURES_END), PERCENT, truthOf(4));
        take("res", resLine, endingAt(resLine, DELTA_END), DELTA, truthOf(3));
        take("inst", instLine, endingAt(instLine, FIGURES_END), TWO_PLACES, truthOf(6));
        take("inst", instLine, endingAt(instLine, DELTA_END), DELTA, truthOf(5));
        if (scuWord.word >= 0 && !truthOf(7).empty()) {
            if (self) {
                harvestRead("scu", volumeLine, volume.read, truthOf(7));
                harvestRead("scu", volumeLine, scuWord.read, "SCU");
            } else {
                harvestWords("scu", volumeLine, 0, scuWord.word, truthOf(7) + "SCU");
            }
        }
        std::vector<std::string> parts;
        {
            std::stringstream ss(truthOf(8));
            std::string p;
            while (std::getline(ss, p, ';')) parts.push_back(p);
        }
        for (int row = 0; row < (int)parts.size(); row++) {
            const Line line = lineOf(LIST_TOP + row * LIST_PITCH, -8, FIGURES_END + 8);
            if (line.words.size() < 2) continue;
            const std::string& p = parts[(size_t)row];
            const size_t a = p.find('|'), b = p.rfind('|');
            if (std::abs(line.left(0) - line.unit(L)) <= 4) take("share", line, 0, SHARE, p.substr(0, a) + "%");
            take("quality", line, endingAt(line, QUALITY_END), QUALITY, p.substr(b + 1));
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
        std::snprintf(text, sizeof(text), "%s%s(%.2f/%.2f)", f.read.text.c_str(), f.read.sure ? "" : "?", f.read.least,
                      f.read.margin);
        return std::string(text);
    };
    panel.note += "; read " + shown(mass) + " " + shown(resDelta) + "/" + shown(res) + " " + shown(instDelta) + "/" +
                  shown(inst) + " " + shown(volume);
    auto delta = [](float figure, float d) { return figure < 0 ? std::string("-") : std::to_string((int)std::lround(d)); };
    panel.note += "; delta r" + delta(r.resistance, r.resistanceDelta) + " i" + delta(r.instability, r.instabilityDelta);
    panel.labels = column.labels;
    panel.labelScale = s;
    panel.labelGlyph = FIVE_PANEL;
    panel.confidence = column.score;
    panel.box = cv::Rect(cv::Point(X(-6), Y(-50)), cv::Point(X(FIGURES_END + 12), Y(LIST_TOP + 5 * LIST_PITCH))) &
                cv::Rect(0, 0, frame.cols, frame.rows);
    panel.oreRow = cv::Rect(cv::Point(X(-4), Y(ORE_TOP - 3)), cv::Point(X(FIGURES_END + 4), Y(ORE_TOP + CAP + 3))) &
                   cv::Rect(0, 0, frame.cols, frame.rows);
    endDrawing();
    return true;
}

cv::Rect fiveContentsRegion(const ScanPanel& panel, const cv::Size& frame) {
    if (panel.labels.x < 0 || panel.labelScale <= 0) return cv::Rect();
    const double s = panel.labelScale;
    const cv::Rect wanted(cv::Point((int)std::lround(panel.labels.x - 12 * s), (int)std::lround(panel.labels.y + (LIST_TOP - 8) * s)),
                          cv::Point((int)std::lround(panel.labels.x + (FIGURES_END + 12) * s),
                                    (int)std::lround(panel.labels.y + (LIST_TOP + 7 * LIST_PITCH) * s)));
    return wanted & cv::Rect(0, 0, frame.width, frame.height);
}

std::vector<FiveListLine> readFiveList(const cv::Mat& frame, const ScanPanel& panel) {
    std::vector<FiveListLine> out;
    if (frame.empty() || panel.labels.x < 0 || panel.labelScale <= 0 || knownGlyphs().empty()) return out;
    const double s = panel.labelScale;
    const int L = panel.labels.x, T = panel.labels.y;
    auto X = [&](double u) { return (int)std::lround(L + u * s); };
    auto Y = [&](double u) { return (int)std::lround(T + u * s); };
    // the list's ink, and its lines cut out of it
    const cv::Rect part = cv::Rect(cv::Point(X(-16), Y(LIST_TOP - 12)), cv::Point(X(FIGURES_END + 16), Y(LIST_TOP + 7.5 * LIST_PITCH))) &
                          cv::Rect(0, 0, frame.cols, frame.rows);
    if (part.width < 100 * s || part.height < 2 * LIST_PITCH * s) return out;
    const cv::Mat ink = inkOf(bgrOf(frame)(part), s);
    auto lineOf = [&](double top) {
        const cv::Rect r = cv::Rect(cv::Point(X(-8), Y(top - 4)), cv::Point(X(FIGURES_END + 8), Y(top + CAP + 4))) - part.tl();
        return lineAt(ink, r, s, 12.0);
    };
    static const double sureScore = envOr("SCMINER_FIVE_SCORE", 0.80);
    static const double sureMargin = envOr("SCMINER_FIVE_MARGIN", 0.3);
    double brightest = 0;
    for (int row = 0; row < 7; row++) {
        const Line line = lineOf(LIST_TOP + row * LIST_PITCH);
        // a line of the list begins with its share at the labels' edge
        if (line.words.size() < 2 || std::abs(line.left(0) - line.unit(L - part.x)) > 4) break;
        FiveListLine entry;
        const Reading share = slide(line, line.right(0), SHARE, 2, sureScore, sureMargin);
        entry.shareText = share.text;
        entry.shareSure = share.sure && share.left <= line.left(0) + 2;
        float v = 0;
        if (entry.shareSure && fiveShare(share.text, v)) entry.share = v;
        else entry.shareSure = false;
        const int q = wordEndingAt(line, line.unit(X(QUALITY_END) - part.x));
        if (q > 0) {
            const Reading quality = slide(line, line.right((size_t)q), QUALITY, 2, sureScore, sureMargin);
            entry.qualityText = quality.text;
            entry.qualitySure = quality.sure && quality.left <= line.left((size_t)q) + 2;
            if (entry.qualitySure && fiveWhole(quality.text, v)) entry.quality = (int)v;
            else if (entry.qualitySure && quality.text == "0") entry.quality = 0;
            else entry.qualitySure = false;
        }
        // how bright the share is: the inert rest's line is drawn faint
        double most = 0;
        const cv::Rect shareBox = cv::Rect(cv::Point(line.left(0), line.top), cv::Point(line.right(0), line.bottom)) &
                                  cv::Rect(0, 0, line.ink.cols, line.ink.rows);
        if (shareBox.area() > 0) cv::minMaxLoc(line.ink(shareBox), nullptr, &most);
        entry.bright = most;
        brightest = (std::max)(brightest, most);
        entry.name = (cv::Rect(cv::Point(X(NAME_FROM), Y(LIST_TOP + row * LIST_PITCH - 3)),
                               cv::Point(X(NAME_TO), Y(LIST_TOP + row * LIST_PITCH + CAP + 3)))) &
                     cv::Rect(0, 0, frame.cols, frame.rows);
        out.push_back(entry);
    }
    for (FiveListLine& entry : out) entry.dim = entry.bright < 0.7 * brightest;
    return out;
}

}  // namespace SCMiner
