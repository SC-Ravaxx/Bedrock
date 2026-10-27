#include "TradeBook.h"

#include "CommodityData.h"
#include "Reputation.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace SCMiner {

namespace {

// name[value] in a log line: the value, "" where the line has no such field
std::string_view fieldOf(std::string_view line, std::string_view name) {
    size_t at = 0;
    while ((at = line.find(name, at)) != std::string_view::npos) {
        const size_t open = at + name.size();
        // the name whole: "price[" and not the end of "shopPricePerCentiSCU["
        const bool whole = at == 0 || line[at - 1] == ' ' || line[at - 1] == '[';
        if (whole && open < line.size() && line[open] == '[') {
            const size_t close = line.find(']', open + 1);
            if (close == std::string_view::npos) return {};
            return line.substr(open + 1, close - open - 1);
        }
        at = open;
    }
    return {};
}

double numberOf(std::string_view text) {
    return std::strtod(std::string(text).c_str(), nullptr);
}

// "<2026-09-28T11:37:34.512Z> ..." -> "2026-09-28T11:37:34.512"; "" for a line without its time
std::string stampOf(std::string_view line) {
    if (line.size() < 26 || line[0] != '<' || line[5] != '-' || line[11] != 'T' || line[20] != '.') return {};
    const size_t end = line.find_first_of("Z>", 21);
    if (end == std::string_view::npos || end > 30) return {};
    return std::string(line.substr(1, end - 1));
}

constexpr std::string_view SELL = "SendCommoditySellRequest>";
constexpr std::string_view BUY = "SendCommodityBuyRequest>";
constexpr std::string_view ANSWER = "RmToken_CommodityTransactionResponse>";
constexpr int ANSWER_WITHIN = 5;   // seconds a refusal may take: 0.4 measured

// A resource's name in the game's records in words, upper case: at each
// turn from small letters to a capital, from letters to figures and at
// anything else - "ShipAmmoSize4" SHIP AMMO SIZE 4, "RMC" RMC
std::vector<std::string> recordWords(const std::string& name) {
    std::vector<std::string> words;
    std::string word;
    for (size_t i = 0; i < name.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c)) {
            if (!word.empty()) words.push_back(word);
            word.clear();
            continue;
        }
        if (!word.empty()) {
            const unsigned char before = static_cast<unsigned char>(name[i - 1]);
            if ((std::isdigit(c) != 0) != (std::isdigit(before) != 0) || (std::isupper(c) && std::islower(before))) {
                words.push_back(word);
                word.clear();
            }
        }
        word += static_cast<char>(std::toupper(c));
    }
    if (!word.empty()) words.push_back(word);
    return words;
}

// ...and a commodity's name as it is written: "Ship Ammunition - Size 4"
// SHIP AMMUNITION SIZE 4
std::vector<std::string> writtenWords(const std::string& name) {
    std::vector<std::string> words;
    std::string word;
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            word += static_cast<char>(std::toupper(c));
        } else if (c != '\'' && !word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    if (!word.empty()) words.push_back(word);
    return words;
}

}  // namespace

bool TradeBook::parseRequest(std::string_view line, Trade& out) {
    if (line.empty() || line[0] != '<') return false;
    const bool sold = line.find(SELL) != std::string_view::npos;
    if (!sold && line.find(BUY) == std::string_view::npos) return false;
    Trade trade;
    trade.sold = sold;
    trade.stamp = stampOf(line);
    trade.resource = std::string(fieldOf(line, "resourceGUID"));
    trade.shop = std::string(fieldOf(line, "shopName"));
    trade.total = numberOf(fieldOf(line, sold ? "amount" : "price"));
    // "64" SCU, or "5000.000000 cSCU"
    const std::string_view quantity = fieldOf(line, "quantity");
    trade.scu = numberOf(quantity);
    if (quantity.find("cSCU") != std::string_view::npos) trade.scu /= 100.0;
    if (trade.stamp.empty() || trade.resource.size() != 36 || trade.total <= 0 || trade.scu <= 0) return false;
    out = trade;
    return true;
}

