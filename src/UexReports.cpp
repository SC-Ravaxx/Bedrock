#include "UexReports.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>

#include "AppPaths.h"
#include "MiniJson.h"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace SCMiner {

namespace {

// Lower case, letters and digits only, and the letter O read as a nought:
// the game writes Shubin's facility "SMO-18", UEX "SM0-18"
std::string folded(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c)) out += (char)std::tolower(c);
    }
    std::replace(out.begin(), out.end(), 'o', '0');
    return out;
}

// A commodity's name the way two lists can agree on it: lower case, trimmed,
// and Aluminium the American way UEX spells it
std::string commodityKey(const std::string& name) {
    std::string out;
    for (unsigned char c : name) out += (char)std::tolower(c);
    const size_t at = out.find("aluminium");
    if (at != std::string::npos) out.replace(at, 9, "aluminum");
    const size_t first = out.find_first_not_of(' ');
    const size_t last = out.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
}

// A refining method by its letters alone, without the word "method": the
// terminal says "Cormack Method", UEX "Cormack"
std::string lettersOf(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalpha(c)) out += (char)std::tolower(c);
    }
    if (out.size() > 6 && out.compare(out.size() - 6, 6, "method") == 0) out.resize(out.size() - 6);
    return out;
}

const JsonValue* rowsOf(const JsonValue& answer) {
    if (answer.textOr("status") != "ok") return nullptr;
    const JsonValue* data = answer.find("data");
    return data && data->kind == JsonValue::Kind::Array ? data : nullptr;
}

