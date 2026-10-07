#include "LoadoutReader.h"

#include "DevSwitch.h"
#include "MiningHardware.h"
#include "PanelShapes.h"
#include "ScanPanel.h"
#include "TextLineReader.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>

namespace SCMiner {
namespace {

// Where the laser's table can stand: left of the middle, and low. On the
// Prospector's HUD, which is fixed, its first word stands 0.53 screen
// heights left of the middle and 0.32 below it (1440 lines); the Golem's
// is drawn in the world and sways with the view - 0.63 left and 0.19
// below at one point. Sized by the screen's height, as the HUD is.
cv::Rect laserBand(const cv::Size& frame) {
    const double h = frame.height;
    const double cx = frame.width / 2.0, cy = frame.height / 2.0;
    const cv::Rect band((int)std::lround(cx - 0.95 * h), (int)std::lround(cy - 0.20 * h),
                        (int)std::lround(0.80 * h), (int)std::lround(0.70 * h));
    return band & cv::Rect(0, 0, frame.width, frame.height);
}

// The Prospector's HUD stands at size 1 at 1440 lines; the Golem's runs
// from 1.15 to 1.8 as the view moves
const std::vector<double>& tableScales(int rows) {
    static std::vector<double> scales;
    static int forRows = -1;
    if (forRows != rows) {
        forRows = rows;
        scales.clear();
        for (double f = 0.80; f <= 1.95; f *= 1.06) scales.push_back(f * rows / 1440.0);
    }
    return scales;
}

// How well the table must match: the Prospector's matches at 0.75 to
// 0.96. Nothing is taken on the table alone - the line over it has to
// read as a laser.
constexpr double TABLE_BAR = 0.65;

// A name as the HUD writes it and as the game does, folded so that what the
// second reader may confuse meets: upper case, letters and numerals only, a
// 1 as I and a 0 as O - the numerals in these names are Roman more often
// than not. What the reader gave back may also have a lower-case l for an I,
// which the HUD never writes; the game's own names have lower case of their
// own ("Module") and keep it.
std::string folded(const std::string& text, bool read = false) {
    std::string out;
    for (const unsigned char c : text) {
        if (read && c == 'l') {
            out += 'I';
            continue;
        }
        if (c >= 0x80) continue;   // a byte of something the HUD does not write
        const char u = (char)std::toupper(c);
        if (u >= 'A' && u <= 'Z') out += u;
        else if (u == '1') out += 'I';
        else if (u == '0') out += 'O';
        else if (u >= '2' && u <= '9') out += u;
    }
    return out;
}

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

// The item whose name, its last word left off, is what was read: the very
// name, or one edit away when no other name is - of the items `keep` lets in
template <typename Item, typename Keep>
const Item* byName(const std::string& read, const Item* items, int count, const std::string& lastWords, Keep keep) {
    if (read.size() < 3) return nullptr;
    const std::string tail = folded(lastWords);
    const Item* near = nullptr;
    int nearCount = 0;
    for (int i = 0; i < count; i++) {
        if (!keep(items[i])) continue;
        std::string name = folded(items[i].name);
        if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
            name.resize(name.size() - tail.size());
        }
        if (name == read) return &items[i];
        if (read.size() >= 5 && editDistance(name, read) == 1) {
            near = &items[i];
            nearCount++;
        }
    }
    return nearCount == 1 ? near : nullptr;
}

// The rows of text in a stretch of the HUD. The lettering is what stands
// out of the ground under it - a top hat a little wider than a stroke - so
// that white, cyan, green and amber all come out of rock, sky and sand
// alike; on Daymar's sand the Golem's pale MINING MODULES stands at 37
// against 8 for the sand, so the bar is a fixed one and not a share of
// the brightest thing about, which may be a glowing screen. Rows are
// built letter by letter: each piece of lettering joins the row its middle
// falls in, so that two lines with no gap between them - MODULE over PASSIVE
// at 1080 lines - stay two, and a long row is cut where a gap wider than a
// few letters opens.
cv::Mat letteringInk(const cv::Mat& bgr, double s);
std::vector<cv::Rect> rowsOfPieces(std::vector<cv::Rect> pieces, int width, double s);

std::vector<cv::Rect> textRows(const cv::Mat& bgr, double s) {
    std::vector<cv::Rect> rows;
    if (bgr.empty()) return rows;
    const cv::Mat ink = letteringInk(bgr, s);
    if (const char* dump = DEV_ENV("SCMINER_LOADOUT_DUMP")) {
        static int n = 0;
        cv::imwrite(std::string(dump) + "_" + std::to_string(n) + "_region.png", bgr);
        cv::imwrite(std::string(dump) + "_" + std::to_string(n) + "_ink.png", ink);
        n++;
    }
    cv::Mat labels, stats, centres;
    const int count = cv::connectedComponentsWithStats(ink, labels, stats, centres, 8, CV_32S);
    std::vector<cv::Rect> pieces;
    for (int i = 1; i < count; i++) {
        const cv::Rect r(stats.at<int>(i, cv::CC_STAT_LEFT), stats.at<int>(i, cv::CC_STAT_TOP),
                         stats.at<int>(i, cv::CC_STAT_WIDTH), stats.at<int>(i, cv::CC_STAT_HEIGHT));
        if (r.height < 5 * s || r.height > 22 * s || r.width > 60 * s) continue;
        if (stats.at<int>(i, cv::CC_STAT_AREA) < 4) continue;
        pieces.push_back(r);
    }
    return rowsOfPieces(pieces, ink.cols, s);
}

cv::Mat letteringInk(const cv::Mat& bgr, double s) {
    // ...in whichever channel it stands out most: the Golem's amber over sand
    // is 255/202/87 against 230/140/100 - nothing in red, everything in green
    std::vector<cv::Mat> planes;
    cv::split(bgr, planes);
    const int k = (std::max)(5, (int)std::lround(9 * s)) | 1;
    const cv::Mat square = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k, k));
    cv::Mat lifted;
    for (const cv::Mat& plane : planes) {
        cv::Mat one;
        cv::morphologyEx(plane, one, cv::MORPH_TOPHAT, square);
        lifted = lifted.empty() ? one : cv::max(lifted, one);
    }
    // A stroke is lettering; its glow is not. At 1440 lines the glow of a
    // module's name ran into PASSIVE under it and both into Laser Power, and
    // the first letters of all three came out one piece too tall for a
    // letter. So a pixel counts when it stands at half of the brightest
    // about it, and at 26 at least.
    cv::Mat about;
    cv::dilate(lifted, about, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k + 2, k + 2)));
    about.convertTo(about, CV_16S, 0.5);
    cv::Mat lifted16;
    lifted.convertTo(lifted16, CV_16S);
    cv::Mat ink = (lifted > 26) & (lifted16 > about);
    // ...less the rules: a heading's underline touches its letters on the
    // Golem, and the whole of PITMAN MINING LASER came out one piece with it
    cv::Mat rules;
    cv::morphologyEx(ink, rules, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size((std::max)(9, (int)std::lround(25 * s)), 1)));
    ink.setTo(0, rules);
    return ink;
}

