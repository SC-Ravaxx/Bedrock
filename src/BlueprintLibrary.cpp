#include "BlueprintLibrary.h"

#include "AppPaths.h"
#include "CraftingData.h"
#include "GameTextMarks.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string_view>

namespace SCMiner {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

std::string now() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
    return buf;
}

std::string lowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
    return s;
}

// The time now the way the game's log writes it, "2026-10-04T13:05:00.000"
// in UTC: a rank set by hand is told apart from the contracts before and
// after it by comparing the two
std::string logStampNow() {
    const std::time_t t = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000", &utc);
    return buf;
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil)
std::int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (std::int64_t)era * 146097 + (std::int64_t)doe - 719468;
}

// "<2026-09-30T14:01:53.116Z> ..." -> "2026-09-30 14:01"
std::string stampOf(std::string_view line) {
    if (line.size() < 20 || line[0] != '<' || line[11] != 'T') return {};
    return std::string(line.substr(1, 10)) + " " + std::string(line.substr(12, 5));
}

}  // namespace

// --- contracts that paid nothing -----------------------------------------------

std::int64_t ContractScan::secondsOf(std::string_view line) {
    // <2026-09-30T14:01:53.116Z>
    if (line.size() < 21 || line[0] != '<' || line[5] != '-' || line[8] != '-' || line[11] != 'T' ||
        line[14] != ':' || line[17] != ':') {
        return -1;
    }
    auto num = [&](size_t at, size_t n) {
        int v = 0;
        for (size_t i = at; i < at + n; i++) {
            if (line[i] < '0' || line[i] > '9') return -1;
            v = v * 10 + (line[i] - '0');
        }
        return v;
    };
    const int y = num(1, 4), mo = num(6, 2), d = num(9, 2), h = num(12, 2), mi = num(15, 2), s = num(18, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || h < 0 || mi < 0 || s < 0) return -1;
    return daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
}