bool TradeBook::parseRefusal(std::string_view line, bool& sold) {
    if (line.find(ANSWER) == std::string_view::npos || line.find("Error") == std::string_view::npos) return false;
    const std::string_view type = fieldOf(line, "type");
    if (type != "Selling" && type != "Buying") return false;
    sold = type == "Selling";
    return true;
}

const char* TradeBook::nameOf(const std::string& resource) {
    for (int i = 0; i < COMMODITY_NAME_COUNT; i++) {
        if (resource == COMMODITY_NAMES[i].guid) return COMMODITY_NAMES[i].name;
    }
    return "";
}

std::string TradeBook::placeOf(const GameState& state) {
    if (state.place().empty()) return state.system();
    // A gateway stands on both sides of its jump - there is a Pyro Gateway in
    // Stanton and one in Nyx - and UEX tells them apart by the system:
    // "Pyro Gateway (Stanton)"
    const std::string& place = state.place();
    static const std::string GATEWAY = " Gateway";
    if (place.size() > GATEWAY.size() && place.compare(place.size() - GATEWAY.size(), GATEWAY.size(), GATEWAY) == 0 &&
        !state.system().empty()) {
        return place + " (" + state.system() + ")";
    }
    return place;
}

bool TradeBook::line(std::string_view text, const std::string& place) {
    Trade trade;
    if (parseRequest(text, trade)) {
        trade.place = place;
        const std::string key = trade.stamp + (trade.sold ? "S" : "B") + trade.resource;
        lastKey_[trade.sold ? 1 : 0] = key;
        const auto known = byKey_.find(key);
        if (known == byKey_.end()) {
            byKey_[key] = trade;
        } else {
            // The same line read again - a library read anew, the live log
            // become a backup. Whether it was refused is said again by the
            // line after it; where it was made is kept if this reading does
            // not know.
            if (trade.place.empty()) trade.place = known->second.place;
            if (known->second.place == trade.place && !known->second.refused) return false;
            known->second = trade;
        }
        dirty_ = true;
        changes_++;
        return true;
    }
    bool sold = false;
    if (!parseRefusal(text, sold)) return false;
    // The request it answers: the last of its kind, a moment before
    const auto asked = byKey_.find(lastKey_[sold ? 1 : 0]);
    if (asked == byKey_.end() || asked->second.refused) return false;
    const long long at = Reputation::epochOf(stampOf(text).substr(0, 19));
    const long long then = Reputation::epochOf(asked->second.stamp.substr(0, 19));
    if (at <= 0 || then <= 0 || at < then || at - then > ANSWER_WITHIN) return false;
    asked->second.refused = true;
    dirty_ = true;
    changes_++;
    return true;
}

bool TradeBook::live(std::string_view text) {
    GameState::Event event;
    if (GameState::parse(text, &event)) where_.apply(event);
    if (text.find("Commodity") == std::string_view::npos) return false;
    // a commodity terminal in use: any line of its own
    if (text.find("CEntityComponentCommodityUIProvider") != std::string_view::npos ||
        text.find("AddingCommodityBox") != std::string_view::npos) {
        const long long at = millisOf(stampOf(text));
        if (at > terminalUsedMs_) {
            terminalUsedMs_ = at;
            const std::string_view shop = fieldOf(text, "shopName");
            if (!shop.empty()) terminalUsedShop_ = std::string(shop);
        }
    }
    std::string resource, sizes;
    if (parseBoxes(text, resource, sizes)) {
        // a terminal's list filling: its lines come at once, a new fill is
        // another shop or the same one a few seconds later
        const long long at = millisOf(stampOf(text));
        const std::string shopId(fieldOf(text, "shopId"));
        if (fills_.empty() || fills_.back().shopId != shopId || at < 0 || at - fills_.back().ms > 2000) {
            TerminalFill fill;
            fill.ms = at;
            fill.shopId = shopId;
            fill.shop = std::string(fieldOf(text, "shopName"));
            fill.place = placeOf(where_);
            fills_.push_back(std::move(fill));
            while (fills_.size() > FILLS_KEPT) fills_.pop_front();
        }
        fills_.back().boxes[resource] = sizes;
        return false;
    }
    return line(text, placeOf(where_));
}

