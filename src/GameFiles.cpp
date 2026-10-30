#include "GameFiles.h"

#include "DevSwitch.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace SCMiner {
namespace GameFiles {

namespace fs = std::filesystem;

std::string textOf(const fs::path& path) {
    const std::u8string u = path.u8string();
    return std::string(u.begin(), u.end());
}

fs::path pathOf(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

namespace {

constexpr const char* INFO_FILE = "backup-info.txt";

// ---- names ----------------------------------------------------------------

// Upper and lower case as .NET's invariant culture has them for the names
// that come up: ASCII, and the accented letters of Latin-1 (U+00C0..U+00DE
// and U+00E0..U+00FE, but for the two signs between)
std::string recased(const std::string& s, bool upper) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += static_cast<char>(upper ? std::toupper(c) : std::tolower(c));
        } else if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (upper && d >= 0xA0 && d <= 0xBE && d != 0xB7) d = static_cast<unsigned char>(d - 0x20);
            if (!upper && d >= 0x80 && d <= 0x9E && d != 0x97) d = static_cast<unsigned char>(d + 0x20);
            out += static_cast<char>(c);
            out += static_cast<char>(d);
            i++;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

bool sameText(const std::string& a, const std::string& b) {
    return recased(a, true) == recased(b, true);
}

// .NET's StringComparer.OrdinalIgnoreCase, as SC Blackbox sorts
bool ordinalIgnoreCaseLess(const std::string& a, const std::string& b) {
    return recased(a, true) < recased(b, true);
}

// A name a folder or file can have on every system: no separators or
// wildcards, not "." or "..", no control characters, 200 bytes at most
bool plainPart(const std::string& s) {
    if (s.empty() || s.size() > 200 || s == "." || s == "..") return false;
    for (unsigned char c : s) {
        if (c < 0x20 || std::strchr("\\/:*?\"<>|", c)) return false;
    }
    return s.back() != ' ' && s.back() != '.';
}

bool endsWithText(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() > n && sameText(s.substr(s.size() - n), tail);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t from = 0;
    while (true) {
        const size_t at = s.find(sep, from);
        parts.push_back(s.substr(from, at == std::string::npos ? std::string::npos : at - from));
        if (at == std::string::npos) break;
        from = at + 1;
    }
    return parts;
}

// ---- files ------------------------------------------------------------------

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

#ifdef _WIN32
std::wstring wide(const fs::path& p) { return p.wstring(); }

long long msOfFiletime(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

FILETIME filetimeOfMs(long long ms) {
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<unsigned long long>(ms) * 10000ULL + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}
#endif

long long writeTimeOf(const fs::path& file) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wide(file).c_str(), GetFileExInfoStandard, &a)) return 0;
    return msOfFiletime(a.ftLastWriteTime);
#else
    struct stat st;
    if (stat(file.c_str(), &st) != 0) return 0;
    return static_cast<long long>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
}