std::string ContractScan::bareTitle(std::string_view logTitle) {
    std::string s(logTitle);
    // Bedrock's own marks, when its language file is in: "<EM4>[BP]</EM4>"
    for (;;) {
        const size_t a = s.find('<');
        if (a == std::string::npos) break;
        const size_t b = s.find('>', a);
        if (b == std::string::npos) break;
        s.erase(a, b - a + 1);
    }
    for (const char* mark : {"[BP*]", "[BP]"}) {
        for (size_t k; (k = s.find(mark)) != std::string::npos;) s.erase(k, std::strlen(mark));
    }
    auto trimEnd = [&] {
        while (!s.empty() && (s.back() == ' ' || s.back() == '*')) s.pop_back();
    };
    trimEnd();
    // ...and the reputation the game writes after it: " [300 Rep]", " [60/100 Rep]"
    if (s.size() > 6 && s.compare(s.size() - 5, 5, " Rep]") == 0) {
        const size_t open = s.rfind('[');
        if (open != std::string::npos &&
            std::all_of(s.begin() + (std::ptrdiff_t)open + 1, s.end() - 5,
                        [](char c) { return (c >= '0' && c <= '9') || c == '/'; })) {
            s.erase(open);
            trimEnd();
        }
    }
    // one space between words, none at either end
    std::string out;
    for (char c : s) {
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool ContractScan::titleMatches(const char* pattern, const std::string& bare) {
    const std::string p = lowerAscii(pattern), t = lowerAscii(bare);
    static const std::string ANY = "{*}";
    std::vector<std::string> parts;
    for (size_t at = 0;;) {
        const size_t k = p.find(ANY, at);
        parts.push_back(p.substr(at, k == std::string::npos ? std::string::npos : k - at));
        if (k == std::string::npos) break;
        at = k + ANY.size();
    }
    if (parts.size() == 1) return p == t;
    // the first piece at the start, the last at the end, the rest in order
    if (t.compare(0, parts.front().size(), parts.front()) != 0) return false;
    size_t at = parts.front().size();
    for (size_t i = 1; i + 1 < parts.size(); i++) {
        const size_t k = t.find(parts[i], at);
        if (k == std::string::npos) return false;
        at = k + parts[i].size();
    }
    const std::string& tail = parts.back();
    return t.size() >= at + tail.size() && t.compare(t.size() - tail.size(), tail.size(), tail) == 0;
}

const ContractPayout* ContractScan::payoutFor(const std::string& bare) {
    const ContractPayout* found = nullptr;
    for (int i = 0; i < CONTRACT_PAYOUT_COUNT; i++) {
        if (!titleMatches(CONTRACT_PAYOUTS[i].title, bare)) continue;
        if (found) return nullptr;   // two titles could be this one: not sure which
        found = &CONTRACT_PAYOUTS[i];
    }
    return found;
}

void ContractScan::reset() {
    ruleHolds_ = false;
    versionSeen_ = false;
    pending_.clear();
    last_ = -1;
    lastReceived_ = -1;
    env_.clear();
    version_ = 0;
    contractOf_.clear();
    shared_.clear();
    rankEvents_.clear();
    if (!running_.empty()) runningChanges_++;
    running_.clear();
}

void ContractScan::endRunning(std::string_view mission) {
    for (auto it = running_.begin(); it != running_.end(); ++it) {
        if (it->mission == mission) {
            running_.erase(it);
            runningChanges_++;
            return;
        }
    }
}

// The lines that make a RankEvent. All three kinds come in this order in the
// game's logs - the marker before the notification in 3,461 of 3,462, the
// share always before - so one pass down the file is enough.
void ContractScan::rankLine(std::string_view text) {
    static constexpr std::string_view MARKER = "Creating objective marker: missionId [";
    static constexpr std::string_view SHARE = "Received share push message:";
    static constexpr std::string_view NOTE = "<SHUDEvent_OnNotification> Added notification \"Contract ";
    // what stands in brackets after a word, from `from` on; `end` past it
    auto bracketed = [&](std::string_view word, size_t from, size_t& end) -> std::string_view {
        const size_t w = text.find(word, from);
        if (w == std::string_view::npos) return {};
        const size_t a = w + word.size();
        const size_t b = text.find(']', a);
        if (b == std::string_view::npos) return {};
        end = b + 1;
        return text.substr(a, b - a);
    };
    size_t at = text.find(MARKER);
    if (at != std::string_view::npos) {
        // ...missionId [M], generator name [G], contract [D]
        size_t end = at;
        const std::string_view mission = bracketed("missionId [", at, end);
        const std::string_view generator = bracketed("generator name [", end, end);
        const std::string_view contract = bracketed("contract [", end, end);
        if (mission.size() == 36 && !generator.empty() && !contract.empty()) {
            contractOf_.emplace(std::string(mission), std::string(generator) + "/" + std::string(contract));
        }
        return;
    }
    at = text.find(SHARE);
    if (at != std::string_view::npos) {
        // ...ownerId[..] - missionId[M]
        size_t end = at;
        const std::string_view mission = bracketed("missionId[", at, end);
        if (mission.size() == 36) shared_.insert(std::string(mission));
        return;
    }
    static constexpr std::string_view END = "<EndMission> Ending mission for player. MissionId[";
    static constexpr std::string_view ENDED = "<MissionEnded> Received MissionEnded push message for: mission_id ";
    for (const std::string_view key : {END, ENDED}) {
        at = text.find(key);
        if (at == std::string_view::npos) continue;
        const std::string_view mission = text.substr(at + key.size(), 36);
        if (mission.size() == 36) endRunning(mission);
        return;
    }
    at = text.find(NOTE);
    if (at == std::string_view::npos) return;
    const std::string_view kind = text.substr(at + NOTE.size());
    bool completed = false;
    const bool failed = kind.rfind("Failed:", 0) == 0;
    if (kind.rfind("Accepted:", 0) == 0) completed = false;
    else if (kind.rfind("Complete:", 0) == 0) completed = true;
    else if (!failed) return;
    size_t end = at;
    const std::string_view mission = bracketed("MissionId: [", at, end);
    if (mission.size() != 36) return;
    if (completed || failed) endRunning(mission);
    if (failed) return;
    const bool shared = shared_.count(std::string(mission)) > 0;
    const auto named = contractOf_.find(std::string(mission));
    if (named == contractOf_.end()) return;
    // <2026-10-02T16:39:32.688Z> at the front
    if (text.size() < 25 || text[11] != 'T') return;
    if (!completed) {
        bool known = false;
        for (const Running& r : running_) known = known || r.mission == mission;
        if (!known) {
            running_.push_back(Running{std::string(mission), named->second, std::string(text.substr(1, 23)), shared});
            runningChanges_++;
        }
    }
    // a contract somebody shared proves no rank and earns its owner's board nothing
    if (shared) return;
    rankEvents_.push_back(RankEvent{std::string(text.substr(1, 23)), completed, std::string(mission),
                                    named->second, version_});
}

void ContractScan::line(std::string_view text, std::vector<Unpaid>& out) {
    if (text.empty() || text[0] != '<') return;
    // The header says which game wrote the log, in the name of the servers it
    // talks to: "@env_session: 'pub-sc-alpha-4100-12519617'" is 4.10.0,
    // "...-490-..." 4.9.0. (Its FileVersion is no help: 1.0.191 in August's.)
    if (!versionSeen_) {
        const size_t v = text.find("sc-alpha-");
        if (v != std::string_view::npos) {
            size_t i = v + 9;
            std::string digits;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') digits += text[i++];
            if (digits.size() >= 3 && i < text.size() && text[i] == '-') {
                const int major = digits[0] - '0';
                const std::string rest = digits.substr(1);
                const int minor = rest.size() >= 3 ? std::atoi(rest.substr(0, 2).c_str()) : rest[0] - '0';
                const int patch = rest.size() >= 3 ? rest[2] - '0' : rest[1] - '0';
                ruleHolds_ = major > 4 || (major == 4 && minor >= 10);
                versionSeen_ = true;
                version_ = major * 10000 + minor * 100 + patch;
                // ...and whose servers: "pub" LIVE, "ptu" the PTU - the word
                // in front of "-sc-alpha-"
                if (v > 0 && text[v - 1] == '-') {
                    size_t b = v - 1;
                    while (b > 0 && std::isalnum((unsigned char)text[b - 1])) b--;
                    env_ = lowerAscii(std::string(text.substr(b, v - 1 - b)));
                }
            }
        }
    }
    rankLine(text);
    static constexpr std::string_view RECEIVED =
        "<SHUDEvent_OnNotification> Added notification \"Received Blueprint: ";
    static constexpr std::string_view COMPLETE =
        "<SHUDEvent_OnNotification> Added notification \"Contract Complete: ";
    const bool received = text.find(RECEIVED) != std::string_view::npos;
    const size_t complete = received ? std::string_view::npos : text.find(COMPLETE);
    if (pending_.empty() && !received && complete == std::string_view::npos) return;

    const std::int64_t at = secondsOf(text);
    if (at < 0) return;
    last_ = at;
    // A completion that has waited its window out without a blueprint paid nothing
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (at - it->at > WINDOW) {
            if (!it->ambiguous) out.push_back(Unpaid{it->title, it->payout, it->stamp});
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    if (received) {
        // ...a blueprint: the completion waiting longest is the one that paid it
        if (!pending_.empty()) pending_.erase(pending_.begin());
        else lastReceived_ = at;
        return;
    }
    if (complete == std::string_view::npos || !ruleHolds_) return;
    const size_t start = complete + COMPLETE.size();
    size_t tail = text.find(" to queue", start);
    if (tail == std::string_view::npos) tail = text.size();
    const size_t end = text.rfind(": \" [", tail);
    if (end == std::string_view::npos || end < start) return;
    // A blueprint just before it, that no completion claimed, is its own
    if (lastReceived_ >= 0 && at - lastReceived_ <= 5) {
        lastReceived_ = -1;
        return;
    }
    const std::string bare = bareTitle(text.substr(start, end - start));
    const ContractPayout* payout = payoutFor(bare);
    if (!payout) return;
    // Two completions inside one window: which of them a blueprint belongs to
    // cannot be told, and neither says anything
    bool ambiguous = false;
    for (Pending& p : pending_) {
        p.ambiguous = true;
        ambiguous = true;
    }
    pending_.push_back(Pending{bare, payout, stampOf(text), at, ambiguous});
}

void ContractScan::finish(std::vector<Unpaid>& out) {
    for (const Pending& p : pending_) {
        if (!p.ambiguous && last_ - p.at > WINDOW) out.push_back(Unpaid{p.title, p.payout, p.stamp});
    }
    reset();
}

// --- the library ---------------------------------------------------------------

std::string BlueprintLibrary::normalise(const std::string& raw) {
    std::string s = raw;
    // "<EM4>[BP]</EM4>": the HUD's own markup, still on the names in older logs
    for (;;) {
        const size_t a = s.find('<');
        if (a == std::string::npos) break;
        const size_t b = s.find('>', a);
        if (b == std::string::npos) break;
        s.erase(a, b - a + 1);
    }
    for (size_t k; (k = s.find("[BP]")) != std::string::npos;) s.erase(k, 4);

    // One case, letters and digits only, one space between words. This also
    // turns the no-break space (C2 A0) some names carry into a plain one.
    std::string out;
    bool space = true;
    for (unsigned char c : s) {
        const bool word = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (word) {
            out += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
            space = false;
        } else if (!space) {
            out += ' ';
            space = true;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();

    // Ship parts are announced as "Ind/1/B Guardian": class, size, grade,
    // name. The table stores the bare name.
    if (out.size() > 8 && std::isalpha((unsigned char)out[0]) && std::isalpha((unsigned char)out[1]) &&
        std::isalpha((unsigned char)out[2]) && out[3] == ' ' && std::isdigit((unsigned char)out[4]) &&
        out[5] == ' ' && out[6] >= 'a' && out[6] <= 'd' && out[7] == ' ') {
        out.erase(0, 8);
    }
    return out;
}

bool BlueprintLibrary::parseReceived(const std::string& line, std::string& name, std::string& stamp) {
    static const std::string KEY = "<SHUDEvent_OnNotification> Added notification \"Received Blueprint: ";
    const size_t k = line.find(KEY);
    if (k == std::string::npos) return false;
    const size_t start = k + KEY.size();
    size_t tail = line.find(" to queue", start);
    if (tail == std::string::npos) tail = line.size();
    // The name ends at the last ': " [' before the queue note - the name
    // itself may carry quotes (Arclight "Stormfall" Pistol)
    const size_t end = line.rfind(": \" [", tail);
    if (end == std::string::npos || end < start) return false;
    name = trim(line.substr(start, end - start));
    if (name.empty()) return false;
    // <2026-08-30T21:17:49.987Z> at the front
    stamp.clear();
    if (line.size() > 20 && line[0] == '<' && line[11] == 'T') {
        stamp = line.substr(1, 10) + " " + line.substr(12, 5);
    }
    return true;
}

void BlueprintLibrary::buildIndex() {
    byName_.clear();
    for (int b = 0; b < CRAFT_BLUEPRINT_COUNT; b++) {
        // The first row wins where two blueprints share a name
        byName_.emplace(normalise(CRAFT_BLUEPRINTS[b].name), b);
    }
}

void BlueprintLibrary::load() {
    buildIndex();
    owned_.clear();
    denied_.clear();
    unknown_.clear();
    doubted_.clear();
    historyUntil_.clear();
    legacyUntil_ = 0;
    payoutsRead_ = false;
    ranksRead_ = false;
    tradesRead_ = false;
    checkingLogs_ = false;
    foreignLines_.clear();
    std::ifstream in(AppPaths::file(fileName_.c_str()));
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line == "payouts\t1") {
            payoutsRead_ = true;
            continue;
        }
        if (line == "ranks\t1") {
            ranksRead_ = true;
            continue;
        }
        if (line == "trades\t1") {
            tradesRead_ = true;
            continue;
        }
        if (line.rfind("doubt\t", 0) == 0) {
            doubted_.insert(lowerAscii(line.substr(6)));
            continue;
        }
        if (line.rfind("history_until=", 0) == 0) {
            // 3.6.0 wrote one watermark for its one folder
            try { legacyUntil_ = std::stoll(line.substr(14)); } catch (...) {}
            continue;
        }
        if (line.rfind("history_until\t", 0) == 0) {
            // history_until <TAB> folder <TAB> seconds
            const size_t a = line.find('\t'), b = line.find('\t', a + 1);
            if (b != std::string::npos) {
                try { historyUntil_[line.substr(a + 1, b - a - 1)] = std::stoll(line.substr(b + 1)); } catch (...) {}
            }
            continue;
        }
        // state <TAB> stamp <TAB> name <TAB> source
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, '\t')) f.push_back(part);
        if (f.size() < 3) continue;
        const std::string key = normalise(f[2]);
        if (f[0] == "owned") {
            const auto it = byName_.find(key);
            if (it == byName_.end()) unknown_[f[2]] = f[1];
            else owned_[it->second] = Entry{f[1], f.size() > 3 ? f[3] : "log", f[2]};
        } else if (f[0] == "not-owned") {
            denied_.insert(key);
        }
    }
    // A library read before contracts were looked at has never seen one: the
    // game's old logs are read once more, from the first (what they add of
    // blueprints is owned already and changes nothing)
    if (!payoutsRead_) {
        historyUntil_.clear();
        legacyUntil_ = 0;
        payoutsRead_ = true;   // written with the watermarks as the reading goes on
    }
    // ...and for the contracts the player took and which game each log is
    // of: the flag is written once the last of them has been read (see
    // finishLogCheck), so a reading cut short starts over
    if (!ranksRead_) {
        historyUntil_.clear();
        legacyUntil_ = 0;
        checkingLogs_ = true;
    }
    // ...and for what was traded at the terminals
    if (!tradesRead_) {
        historyUntil_.clear();
        legacyUntil_ = 0;
        tradesRead_ = true;   // written with the watermarks as the reading goes on
    }
    loadArchive();
    loadRanks();
    trades_.load(AppPaths::file(channelFile(TradeBook::FILE_NAME).c_str()));
    dirty_ = false;
}

// The kept lines, read back through the same parser that put them there.
// This is what makes the file a backup rather than an archive: with no game
// logs at all - a new machine, a fresh install, a wiped folder - the library
// comes back from this alone.
void BlueprintLibrary::loadArchive() {
    archived_.clear();
    std::ifstream in(AppPaths::file(archiveName().c_str()));
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] != '<') continue;
        archived_.insert(line);
        importLine(line);
    }
}

