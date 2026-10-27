#include "Datarunner.h"

#include "TextLineReader.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace SCMiner {
namespace Datarunner {

namespace {

// Letters and figures only, upper case: what two spellings of a name share
std::string folded(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) out += static_cast<char>(std::toupper(c));
    }
    return out;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A file's time on disk, in Unix seconds
long long writtenAt(const std::filesystem::path& file) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(file, ec);
    if (ec) return 0;
#ifdef _WIN32
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
#else
    // clock_cast is GCC 13's; the Linux package is built with GCC 11
    const auto system = std::chrono::file_clock::to_sys(written);
#endif
    return (long long)std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

// A row read whole: a name, the level, the stock and a price per SCU
bool complete(const Row& row) {
    return !folded(row.read).empty() && row.level >= 1 && row.level <= 7 && row.stock >= 0 && row.price >= 0 &&
           !row.perUnit && row.levelSure != Certainty::Missing && row.stockSure != Certainty::Missing &&
           row.priceSure != Certainty::Missing;
}

}  // namespace

std::filesystem::path screenshotsBeside(const std::filesystem::path& gameLog) {
    if (gameLog.empty()) return {};
    return gameLog.parent_path() / "ScreenShots";
}

std::string channelOf(const std::filesystem::path& screenshots) {
    // ...\StarCitizen\LIVE\ScreenShots: the folder the ScreenShots folder is in
    std::filesystem::path folder = screenshots;
    if (upper(folder.filename().string()) == "SCREENSHOTS") folder = folder.parent_path();
    const std::string name = upper(folder.filename().string());
    // the hotfix servers are LIVE's, their prices LIVE's too
    if (name == "LIVE" || name == "HOTFIX") return "LIVE";
    if (name == "PTU" || name == "EPTU" || name == "TECH-PREVIEW" || name == "TECHPREVIEW") return "PTU";
    return "";
}

bool boxesUexTakes(const std::string& sizes) {
    if (sizes.empty()) return false;
    size_t from = 0;
    while (from <= sizes.size()) {
        const size_t end = sizes.find(',', from);
        const std::string part = sizes.substr(from, end == std::string::npos ? std::string::npos : end - from);
        if (part != "1" && part != "2" && part != "4" && part != "8" && part != "16" && part != "24" && part != "32") {
            return false;
        }
        if (end == std::string::npos) break;
        from = end + 1;
    }
    return true;
}

std::vector<unsigned char> pictureForUex(cv::Mat frame) {
    std::vector<unsigned char> jpeg;
    if (frame.empty()) return jpeg;
    if (frame.channels() == 4) cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);
    if (frame.rows > 1080) {
        cv::resize(frame, frame, cv::Size((int)((long long)frame.cols * 1080 / frame.rows), 1080), 0, 0, cv::INTER_AREA);
    }
    for (int quality : {85, 72, 58, 44}) {
        if (!cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, quality})) jpeg.clear();
        // base64 makes four of three; UEX takes up to 10 MB
        if (jpeg.empty() || jpeg.size() / 3 * 4 < 9u * 1024 * 1024) break;
    }
    return jpeg;
}

Shot readPicture(const cv::Mat& bgr, TextLineReader& reader) {
    Shot shot;
    if (bgr.empty()) return shot;
    shot.width = bgr.cols;
    shot.height = bgr.rows;
    const CommodityScreen screen = readCommodityScreen(bgr, reader);
    if (screen.cards.empty()) return shot;
    shot.side = screen.side;
    shot.sideSure = screen.side != 0;
    for (const CommodityCard& card : screen.cards) {
        Row row;
        row.read = card.name;
        row.level = card.level >= 1 && card.level <= 7 ? card.level : -1;
        row.stock = card.stock;
        row.price = card.price;
        row.perUnit = card.perUnit && !card.oneScuBox();
        row.oneScuBox = card.oneScuBox();
        row.levelSure = card.levelSure();
        row.stockSure = card.stockSure();
        row.priceSure = card.priceSure();
        row.card = card.card;
        // the card with a little of its ground around it, the level's bar
        // left of the name in it
        const int margin = (std::max)(3, card.card.height / 8);
        const cv::Rect piece = cv::Rect(card.card.x - 2 * margin, card.card.y - margin, card.card.width + 3 * margin,
                                        card.card.height + 2 * margin) &
                               cv::Rect(0, 0, bgr.cols, bgr.rows);
        if (piece.area() > 0) row.snippet = bgr(piece).clone();
        shot.rows.push_back(std::move(row));
    }
    // the very picture the rows were read off
    shot.jpeg = pictureForUex(bgr);
    return shot;
}

