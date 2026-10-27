#include "PluginHost.h"

#include "MiniJson.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>

namespace SCMiner {
namespace Plugins {

namespace {

std::string readWhole(const std::filesystem::path& file, size_t limit) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::string text;
    char buffer[1 << 15];
    while (in && text.size() <= limit) {
        in.read(buffer, sizeof buffer);
        text.append(buffer, static_cast<size_t>(in.gcount()));
    }
    return text.size() > limit ? std::string() : text;
}

bool writeWhole(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::int64_t writeTime(const std::filesystem::path& file) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

std::string pathText(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

// A file of a plugin, by its path in the folder: plain names, '/' between
// folders, three deep at most, and text alone - .lua, .json, .txt
bool plainFile(const std::string& path) {
    if (path.empty() || path.size() > 120) return false;
    size_t start = 0;
    int depth = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part[0] == '.' || ++depth > 3) return false;
        for (char c : part) {
            const bool fine = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                              c == '-' || c == '.';
            if (!fine) return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    auto ends = [&](const char* tail) {
        const size_t n = std::char_traits<char>::length(tail);
        return path.size() > n && path.compare(path.size() - n, n, tail) == 0;
    };
    return ends(".lua") || ends(".json") || ends(".txt");
}

constexpr size_t PACKAGE_LIMIT = 16u << 20;
constexpr size_t FILES_LIMIT = 200;
constexpr size_t HELD_LIMIT = 200000;

}  // namespace

// ---- what the server offers --------------------------------------------------

bool readIndex(const std::string& json, std::vector<Offer>& offers, std::string& why) {
    offers.clear();
    JsonValue root;
    if (!parseJson(json, root, why)) return false;
    const JsonValue* list = root.find("plugins");
    if (!list || list->kind != JsonValue::Kind::Array) {
        why = "no list of plugins in it";
        return false;
    }
    for (const JsonValue& item : list->items) {
        if (item.kind != JsonValue::Kind::Object) continue;
        Offer o;
        o.id = item.textOr("id");
        o.name = item.textOr("name", o.id);
        o.version = item.textOr("version");
        o.description = item.textOr("description");
        o.needs = item.textOr("bedrock");
        o.url = item.textOr("url");
        if (!plainName(o.id, 32) || o.version.empty() || o.url.empty() || o.url.find(' ') != std::string::npos) continue;
        offers.push_back(o);
    }
    return true;
}

std::string packageUrl(const std::string& indexUrl, const std::string& url) {
    if (url.rfind("https://", 0) == 0) return url;
    if (url.find("://") != std::string::npos || url.empty() || url[0] == '/') return {};
    const size_t slash = indexUrl.rfind('/');
    if (slash == std::string::npos) return {};
    return indexUrl.substr(0, slash + 1) + url;
}

std::string indexUrlBeside(const std::string& updateUrl) {
    if (updateUrl.rfind("https://", 0) != 0) return {};
    const size_t slash = updateUrl.rfind('/');
    if (slash == std::string::npos || slash < 8) return {};
    return updateUrl.substr(0, slash + 1) + "plugins/index.json";
}

// ---- packages ----------------------------------------------------------------

std::string pack(const std::filesystem::path& folder, const Key& seed, std::string& why) {
    JsonValue manifest;
    const std::string pluginJson = readWhole(folder / "plugin.json", 1 << 16);
    if (pluginJson.empty() || !parseJson(pluginJson, manifest, why) || manifest.kind != JsonValue::Kind::Object) {
        why = "plugin.json is missing or no JSON object";
        return {};
    }
    const std::string id = manifest.textOr("id"), version = manifest.textOr("version");
    if (!plainName(id, 32) || version.empty()) {
        why = "plugin.json names no plain id and version";
        return {};
    }
    std::error_code ec;
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string relative = pathText(std::filesystem::relative(it->path(), folder, ec));
        if (relative == "plugin.sig") continue;
        if (!plainFile(relative)) {
            why = relative + " is no plugin file - plain names, .lua, .json or .txt";
            return {};
        }
        files.push_back({relative, it->path()});
    }
    if (ec || files.size() > FILES_LIMIT) {
        why = ec ? "the folder cannot be read" : "too many files";
        return {};
    }
    std::sort(files.begin(), files.end());
    const std::string signature = signatureOf(manifestOf(folder), seed);
    std::string out = "{\"format\":1,\"id\":" + jsonText(id) + ",\"version\":" + jsonText(version) +
                      ",\"signature\":" + jsonText(signature) + ",\"files\":{";
    for (size_t i = 0; i < files.size(); i++) {
        const std::string text = readWhole(files[i].second, PACKAGE_LIMIT);
        if (i) out += ',';
        out += jsonText(files[i].first) + ":" + jsonText(text);
    }
    out += "}}\n";
    if (out.size() > PACKAGE_LIMIT) {
        why = "bigger than 16 MB";
        return {};
    }
    return out;
}

bool unpack(const std::string& package, const std::filesystem::path& root, const std::vector<Key>& keys,
            bool acceptUnsigned, std::string& id, std::string& why) {
    JsonValue p;
    if (package.size() > PACKAGE_LIMIT || !parseJson(package, p, why) || p.kind != JsonValue::Kind::Object ||
        p.numberOr("format") != 1) {
        why = "not a Bedrock plugin";
        return false;
    }
    id = p.textOr("id");
    const std::string signature = p.textOr("signature");
    const JsonValue* files = p.find("files");
    if (!plainName(id, 32) || !files || files->kind != JsonValue::Kind::Object || files->members.empty() ||
        files->members.size() > FILES_LIMIT) {
        why = "not a Bedrock plugin";
        return false;
    }
    std::set<std::string> names;
    for (const auto& [name, text] : files->members) {
        if (!plainFile(name) || text.kind != JsonValue::Kind::String || !names.insert(name).second) {
            why = "it holds a file that is no plugin file";
            return false;
        }
    }
    if (!names.count("plugin.json") || !names.count("main.lua")) {
        why = "it has no plugin.json or main.lua";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const std::filesystem::path staging = root / (".new-" + id);
    std::filesystem::remove_all(staging, ec);
    bool written = true;
    for (const auto& [name, text] : files->members) written = writeWhole(staging / name, text.text) && written;
    written = writeWhole(staging / "plugin.sig", signature + "\n") && written;
    JsonValue own;
    std::string ownWhy;
    const bool named = parseJson(readWhole(staging / "plugin.json", 1 << 16), own, ownWhy) && own.textOr("id") == id;
    const bool trusted = signedBy(manifestOf(staging), signature, keys);
    if (!written || !named || (!trusted && !acceptUnsigned)) {
        std::filesystem::remove_all(staging, ec);
        why = !written ? "it could not be written to the data folder"
              : !named ? "its plugin.json names another plugin"
                       : "it is not signed by Ravaxx";
        return false;
    }
    const std::filesystem::path target = root / id, old = root / (".old-" + id);
    std::filesystem::remove_all(old, ec);
    if (std::filesystem::exists(target, ec)) {
        std::filesystem::rename(target, old, ec);
        if (ec) {
            std::filesystem::remove_all(staging, ec);
            why = "the plugin there before could not be taken out";
            return false;
        }
    }
    std::filesystem::rename(staging, target, ec);
    if (ec) {
        // the one before goes back where it was
        std::error_code back;
        std::filesystem::rename(old, target, back);
        std::filesystem::remove_all(staging, back);
        why = "it could not be put in place";
        return false;
    }
    std::filesystem::remove_all(old, ec);
    return true;
}

// ---- the plugins running -------------------------------------------------------

struct Host::Slot {
    Info info;
    std::unique_ptr<Plugin> plugin;
    // the older logs: the backups still to read, oldest first
    std::vector<std::string> texts;           // what its watches look for in them
    std::vector<std::filesystem::path> backlog;
    size_t next = 0;
    std::int64_t until = 0;                   // the newest backup read, by its write time
    bool historyDone = true;
    std::vector<std::string> held;            // the live log's lines, held back meanwhile
};

Host::Host(std::filesystem::path root, std::filesystem::path stateDir, std::string bedrockVersion,
           std::vector<Key> keys, bool acceptUnsigned)
    : root_(std::move(root)),
      stateDir_(std::move(stateDir)),
      version_(std::move(bedrockVersion)),
      keys_(std::move(keys)),
      acceptUnsigned_(acceptUnsigned) {}

Host::~Host() = default;

std::filesystem::path Host::statePath(const std::string& id, const char* ending) const {
    return stateDir_ / (id + ending);
}

Host::Slot* Host::slot(const std::string& id) {
    for (auto& s : slots_) {
        if (s->info.id == id) return s.get();
    }
    return nullptr;
}

Plugin* Host::plugin(const std::string& id) {
    Slot* s = slot(id);
    return s ? s->plugin.get() : nullptr;
}

void Host::load() {
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    found_ = discover(root_, keys_, acceptUnsigned_, version_);
    // gone from the folder: stopped
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [&](const std::unique_ptr<Slot>& s) {
                                    return std::none_of(found_.begin(), found_.end(),
                                                        [&](const Info& i) { return i.id == s->info.id; });
                                }),
                 slots_.end());
    for (const Info& info : found_) {
        Slot* s = slot(info.id);
        if (!s) {
            slots_.push_back(std::make_unique<Slot>());
            s = slots_.back().get();
            s->info = info;
            startSlot(*s);
        } else if (s->info.version != info.version || s->info.trusted != info.trusted ||
                   s->info.problem != info.problem) {
            s->info = info;
            startSlot(*s);
        }
    }
}

void Host::startSlot(Slot& s) {
    s.plugin.reset();
    s.held.clear();
    s.backlog.clear();
    s.next = 0;
    s.historyDone = true;
    s.plugin = std::make_unique<Plugin>(s.info, statePath(s.info.id, ".json"));
    if (!s.plugin->start()) return;
    s.texts = s.plugin->oldWatches();
    queueHistory(s);
}

bool Host::restart(const std::string& id) {
    Slot* s = slot(id);
    if (!s) return false;
    startSlot(*s);
    return s->plugin && s->plugin->running();
}

bool Host::remove(const std::string& id, std::string& why) {
    if (!plainName(id, 32)) {
        why = "no such plugin";
        return false;
    }
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(),
                                [&](const std::unique_ptr<Slot>& s) { return s->info.id == id; }),
                 slots_.end());
    std::error_code ec;
    std::filesystem::remove_all(root_ / id, ec);
    if (ec) {
        why = "its folder could not be taken away";
        load();
        return false;
    }
    load();
    return true;
}