// The rows a stretch of lettering's pieces make - pieces as tall as a letter
// of the HUD's lists, 5 to 22 at size 1, and no wider than a few letters run
// together
std::vector<cv::Rect> rowsOfPieces(std::vector<cv::Rect> pieces, int width, double s) {
    std::vector<cv::Rect> rows;
    // ...and of a letter's height: an icon or a bracket that spans two lines
    // would join them (a module's icon beside RIEGER-C3 took PASSIVE in)
    if (pieces.empty()) return rows;
    std::vector<int> tall;
    for (const cv::Rect& p : pieces) tall.push_back(p.height);
    std::nth_element(tall.begin(), tall.begin() + tall.size() / 2, tall.end());
    const int typical = tall[tall.size() / 2];
    pieces.erase(std::remove_if(pieces.begin(), pieces.end(),
                                [&](const cv::Rect& p) { return p.height > typical * 1.6 || p.height < typical * 0.5; }),
                 pieces.end());
    std::sort(pieces.begin(), pieces.end(), [](const cv::Rect& a, const cv::Rect& b) {
        return a.y * 2 + a.height < b.y * 2 + b.height;
    });
    struct Row {
        int top, bottom;
        std::vector<cv::Rect> parts;
    };
    std::vector<Row> built;
    for (const cv::Rect& p : pieces) {
        const int middle = p.y + p.height / 2;
        Row* into = nullptr;
        for (Row& r : built) {
            if (middle >= r.top && middle <= r.bottom) into = &r;
        }
        if (!into) {
            built.push_back({p.y, p.y + p.height - 1, {}});
            into = &built.back();
        }
        into->parts.push_back(p);
        // (a row grows by a letter at most: pieces much taller than one were
        // left out above)
        into->top = (std::min)(into->top, p.y);
        into->bottom = (std::max)(into->bottom, p.y + p.height - 1);
    }
    // Rows begun by two pieces of one line - a hyphen or a small figure
    // first - stand over one another: one row
    std::sort(built.begin(), built.end(), [](const Row& a, const Row& b) { return a.top < b.top; });
    for (size_t i = 0; i + 1 < built.size();) {
        Row& a = built[i];
        Row& b = built[i + 1];
        const int overlap = (std::min)(a.bottom, b.bottom) - (std::max)(a.top, b.top) + 1;
        const int smaller = (std::min)(a.bottom - a.top, b.bottom - b.top) + 1;
        if (overlap * 2 >= smaller) {
            a.top = (std::min)(a.top, b.top);
            a.bottom = (std::max)(a.bottom, b.bottom);
            a.parts.insert(a.parts.end(), b.parts.begin(), b.parts.end());
            built.erase(built.begin() + (long)(i + 1));
        } else {
            i++;
        }
    }
    // The letters say where a row runs, up and down and across: a run of
    // them with no gap wider than three letters, and a letter's width of
    // room either side. All of the ink in the band would take in what stands
    // beside the list - over an asteroid field the rows run the whole width
    // of the list and come back "FOCUSOULE" and "INGHOLES".
    for (Row& r : built) {
        if (r.parts.size() < 3) continue;
        std::vector<int> heights;
        for (const cv::Rect& p : r.parts) heights.push_back(p.height);
        std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
        const int letter = heights[heights.size() / 2];
        cv::Rect run;
        int letters = 0;
        auto close = [&]() {
            if (letters >= 3 && run.width >= 20 * s) {
                const int left = (std::max)(0, run.x - letter), right = (std::min)(width, run.x + run.width + letter);
                rows.push_back(cv::Rect(left, r.top, right - left, r.bottom - r.top + 1));
            }
            run = cv::Rect();
            letters = 0;
        };
        for (const cv::Rect& p : r.parts) {
            if (letters > 0 && p.x - (run.x + run.width) > 3 * letter) close();
            run = letters == 0 ? p : (run | p);
            letters++;
        }
        close();
    }
    // ...and two rows over the same stretch of the same line are one: a
    // line read twice cost 50 ms, and its second reading could stand for it
    std::sort(rows.begin(), rows.end(), [](const cv::Rect& a, const cv::Rect& b) { return a.y < b.y; });
    for (size_t i = 0; i < rows.size(); i++) {
        for (size_t j = i + 1; j < rows.size();) {
            const cv::Rect both = rows[i] & rows[j];
            const int larger = (std::max)(rows[i].height, rows[j].height);
            if (both.height * 10 >= larger * 7 && both.width > 0) {
                rows[i] |= rows[j];
                rows.erase(rows.begin() + (long)j);
            } else {
                j++;
            }
        }
    }
    return rows;
}

