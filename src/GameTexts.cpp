#include "GameTexts.h"

#include "AppPaths.h"
#include "CraftingData.h"
#include "GameTextMarks.h"
#include "P4kReader.h"
#include "resource.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace SCMiner {
namespace {

constexpr const char* DOT = " \xC2\xB7 ";       // " · " in UTF-8
constexpr const char* ARCHIVE_NAME = "Data\\Localization\\english\\global.ini";

// What is still to get is blue - <EM4>, the highlight the game's own English
// text uses and the mobiGlas shows light blue - and what is owned is plain.
// In the game, of <EM0> to <EM9> the mobiGlas shows EM4 blue and EM3
// underlined, the rest as plain text; <b>, <i>, <u>, <span style=color>,
// <color=> and <font color> come out as raw text, tags and all; and its
// font has none of the symbols tried (a tick, a cross, dots, stars,
// arrows - all empty boxes). Blue against plain is the whole choice.
std::string stillToGet(const std::string& text) { return "<EM4>" + text + "</EM4>"; }

std::string lower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
    return s;
}

// "Foxwell_..._Desc,P" and "Foxwell_..._Desc" are one key to the game
std::string bare(const std::string& key) {
    std::string k = lower(key);
    if (k.size() > 2 && k.compare(k.size() - 2, 2, ",p") == 0) k.resize(k.size() - 2);
    return k;
}

// Two names that Windows, and Wine, take for one: the same but for case.
// Compared in the system's own spelling of a path, so that a name with
// letters no code page has cannot throw.
bool sameName(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto fold = [](std::filesystem::path::string_type s) {
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') c = (std::filesystem::path::value_type)(c + 32);
        }
        return s;
    };
    return fold(a.native()) == fold(b.native());
}

const std::vector<std::string>& targetSteps() {
    static const std::vector<std::string> steps = {"data", "Localization", "english", "global.ini"};
    return steps;
}

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream s;
    s << in.rdbuf();
    out = s.str();
    return true;
}

bool writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(text.data(), (std::streamsize)text.size());
    return out.good();
}

std::string now() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &local);
    return buf;
}

// The game's own text for this channel, out of its Data.p4k - or out of the
// copy kept in Bedrock's data folder, while the archive is the same one
bool baseText(const std::filesystem::path& channel, std::string& text, std::string& error) {
    const std::filesystem::path archive = GameTexts::placeFor(channel, {"Data.p4k"});
    std::error_code ec;
    const auto size = std::filesystem::file_size(archive, ec);
    if (ec) {
        error = "no Data.p4k in " + channel.string();
        return false;
    }
    const auto time = std::filesystem::last_write_time(archive, ec);
    const std::string stamp = std::to_string(size) + " " + std::to_string((long long)time.time_since_epoch().count());
    // one copy per channel: LIVE and PTU are different builds
    const std::string name = "game_text_" + channel.filename().string();
    const std::filesystem::path copy = AppPaths::data() / (name + ".ini");
    const std::filesystem::path copyStamp = AppPaths::data() / (name + ".txt");
    std::string kept;
    if (readFile(copyStamp, kept) && kept == stamp && readFile(copy, text) && !text.empty()) return true;
    if (!P4kReader::read(archive, ARCHIVE_NAME, text, error)) return false;
    // kept for the next time; a copy that could not be written only costs
    // the next write another read of the archive
    if (writeFile(copy, text)) writeFile(copyStamp, stamp);
    return true;
}

}  // namespace

std::vector<std::filesystem::path> GameTexts::spellings(const std::filesystem::path& base,
                                                       const std::vector<std::string>& steps) {
    std::vector<std::filesystem::path> found = {base};
    for (size_t s = 0; s < steps.size() && !found.empty(); s++) {
        const std::filesystem::path step = steps[s];
        const bool last = s + 1 == steps.size();
        std::vector<std::filesystem::path> next;
        for (const auto& dir : found) {
            std::error_code ec;
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (!sameName(it->path().filename(), step)) continue;
                std::error_code kind;
                if (last ? it->is_regular_file(kind) : it->is_directory(kind)) next.push_back(it->path());
            }
        }
        found = std::move(next);
    }
    return found;
}

