#include "CommodityTerminal.h"

#include "DevSwitch.h"
#include "TextLineReader.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>

namespace SCMiner {

namespace {

// Letters and figures only, upper case: what two spellings of a name share
std::string folded(const std::string& s, bool figures) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalpha(c)) out += static_cast<char>(std::toupper(c));
        else if (figures && std::isdigit(c)) out += static_cast<char>(c);
    }
    return out;
}

int distance(const std::string& a, const std::string& b) {
    std::vector<int> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) row[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); i++) {
        int diagonal = row[0];
        row[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); j++) {
            const int above = row[j];
            row[j] = (std::min)({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

// A figure's character as the reader may have taken it
char figureChar(char c) {
    switch (c) {
        case 'O': case 'o': case 'D': case 'Q': return '0';
        case 'l': case 'I': case 'i': case '|': case '!': return '1';
        case 'Z': case 'z': return '2';
        case 'S': case 's': return '5';
        case 'B': return '8';
        case '.': return ',';   // the terminal writes whole amounts, its thousands set off by commas
        default: return c;
    }
}

// "1,050", "24,000", "316": a whole amount, its thousands in threes set off
// by commas; -1 where the characters are not one. A currency sign read as a
// nought in front of it goes.
int wholeFigure(const std::string& s) {
    std::string t;
    for (char c : s) {
        if (c == ' ') continue;
        const char d = figureChar(c);
        if (!std::isdigit(static_cast<unsigned char>(d)) && d != ',') return -1;
        t += d;
    }
    while (t.size() > 1 && t[0] == '0' && std::isdigit(static_cast<unsigned char>(t[1]))) t.erase(0, 1);
    if (t.empty() || t.front() == ',' || t.back() == ',') return -1;
    const size_t first = t.find(',');
    // the terminal sets off every thousand: four figures in a row are a
    // misread - a currency sign taken for a 1, "1316" for 316
    if (first == std::string::npos && t.size() >= 4) return -1;
    if (first != std::string::npos) {
        if (first > 3) return -1;
        for (size_t at = first; at != std::string::npos;) {
            const size_t next = t.find(',', at + 1);
            if ((next == std::string::npos ? t.size() : next) - at - 1 != 3) return -1;
            at = next;
        }
    }
    std::string digits;
    for (char c : t) {
        if (c != ',') digits += c;
    }
    if (digits.empty() || digits.size() > 9) return -1;
    return std::stoi(digits);
}

struct Line {
    cv::Rect box;
    std::string text;
    double mean = 0;
};

// The lines of lettering right of the middle of the view, where the
// terminal's SHOP INVENTORY stands while it is used: what stands out of its
// ground by brightness or colour (the larger of the three channels - the
// levels are written in red, blue and green), small enough to be a line of
// text. The top hat - the picture less its opening - keeps strokes thinner
// than the kernel and drops filled shapes (the picked tab, the BUY button).
//
// Beside a name stand the commodity's icon and the level's bar, a slot
// filled from below in the level's colour - each about six pixels from the
// next in 1080, where letters stand one or two apart. So the letters are
// run together over four pixels only, a bar - a narrow, solid upright - is
// dropped (a full one already by its height), and a line's words are then
// joined where they are of a height - a quarter apart at most, as a piece
// of the icon beside IRON is not - and stand within a line's height and a
// half of each other: "4" and "SCU", "MAX" and "INVENTORY".
std::vector<cv::Rect> textLines(const cv::Mat& bgr) {
    const int H = bgr.rows;
    const int half = (std::min)(bgr.cols / 2, static_cast<int>(std::lround(0.95 * H)));
    const int from = bgr.cols / 2 - static_cast<int>(std::lround(0.1 * H));
    const cv::Rect middle(from, 0, bgr.cols / 2 + half - from, H);
    std::vector<cv::Mat> channels;
    cv::split(bgr(middle), channels);
    const cv::Mat value = cv::max(cv::max(channels[0], channels[1]), channels[2]);
    const int k = (std::max)(9, static_cast<int>(std::lround(0.04 * H))) | 1;
    cv::Mat opened;
    cv::morphologyEx(value, opened, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k, k)));
    cv::Mat ink = (value - opened) >= 45;
    cv::Mat tall;
    const int barH = (std::max)(12, static_cast<int>(std::lround(0.028 * H)));
    cv::morphologyEx(ink, tall, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(1, barH)));
    cv::dilate(tall, tall, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
    ink.setTo(0, tall);
    const int gap = (std::max)(3, static_cast<int>(std::lround(0.0035 * H)));
    cv::morphologyEx(ink, ink, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(gap, 1)));
    cv::Mat labels, stats, centroids;
    const int n = cv::connectedComponentsWithStats(ink, labels, stats, centroids, 8);
    const int minH = (std::max)(6, static_cast<int>(std::lround(0.007 * H)));
    const int maxH = static_cast<int>(std::lround(0.034 * H));
    const int barW = (std::max)(3, static_cast<int>(std::lround(0.008 * H)));
    static const bool trace = DEV_ENV("SCMINER_COMMODITY_BLOBS") != nullptr;
    std::vector<cv::Rect> pieces;
    for (int i = 1; i < n; i++) {
        const cv::Rect r(stats.at<int>(i, cv::CC_STAT_LEFT) + middle.x, stats.at<int>(i, cv::CC_STAT_TOP),
                         stats.at<int>(i, cv::CC_STAT_WIDTH), stats.at<int>(i, cv::CC_STAT_HEIGHT));
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (r.height < minH || r.height > maxH) continue;
        const bool bar = r.width <= barW && r.height * 5 >= r.width * 6 && area * 5 >= r.area() * 4;
        if (trace) {
            std::printf("    piece %d,%d %dx%d, %d%% solid%s\n", r.x, r.y, r.width, r.height, area * 100 / (std::max)(1, r.area()),
                        bar ? " - a bar" : "");
        }
        if (bar) continue;
        pieces.push_back(r);
    }
    // one row's words, left to right, joined where they are of a height and
    // stand close
    std::sort(pieces.begin(), pieces.end(), [](const cv::Rect& a, const cv::Rect& b) { return a.x < b.x; });
    for (bool joined = true; joined;) {
        joined = false;
        for (size_t i = 0; i < pieces.size() && !joined; i++) {
            for (size_t j = 0; j < pieces.size(); j++) {
                if (i == j) continue;
                const cv::Rect& a = pieces[i];
                const cv::Rect& b = pieces[j];
                if (b.x < a.x) continue;
                const int lower = (std::min)(a.height, b.height), higher = (std::max)(a.height, b.height);
                const int overlap = (std::min)(a.y + a.height, b.y + b.height) - (std::max)(a.y, b.y);
                if (overlap < lower * 3 / 5 || higher * 4 > lower * 5) continue;
                if (b.x - (a.x + a.width) > lower * 3 / 2) continue;
                pieces[i] = a | b;
                pieces.erase(pieces.begin() + j);
                joined = true;
                break;
            }
        }
    }
    std::vector<cv::Rect> lines;
    for (const cv::Rect& r : pieces) {
        if (r.width >= r.height * 2) lines.push_back(r);
    }
    std::sort(lines.begin(), lines.end(),
              [](const cv::Rect& a, const cv::Rect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    return lines;
}

Line readLine(const cv::Mat& bgr, const cv::Rect& r, TextLineReader& reader) {
    const int mx = (std::max)(3, r.height / 2), my = (std::max)(2, r.height / 4);
    const cv::Rect box = cv::Rect(r.x - mx, r.y - my, r.width + 2 * mx, r.height + 2 * my) & cv::Rect(0, 0, bgr.cols, bgr.rows);
    cv::Mat piece = bgr(box);
    // the small lines - SHOP QUANTITY, the level, the price - read whole at twice their size
    if (piece.rows < 28) cv::resize(piece, piece, cv::Size(), 2.0, 2.0, cv::INTER_CUBIC);
    const TextLineReader::Line read = reader.read(piece);
    return {r, read.text, read.mean};
}

int right(const cv::Rect& r) { return r.x + r.width; }
int bottom(const cv::Rect& r) { return r.y + r.height; }

}  // namespace

int commodityLevelIn(const std::string& text) {
    // The game's words for the shop's stock, buying and selling alike
    // (shop_ui_CommodityKiosk_Supply_* and _Demand_*), on UEX's scale
    static const struct {
        const char* words;
        int level;
    } LEVELS[] = {{"OUTOFSTOCK", 1},      {"NOINVENTORY", 1},       {"VERYLOWINVENTORY", 2},
                  {"LOWINVENTORY", 3},    {"MEDIUMINVENTORY", 4},   {"HIGHINVENTORY", 5},
                  {"VERYHIGHINVENTORY", 6}, {"MAXINVENTORY", 7},    {"INVENTORYUPDATING", 0}};
    const std::string t = folded(text, false);
    if (t.size() < 6) return -1;
    int best = 1 << 20, second = 1 << 20, level = -1, allowed = 0;
    for (const auto& l : LEVELS) {
        const int d = distance(t, l.words);
        if (d < best) {
            second = best;
            best = d;
            level = l.level;
            allowed = std::string(l.words).size() >= 14 ? 2 : 1;
        } else if (d < second) {
            second = d;
        }
    }
    // A slip in a short phrase, two in a long one, where no other level is as
    // near: "NAX" is MAX and "HEDIUN" MEDIUM, but "NOW" could be NO or LOW, and
    // a bare "INVENTORY" is not NO INVENTORY
    if (best > allowed || second <= best) return -1;
    return level;
}

int parseShopQuantity(const std::string& text) {
    // the figure, then the unit: the letters at the end read as SCU
    size_t end = text.size();
    while (end > 0 && text[end - 1] == ' ') end--;
    size_t unit = end;
    while (unit > 0 && std::isalpha(static_cast<unsigned char>(text[unit - 1]))) unit--;
    if (end - unit < 2 || distance(folded(text.substr(unit, end - unit), false), "SCU") > 1) return -1;
    return wholeFigure(text.substr(0, unit));
}

int parseCommodityPrice(const std::string& text, bool& perUnit) {
    const size_t slash = text.rfind('/');
    if (slash == std::string::npos) return -1;
    const std::string unit = folded(text.substr(slash + 1), false);
    if (distance(unit, "SCU") <= 1) perUnit = false;
    else if (distance(unit, "UNITS") <= 1 || distance(unit, "UNIT") <= 1) perUnit = true;
    else return -1;
    // the figure right before the slash, back to the currency sign
    std::string figure;
    for (size_t i = slash; i-- > 0;) {
        const char c = text[i];
        const char d = figureChar(c);
        if (std::isdigit(static_cast<unsigned char>(d)) || d == ',') figure.insert(figure.begin(), c);
        else if (c != ' ') break;
    }
    return wholeFigure(figure);
}

bool commodityNamed(const std::string& read, const std::string& name) {
    // the letters with a slip or two, the figures exactly: SHIP AMMUNITION -
    // SIZE 1 is not SIZE 2
    const std::string a = folded(read, false), b = folded(name, false);
    if (a.empty() || b.empty()) return false;
    std::string figuresA, figuresB;
    for (unsigned char c : read) {
        if (std::isdigit(c)) figuresA += static_cast<char>(c);
    }
    for (unsigned char c : name) {
        if (std::isdigit(c)) figuresB += static_cast<char>(c);
    }
    return figuresA == figuresB && distance(a, b) <= (std::max)(1, static_cast<int>(b.size()) / 6);
}

CommodityScreen readCommodityScreen(const cv::Mat& bgr, TextLineReader& reader, const std::string& wanted) {
    const auto t0 = std::chrono::steady_clock::now();
    CommodityScreen screen;
    if (bgr.empty()) return screen;
    const std::vector<cv::Rect> boxes = textLines(bgr);
    // each line read when it is wanted, and once
    std::vector<std::optional<Line>> lines(boxes.size());
    static const bool trace = DEV_ENV("SCMINER_COMMODITY_TRACE") != nullptr;
    auto text = [&](size_t i) -> const std::string& {
        if (!lines[i]) {
            lines[i] = readLine(bgr, boxes[i], reader);
            screen.lines++;
            if (trace) {
                std::printf("      line %d,%d %dx%d: '%s' %.2f\n", boxes[i].x, boxes[i].y, boxes[i].width,
                            boxes[i].height, lines[i]->text.c_str(), lines[i]->mean);
            }
        }
        return lines[i]->text;
    };
    // SHOP QUANTITY, the stock and the price of every card stand flush right
    // on one edge, the edge more lines end on than any other: found without
    // reading a word
    int edge = -1, most = 0;
    for (const cv::Rect& b : boxes) {
        int on = 0;
        for (const cv::Rect& o : boxes) on += std::abs(right(o) - right(b)) <= 4 ? 1 : 0;
        if (on > most) {
            most = on;
            edge = right(b);
        }
    }
    if (most < 3) {
        screen.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return screen;
    }
    std::vector<size_t> column;
    for (size_t i = 0; i < boxes.size(); i++) {
        if (std::abs(right(boxes[i]) - edge) <= (std::max)(4, boxes[i].height / 2)) column.push_back(i);
    }
    // ...three to a card, close one under the other, the stock the largest
    for (size_t c = 0; c + 2 < column.size();) {
        const cv::Rect& a = boxes[column[c]];
        const cv::Rect& q = boxes[column[c + 1]];
        const cv::Rect& p = boxes[column[c + 2]];
        const bool close = q.y - bottom(a) <= a.height && p.y - bottom(q) <= q.height;
        if (!close || q.height < a.height || q.height < p.height) {
            c++;
            continue;
        }
        c += 3;
        const int h = a.height;
        CommodityCard card;
        cv::Rect band = a | q | p;
        // ...and left of them the name and the level, one under the other,
        // flush left: the lines within the card's height
        const int top = band.y - h, foot = bottom(band) + h;
        std::vector<size_t> left;
        for (size_t i = 0; i < boxes.size(); i++) {
            const cv::Rect& l = boxes[i];
            const int middleY = l.y + l.height / 2;
            if (middleY < top || middleY > foot) continue;
            if (right(l) > a.x - h / 2 || l.x < a.x - 30 * h) continue;
            left.push_back(i);
        }
        if (left.empty()) continue;
        // looking for one commodity: a card whose name begins otherwise is
        // left unread
        if (!wanted.empty()) {
            const std::string first = folded(text(left.front()), true), name = folded(wanted, true);
            const std::string head = name.substr(0, (std::min)(name.size(), first.size()));
            if (first.empty() || distance(first, head) > (std::max)(1, static_cast<int>(head.size()) / 5)) continue;
        }
        size_t levelLine = left.size();
        for (size_t k = left.size(); k-- > 0 && levelLine == left.size();) {
            const int level = commodityLevelIn(text(left[k]));
            if (level >= 0) {
                card.level = level;
                levelLine = k;
            }
        }
        for (size_t k = 0; k < left.size(); k++) {
            const cv::Rect& l = boxes[left[k]];
            if (levelLine < left.size() &&
                (k >= levelLine || std::abs(l.x - boxes[left[levelLine]].x) > h * 3 / 2)) continue;
            card.name += (card.name.empty() ? "" : " ") + text(left[k]);
            band |= l;
        }
        if (levelLine < left.size()) band |= boxes[left[levelLine]];
        card.stock = parseShopQuantity(text(column[c - 2]));
        card.price = parseCommodityPrice(text(column[c - 1]), card.perUnit);
        card.card = band;
        if (!wanted.empty() && !commodityNamed(card.name, wanted)) continue;
        screen.cards.push_back(card);
    }
    screen.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return screen;
}

TradeCard readTradeCard(const std::vector<TerminalPicture>& pictures, const std::string& name, long long tradeMs,
                        double perScu, TextLineReader& reader) {
    TradeCard out;
    if (name.empty() || perScu <= 0) return out;
    std::vector<size_t> before, after;
    for (size_t i = 0; i < pictures.size(); i++) {
        if (pictures[i].jpeg.empty()) continue;
        if (pictures[i].ms < tradeMs) before.push_back(i);
        else if (pictures[i].ms > tradeMs + 3000) after.push_back(i);   // the confirmation and the loading first
    }
    std::sort(before.begin(), before.end(), [&](size_t a, size_t b) { return pictures[a].ms > pictures[b].ms; });
    std::sort(after.begin(), after.end(), [&](size_t a, size_t b) { return pictures[a].ms < pictures[b].ms; });
    auto look = [&](const std::vector<size_t>& order) {
        struct Seen {
            int level, stock, price;
            size_t picture;
        };
        std::vector<Seen> seen;
        for (size_t k = 0; k < order.size() && k < 6; k++) {
            const cv::Mat bgr = cv::imdecode(pictures[order[k]].jpeg, cv::IMREAD_COLOR);
            if (bgr.empty()) continue;
            out.read++;
            for (const CommodityCard& c : readCommodityScreen(bgr, reader, name).cards) {
                if (c.level < 1 || c.stock < 0 || c.price < 0 || c.perUnit) continue;
                if (std::abs(c.price - perScu) > 1.0) continue;
                for (const Seen& s : seen) {
                    if (s.level == c.level && s.stock == c.stock && s.price == c.price) {
                        out.found = true;
                        out.level = c.level;
                        out.stock = c.stock;
                        out.price = c.price;
                        out.picture = s.picture;
                        return true;
                    }
                }
                seen.push_back({c.level, c.stock, c.price, order[k]});
                break;
            }
        }
        return false;
    };
    if (!look(before)) look(after);
    return out;
}

}  // namespace SCMiner