std::string BlueprintLibrary::archiveName() const {
    // One per channel, the way the library file is: PTU is a different
    // character and its evidence does not belong with LIVE's
    if (fileName_ == FILE_NAME) return ARCHIVE_NAME;
    const size_t under = fileName_.find('_', 6);      // owned_blueprints_PTU.txt
    const size_t dot = fileName_.rfind('.');
    if (under == std::string::npos || dot == std::string::npos || dot <= under) return ARCHIVE_NAME;
    return "blueprint_log_backup" + fileName_.substr(under, dot - under) + ".txt";
}

// Appended, never rewritten: one line per blueprint ever received, and
// nothing written at all on a run that finds nothing new.
void BlueprintLibrary::flushArchive() {
    if (toArchive_.empty()) return;
    const bool fresh = !std::filesystem::exists(AppPaths::file(archiveName().c_str()));
    std::ofstream out(AppPaths::file(archiveName().c_str()), std::ios::app);
    if (!out.is_open()) {
        toArchive_.clear();
        return;
    }
    if (fresh) {
        out << "# Bedrock - every \"Received Blueprint\" line the game has written, kept.\n"
               "# The game's own logbackups folder runs to gigabytes and the launcher\n"
               "# prunes it; these are the few hundred kilobytes of it that say what you\n"
               "# own. Nothing else is copied. Take this file to another machine, or keep\n"
               "# it in a backup, and the library rebuilds from it with no game logs at\n"
               "# all. Lines are appended, never rewritten - delete the file to start over.\n";
    }
    for (const auto& line : toArchive_) out << line << "\n";
    toArchive_.clear();
}