void setWriteTime(const fs::path& file, long long ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    HANDLE h = CreateFileW(wide(file).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const FILETIME ft = filetimeOfMs(ms);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
#else
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = static_cast<time_t>(ms / 1000);
    times[1].tv_nsec = static_cast<long>((ms % 1000) * 1000000);
    utimensat(AT_FDCWD, file.c_str(), times, 0);
#endif
}

// The whole file, read while the game may hold it open; false where it
// cannot be read
bool readShared(const fs::path& file, std::string& out, size_t limit = size_t(16) << 20) {
    out.clear();
#ifdef _WIN32
    HANDLE h = CreateFileW(wide(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buffer[1 << 15];
    DWORD got = 0;
    bool ok = true;
    while (true) {
        if (!ReadFile(h, buffer, sizeof buffer, &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        out.append(buffer, got);
        if (out.size() > limit) {
            ok = false;
            break;
        }
    }
    CloseHandle(h);
    return ok;
#else
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    char buffer[1 << 15];
    while (in) {
        in.read(buffer, sizeof buffer);
        out.append(buffer, static_cast<size_t>(in.gcount()));
        if (out.size() > limit) return false;
    }
    return !in.bad();
#endif
}

bool writeWhole(const fs::path& file, const std::string& data) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    return !out.fail();
}

bool readOnly(const fs::path& p) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && !(st.st_mode & S_IWUSR);
#endif
}

void setReadOnly(const fs::path& p, bool on) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(wide(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    const DWORD b = on ? (a | FILE_ATTRIBUTE_READONLY) : (a & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    if (b != a) SetFileAttributesW(wide(p).c_str(), b);
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return;
    const mode_t m = on ? (st.st_mode & ~(S_IWUSR | S_IWGRP | S_IWOTH)) : (st.st_mode | S_IWUSR);
    if (m != st.st_mode) chmod(p.c_str(), m);
#endif
}

// Tries a step a few times while something holds the folder for a moment:
// OneDrive opens new files to upload them, and a folder with an open file in
// it cannot be renamed. 1.15 s at most - this runs on Bedrock's own thread
template <typename Step>
bool retried(Step step) {
    static const int waits[] = {50, 100, 200, 300, 500};
    for (int i = 0;; i++) {
        if (step()) return true;
        if (i >= 5) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(waits[i]));
    }
}

// A link (or junction) to somewhere else: its own path resolves elsewhere
// than its parent's plus its name
bool pointsElsewhere(const fs::path& p) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(p, ec))) return true;
    const fs::path real = fs::weakly_canonical(p, ec);
    if (ec) return false;
    const fs::path parent = fs::weakly_canonical(p.parent_path(), ec);
    if (ec) return false;
    return !sameText(textOf(real), textOf(parent / p.filename()));
}

// A folder and all in it; read-only taken off first (OneDrive marks its
// folders so), links removed and never followed, the folders inside before
// the files, so a backup removed half way keeps its info and is seen
bool removeTree(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(dir, ec))) return true;
    setReadOnly(dir, false);
    if (pointsElsewhere(dir)) return retried([&] { return fs::remove(dir, ec) || !fs::exists(fs::symlink_status(dir, ec)); });
    std::vector<fs::path> dirs, files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (e.is_directory(ec2) && !e.is_symlink(ec2)) dirs.push_back(e.path());
        else files.push_back(e.path());
    }
    bool ok = true;
    for (const auto& d : dirs) ok &= removeTree(d);
    for (const auto& f : files) {
        setReadOnly(f, false);
        ok &= retried([&] { return fs::remove(f, ec) || !fs::exists(fs::symlink_status(f, ec)); });
    }
    ok &= retried([&] { return fs::remove(dir, ec) || !fs::exists(fs::symlink_status(dir, ec)); });
    return ok;
}

// A child of a folder by its name, case aside: under Linux the game's
// folders sit in a Wine prefix, whose case is the game's, not ours
fs::path child(const fs::path& dir, const std::string& name) {
    const fs::path direct = dir / pathOf(name);
#ifdef _WIN32
    return direct;
#else
    std::error_code ec;
    if (fs::exists(direct, ec)) return direct;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (sameText(textOf(e.path().filename()), name)) return e.path();
    }
    return direct;
#endif
}

fs::path placeOf(const fs::path& user, const std::string& rel) {
    fs::path p = user;
    for (const std::string& part : split(rel, '\\')) p = child(p, part);
    return p;
}

std::vector<std::string> filesIn(const fs::path& dir, const char* ending) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2)) continue;
        const std::string name = textOf(e.path().filename());
        if (endsWithText(name, ending) && plainPart(name)) out.push_back(name);
    }
    return out;
}

// ---- SHA-256 ------------------------------------------------------------------

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block[64];
    size_t used = 0;
    uint64_t bits = 0;

    static uint32_t rot(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const unsigned char* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
                   uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rot(e, 6) ^ rot(e, 11) ^ rot(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rot(a, 2) ^ rot(a, 13) ^ rot(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void add(const unsigned char* p, size_t n) {
        bits += uint64_t(n) * 8;
        while (n > 0) {
            const size_t take = (std::min)(n, size_t(64) - used);
            std::memcpy(block + used, p, take);
            used += take;
            p += take;
            n -= take;
            if (used == 64) {
                compress(block);
                used = 0;
            }
        }
    }

    std::string hex() {
        const uint64_t length = bits;
        const unsigned char one = 0x80, zero = 0;
        add(&one, 1);
        while (used != 56) add(&zero, 1);
        unsigned char tail[8];
        for (int i = 0; i < 8; i++) tail[i] = static_cast<unsigned char>(length >> (56 - 8 * i));
        add(tail, 8);
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (uint32_t v : h) {
            for (int i = 28; i >= 0; i -= 4) out += digits[(v >> i) & 15];
        }
        return out;
    }
};

// ---- times as backup-info.txt writes them ------------------------------------

std::tm localOf(std::time_t t) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return local;
}

// Seconds the local time is ahead of UTC at `t`
long offsetAt(std::time_t t) {
    std::tm local = localOf(t);
#ifdef _WIN32
    return static_cast<long>(_mkgmtime(&local) - t);
#else
    return static_cast<long>(timegm(&local) - t);
#endif
}

// .NET's "o": 2026-10-29T15:47:24.5681841+01:00
std::string roundTrip(long long ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    const std::tm local = localOf(t);
    const long off = offsetAt(t);
    char text[64];
    std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%03d0000%c%02ld:%02ld", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<int>(ms % 1000),
                  off < 0 ? '-' : '+', std::labs(off) / 3600, (std::labs(off) % 3600) / 60);
    return text;
}