// A row read before is not read again while it looks the same: the list
// stands still while the ship is mining, and a line costs 50 ms. The same
// is the same size to a pixel and nowhere much brighter or darker: RIEGER-C2
// and RIEGER-C3 are one numeral apart, and the other ship's list stands
// where this one's does
std::string readRowOnce(const cv::Mat& bgr, const cv::Rect& row, double s, TextLineReader& lines);

// ...and a row that came back with part of a word it is there for - MODU,
// MINING, LASER cut short where a letter touched the canopy's strut or a
// rule - once more, wider by four letters either way
std::string readRow(const cv::Mat& bgr, const cv::Rect& row, double s, TextLineReader& lines) {
    const std::string text = readRowOnce(bgr, row, s, lines);
    const std::string f = folded(text, true);
    const bool whole = f.find("MODULE") != std::string::npos || f.find("MININGLASER") != std::string::npos;
    // (not LASER alone: every module says what it does to the Laser Power)
    const bool part = f.find("MODU") != std::string::npos || f.find("ODULE") != std::string::npos ||
                      f.find("MINING") != std::string::npos || f.find("NINGLASER") != std::string::npos;
    if (whole || !part) return text;
    const int more = 4 * row.height;
    const cv::Rect wider = cv::Rect(row.x - more, row.y, row.width + 2 * more, row.height) & cv::Rect(0, 0, bgr.cols, bgr.rows);
    return readRowOnce(bgr, wider, s, lines);
}

std::string readRowOnce(const cv::Mat& bgr, const cv::Rect& row, double s, TextLineReader& lines) {
    struct Kept {
        cv::Size size;
        cv::Mat small;
        std::string text;
    };
    static std::vector<Kept> kept;
    const int pad = (std::max)(2, (int)std::lround(3 * s));
    const cv::Rect safe = cv::Rect(row.x - pad, row.y - pad, row.width + 2 * pad, row.height + 2 * pad) &
                          cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (safe.width < 8 || safe.height < 6) return "";
    cv::Mat small;
    cv::cvtColor(bgr(safe), small, cv::COLOR_BGR2GRAY);
    cv::resize(small, small, cv::Size(128, 16), 0, 0, cv::INTER_AREA);
    small.convertTo(small, CV_32F);
    for (const Kept& k : kept) {
        if (k.size != safe.size()) continue;
        cv::Mat diff;
        cv::absdiff(small, k.small, diff);
        double most = 0;
        cv::minMaxLoc(diff, nullptr, &most);
        if (most < 32.0 && cv::sum(diff)[0] / (double)diff.total() < 4.0) return k.text;
    }
    const TextLineReader::Line line = lines.read(bgr(safe));
    const std::string text = line.mean >= 0.5 ? line.text : std::string();
    kept.push_back({safe.size(), small, text});
    if (kept.size() > 32) kept.erase(kept.begin());
    if (DEV_ENV("SCMINER_LOADOUT_TRACE")) {
        std::cerr << "  [loadout] row " << safe << " read '" << line.text << "' mean " << line.mean << std::endl;
    }
    return text;
}