// The backups newer than the last one read, for the version that read them:
// a new version of a plugin may look for more, and reads them all again
void Host::queueHistory(Slot& s) {
    s.backlog.clear();
    s.next = 0;
    s.until = 0;
    s.historyDone = true;
    if (s.texts.empty() || gameFolder_.empty()) return;
    {
        std::ifstream in(statePath(s.info.id, ".logs"));
        std::string version;
        std::int64_t until = 0;
        if (std::getline(in, version, '\t') && (in >> until) && version == s.info.version) s.until = until;
    }
    std::vector<std::pair<std::int64_t, std::filesystem::path>> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(gameFolder_ / "logbackups", ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".log") continue;
        const std::int64_t t = writeTime(entry.path());
        if (t > s.until) found.push_back({t, entry.path()});
    }
    std::sort(found.begin(), found.end());
    for (auto& f : found) s.backlog.push_back(std::move(f.second));
    s.historyDone = s.backlog.empty();
}

void Host::finishHistory(Slot& s) {
    {
        std::error_code ec;
        std::filesystem::create_directories(stateDir_, ec);
        std::ofstream out(statePath(s.info.id, ".logs"), std::ios::trunc);
        out << s.info.version << '\t' << s.until << '\n';
    }
    s.historyDone = true;
    s.backlog.clear();
    s.next = 0;
    // ...and what the live log said meanwhile, after it
    std::vector<std::string> held;
    held.swap(s.held);
    for (const std::string& line : held) {
        if (!s.plugin || !s.plugin->running()) break;
        s.plugin->logLine(line, false);
    }
}