std::string idOf(long long ms) {
    const std::tm local = localOf(static_cast<std::time_t>(ms / 1000));
    char text[32];
    std::snprintf(text, sizeof text, "%04d-%02d-%02d_%02d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return text;
}

bool figures(const std::string& s, size_t at, size_t n) {
    if (at + n > s.size()) return false;
    for (size_t i = at; i < at + n; i++) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

int number(const std::string& s, size_t at, size_t n) { return std::atoi(s.substr(at, n).c_str()); }

// Local time as Unix ms
long long localMs(int year, int month, int day, int hour, int minute, int second) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    return t == static_cast<std::time_t>(-1) ? 0 : static_cast<long long>(t) * 1000;
}

// "2026-10-29T15:47:24.5681841+01:00", "...Z", or with no zone (local);
// 0 where it is none of them
long long parseRoundTrip(const std::string& s) {
    if (s.size() < 19 || !figures(s, 0, 4) || s[4] != '-' || !figures(s, 5, 2) || s[7] != '-' || !figures(s, 8, 2) ||
        s[10] != 'T' || !figures(s, 11, 2) || s[13] != ':' || !figures(s, 14, 2) || s[16] != ':' || !figures(s, 17, 2)) {
        return 0;
    }
    const int year = number(s, 0, 4), month = number(s, 5, 2), day = number(s, 8, 2);
    const int hour = number(s, 11, 2), minute = number(s, 14, 2), second = number(s, 17, 2);
    size_t at = 19;
    long long ms = 0;
    if (at < s.size() && s[at] == '.') {
        int scale = 100;
        for (at++; at < s.size() && std::isdigit(static_cast<unsigned char>(s[at])); at++) {
            if (scale > 0) {
                ms += (s[at] - '0') * scale;
                scale /= 10;
            }
        }
    }
    // days since 1970 for the date, as UTC
    auto daysOf = [](int y, int m, int d) {
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
    };
    const long long asUtc = ((daysOf(year, month, day) * 24 + hour) * 60 + minute) * 60 + second;
    if (at < s.size() && (s[at] == 'Z' || s[at] == 'z')) return asUtc * 1000 + ms;
    if (at + 6 <= s.size() && (s[at] == '+' || s[at] == '-') && figures(s, at + 1, 2) && s[at + 3] == ':' &&
        figures(s, at + 4, 2)) {
        const long long off = (number(s, at + 1, 2) * 60 + number(s, at + 4, 2)) * 60;
        return (asUtc - (s[at] == '-' ? -off : off)) * 1000 + ms;
    }
    const long long local = localMs(year, month, day, hour, minute, second);
    return local ? local + ms : 0;
}

// The id's own time: yyyy-MM-dd_HH-mm-ss, local
long long timeOfId(const std::string& id) {
    if (id.size() < 19 || !figures(id, 0, 4) || id[4] != '-' || !figures(id, 5, 2) || id[7] != '-' || !figures(id, 8, 2) ||
        id[10] != '_' || !figures(id, 11, 2) || id[13] != '-' || !figures(id, 14, 2) || id[16] != '-' ||
        !figures(id, 17, 2)) {
        return 0;
    }
    return localMs(number(id, 0, 4), number(id, 5, 2), number(id, 8, 2), number(id, 11, 2), number(id, 14, 2),
                   number(id, 17, 2));
}

// ---- backup-info.txt ------------------------------------------------------------

std::string oneLine(std::string v) {
    for (char& c : v) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return v;
}

std::vector<std::pair<std::string, std::string>> readInfo(const fs::path& file, bool& ok) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string text;
    ok = readShared(file, text, 1 << 20);
    if (!ok) return out;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    size_t from = 0;
    while (from <= text.size()) {
        size_t nl = text.find('\n', from);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(from, nl - from);
        from = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0 || line[0] == '#') continue;
        std::string key = line.substr(0, eq);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!key.empty() && key.front() == ' ') key.erase(0, 1);
        out.push_back({key, line.substr(eq + 1)});
        if (nl == text.size()) break;
    }
    return out;
}