// The icons over the laser's table: each module's column is headed by its
// icon - a cross, a beacon - right of "Head", about 15 above the table's
// first row, the first 141 right of the table's first letter and the next
// 38.5 on (at 1440 lines; the Golem's table, with nothing fitted, has
// none). Where each stands, from the table's first letter. A busy ground
// makes more of them than there are, and runs into one now and then.
std::vector<double> moduleIcons(const cv::Mat& bgr, const cv::Point& table, double s) {
    std::vector<double> icons;
    const cv::Rect where = cv::Rect((int)std::lround(table.x + 125 * s), (int)std::lround(table.y - 27 * s),
                                    (int)std::lround(175 * s), (int)std::lround(20 * s)) &
                           cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (where.width < 10 || where.height < 6) return icons;
    const cv::Mat ink = letteringInk(bgr(where), s);
    if (const char* dump = DEV_ENV("SCMINER_LOADOUT_DUMP")) {
        cv::imwrite(std::string(dump) + "_icons_region.png", bgr(where));
        cv::imwrite(std::string(dump) + "_icons_ink.png", ink);
    }
    // the icon's strokes, by columns: a column of ink anywhere in the band,
    // and icons apart where no ink stands for six units across
    std::vector<int> cols(ink.cols, 0);
    for (int x = 0; x < ink.cols; x++) cols[x] = cv::countNonZero(ink.col(x));
    int x = 0;
    const int gap = (std::max)(3, (int)std::lround(6 * s));
    while (x < ink.cols) {
        if (cols[x] == 0) {
            x++;
            continue;
        }
        const int x0 = x;
        int x1 = x, empty = 0, ink_ = 0;
        while (x < ink.cols && empty < gap) {
            if (cols[x] > 0) {
                x1 = x;
                empty = 0;
                ink_ += cols[x];
            } else {
                empty++;
            }
            x++;
        }
        // an icon is 12 to 22 across at size 1, and well inked - and a speck
        // of rock may touch it (a beacon came out 35 across that way)
        const int w = x1 - x0 + 1;
        if (DEV_ENV("SCMINER_LOADOUT_TRACE")) {
            std::cerr << "  [loadout] icon run " << x0 << ".." << x1 << " ink " << ink_ << std::endl;
        }
        if (w >= 9 * s && w <= 40 * s && ink_ >= 20 * s * s) {
            icons.push_back((where.x - table.x + (x0 + x1) / 2.0) / s);
        }
    }
    return icons;
}

// The table's Head column: what the laser and its modules make of the
// fracture power, the rock's resistance and instability, the optimal and
// the overcharge rate and the charge window, in per cent - FRACTURE PWR 119
// for a Focus III (-5 %) beside a Rieger-C3 (+25 %), WINDOW SIZE 83 for the
// Helix I's -40 % with them. Row by row 15.5 apart from the top of FRACTURE
// PWR, the figure 97 to 117 right of its first letter, in a box. Read on
// its own every time: two figures a numeral apart are what it is read for.
int readHeadCell(const cv::Mat& bgr, const cv::Point& table, double s, int row, TextLineReader& lines,
                 double& sure) {
    sure = 0;
    const cv::Rect cell = cv::Rect((int)std::lround(table.x + 88 * s), (int)std::lround(table.y + (15.5 * row - 4) * s),
                                   (int)std::lround(40 * s), (int)std::lround(17 * s)) &
                          cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (cell.width < 10 || cell.height < 6) return -1;
    const TextLineReader::Line line = lines.read(bgr(cell));
    // the figure: two or three numerals, and nothing else that could be one
    // - the box's sides come back as a stroke now and then, which is no
    // numeral
    int value = -1, runs = 0;
    std::string run;
    auto close = [&]() {
        if (!run.empty()) {
            runs++;
            if (run.size() >= 2 && run.size() <= 3) value = std::atoi(run.c_str());
            run.clear();
        }
    };
    for (const char c : line.text) {
        if (c >= '0' && c <= '9') run += c;
        else close();
    }
    close();
    if (runs != 1 || line.mean < 0.5) value = -1;
    sure = value >= 0 ? line.mean : 0;
    if (DEV_ENV("SCMINER_LOADOUT_TRACE")) {
        std::cerr << "  [loadout] head row " << row << " " << cell << " read '" << line.text << "' mean " << line.mean
                  << " -> " << value << std::endl;
    }
    return value;
}

