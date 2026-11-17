#include "DebugZip.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace SCMiner {
namespace DebugZip {

namespace fs = std::filesystem;

namespace {

// ---- masking --------------------------------------------------------------

bool separator(char c) {
    return c == '\\' || c == '/';
}

// A byte that goes on with a path part's name: a letter, a figure, . _ -,
// or a byte of a character beyond ASCII
bool nameByte(char c) {
    const unsigned char u = (unsigned char)c;
    const unsigned char lower = (unsigned char)(u | 0x20);
    return (u >= '0' && u <= '9') || (lower >= 'a' && lower <= 'z') || u == '.' || u == '_' || u == '-' || u >= 0x80;
}

char folded(char c, Rules rules) {
    return rules == Rules::Windows && c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

// Whether `text` holds `word` at `at`, in the rules' case
bool holds(const std::string& text, std::size_t at, const std::string& word, Rules rules) {
    if (at > text.size() || word.size() > text.size() - at) return false;
    for (std::size_t i = 0; i < word.size(); i++) {
        if (folded(text[at + i], rules) != folded(word[i], rules)) return false;
    }
    return true;
}

// A home folder's parts, and whether it begins with a separator
// (/home/mining; C:\Users\Mining does not)
struct Home {
    std::vector<std::string> parts;
    bool rooted = false;
};
Home homeOf(const std::string& path) {
    Home home;
    home.rooted = !path.empty() && separator(path[0]);
    std::string part;
    for (char c : path) {
        if (!separator(c)) {
            part += c;
        } else if (!part.empty()) {
            home.parts.push_back(part);
            part.clear();
        }
    }
    if (!part.empty()) home.parts.push_back(part);
    return home;
}

// Where the home folder ends that begins at `at` - any run of \ and / between
// its parts; npos where it is not there
std::size_t homeEnd(const std::string& text, std::size_t at, const Home& home, Rules rules) {
    std::size_t i = at;
    for (std::size_t p = 0; p < home.parts.size(); p++) {
        if (p > 0 || home.rooted) {
            if (i >= text.size() || !separator(text[i])) return std::string::npos;
            while (i < text.size() && separator(text[i])) i++;
        }
        if (!holds(text, i, home.parts[p], rules)) return std::string::npos;
        i += home.parts[p].size();
    }
    if (i < text.size() && nameByte(text[i])) return std::string::npos;
    return i;
}

#ifdef _WIN32
// A code-page text in UTF-8
std::string utf8Of(const std::string& text) {
    if (text.empty()) return text;
    const int wideLength = MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), nullptr, 0);
    if (wideLength <= 0) return text;
    std::wstring wide((std::size_t)wideLength, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), wide.data(), wideLength);
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return text;
    std::string out((std::size_t)length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, out.data(), length, nullptr, nullptr);
    return out;
}
#endif

// The masks as pick() uses them: as given and, on Windows, where the code
// page spells a name otherwise than UTF-8 (letters beyond ASCII), in UTF-8 as
// well - what Bedrock writes is in the code page, what a player typed into
// region_config.txt may be in UTF-8
std::vector<Masks> spellings(const Masks& masks) {
    std::vector<Masks> out{masks};
#ifdef _WIN32
    const Masks utf8{utf8Of(masks.home), utf8Of(masks.user)};
    if (utf8.home != masks.home || utf8.user != masks.user) out.push_back(utf8);
#endif
    return out;
}

std::string maskedAll(const std::string& text, const std::vector<Masks>& masks) {
    std::string out = text;
    for (const Masks& mask : masks) out = masked(out, mask);
    return out;
}

// ---- reading the folder ---------------------------------------------------

constexpr std::size_t HEADER_MOST = 64 * 1024;   // a CSV's header line is looked for this far