std::string trimmed(std::string v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(0, 1);
    return v;
}

std::string infoText(const Backup& b) {
    std::string out = "# SC Blackbox. The game files are in the \"user\" folder next to this file.\r\n";
    auto add = [&](const char* key, const std::string& value) {
        out += key;
        out += '=';
        out += oneLine(value);
        out += "\r\n";
    };
    add("created", roundTrip(b.created));
    add("channel", b.channel);
    add("source", b.source);
    add("reason", b.reason);
    add("restoredfrom", b.restoredFrom);
    add("restoredparts", b.restoredParts);
    add("summary", b.summary);
    add("detail", b.detail);
    add("flag", b.flag);
    add("kept", b.kept ? "1" : "0");
    add("binds", std::to_string(b.binds));
    add("allhash", b.allHash);
    add("keybindhash", b.keybindHash);
    add("characters", std::to_string(b.characters));
    return out;
}

// The channel's folder in the backups: its name, with what no file name
// may hold made '_'
fs::path storeOf(const fs::path& root, const std::string& channel) {
    std::string safe = channel;
    for (char& c : safe) {
        if (static_cast<unsigned char>(c) < 0x20 || std::strchr("\\/:*?\"<>|", c)) c = '_';
    }
    return root / pathOf(safe);
}

bool plainId(const std::string& id) { return plainPart(id) && id[0] != '.'; }

// Atomic enough for a small text: written beside it, then put in its place
bool replaceText(const fs::path& file, const std::string& text) {
    fs::path partial = file;
    partial += ".sckb-tmp";
    if (!writeWhole(partial, text)) return false;
    std::error_code ec;
    const bool was = readOnly(file);
    if (was) setReadOnly(file, false);
    const bool ok = retried([&] {
        fs::rename(partial, file, ec);
        return !ec;
    });
    if (!ok) fs::remove(partial, ec);
    if (was) setReadOnly(file, true);
    return ok;
}

std::string randomName() {
    static std::mt19937_64 random(std::random_device{}() ^ static_cast<unsigned long long>(nowMs()));
    char text[40];
    std::snprintf(text, sizeof text, ".incomplete-%016llx%04x", static_cast<unsigned long long>(random()),
                  static_cast<unsigned>(random() & 0xFFFF));
    return text;
}

#ifdef _WIN32
fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR folder = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder)) && folder) out = folder;
    if (folder) CoTaskMemFree(folder);
    return out;
}
#else
fs::path home() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path("/tmp");
}

// ~/.config/user-dirs.dirs: XDG_DOCUMENTS_DIR="$HOME/Documents"
fs::path documents() {
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const fs::path dirs = (config && *config ? fs::path(config) : home() / ".config") / "user-dirs.dirs";
    std::ifstream in(dirs);
    std::string line;
    while (std::getline(in, line)) {
        const size_t at = line.find("XDG_DOCUMENTS_DIR");
        if (at == std::string::npos || line.find('#') < at) continue;
        const size_t q1 = line.find('"'), q2 = line.rfind('"');
        if (q1 == std::string::npos || q2 <= q1) continue;
        std::string v = line.substr(q1 + 1, q2 - q1 - 1);
        if (v.rfind("$HOME", 0) == 0) v = textOf(home()) + v.substr(5);
        if (!v.empty()) return pathOf(v);
    }
    return home() / "Documents";
}
#endif

}  // namespace

// ---- names ------------------------------------------------------------------

Kind kindOf(const std::string& rel) {
    const std::vector<std::string> p = split(rel, '\\');
    if (p.size() < 4 || !sameText(p[0], "client") || !plainPart(p[1])) return Kind::None;
    for (const std::string& part : p) {
        if (!plainPart(part)) return Kind::None;
    }
    if (p.size() == 5 && sameText(p[2], "Profiles") && sameText(p[3], "default")) {
        if (sameText(p[4], "actionmaps.xml")) return Kind::Keybinds;
        if (sameText(p[4], "attributes.xml")) return Kind::Settings;
        return Kind::None;
    }
    if (p.size() == 5 && sameText(p[2], "controls") && sameText(p[3], "mappings") && endsWithText(p[4], ".xml")) {
        return Kind::Layout;
    }
    if (p.size() == 4 && sameText(p[2], "CustomCharacters") && endsWithText(p[3], ".chf")) return Kind::Character;
    return Kind::None;
}