void BlueprintLibrary::save() {
    std::ofstream out(AppPaths::file(fileName_.c_str()), std::ios::trunc);
    if (!out.is_open()) return;
    out << "# Bedrock blueprint library - what the game's log said you received, plus what you ticked by hand.\n";
    out << "# One line per blueprint: state, when, name, source. Delete the file to rebuild it from the logs.\n";
    out << "# 'assumed: <contract>' - that contract was completed and paid no blueprint, so it had none\n";
    out << "# left to give; 'doubt' - a contract that paid one later all the same, and assumes nothing now.\n";
    if (!channel_.empty()) out << "# channel: " << channel_ << "\n";
    if (payoutsRead_) out << "payouts\t1\n";
    if (ranksRead_) out << "ranks\t1\n";
    if (tradesRead_) out << "trades\t1\n";
    for (const auto& [folder, until] : historyUntil_) out << "history_until\t" << folder << "\t" << until << "\n";
    for (const auto& title : doubted_) out << "doubt\t" << title << "\n";
    for (const auto& [index, e] : owned_) {
        (void)index;
        out << "owned\t" << e.stamp << "\t" << e.name << "\t" << e.source << "\n";
    }
    for (const auto& [name, stamp] : unknown_) {
        out << "owned\t" << stamp << "\t" << name << "\tlog (no row in this build)\n";
    }
    for (const auto& key : denied_) {
        out << "not-owned\t" << now() << "\t" << key << "\thand\n";
    }
    dirty_ = false;
}