// A file's time on disk, in Unix seconds
std::int64_t writtenAt(const fs::path& file) {
    std::error_code ec;
    const auto written = fs::last_write_time(file, ec);
    if (ec) return 0;
#ifdef _WIN32
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
#else
    // clock_cast is GCC 13's; the Linux package is built with GCC 11
    const auto system = std::chrono::file_clock::to_sys(written);
#endif
    return (std::int64_t)std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

// A file's name as UTF-8, whatever the system's code page
std::string nameOf(const fs::path& file) {
    const std::u8string name = file.filename().u8string();
    return std::string(reinterpret_cast<const char*>(name.data()), name.size());
}

// `count` bytes of an open file from `from` on; false where they cannot be read
bool readAt(std::ifstream& in, std::uint64_t from, std::uint64_t count, std::string& out) {
    out.assign((std::size_t)count, '\0');
    if (count == 0) return true;
    in.clear();
    in.seekg((std::streamoff)from);
    in.read(out.data(), (std::streamsize)count);
    return in.gcount() == (std::streamsize)count;
}

bool fileSize(std::ifstream& in, std::uint64_t& size) {
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0) return false;
    size = (std::uint64_t)end;
    return true;
}

// A text as it goes: with `header` its first line; then the rest from a
// line's start, `most` bytes of its end at most - all of it where it is no
// longer. Masked; where a mask is longer than what it hides, the rest begins
// a line later and stays within `most`. False where the file cannot be read.
bool textOf(const fs::path& file, std::size_t most, bool header, const std::vector<Masks>& masks, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    std::uint64_t size = 0;
    if (!in.is_open() || !fileSize(in, size)) return false;
    std::string first;
    if (header && size > 0) {
        std::string start;
        if (!readAt(in, 0, (std::min)(size, (std::uint64_t)HEADER_MOST), start)) return false;
        const std::size_t lineEnd = start.find('\n');
        if (lineEnd != std::string::npos) first = start.substr(0, lineEnd + 1);
        else if (start.size() == size) first = start;   // a file of one line
    }
    const std::uint64_t headerEnd = first.size();
    std::uint64_t from = size > most ? size - most : 0;
    if (from < headerEnd) from = headerEnd;
    std::string rest;
    if (from > headerEnd) {
        // from the byte before the cut: a line break there makes the cut a
        // line's start, else the rest begins after the next one
        if (!readAt(in, from - 1, size - from + 1, rest)) return false;
        const std::size_t lineEnd = rest.find('\n');
        rest = lineEnd == std::string::npos ? std::string() : rest.substr(lineEnd + 1);
    } else if (!readAt(in, from, size - from, rest)) {
        return false;
    }
    rest = maskedAll(rest, masks);
    while (rest.size() > most) {
        const std::size_t lineEnd = rest.find('\n');
        if (lineEnd == std::string::npos) rest.clear();
        else rest.erase(0, lineEnd + 1);
    }
    out = maskedAll(first, masks) + rest;
    return true;
}

bool wholeFile(const fs::path& file, std::size_t most, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    std::uint64_t size = 0;
    if (!in.is_open() || !fileSize(in, size) || size > most) return false;
    return readAt(in, 0, size, out);
}

// When a snapshot was taken: by the time in its name
// (snapshot_YYYYMMDD_HHMMSS.png, the local time Bedrock wrote), else by the
// file's time
std::int64_t takenAt(const fs::path& file, const std::string& name) {
    auto figures = [&](std::size_t from, std::size_t count) {
        int value = 0;
        for (std::size_t i = from; i < from + count; i++) {
            if (name[i] < '0' || name[i] > '9') return -1;
            value = value * 10 + (name[i] - '0');
        }
        return value;
    };
    if (name.size() == 28 && name.compare(0, 9, "snapshot_") == 0 && name[17] == '_' &&
        name.compare(24, 4, ".png") == 0) {
        const int day = figures(9, 8), time = figures(18, 6);
        std::tm local{};
        local.tm_year = day / 10000 - 1900;
        local.tm_mon = day / 100 % 100 - 1;
        local.tm_mday = day % 100;
        local.tm_hour = time / 10000;
        local.tm_min = time / 100 % 100;
        local.tm_sec = time % 100;
        local.tm_isdst = -1;
        if (day >= 0 && time >= 0 && local.tm_mon >= 0 && local.tm_mon < 12 && local.tm_mday >= 1 &&
            local.tm_mday <= 31 && local.tm_hour < 24 && local.tm_min < 60 && local.tm_sec <= 60) {
            const std::time_t taken = std::mktime(&local);
            if (taken != (std::time_t)-1) return (std::int64_t)taken;
        }
    }
    return writtenAt(file);
}

