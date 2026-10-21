#include "DataBackup.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace SCMiner {
namespace DataBackup {

namespace {

// The folders a backup has no business carrying, by their name at the top
// of the data folder. "uex" is what uexcorp.space last said of the prices -
// two megabytes, and asked for again the next time the Ore Finder opens.
bool skipTop(const std::string& name) {
    return name == "debug" || name == "update" || name == "uex" || name == ASIDE;
}

bool isPicture(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ending = name.substr(dot + 1);
    for (auto& c : ending) c = (char)std::tolower((unsigned char)c);
    return ending == "png" || ending == "jpg" || ending == "jpeg" || ending == "bmp" || ending == "gif";
}

std::string slashes(std::filesystem::path relative) {
    std::string out = relative.generic_string();
    return out;
}

std::string readWhole(const std::filesystem::path& p, bool* ok) {
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) {
        if (ok) *ok = false;
        return {};
    }
    if (ok) *ok = true;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

bool safePath(const std::string& path) {
    if (path.empty() || path.size() > 400) return false;
    if (path.front() == '/' || path.front() == '\\') return false;
    // getline on "notes/" hands back one piece and no empty one after it, so
    // a trailing slash has to be caught here rather than segment by segment
    if (path.back() == '/' || path.back() == '\\') return false;
    if (path.find(':') != std::string::npos) return false;      // a drive letter, or a stream
    if (path.find('\0') != std::string::npos) return false;
    // Walk it a piece at a time rather than searching for "..": a path is
    // only as safe as its weakest segment, and "a/../../b" hides one.
    std::string piece;
    std::istringstream in(path);
    int pieces = 0;
    while (std::getline(in, piece, '/')) {
        if (piece.empty()) return false;                        // "a//b", or a trailing slash
        if (piece == "." || piece == "..") return false;
        if (piece.back() == ' ' || piece.back() == '.') return false;   // Windows drops those
        for (unsigned char c : piece) {
            if (c < 32 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*' ||
                c == '\\') {
                return false;
            }
        }
        pieces++;
    }
    return pieces > 0 && pieces <= 12;
}

bool carried(const std::string& path) {
    const size_t slash = path.find('/');
    const std::string top = path.substr(0, slash);
    if (skipTop(top)) return false;
    // Inside a folder: the notes, pictures and all
    if (slash != std::string::npos) return true;
    if (top.rfind("game_text_", 0) == 0) return false;
    // the player's UEX secret key goes nowhere but where it was put - a
    // backup is a file people hand around
    if (top == "uex_key.dat") return false;
    // ...and the install's own number stays with the install: a backup
    // restored on another PC is another install
    if (top == "install_id.txt") return false;
    return !isPicture(top);
}

std::vector<Entry> collect(const std::filesystem::path& dataFolder) {
    std::vector<Entry> out;
    std::error_code ec;
    if (!std::filesystem::exists(dataFolder, ec)) return out;
    for (const auto& top : std::filesystem::directory_iterator(dataFolder, ec)) {
        const std::string name = top.path().filename().string();
        if (skipTop(name)) continue;
        if (top.is_directory(ec)) {
            for (const auto& sub : std::filesystem::recursive_directory_iterator(top.path(), ec)) {
                if (!sub.is_regular_file(ec)) continue;
                const std::string rel =
                    slashes(std::filesystem::relative(sub.path(), dataFolder, ec));
                if (rel.empty() || !safePath(rel) || !carried(rel)) continue;
                bool ok = false;
                std::string bytes = readWhole(sub.path(), &ok);
                if (!ok) continue;
                out.push_back({rel, std::move(bytes)});
            }
        } else if (top.is_regular_file(ec)) {
            if (!safePath(name) || !carried(name)) continue;
            bool ok = false;
            std::string bytes = readWhole(top.path(), &ok);
            if (!ok) continue;
            out.push_back({name, std::move(bytes)});
        }
    }
    // A steady order, so two backups of the same data are the same file
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });
    return out;
}

std::string write(const std::vector<Entry>& entries) {
    std::string out = std::string(MAGIC) + "\n";
    for (const auto& e : entries) {
        out += "FILE\t" + e.path + "\t" + std::to_string(e.bytes.size()) + "\n";
        out += e.bytes;
        out += "\n";   // ...so the next heading starts a line of its own
    }
    return out;
}