Shot readPicture(const std::filesystem::path& file, TextLineReader& reader, const cv::Mat& own) {
    const cv::Mat bgr = cv::imread(file.string(), cv::IMREAD_COLOR);
    // the game's picture black, or not to be read: Bedrock's own of the
    // moment instead, where it took one
    const bool black = blackPicture(bgr);
    const bool useOwn = !own.empty() && (bgr.empty() || black);
    Shot shot = readPicture(useOwn ? own : bgr, reader);
    shot.own = useOwn;
    shot.black = black && !useOwn;
    shot.file = file;
    shot.taken = writtenAt(file);
    shot.channel = channelOf(file.parent_path());
    return shot;
}

std::string lookKey(const Shot& look) {
    std::string key;
    for (const Row& row : look.rows) {
        if (!complete(row)) continue;
        key += folded(row.read) + ":" + std::to_string(row.level) + ":" + std::to_string(row.stock) + ":" +
               std::to_string(row.price) + ";";
    }
    return key.empty() ? key : std::to_string(look.side) + "|" + key;
}

Shot newRowsOf(const Shot& look, const std::vector<std::shared_ptr<Shot>>& shots, long long since) {
    Shot fresh = look;
    fresh.rows.clear();
    for (const Row& row : look.rows) {
        if (!complete(row)) continue;
        const std::string name = folded(row.read);
        bool seen = false;
        for (const auto& shot : shots) {
            if (!shot || shot->side != look.side || shot->shop != look.shop || shot->taken < since) continue;
            for (const Row& other : shot->rows) {
                if (folded(other.read) == name) seen = true;
            }
        }
        if (!seen) fresh.rows.push_back(row);
    }
    return fresh;
}