// A cut of the rock panel (PanelKeep.h): panel_<n>.png or panel5_<n>.png,
// <n> a number without a leading 0 - not panel_miss_<n>.png, which is the
// whole screen
bool panelCut(const std::string& name) {
    for (const std::string ring : {"panel_", "panel5_"}) {
        if (name.size() <= ring.size() + 4 || name.compare(0, ring.size(), ring) != 0 ||
            name.compare(name.size() - 4, 4, ".png") != 0) {
            continue;
        }
        const std::string number = name.substr(ring.size(), name.size() - ring.size() - 4);
        if (number.size() <= 4 && number[0] != '0' &&
            std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return true;
        }
    }
    return false;
}

// ---- the zip --------------------------------------------------------------

constexpr std::uint32_t SIG_LOCAL = 0x04034b50;
constexpr std::uint32_t SIG_CENTRAL = 0x02014b50;
constexpr std::uint32_t SIG_END = 0x06054b50;
constexpr std::size_t LOCAL_SIZE = 30;     // a local header without its name
constexpr std::size_t CENTRAL_SIZE = 46;   // a central header without its name
constexpr std::size_t END_SIZE = 22;       // the end record without a comment
constexpr std::uint32_t MADE_BY = 63;      // 6.3, the APPNOTE that has the UTF-8 flag; MS-DOS attributes
constexpr std::uint32_t NEEDED = 10;       // 1.0: stored
constexpr std::uint32_t FLAG_UTF8 = 0x0800;
constexpr std::uint32_t STORED = 0;
constexpr const char* FOLDER = "debug/";   // every name's folder in the zip
constexpr std::size_t FOLDER_LENGTH = 6;

constexpr std::array<std::uint32_t, 256> crcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; n++) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}
constexpr std::array<std::uint32_t, 256> CRC_TABLE = crcTable();

void put16(std::string& out, std::uint32_t value) {
    out += (char)(value & 0xFF);
    out += (char)((value >> 8) & 0xFF);
}

void put32(std::string& out, std::uint32_t value) {
    put16(out, value & 0xFFFF);
    put16(out, value >> 16);
}

std::uint32_t get16(const std::string& s, std::size_t at) {
    return (std::uint32_t)(unsigned char)s[at] | ((std::uint32_t)(unsigned char)s[at + 1] << 8);
}

std::uint32_t get32(const std::string& s, std::size_t at) {
    return get16(s, at) | (get16(s, at + 2) << 16);
}

// MS-DOS's date and time: local time to two seconds, 1980 to 2107
struct DosStamp {
    std::uint32_t time = 0;
    std::uint32_t date = (1 << 5) | 1;   // 1 January 1980, for anything earlier
};

DosStamp dosStamp(std::int64_t written) {
    DosStamp out;
    if (written <= 0) return out;
    const std::time_t when = (std::time_t)written;
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &when) != 0) return out;
#else
    if (!localtime_r(&when, &local)) return out;
#endif
    if (local.tm_year < 80) return out;
    if (local.tm_year > 207) {
        out.date = (127u << 9) | (12u << 5) | 31u;
        out.time = (23u << 11) | (59u << 5) | 29u;
        return out;
    }
    out.date = ((std::uint32_t)(local.tm_year - 80) << 9) | ((std::uint32_t)(local.tm_mon + 1) << 5) |
               (std::uint32_t)local.tm_mday;
    out.time = ((std::uint32_t)local.tm_hour << 11) | ((std::uint32_t)local.tm_min << 5) |
               (std::uint32_t)((std::min)(local.tm_sec, 59) / 2);
    return out;
}

