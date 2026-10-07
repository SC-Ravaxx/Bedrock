#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace SCMiner {

// Everything the player has made, in one file they choose the place for.
//
// The notebook can already travel on its own (NoteBook::writeBundle), but
// the shopping list, the ores being watched for, the blueprint library and
// the few hundred log lines it was read from are just as hard to make again
// - and they all live under %LOCALAPPDATA%\Bedrock, which is the right
// place to keep them and the wrong place to ask anybody to go.
//
// The format, for the same reasons the note bundle has its own:
//
//   BEDROCK BACKUP 1
//   FILE<tab>notes/Routes/Monox run.txt<tab>412
//   ...412 bytes, exactly as they lie on disk...
//   FILE<tab>craft_list.txt<tab>193
//   ...
//
// BYTE counts, not line counts, and everything is read and written in
// binary: nothing inside a file is ever looked at, so a note that talks
// about this format, a file with no line ends at all, or one that ends
// mid-line comes back byte for byte. The path is always written with
// forward slashes, so a backup made on one machine reads on another.
//
// The file ends in .bedrock, not .txt: it holds a note's pictures byte for
// byte, and a .txt opens in an editor on a double click, where saving it
// once breaks them. The ending is not checked, so a backup named .txt
// restores too.
namespace DataBackup {

inline constexpr const char* MAGIC = "BEDROCK BACKUP 1";

// Where the current data goes when a backup is restored over it. Nothing is
// ever erased: a restore of the wrong file would otherwise cost somebody
// every note they have, and an undo dialog is not something anyone trusts.
inline constexpr const char* ASIDE = "previous";

struct Entry {
    std::string path;    // relative to the data folder, forward slashes
    std::string bytes;   // the file, as it is
};

// Whether a file belongs in a backup, by its path in the data folder.
// Not `debug` (snapshots and logs - pictures of a moment, and megabytes of
// them), `update` (the staging the updater uses) or `previous` (what
// earlier restores put aside); and at the top, not Bedrock's copy of the
// game's texts (game_text_*, read again from Data.p4k whenever it is
// wanted) or a loose picture - a note's pictures live in its folder, and
// one lying at the top is a developer tool's output. Those three turn half
// a megabyte of data into a 30 MB backup.
bool carried(const std::string& path);

// What is worth keeping, read off disk - every file carried() takes.
std::vector<Entry> collect(const std::filesystem::path& dataFolder);

std::string write(const std::vector<Entry>& entries);

// Structure only. On false, `error` is a sentence saying what is wrong.
bool read(const std::string& text, std::vector<Entry>& out, std::string& error);

// A path a backup may write to: relative, no drive, no "..", no leading
// slash, nothing empty. A backup is a file somebody can be handed, so it
// is treated as something that might be trying it on.
bool safePath(const std::string& path);

// Moves what is in the data folder into <dataFolder>/previous/<stamp> and
// writes the backup in its place. `asideName` comes back as the stamp, so
// the window can say where the old data went. Every path is checked first;
// one bad path refuses the whole restore. What carried() leaves out is left
// out both ways: a backup from before that still holds the game's texts or
// a loose picture does not write them back, and the copies here now stay
// where they are rather than being put aside.
bool restore(const std::filesystem::path& dataFolder, const std::vector<Entry>& entries,
             std::string& asideName, std::string& error);

}  // namespace DataBackup
}  // namespace SCMiner
