#include "LaserRange.h"

#include "GaugeDigits.h"
#include "ScanPanel.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace SCMiner {

namespace {

constexpr double PI = 3.14159265358979323846;

// Everything below is in label units - the scan panel's label size, one
// pixel at size 1 on a 1440-line screen - about the triangle's centre.
// Upright, the triangle points left and the figure follows it on its line:
// the numerals 9.2 units high, their cells 7.4 apart.
constexpr double PX = 2.5;                    // pixels to a unit, cut upright
constexpr double STRIP_X = -6.0, STRIP_Y = -8.0, STRIP_W = 36.0, STRIP_H = 16.0;
constexpr double CELL_Y = -5.6;               // the cells' top
constexpr double ADVANCE = 7.4;

// One HUD's gauge.
struct GaugeHud {
    const char* name;
    const char* glyphs;       // the label columns of its scan panel
    cv::Rect2d window;        // where the triangle can stand, from the labels
    double firstX;            // the first numeral's cell, right of the centre
    double tiltLo, tiltHi;    // how far the figure stands turned, degrees
    // Where the triangle stands for a figure, and how far off that a reading
    // may be: the panel's labels are a long way off, and their place and
    // size come with an error the gauge's place multiplies.
    double (*offTrack)(const cv::Point2d& unitsFromLabels, int metres);
    double tolerance;
};

// The Prospector: the triangle rides a circle about a point 717.7 units
// left of the labels and 38.4 above them, 143.2 across - fitted against the
// MAX mark in September, with a Helix I - and the square root of the metres
// grows with the angle: 4 m at -130.7 degrees, 45 m at -161.5, 82 m at
// -176.8, 113 m past -180 (68 figures read by eye, 1.0 degrees rms from the
// labels' place). The current patch's HUD, whose panel writes RESISTANCE:
// and INSTABILITY: out, puts the same circle 21 units further left of the
// labels (71 figures from 19 to 115 m off recordings of 29.10.2026, 1.5
// degrees rms): either place will do.
double prospectorOffTrack(const cv::Point2d& p, int metres) {
    const double expected = (-17.9876 - std::sqrt((double)metres)) / 0.15297;
    double off = 1e9;
    for (const double centreX : {-717.7, -738.7}) {
        const double angle = std::atan2(p.y + 38.4, p.x - centreX) * 180.0 / PI;
        off = std::min(off, std::abs(std::remainder(angle - expected, 360.0)));
    }
    return off;
}

const GaugeHud HUDS[] = {
    {"prospector", "LM", cv::Rect2d(-910, -170, 125, 155), 8.5, 0.0, 56.0, prospectorOffTrack, 5.0},
};

const GaugeHud* hudOf(char glyph) {
    if (!glyph) return nullptr;
    for (const GaugeHud& h : HUDS) {
        for (const char* g = h.glyphs; *g; g++) {
            if (*g == glyph) return &h;
        }
    }
    return nullptr;
}

// The digits' pictures, each with its mean taken off and its length, for
// the likeness below - and the soft ones where there are.
struct DigitPictures {
    cv::Mat centred[10];
    double norm[10];
    cv::Mat soft[10];  // empty where the numeral has none
    double softNorm[10] = {};
    DigitPictures() {
        const auto take = [](const float* values, cv::Mat& centred, double& norm) {
            cv::Mat m(GAUGE_DIGIT_H, GAUGE_DIGIT_W, CV_32F);
            std::copy(values, values + GAUGE_DIGIT_W * GAUGE_DIGIT_H, m.ptr<float>(0));
            const double mean = cv::mean(m)[0];
            centred = m - mean;
            norm = std::sqrt(centred.dot(centred));
        };
        for (int d = 0; d < 10; d++) take(GAUGE_DIGITS[d], centred[d], norm[d]);
        for (int i = 0; GAUGE_SOFT_NUMERALS[i]; i++) {
            const int d = GAUGE_SOFT_NUMERALS[i] - '0';
            take(GAUGE_SOFT_DIGITS[i], soft[d], softNorm[d]);
        }
    }
};

const DigitPictures& digitPictures() {
    static const DigitPictures pictures;
    return pictures;
}

double median(std::vector<float>& v) {
    if (v.empty()) return 0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

// How much each pixel is the figure's colour rather than the ground's, 0 to
// 255: the ground is the median colour right of the triangle, the figure's
// the median of the pixels whose hue stands furthest from the ground's. The
// figures are green, yellow, orange or red; the rock behind them grey, or
// red where its own colour is weaker than theirs, and space blue - bluer
// than a red figure is red, so the most coloured pixels there are the
// ground's own.
cv::Mat inkAbout(const cv::Mat& frame, const cv::Point2d& centre, double s, cv::Point& corner) {
    const cv::Rect want((int)std::floor(centre.x - 6 * s), (int)std::floor(centre.y - 14 * s), (int)std::ceil(40 * s),
                        (int)std::ceil(48 * s));
    const cv::Rect box = want & cv::Rect(0, 0, frame.cols, frame.rows);
    corner = box.tl();
    if (box.width < 8 || box.height < 8) return cv::Mat();
    const cv::Mat bgr = frame(box);
    const int from = std::max(0, (int)std::lround(centre.x + 4 * s) - box.x);
    std::vector<float> b, g, r;
    const size_t n = (size_t)(box.width - std::min(from, box.width)) * box.height;
    b.reserve(n);
    g.reserve(n);
    r.reserve(n);
    for (int y = 0; y < bgr.rows; y++) {
        const cv::Vec3b* px = bgr.ptr<cv::Vec3b>(y);
        for (int x = from; x < bgr.cols; x++) {
            b.push_back(px[x][0]);
            g.push_back(px[x][1]);
            r.push_back(px[x][2]);
        }
    }
    if (b.size() < 20) return cv::Mat();
    // (median() reorders what it is given, and b, g and r must stay in step)
    std::vector<float> mb = b, mg = g, mr = r;
    const double ground[3] = {median(mb), median(mg), median(mr)};
    // hue without lightness: red against green, yellow against blue - each
    // pixel's distance from the ground's, and the pixels from the 94th
    // percentile of that on
    const double groundRg = ground[2] - ground[1], groundYb = (ground[2] + ground[1]) / 2 - ground[0];
    std::vector<float> apart(b.size());
    for (size_t i = 0; i < b.size(); i++) {
        const double rg = r[i] - g[i] - groundRg, yb = (r[i] + g[i]) / 2 - b[i] - groundYb;
        apart[i] = (float)std::sqrt(rg * rg + yb * yb);
    }
    std::vector<float> sorted = apart;
    const size_t at = (size_t)(0.94 * (double)(sorted.size() - 1));
    std::nth_element(sorted.begin(), sorted.begin() + at, sorted.end());
    const float cut = sorted[at];
    std::vector<float> cb, cg, cr;
    for (size_t i = 0; i < apart.size(); i++) {
        if (apart[i] >= cut) {
            cb.push_back(b[i]);
            cg.push_back(g[i]);
            cr.push_back(r[i]);
        }
    }
    const double ink[3] = {median(cb), median(cg), median(cr)};
    const double span = std::max(1.0, std::sqrt((ink[0] - ground[0]) * (ink[0] - ground[0]) +
                                                (ink[1] - ground[1]) * (ink[1] - ground[1]) +
                                                (ink[2] - ground[2]) * (ink[2] - ground[2])));
    cv::Mat out(bgr.size(), CV_32F);
    for (int y = 0; y < bgr.rows; y++) {
        const cv::Vec3b* px = bgr.ptr<cv::Vec3b>(y);
        float* o = out.ptr<float>(y);
        for (int x = 0; x < bgr.cols; x++) {
            double dg = 0, di = 0;
            for (int c = 0; c < 3; c++) {
                dg += (px[x][c] - ground[c]) * (px[x][c] - ground[c]);
                di += (px[x][c] - ink[c]) * (px[x][c] - ink[c]);
            }
            const double v = (std::sqrt(dg) - std::sqrt(di)) / span;
            o[x] = (float)(255.0 * std::min(1.0, std::max(0.0, v)));
        }
    }
    return out;
}

// The ink about the triangle cut upright at PX pixels to the unit: the strip
// STRIP_X..+STRIP_W along the figure's line, STRIP_Y..+STRIP_H across it.
cv::Mat upright(const cv::Mat& ink, const cv::Point& corner, const cv::Point2d& centre, double s, double tilt) {
    const double a = tilt * PI / 180.0, c = std::cos(a), sn = std::sin(a);
    const cv::Matx23d toInk(c * s / PX, -sn * s / PX, centre.x - corner.x + (c * STRIP_X - sn * STRIP_Y) * s,
                            sn * s / PX, c * s / PX, centre.y - corner.y + (sn * STRIP_X + c * STRIP_Y) * s);
    cv::Mat out;
    cv::warpAffine(ink, out, cv::Mat(toInk), cv::Size((int)(STRIP_W * PX), (int)(STRIP_H * PX)),
                   cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, cv::Scalar(0));
    return out;
}

// The tilt at which the figure's ink lies in the numerals' band rather than
// above or below it, in steps of two degrees.
double levelTilt(const cv::Mat& ink, const cv::Point& corner, const cv::Point2d& centre, double s, const GaugeHud& hud) {
    const int x0 = (int)((hud.firstX - 1 - STRIP_X) * PX), x1 = (int)((hud.firstX + 3 * ADVANCE - STRIP_X) * PX);
    const int b0 = (int)((CELL_Y + 0.5 - STRIP_Y) * PX), b1 = (int)((CELL_Y + 9.0 - STRIP_Y) * PX);
    double best = -1e18, bestTilt = hud.tiltLo;
    for (double t = hud.tiltLo; t <= hud.tiltHi + 1e-6; t += 2.0) {
        const cv::Mat strip = upright(ink, corner, centre, s, t);
        const cv::Mat part = strip.colRange(std::max(0, x0), std::min(strip.cols, x1));
        const double inside = cv::sum(part.rowRange(b0, b1))[0];
        const double outside = cv::sum(part.rowRange(0, b0))[0] + cv::sum(part.rowRange(b1, part.rows))[0];
        const double v = inside - 1.5 * outside;
        if (v > best) {
            best = v;
            bestTilt = t;
        }
    }
    return bestTilt;
}

struct Numeral {
    int digit = -1;
    double likeness = -1;   // normalised cross-correlation with its picture
    double lead = 0;        // ...over the next likeliest numeral's
    double mass = 0;        // the cell's mean ink
};

// A soft picture is like any blob of its size, so it counts for this much
// less than a sharp one: enough that a filled 6 or 0 on grey rock does not
// read as a soft 4, not so much that a red 43 on blue goes unread.
constexpr double SOFT_PENALTY = 0.08;

// The numeral in cell i of the upright strip, its place searched a unit
// and a bit either way.
Numeral numeralAt(const cv::Mat& strip, int i, double firstX) {
    const DigitPictures& pictures = digitPictures();
    double best[10];
    std::fill(best, best + 10, -2.0);
    Numeral out;
    cv::Mat cell;
    for (int sy = -3; sy <= 3; sy++) {
        for (int sx = -3; sx <= 3; sx++) {
            const double x = (firstX + i * ADVANCE + sx * 0.4 - STRIP_X) * PX;
            const double y = (CELL_Y + sy * 0.4 - STRIP_Y) * PX;
            const cv::Matx23d shift(1, 0, -x, 0, 1, -y);
            cv::warpAffine(strip, cell, cv::Mat(shift), cv::Size(GAUGE_DIGIT_W, GAUGE_DIGIT_H), cv::INTER_LINEAR,
                           cv::BORDER_CONSTANT, cv::Scalar(0));
            const double mean = cv::mean(cell)[0];
            if (sx == 0 && sy == 0) out.mass = mean;
            const cv::Mat centred = cell - mean;
            const double norm = std::sqrt(centred.dot(centred));
            if (norm < 1e-6) continue;
            for (int d = 0; d < 10; d++) {
                const double v = centred.dot(pictures.centred[d]) / (norm * pictures.norm[d]);
                if (v > best[d]) best[d] = v;
                if (pictures.soft[d].empty()) continue;
                const double w = centred.dot(pictures.soft[d]) / (norm * pictures.softNorm[d]) - SOFT_PENALTY;
                if (w > best[d]) best[d] = w;
            }
        }
    }
    int first = 0;
    for (int d = 1; d < 10; d++) {
        if (best[d] > best[first]) first = d;
    }
    double second = -2.0;
    for (int d = 0; d < 10; d++) {
        if (d != first && best[d] > second) second = best[d];
    }
    out.digit = first;
    out.likeness = best[first];
    out.lead = best[first] - second;
    return out;
}

// A numeral reads when it is this like its picture and this far ahead of
// the next likeliest; the figure ends at a cell that is like none of them or
// holds little ink against the first. Anything between is a numeral not sure
// which - and a figure with one of those in it is not read at all.
constexpr double LIKE_ENOUGH = 0.80;
constexpr double LEAD_ENOUGH = 0.06;
constexpr double LIKE_NONE = 0.60;
constexpr double MASS_NONE = 0.35;

}  // namespace

LaserRangeLook readLaserRange(const cv::Mat& frame, const ScanPanel& panel) {
    LaserRangeLook look;
    if (frame.empty() || frame.type() != CV_8UC3) {
        look.note = "no frame";
        return look;
    }
    if (panel.labels.x < 0 || panel.labelScale <= 0) {
        look.note = "no labels";
        return look;
    }
    const GaugeHud* hud = hudOf(panel.labelGlyph);
    if (!hud) {
        look.note = std::string("no gauge known beside the '") + (panel.labelGlyph ? panel.labelGlyph : '?') + "' labels";
        return look;
    }
    const double s = panel.labelScale;
    const cv::Rect window = cv::Rect((int)std::floor(panel.labels.x + hud->window.x * s),
                                     (int)std::floor(panel.labels.y + hud->window.y * s),
                                     (int)std::ceil(hud->window.width * s), (int)std::ceil(hud->window.height * s)) &
                            cv::Rect(0, 0, frame.cols, frame.rows);
    if (window.width < 10 || window.height < 10) {
        look.note = "gauge off the screen";
        return look;
    }

    // The triangle: white - every channel bright and close together - and
    // filled, the size of one, and brighter than what is round it.
    const cv::Mat part = frame(window);
    cv::Mat white(part.size(), CV_8U), luma(part.size(), CV_8U);
    for (int y = 0; y < part.rows; y++) {
        const cv::Vec3b* px = part.ptr<cv::Vec3b>(y);
        uchar* w = white.ptr<uchar>(y);
        uchar* l = luma.ptr<uchar>(y);
        for (int x = 0; x < part.cols; x++) {
            const int lo = std::min({px[x][0], px[x][1], px[x][2]}), hi = std::max({px[x][0], px[x][1], px[x][2]});
            w[x] = (lo >= 185 && hi - lo <= 55) ? 255 : 0;
            l[x] = (uchar)((px[x][0] + px[x][1] + px[x][2]) / 3);
        }
    }
    cv::Mat labelsOf, stats, centroids;
    const int n = cv::connectedComponentsWithStats(white, labelsOf, stats, centroids, 8, CV_32S);
    struct Candidate {
        cv::Point2d centre;
        double off;
    };
    std::vector<Candidate> candidates;
    for (int i = 1; i < n; i++) {
        const int w = stats.at<int>(i, cv::CC_STAT_WIDTH), h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        const double units = area / (s * s);
        if (units < 14 || units > 55 || w > 13 * s || h > 13 * s || w < 3 || h < 3) continue;
        // about as wide as high, turned as it may be: a bracket or a 1 of a
        // marker's text nearby is under two-thirds
        if (w < 0.7 * h) continue;
        const double fill = (double)area / (w * h);
        if (fill < 0.40 || fill > 0.85) continue;
        // brighter than a ring two pixels round it
        const cv::Rect box(stats.at<int>(i, cv::CC_STAT_LEFT), stats.at<int>(i, cv::CC_STAT_TOP), w, h);
        const cv::Rect around = cv::Rect(box.x - 2, box.y - 2, box.width + 4, box.height + 4) & cv::Rect(0, 0, part.cols, part.rows);
        double inside = 0, ring = 0;
        int inCount = 0, ringCount = 0;
        for (int y = around.y; y < around.y + around.height; y++) {
            const int* lab = labelsOf.ptr<int>(y);
            const uchar* l = luma.ptr<uchar>(y);
            for (int x = around.x; x < around.x + around.width; x++) {
                if (lab[x] == i) {
                    inside += l[x];
                    inCount++;
                } else if (!white.at<uchar>(y, x)) {
                    ring += l[x];
                    ringCount++;
                }
            }
        }
        if (inCount == 0 || ringCount == 0 || inside / inCount - ring / ringCount < 30) continue;
        Candidate c;
        c.centre = cv::Point2d(window.x + centroids.at<double>(i, 0), window.y + centroids.at<double>(i, 1));
        c.off = std::abs(units - 30.0);
        candidates.push_back(c);
    }
    if (candidates.empty()) {
        look.note = "no pointer";
        return look;
    }
    // the likeliest first - the size of a triangle - and four at most: in
    // the window there is only the one, and a busy ground adds specks
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.off < b.off; });
    if (candidates.size() > 4) candidates.resize(4);

    std::string tried;
    for (const Candidate& c : candidates) {
        cv::Point corner;
        const cv::Mat ink = inkAbout(frame, c.centre, s, corner);
        if (ink.empty()) continue;
        const double tilt = levelTilt(ink, corner, c.centre, s, *hud);
        const cv::Mat strip = upright(ink, corner, c.centre, s, tilt);
        Numeral numerals[3];
        for (int i = 0; i < 3; i++) numerals[i] = numeralAt(strip, i, hud->firstX);
        std::string text;
        bool unsure = false;
        double least = 1, lead = 1;
        for (int i = 0; i < 3; i++) {
            const Numeral& u = numerals[i];
            if (i > 0 && (u.likeness < LIKE_NONE || u.mass < MASS_NONE * numerals[0].mass)) break;
            if (u.likeness < LIKE_ENOUGH || u.lead < LEAD_ENOUGH) {
                unsure = true;
                break;
            }
            text += (char)('0' + u.digit);
            least = std::min(least, u.likeness);
            lead = std::min(lead, u.lead);
        }
        char trace[160];
        std::snprintf(trace, sizeof(trace), "%s%.0f,%.0f t%.0f [%d:%.2f/%.2f %d:%.2f/%.2f %d:%.2f/%.2f]", tried.empty() ? "" : "; ",
                      c.centre.x, c.centre.y, tilt, numerals[0].digit, numerals[0].likeness, numerals[0].lead,
                      numerals[1].digit, numerals[1].likeness, numerals[1].lead, numerals[2].digit, numerals[2].likeness,
                      numerals[2].lead);
        tried += trace;
        if (unsure || text.empty() || text[0] == '0') continue;
        const int metres = std::atoi(text.c_str());
        const cv::Point2d fromLabels((c.centre.x - panel.labels.x) / s, (c.centre.y - panel.labels.y) / s);
        const double off = hud->offTrack(fromLabels, metres);
        if (off > hud->tolerance) {
            char why[64];
            std::snprintf(why, sizeof(why), " %d m off its place by %.1f", metres, off);
            tried += why;
            continue;
        }
        look.metres = metres;
        look.pointer = cv::Point2f((float)c.centre.x, (float)c.centre.y);
        look.tilt = (float)tilt;
        look.least = (float)least;
        look.lead = (float)lead;
        look.note = std::string(hud->name) + " " + text + " m: " + tried;
        return look;
    }
    look.note = std::string(hud->name) + " not read: " + tried;
    return look;
}

}  // namespace SCMiner