std::filesystem::path GameTexts::placeFor(const std::filesystem::path& base, const std::vector<std::string>& steps) {
    std::filesystem::path at = base;
    for (const auto& step : steps) {
        std::filesystem::path next = at / step;
        std::error_code ec;
        if (!std::filesystem::exists(next, ec)) {
            for (std::filesystem::directory_iterator it(at, ec), end; !ec && it != end; it.increment(ec)) {
                if (sameName(it->path().filename(), step)) {
                    next = it->path();
                    break;
                }
            }
        }
        at = next;
    }
    return at;
}

std::filesystem::path GameTexts::target(const std::filesystem::path& channel) {
    return placeFor(channel, targetSteps());
}

std::string GameTexts::render(const char* text, const std::string& value, const OwnedFn& owned) {
    std::string out;
    const char* p = text;
    while (*p) {
        if (*p != '{') {
            out += *p++;
            continue;
        }
        const char* close = std::strchr(p, '}');
        if (!close) {            // not written by gen_game_texts.py; kept as it is
            out += p;
            break;
        }
        const std::string tok(p + 1, close);
        p = close + 1;
        if (tok == "S") {
            if (value.empty() || value.back() != ' ') out += ' ';
            continue;
        }
        if (tok.empty()) continue;
        const char kind = tok[0];
        if (kind == 'I') {
            const size_t bar = tok.find('|');
            if (bar == std::string::npos) continue;
            const std::string line = tok.substr(bar + 1);
            out += owned(std::atoi(tok.c_str() + 1)) ? line : stillToGet(line);
            continue;
        }
        // O, A, T: n:rows, and T its tag after a bar
        const size_t colon = tok.find(':');
        if (colon == std::string::npos) continue;
        const int n = std::atoi(tok.c_str() + 1);
        const size_t bar = tok.find('|', colon);
        const std::string list = tok.substr(colon + 1, bar == std::string::npos ? std::string::npos : bar - colon - 1);
        int have = 0;
        for (size_t at = 0; at < list.size();) {
            const size_t comma = list.find(',', at);
            const std::string one = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (!one.empty() && owned(std::atoi(one.c_str()))) have++;
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        // The count says what the colours mean: blue is what is missing
        if (kind == 'O') {
            if (n > 0 && have == n) out += DOT + std::string("all owned");
            else if (have == 0) out += DOT + stillToGet("none owned");
            else out += DOT + stillToGet(std::to_string(n - have) + " missing");
        } else if (kind == 'A') {
            if (n > 0 && have == n) out += DOT + std::string("all owned");
        } else if (kind == 'T') {
            const std::string tag = bar == std::string::npos ? std::string("[BP]") : tok.substr(bar + 1);
            out += (n > 0 && have == n) ? tag : stillToGet(tag);
        }
    }
    return out;
}

int GameTexts::compose(const std::string& base, const OwnedFn& owned, std::string& out) {
    // The marks by key: as spelt, and without the ',P' some keys carry,
    // which a patch has been known to add or drop
    static const auto byKey = [] {
        std::unordered_map<std::string, int> m;
        for (int i = 0; i < GAME_TEXT_MARK_COUNT; i++) m.emplace(lower(GAME_TEXT_MARKS[i].key), i);
        for (int i = 0; i < GAME_TEXT_MARK_COUNT; i++) m.emplace(bare(GAME_TEXT_MARKS[i].key), i);
        return m;
    }();
    out.clear();
    out.reserve(base.size() + 512 * 1024);
    const bool crlf = base.find("\r\n") != std::string::npos;
    const std::string nl = crlf ? "\r\n" : "\n";
    int marked = 0;
    size_t at = 0;
    while (at <= base.size()) {
        size_t end = base.find(nl, at);
        if (end == std::string::npos) end = base.size();
        const std::string line = base.substr(at, end - at);
        out += line;
        const size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) {
            // the BOM belongs to the file, not to the first key
            std::string key = line.substr(0, eq);
            if (at == 0 && key.compare(0, 3, "\xEF\xBB\xBF") == 0) key.erase(0, 3);
            auto it = byKey.find(lower(key));
            if (it == byKey.end()) it = byKey.find(bare(key));
            if (it != byKey.end()) {
                out += render(GAME_TEXT_MARKS[it->second].text, line.substr(eq + 1), owned);
                marked++;
            }
        }
        if (end == base.size()) break;
        out += nl;
        at = end + nl.size();
    }
    // The last line is Bedrock's: a key the game never asks for, that says
    // whose file this is - and so whose it is to rewrite or take away
    if (!out.empty() && out.compare(out.size() - nl.size(), nl.size(), nl) != 0) out += nl;
    out += std::string(MARKER_KEY) + "=Written by Bedrock " + BEDROCK_VERSION +
           ". Untick the box in its Blueprints window to take this file away again.";
    return marked;
}

