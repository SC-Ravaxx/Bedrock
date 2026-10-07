#include "NoteBook.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

namespace SCMiner {

namespace {

std::string timeText(std::int64_t seconds) {
    const std::time_t t = (std::time_t)seconds;
    std::tm local{};
    if (localtime_s(&local, &t) != 0) return "";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
    return buf;
}

std::string nowText() {
    return timeText((std::int64_t)std::time(nullptr));
}

}  // namespace

std::string NoteBook::sanitise(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        // what Windows will not have in a file name, plus the control range
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*') {
            continue;
        }
        out += (char)c;
    }
    // no leading or trailing spaces or dots - Windows quietly drops those
    const std::string trim = " \t.";
    const size_t a = out.find_first_not_of(trim);
    const size_t b = out.find_last_not_of(trim);
    out = (a == std::string::npos) ? "" : out.substr(a, b - a + 1);
    if (out.size() > 60) out = out.substr(0, 60);
    // the handful of names Windows keeps for itself
    static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
                                     "COM4", "LPT1", "LPT2", "LPT3"};
    std::string upper;
    for (char c : out) upper += (char)std::toupper((unsigned char)c);
    for (const char* r : reserved) {
        if (upper == r) return out + " note";
    }
    return out.empty() ? "Untitled" : out;
}

std::string NoteBook::stampOf(const std::filesystem::path& p, std::int64_t* raw) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) {
        if (raw) *raw = 0;
        return "";
    }
#ifdef _WIN32
    // file_clock to something printable, the long way round because
    // clock_cast is not in every toolchain yet
    const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(t).time_since_epoch().count();
    // file_time epoch is 1601-01-01, time_t is 1970-01-01
    const std::int64_t seconds = (std::int64_t)sys - 11644473600LL;
#else
    // libstdc++ counts file_clock from 2174-01-01, not 1601: the arithmetic
    // above would date every note to the year 1453. The clock itself knows the
    // way to system time.
    const auto sysTime = std::chrono::file_clock::to_sys(t);
    const std::int64_t seconds =
        (std::int64_t)std::chrono::duration_cast<std::chrono::seconds>(sysTime.time_since_epoch()).count();
#endif
    if (raw) *raw = seconds;
    return timeText(seconds);
}

std::string NoteBook::firstLine(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        // The brackets around a link are markup the window hides; a preview
        // that shows them is showing plumbing.
        std::string out;
        for (size_t i = 0; i < line.size(); i++) {
            if ((line[i] == '[' || line[i] == ']') && i + 1 < line.size() && line[i + 1] == line[i]) {
                i++;
                continue;
            }
            out += line[i];
        }
        return out.substr(0, 90);
    }
    return "";
}