// ---- the game's folder ----------------------------------------------------------

std::vector<Channel> channels(const fs::path& gameFolder) {
    std::vector<Channel> out;
    if (gameFolder.empty()) return out;
    const fs::path library = gameFolder.parent_path();
    std::error_code ec;
    struct Found {
        Channel channel;
        std::string real;
        bool link = false;
    };
    std::vector<Found> found;
    for (const auto& e : fs::directory_iterator(library, ec)) {
        std::error_code ec2;
        if (!e.is_directory(ec2)) continue;
        const fs::path d = e.path();
        const bool looks = fs::exists(child(child(d, "Bin64"), "StarCitizen.exe"), ec2) ||
                           fs::exists(child(d, "Data.p4k"), ec2) || fs::exists(child(d, "StarCitizen_Launcher.exe"), ec2) ||
                           fs::is_directory(child(child(d, "user"), "client"), ec2);
        if (!looks) continue;
        Found f;
        f.channel.name = textOf(d.filename());
        f.channel.folder = d;
        const fs::path real = fs::weakly_canonical(d, ec2);
        f.real = recased(textOf(ec2 ? d : real), true);
        f.link = pointsElsewhere(d);
        found.push_back(std::move(f));
    }
    static const char* const ORDER[] = {"LIVE", "HOTFIX", "PTU", "EPTU", "TECH-PREVIEW"};
    auto orderOf = [](const std::string& name) {
        for (int i = 0; i < 5; i++) {
            if (sameText(name, ORDER[i])) return i;
        }
        return 5;
    };
    auto before = [&](const Found& a, const Found& b) {
        if (a.link != b.link) return !a.link;
        if (orderOf(a.channel.name) != orderOf(b.channel.name)) return orderOf(a.channel.name) < orderOf(b.channel.name);
        return ordinalIgnoreCaseLess(a.channel.name, b.channel.name);
    };
    std::sort(found.begin(), found.end(), before);
    std::vector<std::string> reals;
    for (Found& f : found) {
        const auto at = std::find(reals.begin(), reals.end(), f.real);
        if (at != reals.end()) {
            out[static_cast<size_t>(at - reals.begin())].aliases.push_back(f.channel.name);
            continue;
        }
        reals.push_back(f.real);
        out.push_back(f.channel);
    }
    std::sort(out.begin(), out.end(), [&](const Channel& a, const Channel& b) {
        if (orderOf(a.name) != orderOf(b.name)) return orderOf(a.name) < orderOf(b.name);
        return ordinalIgnoreCaseLess(a.name, b.name);
    });
    return out;
}

std::vector<std::string> names(const fs::path& user) {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path clients = child(user, "client");
    for (const auto& e : fs::directory_iterator(clients, ec)) {
        std::error_code ec2;
        if (!e.is_directory(ec2)) continue;
        const std::string n = textOf(e.path().filename());
        if (!plainPart(n)) continue;
        const fs::path profile = child(child(e.path(), "Profiles"), "default");
        for (const char* name : {"actionmaps.xml", "attributes.xml"}) {
            if (fs::is_regular_file(child(profile, name), ec2)) out.push_back("client\\" + n + "\\Profiles\\default\\" + name);
        }
        for (const std::string& f : filesIn(child(child(e.path(), "controls"), "mappings"), ".xml")) {
            out.push_back("client\\" + n + "\\controls\\mappings\\" + f);
        }
        for (const std::string& f : filesIn(child(e.path(), "CustomCharacters"), ".chf")) {
            out.push_back("client\\" + n + "\\CustomCharacters\\" + f);
        }
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& r) { return kindOf(r) == Kind::None; }),
              out.end());
    std::sort(out.begin(), out.end(), ordinalIgnoreCaseLess);
    return out;
}

std::string stamp(const fs::path& user) {
    std::string out;
    for (const std::string& rel : names(user)) {
        const fs::path p = placeOf(user, rel);
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        out += rel + "|" + (ec ? std::string("?") : std::to_string(size)) + "|" + std::to_string(writeTimeOf(p)) + "\n";
    }
    return out;
}