const TradeBook::TerminalFill* TradeBook::terminalAt(long long ms, long long within) const {
    for (auto it = fills_.rbegin(); it != fills_.rend(); ++it) {
        if (it->ms > ms) continue;
        return ms - it->ms <= within ? &*it : nullptr;
    }
    return nullptr;
}

bool TradeBook::parseBoxes(std::string_view line, std::string& resource, std::string& sizes) {
    // the shop's own list (LoadShopInventoryData); the player's boxes come
    // in lines of the same name, with Cargo Box Data and no sizes
    if (line.find("AddingCommodityBox") == std::string_view::npos) return false;
    std::string_view name = fieldOf(line, "commodityName");
    constexpr std::string_view KIND = "ResourceType.";
    if (name.substr(0, KIND.size()) == KIND) name.remove_prefix(KIND.size());
    const size_t listed = line.find("Available Box Sizes:");
    if (name.empty() || listed == std::string_view::npos) return false;
    resource = std::string(name);
    sizes.clear();
    constexpr std::string_view BOX = "boxSize[";
    for (size_t at = line.find(BOX, listed); at != std::string_view::npos; at = line.find(BOX, at + BOX.size())) {
        const size_t close = line.find(']', at + BOX.size());
        if (close == std::string_view::npos) break;
        const std::string_view figure = line.substr(at + BOX.size(), close - at - BOX.size());
        int size = figure.empty() ? -1 : 0;
        for (const char c : figure) size = std::isdigit(static_cast<unsigned char>(c)) && size >= 0 && size < 100000 ? size * 10 + (c - '0') : -1;
        if (size <= 0) continue;
        sizes += (sizes.empty() ? "" : ",") + std::to_string(size);
    }
    return true;
}

bool TradeBook::resourceIs(const std::string& resource, const std::string& name) {
    // the game's own older name for one, its misspellings of two, and a name
    // it shortens
    static const std::pair<const char*, const char*> RECORDS[] = {
        {"PlasmaFuel", "HydrogenFuel"}, {"Atlassium", "Atlasium"}, {"Flourine", "Fluorine"},
        {"CK13SeedBlend", "CK13GIDSeedBlend"}};
    std::string key = resource;
    for (const auto& [record, written] : RECORDS) {
        if (key == record) key = written;
    }
    std::vector<std::string> record = recordWords(key);
    std::vector<std::string> written = writtenWords(name);
    if (record.empty() || written.empty()) return false;
    for (std::string& word : record) {
        if (word == "AMMO") word = "AMMUNITION";
    }
    // the same letters and figures in a row - "LifeCureMedsticks" LifeCure
    // Medsticks, "Etam" E'tam
    std::string recordRun, writtenRun;
    for (const std::string& word : record) recordRun += word;
    for (const std::string& word : written) writtenRun += word;
    if (recordRun == writtenRun) return true;
    // a name's initials: RMC, Recycled Material Composite
    if (record.size() == 1 && written.size() > 1 && record[0].size() == written.size()) {
        bool initials = true;
        for (size_t i = 0; i < written.size() && initials; i++) {
            initials = std::isalpha(static_cast<unsigned char>(record[0][i])) && written[i][0] == record[0][i];
        }
        if (initials) return true;
    }
    // the same words, in any order: SHIP COUNTERMEASURES DECOY is Ship Decoy
    // Countermeasures
    std::sort(record.begin(), record.end());
    std::sort(written.begin(), written.end());
    return record == written;
}

long long TradeBook::millisOf(const std::string& stamp) {
    const long long seconds = Reputation::epochOf(stamp.substr(0, 19));
    if (seconds < 0) return -1;
    long long ms = seconds * 1000;
    // the thousandths after the point, where the stamp has them
    if (stamp.size() >= 23 && stamp[19] == '.') {
        int part = 0;
        for (size_t i = 20; i < 23; i++) {
            if (!std::isdigit(static_cast<unsigned char>(stamp[i]))) return ms;
            part = part * 10 + (stamp[i] - '0');
        }
        ms += part;
    }
    return ms;
}