std::int64_t BlueprintLibrary::writeTime(const std::filesystem::path& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

void BlueprintLibrary::setGameLog(const std::filesystem::path& gameLog) {
    gameLog_ = gameLog;
    offset_ = 0;
    liveEpoch_ = 0;
    liveScan_.reset();
    // read from its start again, so where the player was is said anew by it:
    // after a restored backup, the place the last session ended at would
    // otherwise stand over the deals read again
    trades_.liveLogRestarted();
    backlog_.clear();
    folders_.clear();
    extraLogs_.clear();
    if (gameLog_.empty()) return;

    std::error_code ec;
    const std::filesystem::path folder = gameLog_.parent_path();
    std::string name = folder.filename().string();
    for (auto& c : name) c = (char)std::toupper((unsigned char)c);
    folders_.push_back(folder);
    std::string file = FILE_NAME;
    if (name == "LIVE" || name == "HOTFIX") {
        // One server, one character: the other folder's logs belong to the
        // same library. A junction (the usual case) is the same folder and
        // is not read twice.
        channel_ = "LIVE";
        const std::filesystem::path sibling = folder.parent_path() / (name == "LIVE" ? "HOTFIX" : "LIVE");
        if (std::filesystem::is_directory(sibling, ec) && !std::filesystem::equivalent(sibling, folder, ec)) {
            folders_.push_back(sibling);
            const std::filesystem::path log = sibling / "Game.log";
            if (std::filesystem::exists(log, ec)) extraLogs_.push_back(log);
            channel_ = "LIVE + HOTFIX";
        }
    } else {
        // PTU, EPTU, TECH-PREVIEW: a copy of the character, kept apart
        channel_ = name;
        file = "owned_blueprints_" + name + ".txt";
    }
    if (file != fileName_) {
        fileName_ = file;
        load();
    }
    // A 3.6.0 file knew one folder only: that watermark is this folder's
    if (legacyUntil_ > 0 && !historyUntil_.count(folder.string())) historyUntil_[folder.string()] = legacyUntil_;
    legacyUntil_ = 0;

    queueHistory();
    // The other channel's current log is static while this one runs; it is
    // small and reads once per start (what it holds is already owned after
    // the first time, so nothing is counted twice)
    for (const auto& log : extraLogs_) backlog_.push_back(Pending{log, ""});
}

void BlueprintLibrary::queueHistory() {
    // Every backup newer than the last one read, oldest first, so the
    // watermark can move up as they are consumed
    std::error_code ec;
    struct Found {
        std::int64_t time;
        std::filesystem::path file;
        std::string folderKey;
        bool operator<(const Found& o) const { return time < o.time; }
    };
    std::vector<Found> found;
    for (const auto& folder : folders_) {
        const std::string key = folder.string();
        const std::int64_t until = historyUntil_.count(key) ? historyUntil_[key] : 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder / "logbackups", ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".log") continue;
            const std::int64_t t = writeTime(entry.path());
            if (t > until) found.push_back(Found{t, entry.path(), key});
        }
    }
    std::sort(found.begin(), found.end());
    for (auto& f : found) {
        const bool queued = std::any_of(backlog_.begin(), backlog_.end(),
                                        [&](const Pending& p) { return p.file == f.file; });
        if (!queued) backlog_.push_back(Pending{f.file, f.folderKey});
    }
}

bool BlueprintLibrary::importLine(const std::string& line) {
    std::string name, stamp;
    if (!parseReceived(line, name, stamp)) return false;
    const std::string key = normalise(name);
    if (denied_.count(key)) return false;   // ticked off by hand: the log does not overrule that
    const auto it = byName_.find(key);
    if (it == byName_.end()) {
        if (unknown_.count(name)) return false;
        unknown_[name] = stamp;
        dirty_ = true;
        return true;
    }
    const auto own = owned_.find(it->second);
    if (own != owned_.end()) {
        // Assumed owned, and here it comes after all: the contract that paid
        // nothing had it to give. The game says so, and it is owned now.
        if (own->second.source.rfind("assumed: ", 0) == 0) {
            if (stamp > own->second.stamp) doubt(own->second.source.substr(9));
            owned_[it->second] = Entry{stamp, "log", name};
            dirty_ = true;
            return true;
        }
        return false;
    }
    owned_[it->second] = Entry{stamp, "log", name};
    dirty_ = true;
    return true;
}

int BlueprintLibrary::assume(const ContractScan::Unpaid& unpaid) {
    if (!unpaid.payout || doubted_.count(lowerAscii(unpaid.title))) return 0;
    std::vector<int> rows;
    for (const char* p = unpaid.payout->rows; *p;) {
        char* end = nullptr;
        const long b = std::strtol(p, &end, 10);
        if (end == p) break;
        if (b >= 0 && b < CRAFT_BLUEPRINT_COUNT) rows.push_back((int)b);
        p = (*end == ',') ? end + 1 : end;
    }
    // The log against itself: one of them received AFTER the contract paid
    // nothing means it had one to give then - not for this contract, then
    for (int b : rows) {
        const auto own = owned_.find(b);
        if (own != owned_.end() && own->second.source == "log" && own->second.stamp > unpaid.stamp) {
            doubt(unpaid.title);
            return 0;
        }
    }
    int added = 0;
    for (int b : rows) {
        if (owned_.count(b)) continue;
        if (denied_.count(normalise(CRAFT_BLUEPRINTS[b].name))) continue;   // ticked off by hand
        owned_[b] = Entry{unpaid.stamp, "assumed: " + unpaid.title, CRAFT_BLUEPRINTS[b].name};
        added++;
    }
    if (added > 0) dirty_ = true;
    return added;
}

