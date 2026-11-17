#pragma once
// A bug report's debug folder, as a zip: which files of <data>/debug go
// with a message from the Feedback page, the user's name hidden in them, and
// the zip itself.
//
// What goes, in this order, each only where it is:
//   session.txt, region_config.txt     whole; over 1 MiB the last 1 MiB
//   rock_scans.csv, rock_readings.csv  the header line and the last 4 MiB
//   scan_log.csv                       the header line and the last 2 MiB
//   snapshot_*.png                     the three newest, newest first, each
//                                      followed by its .txt
//   panel_<n>.png, panel5_<n>.png      the ten newest cuts of the rock panel
//                                      (PanelKeep.h) by the file's time,
//                                      newest first, each followed by its .txt
// A part from the end always begins at a line's start. Nothing else goes: no
// .old file, no other picture (panel_miss_<n>.png is the whole screen), no
// subfolder. The texts always go; the zip's limit only leaves pictures out.
// Every text (all but the .png) is masked.
//
// The zip follows PKWARE's APPNOTE: stored entries (method 0 - the pictures
// are PNG already, and no library is needed for it), names under debug/ in
// UTF-8, no ZIP64. Free functions, no state.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace SCMiner {
namespace DebugZip {

inline constexpr std::size_t WHOLE_MOST = 1024 * 1024;       // session.txt, region_config.txt, a snapshot's .txt
inline constexpr std::size_t TABLE_TAIL = 4 * 1024 * 1024;   // rock_scans.csv, rock_readings.csv: beside the header
inline constexpr std::size_t LOG_TAIL = 2 * 1024 * 1024;     // scan_log.csv: beside the header
inline constexpr std::size_t SNAPSHOTS = 3;                  // the newest snapshot pairs tried
inline constexpr std::size_t PANEL_CUTS = 10;                // the newest panel cuts tried, with their .txt

// A file of the zip: its name in the debug folder, its bytes as they go, and
// when it was written (Unix seconds; the zip's date for it)
struct Entry {
    std::string name;
    std::string data;
    std::int64_t written = 0;
};

// What to hide
struct Masks {
    std::string home;   // C:\Users\<name>, /home/<name>
    std::string user;   // <name>
};
// This machine's: USERPROFILE and USERNAME on Windows, HOME and USER on
// Linux; "" where one is not set. On Windows in the system's code page, the
// way Bedrock writes its paths (path::string())
Masks masksHere();

// Windows: %USERPROFILE%, any case. Linux: ~, the case as it stands.
enum class Rules { Windows, Linux };
#ifdef _WIN32
inline constexpr Rules RULES_HERE = Rules::Windows;
#else
inline constexpr Rules RULES_HERE = Rules::Linux;
#endif

// `text` with the user hidden:
// 1. the home folder becomes %USERPROFILE% (Windows) or ~ (Linux) - written
//    with \, / or \\ (escaped) between its parts, on Windows in any case of
//    A to Z; not where a letter, a figure, '.', '_' or '-' goes on before or
//    after it (C:\Users\Mining2 is not C:\Users\Mining)
// 2. then the user name, where it is a path part, becomes <user>: right after
//    \ or /, and not followed by a letter, a figure, '.', '_' or '-' - on
//    Windows in any case of A to Z. A word equal to the name outside a path
//    stays: a player called "Mining" keeps his logs readable.
// What it does not catch: the name inside a longer part (Mining.old,
// Mining-PC, Mining_2); the name as the first part of a relative path
// (Mining\notes) or in anything that is no path (a computer's name, a game
// handle); the home folder by another way there (its 8.3 short name
// C:\Users\MININ~1, a junction, a mapped drive) - where its last part is the
// name whole, step 2 still hides it; on Windows, letters beyond A to Z in
// another case than the system's own (JÜRGEN for Jürgen), and the name in
// another encoding than `masks` (pick() also tries the UTF-8 spelling of a
// code-page name); and anything in the pictures, which go as they are.
std::string masked(const std::string& text, const Masks& masks, Rules rules = RULES_HERE);

// The debug folder's files as they go (see above), the texts masked by this
// system's rules. Snapshots: of the three newest - by the time in the name
// (snapshot_YYYYMMDD_HHMMSS.png), else by the file's time - as many as keep
// the zip within `most` bytes, newest first; a pair that would take it over
// is left out, and no older one is tried in its place. The panel's cuts after
// them the same way, of the ten newest.
std::vector<Entry> pick(const std::filesystem::path& debugFolder, const Masks& masks, std::size_t most);

// The zip of `entries`, each under debug/: stored, the UTF-8 flag (bit 11),
// CRC-32, the DOS date and time of `written` in local time, the central
// directory and the end record. The same entries give the same bytes. ""
// where it would need ZIP64: 4 GiB or more, 65,535 entries or more (0xFFFF
// and 0xFFFFFFFF are ZIP64's marks).
std::string zip(const std::vector<Entry>& entries);
// zip(entries).size(), without building it; 0 where zip() gives ""
std::size_t zipSize(const std::vector<Entry>& entries);
// A stored zip read back: the central directory walked, each entry's local
// header checked against it and its CRC against its bytes. Names as they
// stand in the zip (debug/...), `written` from the DOS time (to 2 s). False,
// and `why`, at the first fault - `out` is then empty.
bool unzip(const std::string& zip, std::vector<Entry>& out, std::string& why);

// CRC-32 as zip takes it (IEEE 802.3, reflected, 0xEDB88320)
std::uint32_t crc32(const std::string& data);

}  // namespace DebugZip
}  // namespace SCMiner