// One line of what a module does, as the HUD writes it under its name:
// "Laser Power (-5%)", "Optimal Charge Window (+40%)". Its words in capitals
// and the signed figure in the brackets; a line the list's edge cut off
// ("(+4") has a figure that is not to be trusted.
struct Effect {
    std::string words;
    float value = 0;
    bool whole = false;
};

bool parseEffect(const std::string& text, Effect& e) {
    e = Effect();
    size_t open = std::string::npos;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '(') {
            open = i;
            break;
        }
        // the second reader writes the bracket full width now and then: EF BC 88
        if ((unsigned char)text[i] == 0xEF && i + 2 < text.size() && (unsigned char)text[i + 1] == 0xBC &&
            (unsigned char)text[i + 2] == 0x88) {
            open = i;
            break;
        }
    }
    if (open == std::string::npos) return false;
    for (size_t i = 0; i < open; i++) {
        const char u = (char)std::toupper((unsigned char)text[i]);
        if (u >= 'A' && u <= 'Z') e.words += u;
    }
    float sign = 1;
    size_t i = open;
    while (i < text.size() && !std::isdigit((unsigned char)text[i])) {
        if (text[i] == '-') sign = -1;
        i++;
    }
    if (i >= text.size()) return false;
    float v = 0;
    while (i < text.size() && std::isdigit((unsigned char)text[i])) v = v * 10 + (float)(text[i++] - '0');
    if (i < text.size() && text[i] == '.') {
        float place = 0.1f;
        for (i++; i < text.size() && std::isdigit((unsigned char)text[i]); i++, place /= 10) v += place * (text[i] - '0');
    }
    e.value = sign * v;
    e.whole = text.find('%', i) != std::string::npos;
    return !e.words.empty();
}

// What a module's effect line says it does, in its own terms: a figure in
// per cent, as the HUD prints it. NaN when the line is about something the
// module data does not hold. The lines are the game's own texts
// (hud_mining_modifier_*): Laser Power, Optimal Charge Window, Extraction
// Rate, Overcharge Rate, Optimal Charge Rate, Resistance, Instability,
// Shatter Damage, Inert Material Filtering. Extraction Rate is the
// drawing-in beam's power and no charge rate: taken for the optimal charge
// rate, it would count against every FLTR, XTR and Vaux and the Forel, and
// none of them would be recognised (a FLTR-XL shows
// "Extraction Rate (-5%)").
float effectOf(const MiningModule& m, const std::string& words) {
    if (words.find("POWER") != std::string::npos) return std::round((m.power - 1.0f) * 100.0f);
    if (words.find("WINDOW") != std::string::npos) return m.window;
    if (words.find("EXTRAC") != std::string::npos) return m.extraction;
    if (words.find("OVERCH") != std::string::npos) return m.danger;
    if (words.find("RATE") != std::string::npos) return m.rate;
    if (words.find("RESIST") != std::string::npos) return m.resistance;
    if (words.find("INSTAB") != std::string::npos) return m.instability;
    if (words.find("SHATTER") != std::string::npos) return m.shatter;
    if (words.find("INERT") != std::string::npos || words.find("FILTER") != std::string::npos) return m.filter;
    return std::nanf("");
}

// A module's name, folded, without MODULE and without the Roman numeral at
// its end: Focus, Focus II and Focus III are one family to the second reader,
// which writes III as I - three like letters in a row run together in what
// it is trained to put out ("FOCUS I MODULE" as often as not).
std::string familyOf(std::string folded) {
    while (!folded.empty() && folded.back() == 'I') folded.pop_back();
    return folded;
}

std::string moduleBase(const MiningModule& m) {
    std::string name = folded(m.name);
    if (name.size() > 6 && name.compare(name.size() - 6, 6, "MODULE") == 0) name.resize(name.size() - 6);
    return name;
}