bool GameTexts::isOurs(const std::string& text) {
    // Bedrock's last line - or, in a file made by hand before Bedrock wrote
    // them, the heading of its lists
    return text.find(std::string("\n") + MARKER_KEY + "=") != std::string::npos ||
           text.find("<EM4>BLUEPRINTS</EM4>") != std::string::npos;
}

bool GameTexts::wantEnglish(std::string& cfg, std::string& language, bool& changed) {
    changed = false;
    size_t at = 0;
    while (at < cfg.size()) {
        size_t end = cfg.find('\n', at);
        if (end == std::string::npos) end = cfg.size();
        std::string line = cfg.substr(at, end - at);
        size_t a = line.find_first_not_of(" \t");
        if (a != std::string::npos && lower(line.substr(a, 10)) == "g_language") {
            // g_language, then '=' - not g_languageAudio
            size_t b = line.find_first_not_of(" \t", a + 10);
            if (b != std::string::npos && line[b] == '=') {
                std::string value = line.substr(b + 1);
                const size_t v0 = value.find_first_not_of(" \t\"");
                const size_t v1 = value.find_last_not_of(" \t\r\"");
                value = (v0 == std::string::npos) ? std::string() : value.substr(v0, v1 - v0 + 1);
                language = value;
                return lower(value) == "english";
            }
        }
        at = end + 1;
    }
    // Not set: the game reads its own text from the archive, and this file
    // would never be looked at. English is what an unset game runs in anyway.
    const std::string nl = cfg.find("\r\n") != std::string::npos || cfg.empty() ? "\r\n" : "\n";
    if (!cfg.empty() && cfg.back() != '\n') cfg += nl;
    cfg += "g_language = english" + nl;
    language = "english";
    changed = true;
    return true;
}

bool GameTexts::build(const std::filesystem::path& channel, const std::vector<bool>& owned,
                      std::string& text, int& marked, std::string& note) {
    std::string base, error;
    if (!baseText(channel, base, error)) {
        note = "the game's text could not be read: " + error;
        return false;
    }
    const OwnedFn isOwned = [&owned](int b) { return b >= 0 && b < (int)owned.size() && owned[b]; };
    marked = compose(base, isOwned, text);
    if (marked == 0) {
        note = "none of the contracts Bedrock knows is in this game's text";
        return false;
    }
    return true;
}

