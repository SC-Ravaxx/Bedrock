#pragma once
// The plugins Bedrock has (Plugins.h for one of them): found in the data
// folder's plugins, started, fed the lines of Game.log they watch - those
// of the older logs once, the live log's as they come - and put in and
// taken out again.
//
// Plugins come in through Bedrock alone: the server offers them in a list
// of its own (plugins/index.json beside the update manifest, linked from
// nowhere), and Bedrock fetches a package from it and unpacks it only once
// its signature checks out.
#include "Plugins.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace SCMiner {
namespace Plugins {

// ---- what the server offers ------------------------------------------------
// The index:
//   {"plugins": [{"id": "wikelo", "name": "Wikelo", "version": "1.0.0",
//                 "bedrock": "2.8.0", "description": "...",
//                 "url": "wikelo-1.0.0.bplugin"}]}
// the url whole, or beside the index
struct Offer {
    std::string id, name, version, description;
    std::string needs;   // the oldest Bedrock it runs on; "" any
    std::string url;
};
bool readIndex(const std::string& json, std::vector<Offer>& offers, std::string& why);
// An offer's package address: whole as it stands, else beside the index
std::string packageUrl(const std::string& indexUrl, const std::string& url);
// The index's address beside the update manifest's, "" where there is none
std::string indexUrlBeside(const std::string& updateUrl);

// ---- packages --------------------------------------------------------------
// A plugin in one file - JSON, its files as text and its signature:
//   {"format": 1, "id": "wikelo", "version": "1.0.0", "signature": "<plugin.sig>",
//    "files": {"plugin.json": "...", "main.lua": "...", "data/x.json": "..."}}
// Text files only: .lua, .json and .txt, plain names.
std::string pack(const std::filesystem::path& folder, const std::string& signature, std::string& why);
// ...signed with a seed (test key, self-test)
std::string pack(const std::filesystem::path& folder, const Key& seed, std::string& why);
// Into root/<id>, the plugin there before replaced whole - but only once it
// checks out: its files plain text with plain names, its signature by one of
// `keys` (or none at all with `acceptUnsigned`, for a developer)
bool unpack(const std::string& package, const std::filesystem::path& root, const std::vector<Key>& keys,
            bool acceptUnsigned, std::string& id, std::string& why);

// ---- the plugins running -----------------------------------------------------
class Host {
public:
    Host(std::filesystem::path root, std::filesystem::path stateDir, std::string bedrockVersion,
         std::vector<Key> keys, bool acceptUnsigned);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // The folder looked at again: what is new or changed is started, what
    // is gone stopped
    void load();
    const std::vector<Info>& found() const { return found_; }
    // A plugin found, running or stopped; nullptr where there is none
    Plugin* plugin(const std::string& id);
    // ...started again, after an error stopped it
    bool restart(const std::string& id);
    // ...its folder taken away; what it saved stays, for when it comes back
    bool remove(const std::string& id, std::string& why);

    // The game's folder, whose logbackups hold the older logs
    void setGameFolder(const std::filesystem::path& folder);
    // A line of the live Game.log
    void liveLine(std::string_view line);
    // A while spent on the older logs, `budgetMs` at most; true where a
    // plugin took lines of them
    bool tick(int budgetMs = 8);
    // Whether a plugin still reads the older logs, and how far it is
    bool readingHistory(const std::string& id, int* done = nullptr, int* all = nullptr) const;

    // What a plugin's window shows, the player's doings taken in
    std::vector<UiItem> draw(const std::string& id, UiState& state);
    // Whether its window wants drawing anew since last asked
    bool takeChanged(const std::string& id);
    // A note a plugin left for the foot of the main window
    bool takeNote(std::string& id, std::string& note);

    const std::filesystem::path& root() const { return root_; }
    const std::vector<Key>& keys() const { return keys_; }
    bool acceptsUnsigned() const { return acceptUnsigned_; }

private:
    struct Slot;
    Slot* slot(const std::string& id);
    void startSlot(Slot& s);
    void queueHistory(Slot& s);
    void finishHistory(Slot& s);
    std::filesystem::path statePath(const std::string& id, const char* ending) const;

    std::filesystem::path root_, stateDir_, gameFolder_;
    std::string version_;
    std::vector<Key> keys_;
    bool acceptUnsigned_ = false;
    std::vector<Info> found_;
    std::vector<std::unique_ptr<Slot>> slots_;
};

}  // namespace Plugins
}  // namespace SCMiner