// The module a name line and the lines under it name. Every module whose name
// is the one read, one of its family, or within two letters of it is a
// candidate, and what the lines under the name say it does decides: a
// figure that agrees counts for it, one that disagrees against. Without a
// figure only the very name will do. XTR-L and XTR-XL, Torrent II and III
// are a letter apart; what they do is not.
const MiningModule* moduleFromBlock(const std::string& nameLine, const std::vector<std::string>& below) {
    const std::string f = folded(nameLine, true);
    const size_t at = f.find("MODULE");
    if (at == std::string::npos) return nullptr;
    const std::string read = f.substr(0, at);
    if (read.size() < 3) return nullptr;
    const std::string family = familyOf(read);
    std::vector<const MiningModule*> kin;
    const MiningModule* exact = nullptr;
    for (int i = 0; i < MINING_MODULE_COUNT; i++) {
        const std::string base = moduleBase(MINING_MODULES[i]);
        if (base == read) exact = &MINING_MODULES[i];
        if (base == read || (family.size() >= 3 && familyOf(base) == family) || editDistance(base, read) <= 2) {
            kin.push_back(&MINING_MODULES[i]);
        }
    }
    std::vector<Effect> effects;
    for (const std::string& line : below) {
        Effect e;
        if (parseEffect(line, e)) effects.push_back(e);
    }
    const MiningModule* best = nullptr;
    int bestScore = 0, ties = 0, usable = 0;
    for (const Effect& e : effects) {
        if (!kin.empty() && !std::isnan(effectOf(*kin.front(), e.words))) usable++;
    }
    // In a family of numbered ones the name read says nothing of the numeral
    // - FOCUS III came back FOCUS as well - so only what the module does can
    // say which one it is
    int family3 = 0;
    for (const MiningModule* m : kin) {
        if (family.size() >= 3 && familyOf(moduleBase(*m)) == family) family3++;
    }
    for (const MiningModule* m : kin) {
        int score = 0;
        for (const Effect& e : effects) {
            const float expected = effectOf(*m, e.words);
            if (std::isnan(expected)) continue;
            if (std::abs(expected - e.value) < 0.6f) score += 2;
            else if (e.whole) score -= 2;
        }
        // the very name breaks a tie between two that do the same - but not
        // within a family, where the name read may have lost its numeral
        if (m == exact && family3 <= 1) score += 1;
        if (score > bestScore) {
            best = m;
            bestScore = score;
            ties = 0;
        } else if (score == bestScore && score > 0) {
            ties++;
        }
    }
    // nothing said what it does: the name alone, and only the very name -
    // and not one of a family, which the reader cannot count the numerals of
    if (usable == 0) {
        best = exact && family3 <= 1 ? exact : nullptr;
        ties = 0;
    }
    // ...and in a family, a figure has to have agreed
    if (family3 > 1 && bestScore < 2) best = nullptr;
    // A name read whole that its lines say is another module: where the
    // Head column cannot tell the two apart either, it is neither. The FLTR
    // three change nothing in it, and on the Prospector's list, cut at 25
    // letters, an FLTR is told by its Extraction Rate alone - with the 1 of
    // "-15%" lost, an FLTR reads as an FLTR-XL.
    auto headBlind = [](const MiningModule& m) {
        return m.power == 1.0f && m.window == 0.0f && m.rate == 0.0f && m.resistance == 0.0f &&
               m.instability == 0.0f;
    };
    if (exact && best && best != exact && family3 <= 1 && headBlind(*best) && headBlind(*exact)) best = nullptr;
    if (DEV_ENV("SCMINER_LOADOUT_TRACE")) {
        std::cerr << "  [loadout] '" << nameLine << "' over";
        for (const std::string& line : below) std::cerr << " '" << line << "'";
        std::cerr << ": " << kin.size() << " candidates, " << effects.size() << " effects ->";
        for (const Effect& e : effects) std::cerr << " " << e.words << "=" << e.value << (e.whole ? "" : "?");
        std::cerr << " -> " << (best && ties == 0 ? best->name : "-") << " (score " << bestScore << ", ties " << ties
                  << ")" << std::endl;
    }
    return ties == 0 ? best : nullptr;
}

}  // namespace

// Of the size the HUD is read on: the Prospector's and the Golem's lasers are
// size 1, and each size 1 is one letter from its size 2 - HELIX I and HELIX
// II, IMPACT I and II, KLEIN-S1 and -S2 - which multiply the table's Head
// column alike: a numeral read twice over would have put a MOLE's laser on
// a Prospector
const MiningLaser* laserByHudLine(const std::string& line) {
    const std::string f = folded(line, true);
    const size_t at = f.find("MININGLASER");
    if (at == std::string::npos) return nullptr;
    return byName(f.substr(0, at), MINING_LASERS, MINING_LASER_COUNT, "Mining Laser",
                  [](const MiningLaser& l) { return l.size == 1; });
}

const MiningModule* moduleByHudLine(const std::string& line) {
    const std::string f = folded(line, true);
    // "MINING MODULES" heads the list; it names none
    if (f.find("MODULES") != std::string::npos) return nullptr;
    const size_t at = f.find("MODULE");
    if (at == std::string::npos) return nullptr;
    return byName(f.substr(0, at), MINING_MODULES, MINING_MODULE_COUNT, "Module",
                  [](const MiningModule&) { return true; });
}

const MiningModule* moduleByHudBlock(const std::string& nameLine, const std::vector<std::string>& below) {
    return moduleFromBlock(nameLine, below);
}