std::int64_t unixOf(std::uint32_t date, std::uint32_t time) {
    std::tm local{};
    local.tm_year = 80 + (int)(date >> 9);
    local.tm_mon = (int)((date >> 5) & 15) - 1;
    local.tm_mday = (int)(date & 31);
    local.tm_hour = (int)(time >> 11);
    local.tm_min = (int)((time >> 5) & 63);
    local.tm_sec = (int)(time & 31) * 2;
    local.tm_isdst = -1;
    const std::time_t when = std::mktime(&local);
    return when == (std::time_t)-1 ? 0 : (std::int64_t)when;
}

}  // namespace

Masks masksHere() {
    auto variable = [](const char* name) {
        const char* value = std::getenv(name);
        return std::string(value ? value : "");
    };
#ifdef _WIN32
    return Masks{variable("USERPROFILE"), variable("USERNAME")};
#else
    return Masks{variable("HOME"), variable("USER")};
#endif
}

std::string masked(const std::string& text, const Masks& masks, Rules rules) {
    std::string out = text;
    // 1. the home folder - not one of a single part: "C:\" alone would hide
    //    every path on that drive
    const Home home = homeOf(masks.home);
    if (home.parts.size() >= 2 || (home.rooted && !home.parts.empty())) {
        const std::string mark = rules == Rules::Windows ? "%USERPROFILE%" : "~";
        std::string next;
        next.reserve(out.size());
        for (std::size_t i = 0; i < out.size();) {
            const std::size_t end =
                (i == 0 || !nameByte(out[i - 1])) ? homeEnd(out, i, home, rules) : std::string::npos;
            if (end != std::string::npos) {
                next += mark;
                i = end;
            } else {
                next += out[i++];
            }
        }
        out.swap(next);
    }
    // 2. the name as a path part
    const std::string& user = masks.user;
    if (!user.empty() && user.find_first_of("\\/") == std::string::npos) {
        std::string next;
        next.reserve(out.size());
        for (std::size_t i = 0; i < out.size();) {
            next += out[i];
            const std::size_t after = i + 1 + user.size();
            if (separator(out[i]) && holds(out, i + 1, user, rules) && (after >= out.size() || !nameByte(out[after]))) {
                next += "<user>";
                i = after;
            } else {
                i++;
            }
        }
        out.swap(next);
    }
    return out;
}

std::vector<Entry> pick(const fs::path& debugFolder, const Masks& masks, std::size_t most) {
    const std::vector<Masks> spelled = spellings(masks);
    std::vector<Entry> out;
    std::error_code ec;
    auto text = [&](const fs::path& file, std::size_t limit, bool header) {
        Entry entry;
        if (!fs::is_regular_file(file, ec) || !textOf(file, limit, header, spelled, entry.data)) return false;
        entry.name = nameOf(file);
        entry.written = writtenAt(file);
        out.push_back(std::move(entry));
        return true;
    };
    text(debugFolder / "session.txt", WHOLE_MOST, false);
    text(debugFolder / "region_config.txt", WHOLE_MOST, false);
    text(debugFolder / "rock_scans.csv", TABLE_TAIL, true);
    text(debugFolder / "rock_readings.csv", TABLE_TAIL, true);
    text(debugFolder / "scan_log.csv", LOG_TAIL, true);

    struct Shot {
        fs::path png;
        std::string name;
        std::int64_t taken = 0;
    };
    std::vector<Shot> shots;
    for (fs::directory_iterator it(debugFolder, ec), done; !ec && it != done; it.increment(ec)) {
        std::error_code kind;
        if (!it->is_regular_file(kind)) continue;
        const std::string name = nameOf(it->path());
        if (name.size() <= 13 || name.compare(0, 9, "snapshot_") != 0 || name.compare(name.size() - 4, 4, ".png") != 0) {
            continue;
        }
        shots.push_back({it->path(), name, takenAt(it->path(), name)});
    }
    std::sort(shots.begin(), shots.end(),
              [](const Shot& a, const Shot& b) { return a.taken != b.taken ? a.taken > b.taken : a.name > b.name; });
    if (shots.size() > SNAPSHOTS) shots.resize(SNAPSHOTS);
    // a picture and its .txt, or neither where they would take the zip over
    // `most`
    auto pair = [&](const fs::path& png, const std::string& name) {
        const std::size_t before = out.size();
        Entry picture;
        if (!wholeFile(png, most, picture.data)) return;   // no picture over the limit is read at all
        picture.name = name;
        picture.written = writtenAt(png);
        out.push_back(std::move(picture));
        fs::path note = png;
        note.replace_extension(".txt");
        text(note, WHOLE_MOST, false);
        const std::size_t size = zipSize(out);
        if (size == 0 || size > most) out.resize(before);
    };
    for (const Shot& shot : shots) pair(shot.png, shot.name);

    // The rock panel's cuts, newest first by the file's time - the ring's
    // numbers say nothing of their order
    struct Cut {
        fs::path png;
        std::string name;
        fs::file_time_type when{};
    };
    std::vector<Cut> cuts;
    for (fs::directory_iterator it(debugFolder, ec), done; !ec && it != done; it.increment(ec)) {
        std::error_code kind;
        if (!it->is_regular_file(kind)) continue;
        const std::string name = nameOf(it->path());
        if (!panelCut(name)) continue;
        std::error_code time;
        const fs::file_time_type when = fs::last_write_time(it->path(), time);
        if (!time) cuts.push_back({it->path(), name, when});
    }
    std::sort(cuts.begin(), cuts.end(),
              [](const Cut& a, const Cut& b) { return a.when != b.when ? a.when > b.when : a.name > b.name; });
    if (cuts.size() > PANEL_CUTS) cuts.resize(PANEL_CUTS);
    for (const Cut& cut : cuts) pair(cut.png, cut.name);
    return out;
}