bool read(const std::string& text, std::vector<Entry>& out, std::string& error) {
    out.clear();
    error.clear();
    size_t at = 0;
    // what Notepad leaves at the front of a file it has been through
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) at = 3;
    const std::string magic(MAGIC);
    if (text.compare(at, magic.size(), magic) != 0) {
        error = "that is not a Bedrock backup - its first line should read " + magic;
        return false;
    }
    at += magic.size();
    if (at < text.size() && text[at] == '\r') at++;
    if (at >= text.size() || text[at] != '\n') {
        error = "that is not a Bedrock backup - its first line should read " + magic;
        return false;
    }
    at++;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos) eol = text.size();
        std::string head = text.substr(at, eol - at);
        while (!head.empty() && head.back() == '\r') head.pop_back();
        if (head.empty()) { at = eol + 1; continue; }

        std::vector<std::string> field;
        {
            std::string piece;
            std::istringstream in(head);
            while (std::getline(in, piece, '\t')) field.push_back(piece);
        }
        if (field.size() != 3 || field[0] != "FILE") {
            error = "a heading in that backup does not name a file";
            return false;
        }
        long long count = -1;
        try {
            size_t used = 0;
            count = std::stoll(field[2], &used);
            if (used != field[2].size() || count < 0) count = -1;
        } catch (...) {
            count = -1;
        }
        if (count < 0) {
            error = "\"" + field[1] + "\" does not say how long it is";
            return false;
        }
        const size_t start = eol + 1;
        if (start + (size_t)count > text.size()) {
            error = "the backup stops in the middle of \"" + field[1] + "\"";
            return false;
        }
        Entry e;
        e.path = field[1];
        e.bytes = text.substr(start, (size_t)count);
        out.push_back(std::move(e));
        at = start + (size_t)count;
        if (at < text.size() && text[at] == '\r') at++;
        if (at < text.size() && text[at] == '\n') at++;
    }
    if (out.empty()) {
        error = "there is nothing in that backup";
        return false;
    }
    return true;
}

bool restore(const std::filesystem::path& dataFolder, const std::vector<Entry>& entries,
             std::string& asideName, std::string& error) {
    asideName.clear();
    error.clear();
    // Every path first, then anything is touched: half a restore is worse
    // than none, and the one thing a backup file can try on is its paths.
    for (const auto& e : entries) {
        if (!safePath(e.path)) {
            error = "\"" + e.path + "\" is not a path a backup may write to";
            return false;
        }
    }
    // ...and before anything is moved, whether there is anything to put back
    // at all - a backup of nothing but a copy of the game's texts is none
    const bool anything = std::any_of(entries.begin(), entries.end(),
                                      [](const Entry& e) { return carried(e.path); });
    if (!anything) {
        error = "there is nothing in that backup to put back";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dataFolder, ec);

    // What is here now goes into previous\<when>, whole. Nothing is erased,
    // so a restore of the wrong file costs a drag and drop, not an evening.
    char stamp[32] = {};
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        if (localtime_s(&local, &now) == 0) {
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M", &local);
        } else {
            std::snprintf(stamp, sizeof(stamp), "restored");
        }
    }
    std::filesystem::path aside = dataFolder / ASIDE / stamp;
    for (int n = 2; std::filesystem::exists(aside, ec) && n < 100; n++) {
        aside = dataFolder / ASIDE / (std::string(stamp) + " " + std::to_string(n));
    }
    std::filesystem::create_directories(aside, ec);
    if (ec) {
        error = "cannot make " + aside.string();
        return false;
    }
    for (const auto& top : std::filesystem::directory_iterator(dataFolder, ec)) {
        const std::string name = top.path().filename().string();
        if (skipTop(name)) continue;
        // what no backup carries stays - the game's texts are not read out
        // of Data.p4k again for want of a copy that was fine
        if (top.is_regular_file(ec) && !carried(name)) continue;
        std::error_code moveEc;
        std::filesystem::rename(top.path(), aside / name, moveEc);
        if (moveEc) {
            // A file something else holds open stays where it is; the
            // restore below writes over it, which is what was asked for
            std::filesystem::copy(top.path(), aside / name,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing,
                                  moveEc);
        }
    }
    asideName = aside.filename().string();

    int written = 0;
    for (const auto& e : entries) {
        if (!carried(e.path)) continue;
        const std::filesystem::path p = dataFolder / std::filesystem::path(e.path);
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) continue;
        out.write(e.bytes.data(), (std::streamsize)e.bytes.size());
        if (out) written++;
    }
    if (written == 0) {
        error = "nothing could be written into " + dataFolder.string();
        return false;
    }
    return true;
}

}  // namespace DataBackup
}  // namespace SCMiner