// Whether the table's Head column is what the laser and the modules read
// make of it (readHeadCell). They multiply, as everywhere in the game. An
// active module counts only while it fires, which the HUD may show it
// doing: any of them may be. Every figure that is not 100 has to have been
// read - a module the list lost changes at least one of them, unless all
// it does is filter what is drawn in. One that is 100 the HUD writes in a
// dim grey, which the reader may lose or take for another figure, unsure
// of it (misread as 200 at 0.56, as 160 at 0.83): only a figure read for
// sure says it is not 100, as one in colour is.
constexpr double HEAD_SURE = 0.9;

bool headColumnAgrees(const MiningLaser& laser, const std::vector<const MiningModule*>& modules, const int head[6],
                      const double sure[6]) {
    const size_t n = (std::min)(modules.size(), (size_t)8);
    for (unsigned firing = 0; firing < (1u << n); firing++) {
        bool skip = false;
        double power = 1, resistance = 1 + laser.resistance / 100.0, instability = 1 + laser.instability / 100.0;
        double rate = 1 + laser.rate / 100.0, window = 1 + laser.window / 100.0;
        for (size_t i = 0; i < n; i++) {
            const MiningModule& m = *modules[i];
            const bool on = (firing >> i) & 1u;
            if (on && !m.active) skip = true;
            if (m.active && !on) continue;
            power *= m.power;
            resistance *= 1 + m.resistance / 100.0;
            instability *= 1 + m.instability / 100.0;
            rate *= 1 + m.rate / 100.0;
            window *= 1 + m.window / 100.0;
        }
        if (skip) continue;
        const double expected[6] = {power * 100, resistance * 100, instability * 100, rate * 100, -1, window * 100};
        bool agrees = true;
        for (int r = 0; r < 6 && agrees; r++) {
            if (expected[r] < 0) continue;
            const long want = std::lround(expected[r]);
            if (want == 100) agrees = head[r] < 0 || std::abs(head[r] - 100) <= 1 || sure[r] < HEAD_SURE;
            else agrees = head[r] >= 0 && std::abs(head[r] - want) <= 1;
        }
        if (agrees) return true;
    }
    return false;
}

bool findLaserTable(const cv::Mat& frame, SeenLoadout& seen) {
    seen = SeenLoadout();
    if (frame.empty()) return false;
    static const PanelShape* table = [] {
        for (int i = 0; i < PANEL_SHAPE_COUNT; i++) {
            if (PANEL_SHAPES[i].glyph == 'F') return &PANEL_SHAPES[i];
        }
        return (const PanelShape*)nullptr;
    }();
    if (!table) return false;
    // in whiteness: the table's words are white on every HUD it has been seen
    // on, over the Prospector's blue and the Golem's sand
    const ShapeFound found = findShapeIn(frame, laserBand(frame.size()), 5, *table, tableScales(frame.rows));
    seen.score = found.score;
    if (found.score < TABLE_BAR) return false;
    seen.tableFound = true;
    seen.table = found.at;
    seen.scale = found.scale;
    return true;
}

cv::Mat hudLetteringInk(const cv::Mat& bgr, double scale) {
    return letteringInk(bgr, scale);
}