void Host::setGameFolder(const std::filesystem::path& folder) {
    if (folder == gameFolder_) return;
    gameFolder_ = folder;
    for (auto& s : slots_) {
        if (s->plugin && s->plugin->running() && s->historyDone && s->held.empty()) queueHistory(*s);
    }
}

void Host::liveLine(std::string_view line) {
    for (auto& s : slots_) {
        if (!s->plugin || !s->plugin->running() || !s->plugin->wants(line, false)) continue;
        if (!s->historyDone) {
            if (s->held.size() < HELD_LIMIT) s->held.emplace_back(line);
        } else {
            s->plugin->logLine(line, false);
        }
    }
}

bool Host::tick(int budgetMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    bool took = false;
    for (auto& sp : slots_) {
        Slot& s = *sp;
        while (!s.historyDone && std::chrono::steady_clock::now() < deadline) {
            if (!s.plugin || !s.plugin->running()) {
                s.historyDone = true;
                s.backlog.clear();
                s.held.clear();
                break;
            }
            if (s.next >= s.backlog.size()) {
                finishHistory(s);
                took = true;
                break;
            }
            const std::filesystem::path file = s.backlog[s.next++];
            const std::string data = readWhole(file, size_t(1) << 31);
            size_t pos = 0;
            while (pos < data.size() && s.plugin->running()) {
                size_t nl = data.find('\n', pos);
                if (nl == std::string::npos) nl = data.size();
                const std::string_view line(data.data() + pos, nl - pos);
                pos = nl + 1;
                if (line.empty() || line[0] != '<') continue;
                for (const std::string& text : s.texts) {
                    if (line.find(text) != std::string_view::npos) {
                        s.plugin->logLine(line, true);
                        took = true;
                        break;
                    }
                }
            }
            s.until = (std::max)(s.until, writeTime(file));
            // kept now and then, so a Bedrock closed half way starts there again
            if (s.next % 50 == 0) {
                std::ofstream out(statePath(s.info.id, ".logs"), std::ios::trunc);
                out << s.info.version << '\t' << s.until << '\n';
            }
        }
    }
    return took;
}

bool Host::readingHistory(const std::string& id, int* done, int* all) const {
    for (const auto& s : slots_) {
        if (s->info.id != id) continue;
        if (done) *done = static_cast<int>(s->next);
        if (all) *all = static_cast<int>(s->backlog.size());
        return !s->historyDone;
    }
    return false;
}

std::vector<UiItem> Host::draw(const std::string& id, UiState& state) {
    Slot* s = slot(id);
    if (!s || !s->plugin) return {};
    return s->plugin->draw(state);
}

bool Host::takeChanged(const std::string& id) {
    Slot* s = slot(id);
    return s && s->plugin && s->plugin->takeChanged();
}

bool Host::takeNote(std::string& id, std::string& note) {
    for (auto& s : slots_) {
        if (!s->plugin) continue;
        std::string n = s->plugin->takeNote();
        if (!n.empty()) {
            id = s->info.id;
            note = std::move(n);
            return true;
        }
    }
    return false;
}

}  // namespace Plugins
}  // namespace SCMiner