// The rule failed for this contract: it assumes nothing from now on, and what
// it did assume goes back to missing - the blue of a blueprint still to get
// is worth more than a guess
void BlueprintLibrary::doubt(const std::string& title) {
    doubted_.insert(lowerAscii(title));
    const std::string source = lowerAscii("assumed: " + title);
    for (auto it = owned_.begin(); it != owned_.end();) {
        if (lowerAscii(it->second.source) == source) it = owned_.erase(it);
        else ++it;
    }
    dirty_ = true;
}

int BlueprintLibrary::readFile(const std::filesystem::path& path, std::uintmax_t from, std::uintmax_t* endOffset,
                               ContractScan& scan, bool whole) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return 0;
    in.seekg((std::streamoff)from);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // The game may be mid-line: everything up to the last newline is whole,
    // the rest waits for the next round
    size_t usable = data.rfind('\n');
    if (usable == std::string::npos) {
        if (endOffset) *endOffset = from;
        return 0;
    }
    usable += 1;
    int added = 0;
    size_t pos = 0;
    size_t stamped = std::string::npos;      // the last line that carries its time
    while (pos < usable) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos || nl >= usable) nl = usable - 1;
        const std::string_view text(data.data() + pos, nl - pos);
        added += feedLine(text, scan);
        // (the log being written: where the player is, and what they trade there)
        if (!whole && !foreign(scan)) trades_.live(text);
        if (nl - pos > 21 && data[pos] == '<' && data[pos + 11] == 'T') stamped = pos;
        pos = nl + 1;
    }
    // A log that is over is read a second time for its trades, where it has
    // any: where each was made is said by the lines before it, and reading
    // every log for that would double the work of all of them (two of a
    // hundred have a trade in them).
    if (whole && !foreign(scan) && TradeBook::hasRequest(std::string_view(data.data(), usable))) {
        trades_.replay(std::string_view(data.data(), usable));
    }
    // (of the log being written, for liveLogEpoch())
    if (!whole && stamped != std::string::npos) {
        const long long at = Reputation::epochOf(data.substr(stamped + 1, 19));
        if (at > 0) liveEpoch_ = at;
    }
    if (whole) added += finishFile(scan);
    if (endOffset) *endOffset = from + usable;
    flushArchive();
    flushRankArchive();
    trades_.save();
    return added;
}

int BlueprintLibrary::feedLine(std::string_view line, ContractScan& scan) {
    // Only lines that can be the event are worth parsing - and the look
    // stays inside the line, or a quiet file would be rescanned per line
    if (line.empty() || line[0] != '<') return 0;
    if (byName_.empty()) buildIndex();
    int added = 0;
    if (line.find("Received Blueprint") != std::string_view::npos) {
        std::string text(line);
        while (!text.empty() && text.back() == '\r') text.pop_back();
        if (foreign(scan)) {
            // the other game's: not owned here. Noted while the old logs are
            // checked, so what an earlier reading took from it can go again
            if (checkingLogs_) foreignLines_.insert(text);
        } else {
            // Kept whether or not it is news to the library: the point is
            // to hold the evidence, and a line already owned is still the
            // proof of it once the game's own logs are gone
            if (archived_.insert(text).second) toArchive_.push_back(text);
            if (importLine(text)) added++;
        }
    }
    // ...and the contracts completed, and whether they paid - after the
    // blueprint above is in, so that it counts as owned
    std::vector<ContractScan::Unpaid> unpaid;
    scan.line(line, unpaid);
    if (!foreign(scan)) {
        for (const auto& u : unpaid) added += assume(u);
    }
    takeRankEvents(scan);
    return added;
}

int BlueprintLibrary::finishFile(ContractScan& scan) {
    std::vector<ContractScan::Unpaid> unpaid;
    const bool theirs = foreign(scan);
    takeRankEvents(scan);
    scan.finish(unpaid);
    int added = 0;
    if (!theirs) {
        for (const auto& u : unpaid) added += assume(u);
    }
    return added;
}

// A log of the other game: a PTU log in LIVE's folder - the launcher leaves
// them there (80 of 1,288 in one install, from 4.6 to 4.8) - or a LIVE one
// in the PTU's. A log whose header was never read counts as this channel's.
bool BlueprintLibrary::foreign(const ContractScan& scan) const {
    if (scan.env().empty()) return false;
    const bool live = channel_.empty() || channel_.rfind("LIVE", 0) == 0;
    return live ? scan.env() != "pub" : scan.env() == "pub";
}

void BlueprintLibrary::takeRankEvents(ContractScan& scan) {
    std::vector<RankEvent>& fresh = scan.rankEvents();
    if (fresh.empty()) return;
    if (!foreign(scan)) {
        for (const RankEvent& e : fresh) {
            if (e.version <= 0 || !keepRankEvent(e)) continue;
            toRankArchive_.push_back(std::string(e.completed ? "completed" : "accepted") + "\t" + e.stamp + "\t" +
                                     e.mission + "\t" + e.contract + "\t" + std::to_string(e.version));
        }
    }
    fresh.clear();
}

bool BlueprintLibrary::keepRankEvent(const RankEvent& e) {
    const std::string key = std::string(e.completed ? "c" : "a") + e.mission;
    if (!rankEvents_.emplace(key, e).second) return false;
    ranksChanged_ = true;
    return true;
}