bool readSeenLoadout(const cv::Mat& frame, TextLineReader& lines, SeenLoadout& seen) {
    if (!findLaserTable(frame, seen) || !lines.ready()) return false;
    cv::Mat bgr;
    if (frame.channels() == 4) cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
    else if (frame.channels() == 1) cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
    else bgr = frame;
    const double s = seen.scale;
    const cv::Rect screen(0, 0, bgr.cols, bgr.rows);
    auto around = [&](double x0, double y0, double x1, double y1) {
        return cv::Rect((int)std::lround(seen.table.x + x0 * s), (int)std::lround(seen.table.y + y0 * s),
                        (int)std::lround((x1 - x0) * s), (int)std::lround((y1 - y0) * s)) &
               screen;
    };
    // The laser's name, on the line over the table: 41 to 55 above its first
    // word on the Prospector, 47 to 59 on the Golem. It ends where the table
    // ends and runs left as far as it is long: HELIX I MINING LASER begins 36
    // to the right of the table's first word, HOFSTEDE-S1 MINING LASER some
    // 10 to its left, which a line read from 15 cuts to "FSTEDE-SI MINING
    // LASER", no laser at all. Every laser a Prospector or a Golem takes but
    // the Helix I and the Pitman has a longer name. From 60 to the left,
    // though, the line takes in more of whatever is about and the reader
    // squeezes it into its fixed width, which loses Helix and Pitman names
    // (39 in 5,766 pictures): so from 15 first, and from 60 only when that
    // named none.
    for (const double from : {15.0, -60.0}) {
        const cv::Rect header = around(from, -80, 330, -28);
        for (const cv::Rect& row : textRows(bgr(header), s)) {
            const std::string text = readRow(bgr(header), row, s, lines);
            if (const MiningLaser* laser = laserByHudLine(text)) {
                seen.laserText = text;
                seen.laser = laser;
                break;
            }
            if (seen.laserText.empty()) seen.laserText = text;
        }
        if (seen.laser) break;
        seen.laserText.clear();
    }
    if (!seen.laser) return false;
    // The modules, above it: each under its own name - "FOCUS III MODULE" -
    // with what it is and what it does on the lines below. The list begins
    // under MINING MODULES, about 500 above the table on both HUDs; GEAR,
    // CPLD and ESP stand between it and the laser.
    const cv::Rect list = around(-10, -560, 360, -105);
    bool headed = false, unknown = false;
    std::vector<std::pair<std::string, std::vector<std::string>>> blocks;
    // Each module's name is followed by PASSIVE or ACTIVE. One of those with
    // no name over it is a name that was not read - and a list read without
    // it is not the list: a Focus III lost in the asteroids behind leaves the
    // Rieger-C3 under it named alone.
    bool named = false;
    // ...and no more rows than a list of three modules has, read from the
    // top: over a busy ground the rest is what stands about
    const std::vector<cv::Rect> rows = textRows(bgr(list), s);
    int read = 0;
    for (const cv::Rect& row : rows) {
        if (++read > 18) break;
        const std::string text = readRow(bgr(list), row, s, lines);
        const std::string f = folded(text, true);
        // What the HUD writes under an active module, or over a list of them, is
        // unknown; the game's texts have "Active Modules"
        // (hud_mining_consumables_activemodules). Whatever it is, it is no heading
        // and no module's name: taken for MINING MODULES, it would drop every
        // module read above it, and an active module that is not firing changes
        // nothing in the Head column to miss it by. As a kind's line it is safe
        // either way - under a name it is that name's, and over no name it says
        // one was lost.
        const bool active = f.find("ACTIV") != std::string::npos;
        // MINING MODULES heads the list - no module's name ends in MODULES,
        // so that word is enough when MINING came back as HINING
        if (!active && f.find("MODULES") != std::string::npos) {
            // whatever stood above it was not the list
            headed = true;
            blocks.clear();
            unknown = false;
            named = false;
            continue;
        }
        if (!active && f.find("MODULE") != std::string::npos) {
            blocks.push_back({text, {}});
            named = true;
        } else if (f == "PASSIVE" || active) {
            if (!named && headed) unknown = true;
            named = false;
            // ...which no line further down puts right: the rest is not read
            if (unknown) break;
        } else if (!blocks.empty()) {
            blocks.back().second.push_back(text);
        }
    }
    seen.moduleTexts.clear();
    seen.modules.clear();
    for (const auto& block : blocks) {
        seen.moduleTexts.push_back(block.first);
        if (const MiningModule* module = moduleFromBlock(block.first, block.second)) {
            seen.modules.push_back(module);
        } else {
            unknown = true;
        }
    }
    // A list with nothing in it counts only under its heading - the Golem's,
    // with nothing fitted
    const bool listed = (headed || !seen.modules.empty()) && !unknown;
    // ...and it is checked against what the table says the laser and its
    // modules do together, which a module the list lost changes. Counting the
    // icons over the table's columns is no check: a busy ground loses them as
    // it loses the names.
    if (listed) {
        for (int r = 0; r < 6; r++) {
            if (r != 4) seen.head[r] = readHeadCell(bgr, seen.table, s, r, lines, seen.headSure[r]);
        }
        seen.headAgrees = headColumnAgrees(*seen.laser, seen.modules, seen.head, seen.headSure);
    }
    // ...but a module that only filters what is drawn in - an FLTR - changes
    // nothing in the table, and its icon is the one sign of it: an icon where
    // the list's next module's would stand is a module the list lost. Not the
    // count of them, which the ground behind makes too many as often as not
    // (a Golem with nothing fitted had 1 to 4).
    const std::vector<double> icons = moduleIcons(bgr, seen.table, s);
    seen.moduleColumns = (int)icons.size();
    const double next = 141 + 38.5 * (double)seen.modules.size();
    for (const double x : icons) {
        if (std::abs(x - next) <= 12) seen.iconPastList = true;
    }
    seen.modulesRead = listed && seen.headAgrees && !seen.iconPastList;
    return true;
}

const char* hudShipOf(char glyph, const std::string& logShip, bool inSeat) {
    if (glyph == 'L' || glyph == 'M') return "prospector";
    if (glyph == 'G' || glyph == 'H') return "golem";
    // another ship's panel - a MOLE's - or nobody at the controls
    if (glyph != 0 || !inSeat) return nullptr;
    std::string ship = logShip;
    for (char& ch : ship) ch = (char)std::tolower((unsigned char)ch);
    if (ship.find("prospector") != std::string::npos) return "prospector";
    if (ship.find("golem") != std::string::npos) return "golem";
    return nullptr;
}

}  // namespace SCMiner