Set read(const fs::path& user) {
    Set set;
    for (const std::string& rel : names(user)) {
        const fs::path p = placeOf(user, rel);
        File f;
        if (!readShared(p, f.data)) continue;   // gone between listing and reading
        f.time = writeTimeOf(p);
        set[rel] = std::move(f);
    }
    return set;
}

long long played(const fs::path& user) {
    return writeTimeOf(placeOf(user, "client\\0\\Profiles\\default\\actionmaps.xml"));
}

int write(const fs::path& user, const std::map<std::string, std::string>& files, std::string& why) {
    int written = 0;
    for (const auto& [rel, data] : files) {
        if (kindOf(rel) == Kind::None) {
            why = rel + " is none of the game's settings files";
            return -1;
        }
        const fs::path target = placeOf(user, rel);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        fs::path partial = target;
        partial += ".bedrock-tmp";
        if (!writeWhole(partial, data)) {
            fs::remove(partial, ec);
            why = "could not write " + rel;
            return -1;
        }
        // some players keep their keybinds read-only on purpose: so it stays
        const bool was = readOnly(target);
        if (was) setReadOnly(target, false);
        const bool ok = retried([&] {
            fs::rename(partial, target, ec);
            return !ec;
        });
        if (was) setReadOnly(target, true);
        if (!ok) {
            fs::remove(partial, ec);
            why = "could not write " + rel + " - " + ec.message();
            return -1;
        }
        written++;
    }
    return written;
}

bool gameRunning() {
    // (a test's own answer, in a developer's build)
    if (const char* own = DEV_ENV("BEDROCK_GAME_RUNNING")) return *own == '1';
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!_wcsicmp(entry.szExeFile, L"StarCitizen.exe") || !_wcsicmp(entry.szExeFile, L"StarCitizen_Launcher.exe")) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
#else
    // under Wine the process's name is the exe's, cut to 15 letters
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/proc", ec)) {
        const std::string pid = textOf(e.path().filename());
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        std::ifstream in(e.path() / "comm");
        std::string comm;
        std::getline(in, comm);
        if (comm == "StarCitizen.exe" || comm == "StarCitizen_Lau") return true;
    }
    return false;
#endif
}