std::string BlueprintLibrary::channelFile(const char* base) const {
    std::string suffix;
    if (fileName_ != FILE_NAME) {
        const size_t under = fileName_.find('_', 6);      // owned_blueprints_PTU.txt
        const size_t dot = fileName_.rfind('.');
        if (under != std::string::npos && dot != std::string::npos && dot > under) {
            suffix = fileName_.substr(under, dot - under);
        }
    }
    const std::string name = base;
    const size_t dot = name.rfind('.');
    return name.substr(0, dot) + suffix + name.substr(dot);
}

// The contracts kept, and the ranks set, of this channel
void BlueprintLibrary::loadRanks() {
    rankEvents_.clear();
    toRankArchive_.clear();
    rankSettings_.clear();
    auto fields = [](const std::string& line) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, '\t')) f.push_back(part);
        return f;
    };
    std::string line;
    std::ifstream kept(AppPaths::file(channelFile(RANK_ARCHIVE_NAME).c_str()));
    while (std::getline(kept, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = fields(line);
        if (f.size() < 5 || (f[0] != "accepted" && f[0] != "completed")) continue;
        RankEvent e{f[1], f[0] == "completed", f[2], f[3], std::atoi(f[4].c_str())};
        keepRankEvent(e);
    }
    std::ifstream set(AppPaths::file(channelFile(RANK_FILE_NAME).c_str()));
    while (std::getline(set, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        // set <TAB> outfit's GUID <TAB> ladder's GUID <TAB> reputation <TAB> when <TAB> what, for the eye
        const std::vector<std::string> f = fields(line);
        if (f.size() < 5 || f[0] != "set") continue;
        rankSettings_.push_back(RankSetting{f[1], f[2], std::atoll(f[3].c_str()), f[4]});
    }
    ranksChanged_ = true;
}

// Appended, never rewritten, like the blueprints' evidence
void BlueprintLibrary::flushRankArchive() {
    if (toRankArchive_.empty()) return;
    const std::string name = channelFile(RANK_ARCHIVE_NAME);
    const bool fresh = !std::filesystem::exists(AppPaths::file(name.c_str()));
    std::ofstream out(AppPaths::file(name.c_str()), std::ios::app);
    if (!out.is_open()) {
        toRankArchive_.clear();
        return;
    }
    if (fresh) {
        out << "# Bedrock - every contract you took or finished yourself, as the game's log\n"
               "# names it: when, its MissionId, the contract's generator and name, the\n"
               "# game's version. What the Grind Planner reads your rank with each outfit\n"
               "# from; kept here because the launcher prunes its logbackups folder.\n"
               "# Lines are appended, never rewritten - delete the file to start over.\n";
    }
    for (const auto& line : toRankArchive_) out << line << "\n";
    toRankArchive_.clear();
}

void BlueprintLibrary::saveRankSettings() {
    std::ofstream out(AppPaths::file(channelFile(RANK_FILE_NAME).c_str()), std::ios::trunc);
    if (!out.is_open()) return;
    out << "# Bedrock - the ranks you set in the Grind Planner. Your rank with an outfit\n"
           "# is counted on from here: what you finish after it adds to it.\n"
           "# set, the outfit, the ladder, where the rank begins, when (UTC), which.\n";
    for (const RankSetting& s : rankSettings_) {
        const int f = Reputation::factionByGuid(s.faction), l = Reputation::ladderByGuid(s.ladder);
        std::string which;
        if (f >= 0 && l >= 0) {
            which = std::string(REP_FACTIONS[f].name) + " - " +
                    REP_RANKS[REP_LADDERS[l].first + Reputation::rankOf(l, s.reputation)].name;
        }
        out << "set\t" << s.faction << "\t" << s.ladder << "\t" << s.reputation << "\t" << s.stamp << "\t"
            << which << "\n";
    }
}

std::vector<Standing> BlueprintLibrary::standings() const {
    std::vector<RankEvent> events;
    events.reserve(rankEvents_.size());
    for (const auto& [key, e] : rankEvents_) {
        (void)key;
        events.push_back(e);
    }
    return Reputation::standings(events, rankSettings_, countsFrom());
}

int BlueprintLibrary::countsFrom() const {
    const bool live = channel_.empty() || channel_.rfind("LIVE", 0) == 0;
    if (live) return Reputation::LIVE_WIPE;
    int newest = 0;
    for (const auto& [key, e] : rankEvents_) {
        (void)key;
        newest = (std::max)(newest, e.version);
    }
    return newest;
}

void BlueprintLibrary::setRank(int faction, int ladder, int rank) {
    if (faction < 0 || faction >= REP_FACTION_COUNT || ladder < 0 || ladder >= REP_LADDER_COUNT) return;
    const std::string f = REP_FACTIONS[faction].guid, l = REP_LADDERS[ladder].guid;
    // The rank the logs already put them at, picked to say so: counted on
    // from where in it the logs have them, not from its bottom
    long long from = Reputation::rankMin(ladder, rank);
    for (const Standing& s : standings()) {
        if (s.faction == faction && s.ladder == ladder && s.rank == rank) from = s.reputation;
    }
    rankSettings_.erase(std::remove_if(rankSettings_.begin(), rankSettings_.end(),
                                       [&](const RankSetting& s) { return s.faction == f && s.ladder == l; }),
                        rankSettings_.end());
    if (rank >= 0) rankSettings_.push_back(RankSetting{f, l, from, logStampNow()});
    saveRankSettings();
    ranksChanged_ = true;
}

// The end of the reading started in load(): what the other game's logs had
// put in goes again. Their lines leave the evidence file, and a blueprint
// nothing else speaks for leaves the library; one a LIVE log received as
// well takes the date of that line. Ticked by hand, or assumed, is left
// alone.
void BlueprintLibrary::finishLogCheck() {
    checkingLogs_ = false;
    ranksRead_ = true;
    if (!foreignLines_.empty()) {
        // the evidence file, without them
        std::vector<std::string> keep;
        {
            std::ifstream in(AppPaths::file(archiveName().c_str()));
            std::string line;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                if (foreignLines_.count(line)) continue;
                keep.push_back(line);
            }
        }
        bool dropped = false;
        for (const auto& line : foreignLines_) dropped = archived_.erase(line) > 0 || dropped;
        if (dropped) {
            std::ofstream out(AppPaths::file(archiveName().c_str()), std::ios::trunc);
            for (const auto& line : keep) out << line << "\n";
        }
        // what the lines left speak for, and from when
        std::map<std::string, std::string> firstSeen;     // normalised name -> stamp
        for (const auto& line : archived_) {
            std::string name, stamp;
            if (!parseReceived(line, name, stamp)) continue;
            const std::string key = normalise(name);
            const auto at = firstSeen.find(key);
            if (at == firstSeen.end() || stamp < at->second) firstSeen[key] = stamp;
        }
        for (const auto& line : foreignLines_) {
            std::string name, stamp;
            if (!parseReceived(line, name, stamp)) continue;
            const std::string key = normalise(name);
            const auto seen = firstSeen.find(key);
            const auto row = byName_.find(key);
            if (row == byName_.end()) {
                if (seen == firstSeen.end() && unknown_.erase(name)) foreignDropped_++;
                continue;
            }
            const auto own = owned_.find(row->second);
            if (own == owned_.end() || own->second.source != "log") continue;
            if (seen == firstSeen.end()) {
                owned_.erase(own);
                foreignDropped_++;
            } else if (own->second.stamp == stamp) {
                own->second.stamp = seen->second;
            }
        }
        foreignLines_.clear();
    }
    dirty_ = true;
    ranksChanged_ = true;
    save();
}