std::uint32_t crc32(const std::string& data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = CRC_TABLE[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::size_t zipSize(const std::vector<Entry>& entries) {
    if (entries.size() >= 0xFFFF) return 0;
    std::uint64_t size = END_SIZE;
    for (const Entry& entry : entries) {
        const std::uint64_t name = FOLDER_LENGTH + entry.name.size();
        if (name > 0xFFFF || entry.data.size() >= 0xFFFFFFFFull) return 0;
        size += LOCAL_SIZE + name + entry.data.size() + CENTRAL_SIZE + name;
        // below 4 GiB, every offset and size fits in 32 bits short of the mark
        if (size >= 0xFFFFFFFFull) return 0;
    }
    return (std::size_t)size;
}

std::string zip(const std::vector<Entry>& entries) {
    const std::size_t size = zipSize(entries);
    if (size == 0) return {};
    std::string out, central;
    out.reserve(size);
    for (const Entry& entry : entries) {
        const std::string name = std::string(FOLDER) + entry.name;
        const std::uint32_t crc = crc32(entry.data);
        const std::uint32_t length = (std::uint32_t)entry.data.size();
        const DosStamp stamp = dosStamp(entry.written);
        const std::uint32_t at = (std::uint32_t)out.size();
        put32(out, SIG_LOCAL);
        put16(out, NEEDED);
        put16(out, FLAG_UTF8);
        put16(out, STORED);
        put16(out, stamp.time);
        put16(out, stamp.date);
        put32(out, crc);
        put32(out, length);   // stored: packed as long as it is
        put32(out, length);
        put16(out, (std::uint32_t)name.size());
        put16(out, 0);        // no extra field
        out += name;
        out += entry.data;

        put32(central, SIG_CENTRAL);
        put16(central, MADE_BY);
        put16(central, NEEDED);
        put16(central, FLAG_UTF8);
        put16(central, STORED);
        put16(central, stamp.time);
        put16(central, stamp.date);
        put32(central, crc);
        put32(central, length);
        put32(central, length);
        put16(central, (std::uint32_t)name.size());
        put16(central, 0);    // no extra field
        put16(central, 0);    // no comment
        put16(central, 0);    // disk 0
        put16(central, 0);    // internal attributes
        put32(central, 0);    // external attributes
        put32(central, at);
        central += name;
    }
    const std::uint32_t centralAt = (std::uint32_t)out.size();
    out += central;
    put32(out, SIG_END);
    put16(out, 0);   // this disk
    put16(out, 0);   // the disk the central directory begins on
    put16(out, (std::uint32_t)entries.size());
    put16(out, (std::uint32_t)entries.size());
    put32(out, (std::uint32_t)central.size());
    put32(out, centralAt);
    put16(out, 0);   // no comment
    return out;
}

bool unzip(const std::string& zip, std::vector<Entry>& out, std::string& why) {
    out.clear();
    why.clear();
    auto fail = [&](const std::string& reason) {
        why = reason;
        return false;
    };
    // the end record: its signature, and its comment reaching the zip's end
    std::size_t end = std::string::npos;
    if (zip.size() >= END_SIZE) {
        const std::size_t last = zip.size() - END_SIZE;
        const std::size_t lowest = last > 0xFFFF ? last - 0xFFFF : 0;
        for (std::size_t at = last + 1; at-- > lowest;) {
            if (get32(zip, at) == SIG_END && at + END_SIZE + get16(zip, at + 20) == zip.size()) {
                end = at;
                break;
            }
        }
    }
    if (end == std::string::npos) return fail("No end record");
    const std::size_t count = get16(zip, end + 10);
    const std::size_t centralSize = get32(zip, end + 12), centralAt = get32(zip, end + 16);
    if (get16(zip, end + 4) != 0 || get16(zip, end + 6) != 0 || get16(zip, end + 8) != count) {
        return fail("A zip of more than one part");
    }
    if (centralAt > end || centralSize > end - centralAt) return fail("Central directory outside the zip");
    const std::size_t centralEnd = centralAt + centralSize;
    std::vector<Entry> entries;
    std::size_t at = centralAt;
    for (std::size_t n = 1; n <= count; n++) {
        const std::string which = "Entry " + std::to_string(n);
        if (at > centralEnd || centralEnd - at < CENTRAL_SIZE || get32(zip, at) != SIG_CENTRAL) {
            return fail(which + ": no central header");
        }
        const std::uint32_t flags = get16(zip, at + 8), method = get16(zip, at + 10);
        const std::uint32_t time = get16(zip, at + 12), date = get16(zip, at + 14);
        const std::uint32_t crc = get32(zip, at + 16), packed = get32(zip, at + 20), length = get32(zip, at + 24);
        const std::size_t nameLength = get16(zip, at + 28);
        const std::size_t skip = nameLength + get16(zip, at + 30) + get16(zip, at + 32);
        const std::size_t local = get32(zip, at + 42);
        if (centralEnd - at - CENTRAL_SIZE < skip) return fail(which + ": central header past the directory");
        const std::string name = zip.substr(at + CENTRAL_SIZE, nameLength);
        const std::string named = which + " (" + name + ")";
        if (flags & 0x2041) return fail(named + ": encrypted");
        if (method != STORED || packed != length) return fail(named + ": not stored");
        if (local > centralAt || centralAt - local < LOCAL_SIZE || get32(zip, local) != SIG_LOCAL) {
            return fail(named + ": no local header");
        }
        const std::size_t localName = get16(zip, local + 26), localExtra = get16(zip, local + 28);
        const std::size_t dataAt = local + LOCAL_SIZE + localName + localExtra;
        if (dataAt > centralAt || centralAt - dataAt < length) return fail(named + ": data past the entries");
        // the local header as the central one has it; CRC and sizes only
        // where no data descriptor follows the data instead (bit 3)
        const bool sizesHere = (get16(zip, local + 6) & 0x0008) == 0;
        if (get16(zip, local + 8) != method || localName != nameLength ||
            zip.compare(local + LOCAL_SIZE, nameLength, name) != 0 ||
            (sizesHere && (get32(zip, local + 14) != crc || get32(zip, local + 18) != packed ||
                           get32(zip, local + 22) != length))) {
            return fail(named + ": local header does not match");
        }
        Entry entry;
        entry.name = name;
        entry.data = zip.substr(dataAt, length);
        if (crc32(entry.data) != crc) return fail(named + ": CRC does not match");
        entry.written = unixOf(date, time);
        entries.push_back(std::move(entry));
        at += CENTRAL_SIZE + skip;
    }
    if (at != centralEnd) return fail("Central directory longer than its entries");
    out = std::move(entries);
    return true;
}

}  // namespace DebugZip
}  // namespace SCMiner