void TradeBook::liveLogRestarted() {
    where_ = GameState();
    lastKey_[0].clear();
    lastKey_[1].clear();
}

bool TradeBook::hasRequest(std::string_view log) {
    return log.find("SendCommodity") != std::string_view::npos;
}

bool TradeBook::replay(std::string_view log) {
    GameState where;
    bool changed = false;
    size_t pos = 0;
    // The live log's last requests stay what they were: a refusal on its way
    // for one of them is still to be matched to it. Backups are read between
    // two looks at the live log (BlueprintLibrary::poll), so the keys are put
    // back after this replay; otherwise a backup read in between would leave
    // that refusal nothing to answer.
    const std::string liveKeys[2] = {lastKey_[0], lastKey_[1]};
    lastKey_[0].clear();
    lastKey_[1].clear();
    while (pos < log.size()) {
        size_t nl = log.find('\n', pos);
        if (nl == std::string_view::npos) nl = log.size();
        std::string_view text = log.substr(pos, nl - pos);
        while (!text.empty() && text.back() == '\r') text.remove_suffix(1);
        GameState::Event event;
        if (GameState::parse(text, &event)) where.apply(event);
        if (text.find("Commodity") != std::string_view::npos) changed = line(text, placeOf(where)) || changed;
        pos = nl + 1;
    }
    lastKey_[0] = liveKeys[0];
    lastKey_[1] = liveKeys[1];
    return changed;
}

std::vector<Trade> TradeBook::trades() const {
    std::vector<Trade> out;
    out.reserve(byKey_.size());
    for (const auto& [key, trade] : byKey_) out.push_back(trade);
    return out;
}

// stamp <TAB> S|B <TAB> resource <TAB> scu <TAB> total <TAB> shop <TAB> place <TAB> refused
void TradeBook::load(const std::filesystem::path& file) {
    file_ = file;
    byKey_.clear();
    dirty_ = false;
    std::ifstream in(file);
    std::string row;
    while (std::getline(in, row)) {
        while (!row.empty() && row.back() == '\r') row.pop_back();
        if (row.empty() || row[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(row);
        std::string part;
        while (std::getline(ss, part, '\t')) f.push_back(part);
        if (f.size() < 6 || (f[1] != "S" && f[1] != "B")) continue;
        Trade trade;
        trade.stamp = f[0];
        trade.sold = f[1] == "S";
        trade.resource = f[2];
        trade.scu = std::strtod(f[3].c_str(), nullptr);
        trade.total = std::strtod(f[4].c_str(), nullptr);
        trade.shop = f[5];
        if (f.size() > 6) trade.place = f[6];
        trade.refused = f.size() > 7 && f[7] == "1";
        if (trade.stamp.empty() || trade.resource.empty() || trade.scu <= 0) continue;
        byKey_[trade.stamp + (trade.sold ? "S" : "B") + trade.resource] = trade;
    }
    changes_++;
}

void TradeBook::save() {
    if (!dirty_ || file_.empty()) return;
    std::ofstream out(file_, std::ios::trunc);
    if (!out.is_open()) return;
    out << "# Bedrock: what was bought and sold at the commodity terminals, off the game's log.\n"
        << "# time (UTC), S sold / B bought, the commodity's GUID, SCU, aUEC in all, the shop, the place, 1 = refused\n";
    char figures[96];
    for (const auto& [key, trade] : byKey_) {
        std::snprintf(figures, sizeof(figures), "%.2f\t%.0f", trade.scu, trade.total);
        out << trade.stamp << "\t" << (trade.sold ? "S" : "B") << "\t" << trade.resource << "\t" << figures << "\t"
            << trade.shop << "\t" << trade.place << "\t" << (trade.refused ? "1" : "0") << "\n";
    }
    dirty_ = false;
}

}  // namespace SCMiner