bool blackboxRunning() {
    if (const char* own = DEV_ENV("BEDROCK_BLACKBOX_RUNNING")) return *own == '1';
#ifdef _WIN32
    // SC Blackbox holds this mutex while it runs (its old name, kept)
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\SCKeybindBackup-4f1c7e2a");
    if (!m) return false;
    CloseHandle(m);
    return true;
#else
    // ...and under Linux a lock on this file
    fs::path folder;
    const char* run = std::getenv("XDG_RUNTIME_DIR");
    std::error_code ec;
    if (run && *run && fs::is_directory(run, ec)) {
        folder = run;
    } else {
        const char* user = std::getenv("USER");
        folder = fs::temp_directory_path(ec) / ("sc-blackbox-" + std::string(user ? user : ""));
    }
    const int fd = open((folder / "sc-blackbox.lock").c_str(), O_RDONLY);
    if (fd < 0) return false;
    const bool held = flock(fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    if (!held) flock(fd, LOCK_UN);
    close(fd);
    return held;
#endif
}

std::string sha256(std::string_view bytes) {
    Sha256 s;
    s.add(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    return s.hex();
}

Hashes hashesOf(const Set& files) {
    std::vector<std::string> order;
    for (const auto& [rel, f] : files) order.push_back(rel);
    std::stable_sort(order.begin(), order.end(), ordinalIgnoreCaseLess);
    std::string all, keybinds, characters;
    for (const std::string& rel : order) {
        const std::string line = recased(rel, false) + "|" + sha256(files.at(rel).data) + "\n";
        const Kind k = kindOf(rel);
        if (k == Kind::Character) {
            characters += line;
            continue;
        }
        all += line;
        if (k == Kind::Keybinds || k == Kind::Layout) keybinds += line;
    }
    return {sha256(all), sha256(keybinds), sha256(characters)};
}

// ---- the backups ------------------------------------------------------------------

Where where() {
    Where w;
#ifdef _WIN32
    const fs::path ini = knownFolder(FOLDERID_RoamingAppData) / "SC Keybind Backup" / "settings.ini";
    const fs::path standard = knownFolder(FOLDERID_Documents) / "SC Keybind Backups";
#else
    const char* config = std::getenv("XDG_CONFIG_HOME");
    const fs::path ini = (config && *config ? fs::path(config) : home() / ".config") / "sc-blackbox" / "settings.ini";
    const fs::path standard = documents() / "SC Blackbox";
#endif
    w.root = standard;
    std::string text;
    if (readShared(ini, text, 1 << 16)) {
        size_t from = 0;
        while (from < text.size()) {
            size_t nl = text.find('\n', from);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(from, nl - from);
            from = nl + 1;
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            const std::string key = recased(trimmed(line.substr(0, eq)), false);
            const std::string value = trimmed(line.substr(eq + 1));
            if (key == "backupfolder" && !value.empty()) {
#ifdef _WIN32
                // %VARIABLES% as Environment.ExpandEnvironmentVariables has them
                const std::wstring raw = pathOf(value).wstring();
                wchar_t expanded[4096];
                const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded, 4096);
                fs::path p = (n > 0 && n <= 4096) ? fs::path(expanded) : fs::path(raw);
#else
                fs::path p = pathOf(value);
#endif
                if (p.is_relative()) p = ini.parent_path() / p;
                std::error_code ec;
                w.root = fs::weakly_canonical(p, ec);
                if (ec) w.root = p;
            } else if (key == "keep" && !value.empty()) {
                w.keep = (std::max)(5, std::atoi(value.c_str()));
            } else if (key == "keepdistinct" && !value.empty()) {
                w.keepDistinct = (std::max)(1, std::atoi(value.c_str()));
            }
        }
    }
    if (const char* own = DEV_ENV("BEDROCK_BLACKBOX_ROOT")) w.root = pathOf(own);
    return w;
}

std::vector<Backup> backups(const fs::path& root, const std::string& channel) {
    std::vector<Backup> out;
    const fs::path store = storeOf(root, channel);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(store, ec)) {
        std::error_code ec2;
        if (!e.is_directory(ec2)) continue;
        const std::string id = textOf(e.path().filename());
        if (id.empty() || id[0] == '.') continue;
        bool ok = false;
        const auto info = readInfo(e.path() / INFO_FILE, ok);
        if (!ok) continue;
        Backup b;
        b.id = id;
        for (const auto& [key, value] : info) {
            if (key == "created") b.created = parseRoundTrip(trimmed(value));
            else if (key == "channel") b.channel = value;
            else if (key == "source") b.source = value;
            else if (key == "reason") b.reason = value;
            else if (key == "restoredfrom") b.restoredFrom = value;
            else if (key == "restoredparts") b.restoredParts = value;
            else if (key == "summary") b.summary = value;
            else if (key == "detail") b.detail = value;
            else if (key == "flag") b.flag = value;
            else if (key == "kept") b.kept = trimmed(value) == "1";
            else if (key == "binds") b.binds = trimmed(value).empty() ? -1 : std::atoi(trimmed(value).c_str());
            else if (key == "allhash") b.allHash = trimmed(value);
            else if (key == "keybindhash") b.keybindHash = trimmed(value);
            else if (key == "characters") b.counted = true;
        }
        if (!b.created) b.created = timeOfId(id);
        if (!b.created) b.created = writeTimeOf(e.path());
        for (const std::string& rel : names(e.path() / "user")) b.characters += kindOf(rel) == Kind::Character;
        out.push_back(std::move(b));
    }
    std::sort(out.begin(), out.end(), [](const Backup& a, const Backup& b) {
        if (a.created != b.created) return a.created > b.created;
        return a.id > b.id;
    });
    return out;
}

Set backupFiles(const fs::path& root, const std::string& channel, const std::string& id, std::string& why) {
    Set set;
    const fs::path folder = storeOf(root, channel) / pathOf(id);
    std::error_code ec;
    if (!plainId(id) || !fs::is_regular_file(folder / INFO_FILE, ec)) {
        why = "no such backup";
        return set;
    }
    const fs::path user = folder / "user";
    for (const std::string& rel : names(user)) {
        const fs::path p = placeOf(user, rel);
        File f;
        if (!readShared(p, f.data)) {
            why = "cannot read " + rel;
            return {};
        }
        f.time = writeTimeOf(p);
        set[rel] = std::move(f);
    }
    return set;
}