bool blackPicture(const cv::Mat& bgr) {
    if (bgr.empty()) return false;
    cv::Mat gray;
    if (bgr.channels() == 1) gray = bgr;
    else cv::cvtColor(bgr, gray, bgr.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    double low = 0, high = 0;
    cv::minMaxLoc(gray, &low, &high);
    // the game's black picture is nought throughout; a little room for a
    // JPEG's ringing
    return high <= 8;
}

int matchCommodity(const std::string& read, const std::vector<std::pair<std::string, int>>& names,
                   const std::vector<int>& listed, std::string& name, Certainty& sure) {
    name.clear();
    sure = Certainty::Missing;
    const std::string key = folded(read);
    if (key.empty()) return 0;
    for (const auto& [written, id] : names) {
        if (folded(written) == key) {
            name = written;
            sure = Certainty::Sure;
            return id;
        }
    }
    // with a slip or two: the one that comes so near, or the one of those
    // the log listed for the terminal
    std::vector<std::pair<std::string, int>> near;
    for (const auto& entry : names) {
        if (commodityNamed(read, entry.first)) near.push_back(entry);
    }
    if (near.size() > 1) {
        std::vector<std::pair<std::string, int>> listedNear;
        for (const auto& entry : near) {
            if (std::find(listed.begin(), listed.end(), entry.second) != listed.end()) listedNear.push_back(entry);
        }
        near = listedNear;
    }
    if (near.size() != 1) return 0;
    name = near.front().first;
    sure = Certainty::Check;
    return near.front().second;
}

std::vector<std::pair<std::string, int>> nearestCommodities(const std::string& read,
                                                            const std::vector<std::pair<std::string, int>>& names,
                                                            size_t count) {
    // letters apart, the folded names compared; a name that begins with
    // what was read comes first among equals
    const std::string key = folded(read);
    std::vector<std::pair<int, size_t>> ranked;
    for (size_t i = 0; i < names.size(); i++) {
        const std::string other = folded(names[i].first);
        std::vector<int> row(other.size() + 1);
        for (size_t j = 0; j <= other.size(); j++) row[j] = (int)j;
        for (size_t a = 1; a <= key.size(); a++) {
            int diagonal = row[0];
            row[0] = (int)a;
            for (size_t b = 1; b <= other.size(); b++) {
                const int above = row[b];
                row[b] = (std::min)({row[b] + 1, row[b - 1] + 1, diagonal + (key[a - 1] != other[b - 1] ? 1 : 0)});
                diagonal = above;
            }
        }
        const bool begins = !key.empty() && other.rfind(key, 0) == 0;
        ranked.push_back({row[other.size()] * 2 - (begins ? 1 : 0), i});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<std::string, int>> out;
    for (size_t i = 0; i < ranked.size() && out.size() < count; i++) out.push_back(names[ranked[i].second]);
    return out;
}

void match(Shot& shot, const UexDirectory& directory, const TradeBook::TerminalFill* fill) {
    // the terminal
    shot.terminals.clear();
    if (fill) {
        shot.shop = fill->shop;
        shot.place = fill->place;
        for (int id : directory.tradeTerminals(fill->shop, fill->place)) {
            shot.terminals.push_back({id, directory.terminalName(id)});
        }
    }
    if (shot.terminals.size() == 1) shot.terminal = shot.terminals.front().first;
    // the commodities the log listed for it, UEX's ids for them, and their
    // cargo box sizes: a resource that reads as one of UEX's commodities
    std::vector<int> listed;
    std::vector<std::pair<int, std::string>> boxes;
    if (fill) {
        for (const auto& [resource, sizes] : fill->boxes) {
            int found = 0, count = 0;
            for (const auto& [written, id] : directory.commodityNames()) {
                if (!TradeBook::resourceIs(resource, written)) continue;
                found = id;
                count++;
            }
            if (count != 1) continue;
            listed.push_back(found);
            boxes.push_back({found, sizes});
        }
    }
    for (Row& row : shot.rows) {
        if (row.nameSure != Certainty::Sure || row.commodity == 0) {
            row.commodity = matchCommodity(row.read, directory.commodityNames(), listed, row.name, row.nameSure);
        }
        // the sizes of what the terminal sells, as the log listed them - all
        // of them sizes UEX takes, or none sent (UEX keeps what it has)
        row.boxes.clear();
        if (row.commodity == 0) continue;
        for (const auto& [id, sizes] : boxes) {
            if (id != row.commodity) continue;
            if (boxesUexTakes(sizes)) row.boxes = sizes;
            // the 1 SCU box read as picked, where the log lists none for it:
            // "Total: 1 SCU" misread, the price one box's after all
            if (row.oneScuBox && ("," + sizes + ",").find(",1,") == std::string::npos) {
                row.oneScuBox = false;
                row.perUnit = true;
                row.priceSure = Certainty::Missing;
            }
        }
    }
}

std::string sentKey(const Sent& sent) {
    return "price|" + std::to_string(sent.terminal) + "|" + std::to_string(sent.side) + "|" +
           std::to_string(sent.commodity) + "|" + std::to_string(sent.sent) + "|" + std::to_string(sent.taken);
}

bool readSentKey(const std::string& key, Sent& sent) {
    if (key.rfind("price|", 0) != 0) return false;
    long long parts[5] = {};
    size_t from = 6;
    for (int i = 0; i < 5; i++) {
        const size_t end = key.find('|', from);
        const std::string part = key.substr(from, end == std::string::npos ? std::string::npos : end - from);
        if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos || part.size() > 15) return false;
        parts[i] = std::stoll(part);
        if ((end == std::string::npos) != (i == 4)) return false;
        from = end + 1;
    }
    sent.terminal = (int)parts[0];
    sent.side = (int)parts[1];
    sent.commodity = (int)parts[2];
    sent.sent = parts[3];
    sent.taken = parts[4];
    return sent.terminal > 0 && (sent.side == 1 || sent.side == 2) && sent.commodity > 0;
}

Hold holdOf(const Shot& shot, const Row& row, const std::vector<Sent>& sent, long long now, long long* seconds) {
    if (seconds) *seconds = 0;
    if (shot.terminal <= 0 || row.commodity <= 0 || shot.side == 0) return Hold::None;
    Hold hold = Hold::None;
    for (const Sent& s : sent) {
        if (s.terminal != shot.terminal || s.side != shot.side || s.commodity != row.commodity) continue;
        // the same moment at the terminal, off another picture (one scrolled
        // a little further): it went with that one
        const long long apart = s.taken > shot.taken ? s.taken - shot.taken : shot.taken - s.taken;
        if (shot.taken > 0 && s.taken > 0 && apart < COOLDOWN_SECONDS) return Hold::Sent;
        // ...or a later look, but UEX's five minutes since the last are not up
        if (now - s.sent < COOLDOWN_SECONDS) {
            hold = Hold::Wait;
            if (seconds) *seconds = (std::max)(*seconds, COOLDOWN_SECONDS - (now - s.sent));
        }
    }
    return hold;
}

std::vector<const Row*> rowsToSend(const Shot& shot, const std::vector<Sent>& sent, long long now) {
    std::vector<const Row*> out;
    for (const Row& row : shot.rows) {
        if (row.removed || holdOf(shot, row, sent, now) == Hold::Sent) continue;
        out.push_back(&row);
    }
    return out;
}

std::string whyNotSend(const Shot& shot, const std::vector<Sent>& sent, long long now) {
    if (shot.state == Shot::State::Sending) return "Sending - Waiting for UEX";
    // ...but a report UEX stopped at a duplicate took only the rows before
    // it: the rows after it can still go, and rowsToSend leaves out the rest
    if (shot.state == Shot::State::Sent) return "Sent - This picture's report is on UEX";
    if (shot.taken > 0 && now - shot.taken > OLDEST_SECONDS) return "Picture older than 30 days - UEX takes reports up to 30 days old";
    if (shot.channel.empty()) return "Game version not known - The picture is not in a LIVE or PTU folder";
    if (shot.terminal <= 0) return "Terminal not known - Select the terminal the picture shows";
    if (shot.side != 1 && shot.side != 2) return "List not known - Select BUY or SELL, as the picture shows";
    const std::vector<const Row*> rows = rowsToSend(shot, sent, now);
    if (rows.empty()) return "Nothing to send - Every row is removed or already sent";
    // a picked cargo box: the price on the picture is one box's, which UEX
    // does not take - the row goes, or the picture is taken again (one unit
    // of the 1 SCU box is a SCU, and no perUnit: oneScuBox)
    for (const Row* row : rows) {
        if (row->perUnit) {
            const std::string& name = !row->name.empty() ? row->name : row->read;
            return (name.empty() ? std::string("A row") : name) +
                   " shows a price per unit - Remove its row, or take the picture again with nothing selected";
        }
    }
    int missing = 0, check = 0;
    long long wait = 0;
    for (const Row* row : rows) {
        const Certainty sure[] = {row->nameSure, row->levelSure, row->stockSure, row->priceSure};
        for (Certainty c : sure) {
            if (c == Certainty::Missing) missing++;
            else if (c == Certainty::Check) check++;
        }
        // a value not there at all, whatever its mark - and a cargo box's
        // price, which is no price per SCU
        if (row->commodity <= 0 || row->level < 1 || row->level > 7 || row->stock < 0 || row->price < 0 || row->perUnit) {
            missing++;
        }
        long long seconds = 0;
        if (holdOf(shot, *row, sent, now, &seconds) == Hold::Wait) wait = (std::max)(wait, seconds);
    }
    if (missing) return "Values missing - Fill in the red fields or remove their rows";
    if (check) {
        return std::to_string(check) + (check == 1 ? " value" : " values") +
               " to check - Compare with the picture, then click Confirm";
    }
    if (wait > 0) {
        const long long minutes = (wait + 59) / 60;
        return "Reported less than 5 minutes ago - Send in " + std::to_string(minutes) + " min";
    }
    return "";
}

std::string reportBody(const Shot& shot, const std::vector<Sent>& sent, long long now, const std::string& gameVersion,
                       bool production) {
    std::vector<UexPriceRow> rows;
    for (const Row* row : rowsToSend(shot, sent, now)) {
        UexPriceRow r;
        r.commodity = row->commodity;
        r.price = row->price;
        r.stock = row->stock;
        r.level = row->level;
        r.boxes = shot.side == 1 ? row->boxes : std::string();
        rows.push_back(r);
    }
    const std::string picture = shot.jpeg.empty() ? std::string() : UexReports::base64(shot.jpeg.data(), shot.jpeg.size());
    // the picture's own time, where UEX still takes it
    const long long when = shot.taken > 0 && shot.taken <= now && now - shot.taken <= OLDEST_SECONDS ? shot.taken : 0;
    return UexReports::priceReport(shot.terminal, shot.side == 2, rows, when, gameVersion, picture, production);
}

}  // namespace Datarunner
}  // namespace SCMiner
