#pragma once
// Where the app keeps what belongs to the player: %LOCALAPPDATA%\Bedrock,
// not beside the exe. The program folder can then be replaced, moved or
// deleted at will - an update unzipped into a fresh folder, or the old
// folder deleted - without losing the player's library, kept log lines,
// shopping list, notes or settings.
//
// A file called portable.txt beside the exe keeps them there instead, for a
// USB stick or a folder that is meant to be self-contained.

#include <filesystem>
#include <string>

namespace SCMiner {
namespace AppPaths {

// The folder Bedrock.exe itself sits in
const std::filesystem::path& program();

// Where the player's things live: %LOCALAPPDATA%\Bedrock, or the program
// folder when portable.txt is beside the exe. Made if it is not there.
const std::filesystem::path& data();

// True when portable.txt put the data back beside the exe
bool portable();

// Everything under a folder of the caller's choosing, from this call on.
// The --screenshot run uses it, so that its pictures come from demo data
// and its clicks land in a folder of their own: the hooks that set a window
// up for a picture save exactly like a click does, and would otherwise
// change the player's real files (looking_for.txt and the like).
void useDataFolder(const std::filesystem::path& folder);

// data() / name as a string, for the many callers that open files by name
std::string file(const char* name);

// Once, when the data folder is new: anything an older version left beside
// the exe is COPIED across. Copied, never moved - a half-finished migration
// must not be able to destroy the only copy of someone's library.
// Returns how many things were brought over.
int migrateFromProgramFolder();

}  // namespace AppPaths
}  // namespace SCMiner
