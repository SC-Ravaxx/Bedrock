#pragma once
// The game's own files a player keeps - Star Citizen's keybinds, saved
// layouts, game settings and custom characters - and backups of them in
// SC Blackbox's folder and form, so SC Blackbox and Bedrock read each
// other's backups. Plugins reach this through Bedrock's `bedrock.game` and
// `bedrock.backups` (Plugins.h, "access": ["game-files"]).
//
// A file is named by its place under a channel's user folder, with
// backslashes on every system, as the game writes its own paths:
//   client\<n>\Profiles\default\actionmaps.xml    keybinds
//   client\<n>\controls\mappings\<name>.xml       saved layouts
//   client\<n>\Profiles\default\attributes.xml    game settings
//   client\<n>\CustomCharacters\<name>.chf        characters
// Nothing else is read or written, in the game's folder or in a backup.
//
// A backup: <root>\<channel>\<yyyy-MM-dd_HH-mm-ss>[_2..]\backup-info.txt and
// ...\user\<the files>. The root and how many are kept come from SC
// Blackbox's settings.ini, or its defaults.
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace SCMiner {
namespace GameFiles {

enum class Kind { None, Keybinds, Layout, Settings, Character };
// What a name is; None for anything else - another file, a way out of the
// folder, a name no system takes
Kind kindOf(const std::string& rel);

struct File {
    std::string data;
    long long time = 0;   // written, Unix milliseconds
};
using Set = std::map<std::string, File>;   // by name

// ---- the game's folder ------------------------------------------------------
struct Channel {
    std::string name;                     // LIVE, PTU, ...
    std::filesystem::path folder;
    std::vector<std::string> aliases;     // other names of the same folder (a link to it)
    std::filesystem::path user() const { return folder / "user"; }
};
// The channels beside `gameFolder` - one of them, as Bedrock reads its
// Game.log: LIVE, HOTFIX, PTU, EPTU, TECH-PREVIEW, then others by name
std::vector<Channel> channels(const std::filesystem::path& gameFolder);

// The files under a user folder (a channel's, or a backup's), sorted
std::vector<std::string> names(const std::filesystem::path& user);
// Names, sizes and times: a text that changes when a file does
std::string stamp(const std::filesystem::path& user);
Set read(const std::filesystem::path& user);
// When client\0's keybinds were written, Unix ms; 0 without
long long played(const std::filesystem::path& user);
// Files written into a user folder, each whole or not at all; a file kept
// read-only stays so. Nothing is removed. The number written; -1 and `why`
// where one could not be
int write(const std::filesystem::path& user, const std::map<std::string, std::string>& files, std::string& why);

// Whether Star Citizen runs; whether the SC Blackbox app does
bool gameRunning();
bool blackboxRunning();

// SC Blackbox's hashes: `all` of every file but the characters, `keybinds`
// of the keybinds and layouts; `characters` of the characters alone. Each
// the SHA-256 of "<name, lower case>|<SHA-256 of the file>\n" lines sorted
// by name, case aside
struct Hashes {
    std::string all, keybinds, characters;
};
Hashes hashesOf(const Set& files);
std::string sha256(std::string_view bytes);

// ---- the backups ------------------------------------------------------------
struct Where {
    std::filesystem::path root;
    int keep = 30;          // newest kept per channel
    int keepDistinct = 10;  // ...and the newest of each of the last keybind sets
};
// SC Blackbox's settings.ini, else its defaults (Documents\SC Keybind
// Backups; under Linux <documents>/SC Blackbox)
Where where();

struct Backup {
    std::string id, channel, source, reason, restoredFrom, restoredParts, summary, detail, flag;
    std::string allHash, keybindHash;
    long long created = 0;   // Unix ms
    bool kept = false;
    int binds = -1;
    int characters = 0;      // character files in it
    bool counted = false;    // its info counts them: written by Bedrock
};
// A channel's backups, newest first
std::vector<Backup> backups(const std::filesystem::path& root, const std::string& channel);
// ...the files of one; empty and `why` where it cannot be read
Set backupFiles(const std::filesystem::path& root, const std::string& channel, const std::string& id, std::string& why);
// A new one: `info`'s reason, restoredFrom, restoredParts, summary, detail,
// flag, kept and binds; created now, the hashes and counts from the files.
// Its id; "" and `why` where it could not be written
std::string writeBackup(const std::filesystem::path& root, const std::string& channel, const std::filesystem::path& source,
                        const Set& files, const Backup& info, std::string& why);
bool markBackup(const std::filesystem::path& root, const std::string& channel, const std::string& id, bool kept,
                std::string& why);
bool removeBackup(const std::filesystem::path& root, const std::string& channel, const std::string& id, std::string& why);
// A line in the folder's log.txt, SC Blackbox's
void log(const std::filesystem::path& root, const std::string& text);

// Text and path, UTF-8 both ways
std::string textOf(const std::filesystem::path& path);
std::filesystem::path pathOf(const std::string& text);

}  // namespace GameFiles
}  // namespace SCMiner