bool BlueprintLibrary::poll() {
    bool changed = false;
    const bool fromHistory = !backlog_.empty();
    if (fromHistory) {
        // One backup per call: the largest is a few MB, and the window must
        // not freeze while a year of logs is read
        const Pending p = backlog_.front();
        backlog_.erase(backlog_.begin());
        historyScan_.reset();
        changed = readFile(p.file, 0, nullptr, historyScan_, true) > 0;
        if (!p.folderKey.empty()) {
            historyUntil_[p.folderKey] = (std::max)(historyUntil_[p.folderKey], writeTime(p.file));
        }
        historyRead_++;
        dirty_ = true;
        if (backlog_.empty() && checkingLogs_) {
            finishLogCheck();
            changed = true;     // what it took away, or the dates it moved
        }
        if (backlog_.empty() || historyRead_ % 50 == 0) save();
        // ...and the live log all the same, not only after the whole backlog: the
        // trades' first read queues every backup once - a thousand and more for a
        // player of a year, one a tick - and the orders accepted and the deals made
        // meanwhile would show nothing.
    }
    if (!gameLog_.empty()) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(gameLog_, ec);
        if (!ec) {
            if (size < offset_) {
                // The game started again: the old log went to logbackups
                offset_ = 0;
                liveScan_.reset();
                trades_.liveLogRestarted();
                queueHistory();
            }
            if (size > offset_) changed = readFile(gameLog_, offset_, &offset_, liveScan_, false) > 0 || changed;
        }
        // a backlog is looked for anew once it is done: while it runs, the
        // look would weigh every backup against every one queued
        if (backlog_.empty() && ++ticks_ % 15 == 0) queueHistory();
    }
    // a backup read saves every fiftieth time, not on every tick
    if (dirty_ && !fromHistory) save();
    return changed;
}

std::vector<std::string> BlueprintLibrary::stamps() const {
    std::vector<std::string> out(CRAFT_BLUEPRINT_COUNT);
    for (const auto& [index, e] : owned_) {
        if (index >= 0 && index < CRAFT_BLUEPRINT_COUNT) {
            const bool assumed = e.source.rfind("assumed: ", 0) == 0;
            out[index] = e.stamp + (e.source == "hand" ? " (by hand)" : assumed ? " (assumed)" : "");
        }
    }
    return out;
}

void BlueprintLibrary::setByHand(int blueprint, bool owned) {
    if (blueprint < 0 || blueprint >= CRAFT_BLUEPRINT_COUNT) return;
    const std::string key = normalise(CRAFT_BLUEPRINTS[blueprint].name);
    if (owned) {
        denied_.erase(key);
        owned_[blueprint] = Entry{now(), "hand", CRAFT_BLUEPRINTS[blueprint].name};
    } else {
        owned_.erase(blueprint);
        denied_.insert(key);
    }
    save();
}

int BlueprintLibrary::byHandCount() const {
    int n = 0;
    for (const auto& [index, e] : owned_) {
        (void)index;
        if (e.source == "hand") n++;
    }
    return n;
}

int BlueprintLibrary::assumedCount() const {
    int n = 0;
    for (const auto& [index, e] : owned_) {
        (void)index;
        if (e.source.rfind("assumed: ", 0) == 0) n++;
    }
    return n;
}

std::string BlueprintLibrary::sourceOf(int blueprint) const {
    const auto it = owned_.find(blueprint);
    return it == owned_.end() ? std::string() : it->second.source;
}

}  // namespace SCMiner