GameTexts::Outcome GameTexts::write(const std::filesystem::path& channel, const std::vector<bool>& owned) {
    Outcome o;
    // The file in every spelling there is. Someone else's in any of them keeps
    // Bedrock out - and Bedrock's own goes, should it stand beside a pack in
    // another spelling (only under Linux can it): the game would read one of
    // the two, and never should that be Bedrock's over the pack. Under
    // Windows the pack would have replaced it.
    std::vector<std::filesystem::path> ours;
    bool foreign = false;
    for (const auto& one : spellings(channel, targetSteps())) {
        std::string present;
        if (!readFile(one, present)) continue;
        if (isOurs(present)) ours.push_back(one);
        else foreign = true;
    }
    if (foreign) {
        for (const auto& one : ours) {
            std::error_code ignored;
            std::filesystem::remove(one, ignored);
        }
        o.note = "the game has someone else's language file - left alone";
        return o;
    }
    const std::filesystem::path file = ours.empty() ? target(channel) : ours.front();
    const std::filesystem::path cfgPath = placeFor(channel, {"user.cfg"});
    std::string cfg, language;
    readFile(cfgPath, cfg);
    bool cfgChanged = false;
    if (!wantEnglish(cfg, language, cfgChanged)) {
        o.note = "the game is set to " + language + " - the marks are English only";
        return o;
    }
    std::string text;
    int marked = 0;
    if (!build(channel, owned, text, marked, o.note)) return o;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Written beside and then put in place: a game starting this very moment
    // reads the old file or the new one, never half of one
    const std::filesystem::path fresh = file.string() + ".bedrock";
    if (!writeFile(fresh, text)) {
        o.note = "could not write into " + file.parent_path().string();
        return o;
    }
    std::filesystem::rename(fresh, file, ec);
    if (ec) {
        std::filesystem::copy_file(fresh, file, std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code ignored;
        std::filesystem::remove(fresh, ignored);
        if (ec) {
            o.note = "could not put the file in place: " + ec.message();
            return o;
        }
    }
    // a second one of Bedrock's in another spelling (a folder renamed by hand)
    for (size_t i = 1; i < ours.size(); i++) {
        std::error_code ignored;
        std::filesystem::remove(ours[i], ignored);
    }
    if (cfgChanged && !writeFile(cfgPath, cfg)) {
        o.note = "written, but user.cfg could not be told g_language = english";
        o.written = true;
        return o;
    }
    o.written = true;
    o.note = "in the game from its next start (written " + now() + ")";
    return o;
}

GameTexts::Outcome GameTexts::remove(const std::filesystem::path& channel) {
    Outcome o;
    // Bedrock's in every spelling there is, and nobody else's
    bool any = false, foreign = false;
    std::error_code failed;
    for (const auto& one : spellings(channel, targetSteps())) {
        std::string present;
        if (!readFile(one, present)) continue;
        any = true;
        if (!isOurs(present)) {
            foreign = true;
            continue;
        }
        std::error_code ec;
        std::filesystem::remove(one, ec);
        if (ec) failed = ec;
    }
    if (failed) o.note = "could not take the file away: " + failed.message();
    else if (foreign) o.note = "the game has someone else's language file - left alone";
    else if (!any) o.note = "the game shows its own texts";
    else o.note = "taken away - the game shows its own texts from its next start";
    return o;
}

long long GameTexts::outdatedSince(const std::filesystem::path& channel, std::chrono::seconds settled) {
    std::error_code ec;
    const auto archive = std::filesystem::last_write_time(placeFor(channel, {"Data.p4k"}), ec);
    if (ec) return 0;
    // still being written to: the launcher's patch is not over, and what
    // stands in the archive now is neither build
    if (std::filesystem::file_time_type::clock::now() - archive < settled) return 0;
    for (const auto& one : spellings(channel, targetSteps())) {
        std::error_code e;
        const auto written = std::filesystem::last_write_time(one, e);
        if (e || written >= archive) continue;
        // read only now: ten megabytes, and nearly always the newer one
        std::string present;
        if (readFile(one, present) && isOurs(present)) return (long long)archive.time_since_epoch().count();
    }
    return 0;
}

std::vector<std::filesystem::path> GameTexts::channelsBeside(const std::filesystem::path& channel) {
    std::vector<std::filesystem::path> out = {channel};
    std::error_code ec;
    for (std::filesystem::directory_iterator it(channel.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (!it->is_directory(e)) continue;
        bool listed = false;
        for (const auto& one : out) {
            std::error_code same;
            if (std::filesystem::equivalent(it->path(), one, same)) listed = true;
        }
        if (listed) continue;
        if (std::filesystem::exists(placeFor(it->path(), {"Data.p4k"}), e)) out.push_back(it->path());
    }
    return out;
}

}  // namespace SCMiner