std::string writeBackup(const fs::path& root, const std::string& channel, const fs::path& source, const Set& files,
                        const Backup& info, std::string& why) {
    for (const auto& [rel, f] : files) {
        if (kindOf(rel) == Kind::None) {
            why = rel + " is none of the game's settings files";
            return {};
        }
    }
    const fs::path store = storeOf(root, channel);
    std::error_code ec;
    fs::create_directories(store, ec);
    if (!fs::is_directory(store, ec)) {
        why = "cannot make " + textOf(store);
        return {};
    }
    // what a write cut short left behind
    for (const auto& e : fs::directory_iterator(store, ec)) {
        if (textOf(e.path().filename()).rfind(".incomplete-", 0) == 0) removeTree(e.path());
    }
    Backup b = info;
    b.channel = channel;
    b.source = textOf(source);
    b.created = nowMs();
    const Hashes h = hashesOf(files);
    b.allHash = h.all;
    b.keybindHash = h.keybinds;
    b.characters = 0;
    for (const auto& [rel, f] : files) b.characters += kindOf(rel) == Kind::Character;
    const fs::path partial = store / randomName();
    bool ok = fs::create_directories(partial / "user", ec);
    for (const auto& [rel, f] : files) {
        if (!ok) break;
        fs::path target = partial / "user";
        for (const std::string& part : split(rel, '\\')) target /= pathOf(part);
        fs::create_directories(target.parent_path(), ec);
        ok = writeWhole(target, f.data);
        if (ok) setWriteTime(target, f.time);
    }
    ok = ok && writeWhole(partial / INFO_FILE, infoText(b));
    if (!ok) {
        removeTree(partial);
        why = "cannot write into " + textOf(store);
        return {};
    }
    const std::string base = idOf(b.created);
    std::string id = base;
    for (int i = 2; fs::exists(store / pathOf(id), ec); i++) id = base + "_" + std::to_string(i);
    const bool moved = retried([&] {
        fs::rename(partial, store / pathOf(id), ec);
        return !ec;
    });
    if (!moved) {
        why = "cannot name the backup " + id + " - " + ec.message();
        removeTree(partial);
        return {};
    }
    return id;
}

bool markBackup(const fs::path& root, const std::string& channel, const std::string& id, bool kept, std::string& why) {
    const fs::path file = storeOf(root, channel) / pathOf(id) / INFO_FILE;
    std::string text;
    std::error_code ec;
    if (!plainId(id) || !fs::is_regular_file(file, ec) || !readShared(file, text, 1 << 20)) {
        why = "no such backup";
        return false;
    }
    std::string out;
    bool done = false;
    size_t from = 0;
    while (from < text.size()) {
        size_t nl = text.find('\n', from);
        const size_t end = nl == std::string::npos ? text.size() : nl + 1;
        std::string line = text.substr(from, end - from);
        from = end;
        if (line.rfind("kept=", 0) == 0) {
            line = std::string("kept=") + (kept ? "1" : "0") + "\r\n";
            done = true;
        }
        out += line;
    }
    if (!done) {
        if (!out.empty() && out.back() != '\n') out += "\r\n";
        out += std::string("kept=") + (kept ? "1" : "0") + "\r\n";
    }
    if (!replaceText(file, out)) {
        why = "cannot write " + id + "'s info";
        return false;
    }
    return true;
}

bool removeBackup(const fs::path& root, const std::string& channel, const std::string& id, std::string& why) {
    const fs::path folder = storeOf(root, channel) / pathOf(id);
    std::error_code ec;
    if (!plainId(id) || !fs::is_regular_file(folder / INFO_FILE, ec)) {
        why = "no such backup";
        return false;
    }
    if (!removeTree(folder)) {
        why = "could not remove all of " + id;
        return false;
    }
    return true;
}

void log(const fs::path& root, const std::string& text) {
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path file = root / "log.txt";
    if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) > 512u * 1024u) {
        fs::remove(root / "log.old.txt", ec);
        fs::rename(file, root / "log.old.txt", ec);
    }
    const long long ms = nowMs();
    const std::tm local = localOf(static_cast<std::time_t>(ms / 1000));
    char stampText[32];
    std::snprintf(stampText, sizeof stampText, "%04d-%02d-%02d %02d:%02d:%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    std::ofstream out(file, std::ios::binary | std::ios::app);
#ifdef _WIN32
    out << stampText << "  " << oneLine(text) << "\r\n";
#else
    out << stampText << "  " << oneLine(text) << "\n";
#endif
}

}  // namespace GameFiles
}  // namespace SCMiner