// "24300", "4.5": no more places than it has
std::string figure(double value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.2f", value);
    std::string out = text;
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

bool holds(const std::vector<std::string>& names, const std::string& name) {
    return !name.empty() && std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

std::string UexRefineryJob::key() const {
    std::string out = station + "|" + std::to_string(number) + "|" + std::to_string(placed);
    for (const Item& item : items) out += "|" + item.material + ":" + std::to_string(item.yield);
    return out;
}

bool UexDirectory::read(const std::string& terminalsJson, const std::string& commoditiesJson,
                        const std::string& methodsJson, std::string& why) {
    JsonValue terminals, commodities, methods;
    if (!parseJson(terminalsJson, terminals, why)) {
        why = "the terminal list: " + why;
        return false;
    }
    if (!parseJson(commoditiesJson, commodities, why)) {
        why = "the commodity list: " + why;
        return false;
    }
    if (!parseJson(methodsJson, methods, why)) {
        why = "the refining methods: " + why;
        return false;
    }
    const JsonValue* terminalRows = rowsOf(terminals);
    const JsonValue* commodityRows = rowsOf(commodities);
    const JsonValue* methodRows = rowsOf(methods);
    if (!terminalRows || !commodityRows || !methodRows) {
        why = "a list UEX sent without its rows";
        return false;
    }
    std::vector<Terminal> readTerminals;
    for (const JsonValue& row : terminalRows->items) {
        // a terminal the game no longer has is no place to report from
        if (row.numberOr("is_available", 1) == 0) continue;
        Terminal t;
        t.id = (int)row.numberOr("id", 0);
        if (t.id <= 0) continue;
        t.type = row.textOr("type");
        t.nickname = row.textOr("nickname");
        for (const char* key : {"name", "fullname", "nickname", "displayname", "space_station_name", "outpost_name",
                                "city_name"}) {
            const std::string name = folded(row.textOr(key));
            if (!name.empty() && !holds(t.names, name)) t.names.push_back(name);
        }
        for (const char* key : {"moon_name", "planet_name", "orbit_name"}) {
            const std::string name = folded(row.textOr(key));
            if (!name.empty() && !holds(t.around, name)) t.around.push_back(name);
        }
        readTerminals.push_back(std::move(t));
    }
    std::vector<std::pair<std::string, int>> readCommodities;
    for (const JsonValue& row : commodityRows->items) {
        const int id = (int)row.numberOr("id", 0);
        const std::string name = commodityKey(row.textOr("name"));
        if (id > 0 && !name.empty()) readCommodities.push_back({name, id});
    }
    std::vector<std::pair<std::string, int>> readMethods;
    for (const JsonValue& row : methodRows->items) {
        const int id = (int)row.numberOr("id", 0);
        const std::string name = lettersOf(row.textOr("name"));
        if (id > 0 && !name.empty()) readMethods.push_back({name, id});
    }
    if (readTerminals.empty() || readCommodities.empty()) {
        why = "UEX's lists hold no terminals or no commodities";
        return false;
    }
    terminals_ = std::move(readTerminals);
    commodities_ = std::move(readCommodities);
    methods_ = std::move(readMethods);
    return true;
}

int UexDirectory::commodity(const std::string& name) const {
    // The refinery's names and UEX's part here and there: "Raw Ice" is its
    // "Ice (Raw)", "Stileron (Ore)" its "Stileron (Raw)", "Construction
    // Rubble" its "Construction Material Rubble"
    const std::string key = commodityKey(name);
    std::vector<std::string> tries = {key};
    if (key.rfind("raw ", 0) == 0) tries.push_back(key.substr(4) + " (raw)");
    for (const auto& swap : {std::make_pair("(ore)", "(raw)"), std::make_pair("(raw)", "(ore)")}) {
        const size_t at = key.find(swap.first);
        if (at != std::string::npos) tries.push_back(std::string(key).replace(at, 5, swap.second));
    }
    if (key.rfind("construction ", 0) == 0) tries.push_back("construction material " + key.substr(13));
    for (const std::string& attempt : tries) {
        for (const auto& entry : commodities_) {
            if (entry.first == attempt) return entry.second;
        }
    }
    return 0;
}

int UexDirectory::method(const std::string& name) const {
    const std::string key = lettersOf(name);
    for (const auto& entry : methods_) {
        if (!key.empty() && entry.first == key) return entry.second;
    }
    return 0;
}

int UexDirectory::tradeTerminal(const std::string& shop, const std::string& place, std::string& why) const {
    // What the terminal takes: refined goods, or - at a refinery's ore sales
    // - the ore as it came out of the rock
    const std::string shopFolded = folded(shop);
    const bool raw = shopFolded.find("0resales") != std::string::npos || shopFolded.find("refinery") != std::string::npos;
    const std::string type = raw ? "commodity_raw" : "commodity";
    // The shop's own name first, where a word of it is a terminal's name:
    // SCShop_Levski_CargoOffice_Commodities is at Levski whatever the log says
    // of the place (in sample logs: Nyx 26 times, Crusader 8, Levski 11).
    std::vector<int> byShop;
    size_t from = 0;
    while (from <= shop.size()) {
        const size_t end = shop.find('_', from);
        const std::string word = folded(shop.substr(from, end == std::string::npos ? std::string::npos : end - from));
        if (word.size() >= 4) {
            for (const Terminal& t : terminals_) {
                if (t.type == type && holds(t.names, word) &&
                    std::find(byShop.begin(), byShop.end(), t.id) == byShop.end()) {
                    byShop.push_back(t.id);
                }
            }
        }
        if (end == std::string::npos) break;
        from = end + 1;
    }
    if (byShop.size() == 1) return byShop.front();
    // ...then the place: a station's or a town's name is one terminal, a
    // moon's often more than one - Pyro IV has four outposts that buy
    const std::string at = folded(place);
    if (at.empty()) {
        why = "the game log doesn't say where";
        return 0;
    }
    std::vector<int> byPlace;
    for (const Terminal& t : terminals_) {
        if (t.type == type && (holds(t.names, at) || holds(t.around, at))) byPlace.push_back(t.id);
    }
    if (byPlace.size() == 1) return byPlace.front();
    // "PYR2 L4", the app's name for a Pyro Lagrange point, is UEX's orbit
    // "Monox Lagrange Point 4" - the rest stop that stands there
    if (byPlace.empty()) {
        static const std::regex pyro(R"(^PYR([1-6]) L([1-5])$)", std::regex::icase);
        std::smatch m;
        const std::string spelled = place;
        if (std::regex_match(spelled, m, pyro)) {
            static const char* const BODIES[] = {"Pyro I", "Monox", "Bloom", "Pyro IV", "Pyro V", "Pyro VI"};
            const std::string orbit = folded(std::string(BODIES[m[1].str()[0] - '1']) + " Lagrange Point " + m[2].str());
            for (const Terminal& t : terminals_) {
                if (t.type == type && holds(t.around, orbit)) byPlace.push_back(t.id);
            }
            if (byPlace.size() == 1) return byPlace.front();
        }
    }
    why = byPlace.empty() ? "UEX has no terminal at " + place
                          : std::to_string(byPlace.size()) + " terminals at " + place + ", can't tell which";
    return 0;
}

int UexDirectory::refineryTerminal(const std::string& station, std::string& why) const {
    const std::string at = folded(station);
    std::vector<int> found;
    for (const Terminal& t : terminals_) {
        if (t.type == "refinery" && holds(t.names, at)) found.push_back(t.id);
    }
    if (found.size() == 1) return found.front();
    why = found.empty() ? "UEX has no refinery at " + station
                        : std::to_string(found.size()) + " refineries at " + station + ", can't tell which";
    return 0;
}

std::string UexDirectory::terminalName(int id) const {
    for (const Terminal& t : terminals_) {
        if (t.id == id) return t.nickname;
    }
    return "";
}

namespace UexReports {

std::string priceReport(int terminal, int commodity, bool sold, double perScu, long long when,
                        const std::string& screenshotBase64, bool production) {
    std::string out = "{\"id_terminal\":" + std::to_string(terminal) + ",\"type\":\"commodity\",\"is_production\":" +
                      (production ? "1" : "0") + ",\"prices\":[{\"id_commodity\":" + std::to_string(commodity) +
                      (sold ? ",\"price_sell\":" : ",\"price_buy\":") + figure(perScu) + "}]";
    // when it was seen: UEX takes a report's own date within 30 days
    if (when > 0) out += ",\"date_added\":" + std::to_string(when);
    if (!screenshotBase64.empty()) out += ",\"screenshot\":\"" + screenshotBase64 + "\"";
    out += "}";
    return out;
}

std::string refineryJob(int terminal, int method, double cost, int minutes,
                        const std::vector<std::pair<int, UexRefineryJob::Item>>& items, bool production) {
    std::string out = "{\"is_production\":" + std::string(production ? "1" : "0") +
                      ",\"id_terminal\":" + std::to_string(terminal) + ",\"id_refinery_method\":" + std::to_string(method) +
                      ",\"cost\":" + figure(cost) + ",\"time_minutes\":" + std::to_string(minutes) + ",\"items\":[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i) out += ",";
        out += "{\"id_commodity\":" + std::to_string(items[i].first) +
               ",\"quantity\":" + std::to_string(items[i].second.quantity) +
               ",\"yield\":" + std::to_string(items[i].second.yield) + "}";
    }
    out += "]}";
    return out;
}

namespace {

// The answers that mean the key itself is wrong. user_not_allowed,
// user_disabled and user_not_verified are about the account: one that is
// not a data runner yet answers user_not_allowed to a valid key.
const char* const KEY_ANSWERS[] = {"missing_secret_key", "invalid_secret_key", "user_not_found"};

std::string statusOf(const std::string& body) {
    JsonValue answer;
    std::string why;
    return parseJson(body, answer, why) ? answer.textOr("status") : std::string();
}

}  // namespace

std::string verdict(int httpStatus, const std::string& body, std::string* username) {
    JsonValue answer;
    std::string why;
    const bool reads = parseJson(body, answer, why);
    const std::string status = reads ? answer.textOr("status") : std::string();
    if (status == "ok") {
        if (username) {
            const JsonValue* data = answer.find("data");
            *username = data ? data->textOr("username") : std::string();
        }
        return "";
    }
    if (status == "missing_secret_key" || status == "invalid_secret_key") return "UEX didn't accept the key";
    if (status == "user_not_found") return "UEX has no account with that key";
    // user_not_allowed: "applied restriction (temporary ban, user not a
    // datarunner, etc)" - which is a data runner signing up as a rule
    if (status == "user_not_allowed") return "UEX only takes reports from data runners";
    if (status == "user_disabled") return "UEX has blocked reports from this account";
    if (status == "user_not_verified") return "that UEX account is not verified with RSI yet";
    if (status == "requests_limit_reached") return "too many requests to UEX, try again later";
    if (status == "screenshot_required") return "UEX wants a screenshot with this report";
    if (status == "duplicated_report") return "UEX already has this report";
    if (status == "terminal_not_found") return "UEX doesn't know this terminal";
    if (status == "invalid_date") return "UEX didn't accept the date of the trade";
    if (status == "image_upload_error" || status == "image_storage_error") return "UEX couldn't store the screenshot";
    if (!status.empty()) {
        std::string words = status;
        std::replace(words.begin(), words.end(), '_', ' ');
        return "UEX said: " + words;
    }
    if (httpStatus != 200) return "UEX answered with error " + std::to_string(httpStatus);
    return "UEX's answer couldn't be read";
}

bool keyTrouble(int httpStatus, const std::string& body) {
    const std::string status = statusOf(body);
    for (const char* answer : KEY_ANSWERS) {
        if (status == answer) return true;
    }
    return status.empty() && (httpStatus == 401 || httpStatus == 403);
}

std::string accountTrouble(int httpStatus, const std::string& body) {
    (void)httpStatus;
    const std::string status = statusOf(body);
    if (status == "user_not_allowed") return "datarunner";
    if (status == "user_disabled") return "blocked";
    if (status == "user_not_verified") return "unverified";
    return "";
}

bool keyLooksRight(const std::string& key) {
    if (key.size() != 40) return false;
    return std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
}

std::string base64(const unsigned char* bytes, size_t size) {
    static const char* const ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const unsigned long chunk = ((unsigned long)bytes[i] << 16) |
                                    ((i + 1 < size ? (unsigned long)bytes[i + 1] : 0ul) << 8) |
                                    (i + 2 < size ? (unsigned long)bytes[i + 2] : 0ul);
        out += ALPHABET[(chunk >> 18) & 63];
        out += ALPHABET[(chunk >> 12) & 63];
        out += i + 1 < size ? ALPHABET[(chunk >> 6) & 63] : '=';
        out += i + 2 < size ? ALPHABET[chunk & 63] : '=';
    }
    return out;
}

std::string jsonText(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        } else {
            out += (char)c;
        }
    }
    out += "\"";
    return out;
}

}  // namespace UexReports