void NoteBook::load() {
    folders_.clear();
    error_.clear();
    std::error_code ec;
    std::filesystem::create_directories(root(), ec);
    if (ec) {
        error_ = "cannot make the notes folder: " + ec.message();
        return;
    }

    std::vector<std::pair<bool, std::string>> open;   // remembered fold state
    for (const auto& f : folders_) open.emplace_back(f.open, f.name);

    for (const auto& entry : std::filesystem::directory_iterator(root(), ec)) {
        if (!entry.is_directory(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;    // .trash and friends
        Folder folder;
        folder.name = name;
        folder.path = entry.path();
        for (const auto& file : std::filesystem::directory_iterator(entry.path(), ec)) {
            if (!file.is_regular_file(ec)) continue;
            if (file.path().extension() != ".txt") continue;
            Note note;
            note.name = file.path().stem().string();
            note.path = file.path();
            note.modified = stampOf(file.path(), &note.stamp);
            note.preview = firstLine(file.path());
            folder.notes.push_back(std::move(note));
        }
        std::sort(folder.notes.begin(), folder.notes.end(),
                  [](const Note& a, const Note& b) { return a.stamp > b.stamp; });
        folders_.push_back(std::move(folder));
    }
    std::sort(folders_.begin(), folders_.end(),
              [](const Folder& a, const Folder& b) { return a.name < b.name; });

    // A first run gets somewhere to write rather than an empty window
    if (folders_.empty()) {
        addFolder("Mining");
        addFolder("Crafting");
    }
}

int NoteBook::noteCount() const {
    int n = 0;
    for (const auto& f : folders_) n += (int)f.notes.size();
    return n;
}

bool NoteBook::valid(int folder, int note) const {
    if (folder < 0 || folder >= (int)folders_.size()) return false;
    if (note < 0) return true;
    return note < (int)folders_[folder].notes.size();
}

std::filesystem::path NoteBook::freePath(const std::filesystem::path& dir, const std::string& stem,
                                         const char* extension) const {
    std::error_code ec;
    std::filesystem::path p = dir / (stem + extension);
    for (int n = 2; std::filesystem::exists(p, ec) && n < 200; n++) {
        p = dir / (stem + " " + std::to_string(n) + extension);
    }
    return p;
}

int NoteBook::addFolder(const std::string& name) {
    std::error_code ec;
    const std::filesystem::path p = freePath(root(), sanitise(name), "");
    if (!std::filesystem::create_directories(p, ec) || ec) {
        error_ = "cannot make that folder";
        return -1;
    }
    Folder folder;
    folder.name = p.filename().string();
    folder.path = p;
    folders_.push_back(std::move(folder));
    std::sort(folders_.begin(), folders_.end(),
              [](const Folder& a, const Folder& b) { return a.name < b.name; });
    for (int i = 0; i < (int)folders_.size(); i++) {
        if (folders_[i].path == p) return i;
    }
    return -1;
}

int NoteBook::addNote(int folder, const std::string& name) {
    if (!valid(folder)) return -1;
    const std::filesystem::path p = freePath(folders_[folder].path, sanitise(name), ".txt");
    {
        std::ofstream out(p, std::ios::binary);
        if (!out.is_open()) {
            error_ = "cannot write that note";
            return -1;
        }
    }
    Note note;
    note.name = p.stem().string();
    note.path = p;
    note.modified = nowText();
    note.stamp = (std::int64_t)std::time(nullptr);
    folders_[folder].notes.insert(folders_[folder].notes.begin(), std::move(note));
    return 0;   // newest first, so it lands at the top
}

bool NoteBook::renameFolder(int folder, const std::string& name) {
    if (!valid(folder)) return false;
    const std::string clean = sanitise(name);
    if (clean == folders_[folder].name) return true;
    std::error_code ec;
    const std::filesystem::path p = freePath(root(), clean, "");
    std::filesystem::rename(folders_[folder].path, p, ec);
    if (ec) {
        error_ = "cannot rename that folder";
        return false;
    }
    folders_[folder].path = p;
    folders_[folder].name = p.filename().string();
    for (auto& note : folders_[folder].notes) note.path = p / note.path.filename();
    return true;
}

bool NoteBook::renameNote(int folder, int note, const std::string& name) {
    if (!valid(folder, note)) return false;
    const std::string clean = sanitise(name);
    if (clean == folders_[folder].notes[note].name) return true;
    std::error_code ec;
    const std::filesystem::path p = freePath(folders_[folder].path, clean, ".txt");
    std::filesystem::rename(folders_[folder].notes[note].path, p, ec);
    if (ec) {
        error_ = "cannot rename that note";
        return false;
    }
    // The pictures are named after the note, so they follow it
    const std::filesystem::path wasMedia = mediaDir(folder, note);
    folders_[folder].notes[note].path = p;
    folders_[folder].notes[note].name = p.stem().string();
    if (!wasMedia.empty()) {
        std::error_code mec;
        std::filesystem::rename(wasMedia, mediaDir(folder, note), mec);
    }
    return true;
}

int NoteBook::moveNote(int folder, int note, int toFolder) {
    if (!valid(folder, note) || !valid(toFolder) || folder == toFolder) return -1;
    std::error_code ec;
    // freePath, so that dropping a "Notes" into a folder that already has one
    // makes "Notes 2" rather than overwriting an evening's work
    const std::filesystem::path p =
        freePath(folders_[toFolder].path, folders_[folder].notes[note].name, ".txt");
    std::filesystem::rename(folders_[folder].notes[note].path, p, ec);
    if (ec) {
        error_ = "cannot move that note";
        return -1;
    }
    const std::filesystem::path wasMedia = mediaDir(folder, note);
    Note moved = folders_[folder].notes[note];
    moved.path = p;
    moved.name = p.stem().string();
    if (!wasMedia.empty()) {
        std::error_code mec;
        std::filesystem::rename(wasMedia, p.parent_path() / (moved.name + MEDIA_SUFFIX), mec);
    }
    folders_[folder].notes.erase(folders_[folder].notes.begin() + note);
    auto& list = folders_[toFolder].notes;
    const auto at = std::lower_bound(list.begin(), list.end(), moved,
                                     [](const Note& a, const Note& b) { return a.stamp > b.stamp; });
    const int index = (int)(at - list.begin());
    list.insert(at, std::move(moved));
    return index;
}

bool NoteBook::trashFolder(int folder) {
    if (!valid(folder)) return false;
    std::error_code ec;
    const std::filesystem::path bin = root() / ".trash";
    std::filesystem::create_directories(bin, ec);
    const std::filesystem::path p =
        freePath(bin, folders_[folder].name + " " + nowText().substr(0, 10), "");
    std::filesystem::rename(folders_[folder].path, p, ec);
    if (ec) {
        error_ = "cannot move that folder to the trash";
        return false;
    }
    folders_.erase(folders_.begin() + folder);
    return true;
}

bool NoteBook::trashNote(int folder, int note) {
    if (!valid(folder, note)) return false;
    std::error_code ec;
    // under the folder's own name inside the trash, so it can find its way back
    const std::filesystem::path bin = root() / ".trash" / folders_[folder].name;
    std::filesystem::create_directories(bin, ec);
    const std::filesystem::path p =
        freePath(bin, folders_[folder].notes[note].name + " " + nowText().substr(0, 10), ".txt");
    const std::filesystem::path wasMedia = mediaDir(folder, note);
    std::filesystem::rename(folders_[folder].notes[note].path, p, ec);
    if (ec) {
        error_ = "cannot move that note to the trash";
        return false;
    }
    // Its pictures go with it - a note restored without them would be a
    // note that quietly lost something
    if (!wasMedia.empty()) {
        std::error_code mec;
        std::filesystem::rename(wasMedia, p.parent_path() / (p.stem().string() + MEDIA_SUFFIX), mec);
    }
    folders_[folder].notes.erase(folders_[folder].notes.begin() + note);
    return true;
}

void NoteBook::toggleOpen(int folder) {
    if (valid(folder)) folders_[folder].open = !folders_[folder].open;
}

namespace {
// "Aaron Halo loop 2026-09-07" -> name and date, the way the trash names things
void splitTrashName(const std::string& stem, std::string& name, std::string& date) {
    name = stem;
    date.clear();
    if (stem.size() > 11 && stem[stem.size() - 11] == ' ') {
        const std::string tail = stem.substr(stem.size() - 10);
        if (tail.size() == 10 && tail[4] == '-' && tail[7] == '-') {
            name = stem.substr(0, stem.size() - 11);
            date = tail;
        }
    }
}
}  // namespace

std::vector<NoteBook::Trashed> NoteBook::trashed() const {
    std::vector<Trashed> out;
    std::error_code ec;
    const std::filesystem::path bin = root() / ".trash";
    for (const auto& entry : std::filesystem::directory_iterator(bin, ec)) {
        Trashed t;
        t.path = entry.path();
        t.when = stampOf(entry.path(), &t.stamp);
        if (entry.is_directory(ec)) {
            // Either a folder thrown away whole ("Routes 2026-09-07"), or the
            // trash's own shelf for the notes of a folder that still lives
            std::string date;
            splitTrashName(entry.path().filename().string(), t.name, date);
            if (!date.empty()) {
                t.isFolder = true;
                for (const auto& f : std::filesystem::directory_iterator(entry.path(), ec)) {
                    if (f.is_regular_file(ec) && f.path().extension() == ".txt") t.notes++;
                }
                out.push_back(t);
            } else {
                for (const auto& f : std::filesystem::directory_iterator(entry.path(), ec)) {
                    if (!f.is_regular_file(ec) || f.path().extension() != ".txt") continue;
                    Trashed n;
                    n.path = f.path();
                    n.folder = entry.path().filename().string();
                    n.when = stampOf(f.path(), &n.stamp);
                    splitTrashName(f.path().stem().string(), n.name, date);
                    out.push_back(n);
                }
            }
        } else if (entry.is_regular_file(ec) && entry.path().extension() == ".txt") {
            // a loose note at the top of the trash, as older versions left them
            std::string date;
            splitTrashName(entry.path().stem().string(), t.name, date);
            out.push_back(t);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Trashed& a, const Trashed& b) { return a.stamp > b.stamp; });
    return out;
}

bool NoteBook::restore(const Trashed& item) {
    std::error_code ec;
    if (item.isFolder) {
        const std::filesystem::path p = freePath(root(), item.name, "");
        std::filesystem::rename(item.path, p, ec);
        if (ec) {
            error_ = "cannot bring that folder back";
            return false;
        }
    } else {
        const std::string home = item.folder.empty() ? "Restored" : item.folder;
        std::filesystem::create_directories(root() / home, ec);
        const std::filesystem::path p = freePath(root() / home, item.name, ".txt");
        std::filesystem::rename(item.path, p, ec);
        if (ec) {
            error_ = "cannot bring that note back";
            return false;
        }
        // the shelf for that folder may be empty now; an empty one is clutter
        std::filesystem::remove(item.path.parent_path(), ec);   // fails harmlessly if not empty
    }
    load();
    return true;
}

namespace {

// The body, cut into lines with the line ends thrown away. Both halves of
// the bundle work on lines, and they have to agree on what a line is.
std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

std::vector<NoteBook::BundleNote> NoteBook::collect(int folder, int note) const {
    std::vector<BundleNote> out;
    for (int f = 0; f < (int)folders_.size(); f++) {
        if (folder >= 0 && f != folder) continue;
        for (int n = 0; n < (int)folders_[f].notes.size(); n++) {
            if (note >= 0 && n != note) continue;
            BundleNote b;
            b.folder = folders_[f].name;
            b.name = folders_[f].notes[n].name;
            b.body = body(f, n);
            out.push_back(std::move(b));
        }
    }
    return out;
}

std::string NoteBook::writeBundle(const std::vector<BundleNote>& notes) {
    std::string out = std::string(BUNDLE_MAGIC) + "\r\n";
    for (const auto& n : notes) {
        const auto lines = linesOf(n.body);
        out += "NOTE\t" + n.folder + "\t" + n.name + "\t" + std::to_string(lines.size()) + "\r\n";
        for (const auto& line : lines) out += line + "\r\n";
    }
    return out;
}

bool NoteBook::readBundle(const std::string& text, std::vector<BundleNote>& out,
                          std::string& error) {
    out.clear();
    error.clear();
    auto lines = linesOf(text);
    // what Notepad leaves at the front of a file it saved as UTF-8
    if (!lines.empty() && lines[0].rfind("\xEF\xBB\xBF", 0) == 0) lines[0] = lines[0].substr(3);
    if (lines.empty() || lines[0] != BUNDLE_MAGIC) {
        error = "that is not a Bedrock notes file - its first line should read " +
                std::string(BUNDLE_MAGIC);
        return false;
    }
    size_t at = 1;
    while (at < lines.size()) {
        if (lines[at].empty()) { at++; continue; }   // a blank line between notes is fine
        std::vector<std::string> field;
        {
            std::string piece;
            std::istringstream in(lines[at]);
            while (std::getline(in, piece, '\t')) field.push_back(piece);
        }
        if (field.size() != 4 || field[0] != "NOTE") {
            error = "line " + std::to_string(at + 1) + " of that file is not a note heading";
            return false;
        }
        int count = -1;
        try {
            size_t used = 0;
            count = std::stoi(field[3], &used);
            if (used != field[3].size() || count < 0) count = -1;
        } catch (...) {
            count = -1;
        }
        if (count < 0) {
            error = "line " + std::to_string(at + 1) + " does not say how long the note is";
            return false;
        }
        if (at + 1 + (size_t)count > lines.size()) {
            error = "the file stops in the middle of \"" + field[2] + "\"";
            return false;
        }
        BundleNote note;
        note.folder = field[1];
        note.name = field[2];
        for (int i = 0; i < count; i++) {
            note.body += lines[at + 1 + (size_t)i];
            if (i + 1 < count) note.body += "\r\n";
        }
        out.push_back(std::move(note));
        at += 1 + (size_t)count;
    }
    if (out.empty()) {
        error = "there is no note in that file";
        return false;
    }
    return true;
}

int NoteBook::importNotes(const std::vector<BundleNote>& notes) {
    int landed = 0;
    for (const auto& n : notes) {
        const std::string home = sanitise(n.folder.empty() ? "Imported" : n.folder);
        int f = -1;
        for (int i = 0; i < (int)folders_.size(); i++) {
            if (folders_[i].name == home) { f = i; break; }
        }
        if (f < 0) f = addFolder(home);
        if (f < 0) continue;
        const int made = addNote(f, n.name);
        if (made < 0) continue;
        if (!writeBody(f, made, n.body)) continue;
        landed++;
    }
    return landed;
}

std::string NoteBook::body(int folder, int note) const {
    if (!valid(folder, note)) return "";
    std::ifstream in(folders_[folder].notes[note].path, std::ios::binary);
    if (!in.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool NoteBook::writeBody(int folder, int note, const std::string& text) {
    if (!valid(folder, note)) return false;
    Note& n = folders_[folder].notes[note];
    {
        std::ofstream out(n.path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            error_ = "cannot write that note";
            return false;
        }
        out.write(text.data(), (std::streamsize)text.size());
    }
    n.stamp = (std::int64_t)std::time(nullptr);
    n.modified = nowText();
    // the preview follows the text, without re-reading the file
    n.preview.clear();
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) {
            n.preview = line.substr(0, 90);
            break;
        }
    }
    return true;
}

std::filesystem::path NoteBook::mediaDir(int folder, int note, bool make) const {
    if (!valid(folder, note)) return {};
    const Note& n = folders_[folder].notes[note];
    const std::filesystem::path dir = n.path.parent_path() / (n.name + MEDIA_SUFFIX);
    std::error_code ec;
    if (make) {
        std::filesystem::create_directories(dir, ec);
        return ec ? std::filesystem::path() : dir;
    }
    return std::filesystem::is_directory(dir, ec) ? dir : std::filesystem::path();
}

std::vector<NoteBook::Shot> NoteBook::media(int folder, int note) const {
    std::vector<Shot> out;
    const std::filesystem::path dir = mediaDir(folder, note);
    if (dir.empty()) return out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string ext = entry.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") continue;
        Shot shot;
        shot.path = entry.path();
        shot.when = stampOf(entry.path(), &shot.stamp);
        out.push_back(std::move(shot));
    }
    std::sort(out.begin(), out.end(), [](const Shot& a, const Shot& b) { return a.stamp > b.stamp; });
    return out;
}

bool NoteBook::trashShot(const std::filesystem::path& shot) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(shot, ec)) return false;
    const std::filesystem::path bin = root() / ".trash" / "media";
    std::filesystem::create_directories(bin, ec);
    const std::filesystem::path to =
        freePath(bin, shot.stem().string(), shot.extension().string().c_str());
    std::filesystem::rename(shot, to, ec);
    if (ec) {
        error_ = "cannot move that picture to the trash";
        return false;
    }
    return true;
}

}  // namespace SCMiner