namespace UexKey {

namespace {
std::filesystem::path where() { return AppPaths::data() / FILE_NAME; }
}  // namespace

bool save(const std::string& key) {
    std::string sealed;
#ifdef _WIN32
    DATA_BLOB in{(DWORD)key.size(), (BYTE*)const_cast<char*>(key.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Bedrock UEX key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    sealed.assign((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
#else
    sealed = key;
#endif
    const std::filesystem::path file = where();
    const std::filesystem::path fresh = file.string() + ".new";
    {
        std::ofstream write(fresh, std::ios::binary | std::ios::trunc);
        if (!write.is_open()) return false;
        write.write(sealed.data(), (std::streamsize)sealed.size());
        if (!write) return false;
    }
    std::error_code ec;
#ifndef _WIN32
    // the owner alone may read it
    std::filesystem::permissions(fresh, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
#endif
    std::filesystem::rename(fresh, file, ec);
    return !ec;
}

std::string load() {
    std::ifstream read(where(), std::ios::binary);
    if (!read.is_open()) return "";
    const std::string sealed((std::istreambuf_iterator<char>(read)), std::istreambuf_iterator<char>());
    if (sealed.empty()) return "";
#ifdef _WIN32
    DATA_BLOB in{(DWORD)sealed.size(), (BYTE*)const_cast<char*>(sealed.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string key((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return key;
#else
    return sealed;
#endif
}

void forget() {
    std::error_code ec;
    std::filesystem::remove(where(), ec);
}

}  // namespace UexKey

}  // namespace SCMiner
