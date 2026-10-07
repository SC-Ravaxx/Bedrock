#include "AppPaths.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>

namespace SCMiner {
namespace AppPaths {
namespace {

// Where the running program sits. Windows asks the loader; on Linux the
// kernel keeps the answer in /proc/self/exe, which is a symlink to the
// binary and stays right even when it was started through $PATH or a
// relative path.
std::filesystem::path exeFolder() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, (DWORD)(sizeof(buffer) / sizeof(buffer[0])));
    if (!n) return std::filesystem::current_path();
    return std::filesystem::path(buffer).parent_path();
#else
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || self.empty()) return std::filesystem::current_path();
    return self.parent_path();
#endif
}

// The player's folder, by the convention of the system it runs on:
// %LOCALAPPDATA%\Bedrock on Windows, and on Linux what the XDG base
// directory spec asks for - $XDG_DATA_HOME/bedrock, falling back to
// ~/.local/share/bedrock when the variable is unset, which is what the spec
// itself prescribes.
std::filesystem::path defaultDataFolder() {
#ifdef _WIN32
    const char* local = std::getenv("LOCALAPPDATA");
    if (!local || !*local) return {};
    return std::filesystem::path(local) / "Bedrock";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "bedrock";
    }
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::filesystem::path(home) / ".local" / "share" / "bedrock";
#endif
}

// The four files and two folders an older version left beside the exe.
// owned_blueprints_PTU.txt and its like are picked up by the prefix match.
const char* const KEPT_FILES[] = {
    "region_config.txt",
    "owned_blueprints.txt",
    "blueprint_log_backup.txt",
    "craft_list.txt",
};
const char* const KEPT_FOLDERS[] = {"notes"};

}  // namespace

const std::filesystem::path& program() {
    static const std::filesystem::path here = exeFolder();
    return here;
}

bool portable() {
    static const bool yes = std::filesystem::exists(program() / "portable.txt");
    return yes;
}

namespace {
std::filesystem::path g_chosen;   // see useDataFolder
}

void useDataFolder(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    g_chosen = folder;
}

const std::filesystem::path& data() {
    if (!g_chosen.empty()) return g_chosen;
    static const std::filesystem::path folder = [] {
        if (portable()) return program();
        std::filesystem::path p = defaultDataFolder();
        if (p.empty()) return program();   // no profile: better beside the exe than nowhere
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        if (ec) return program();
        return p;
    }();
    return folder;
}

std::string file(const char* name) {
    return (data() / name).string();
}

int migrateFromProgramFolder() {
    if (portable()) return 0;
    std::error_code ec;
    if (std::filesystem::equivalent(data(), program(), ec)) return 0;

    int brought = 0;
    // Files first. Anything already in the data folder wins: this runs on
    // every start, and a newer library must never be written over by the one
    // an old copy of the app left behind.
    for (const char* name : KEPT_FILES) {
        const std::filesystem::path from = program() / name;
        const std::filesystem::path to = data() / name;
        if (!std::filesystem::exists(from, ec) || std::filesystem::exists(to, ec)) continue;
        std::filesystem::copy_file(from, to, ec);
        if (!ec) brought++;
    }
    // ...and the per-channel library files an older copy may have written
    for (const auto& entry : std::filesystem::directory_iterator(program(), ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("owned_blueprints_", 0) != 0 && name.rfind("blueprint_log_backup_", 0) != 0) {
            continue;
        }
        const std::filesystem::path to = data() / name;
        if (std::filesystem::exists(to, ec)) continue;
        std::filesystem::copy_file(entry.path(), to, ec);
        if (!ec) brought++;
    }
    for (const char* name : KEPT_FOLDERS) {
        const std::filesystem::path from = program() / name;
        const std::filesystem::path to = data() / name;
        if (!std::filesystem::is_directory(from, ec) || std::filesystem::exists(to, ec)) continue;
        std::filesystem::copy(from, to,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::skip_existing,
                              ec);
        if (!ec) brought++;
    }
    return brought;
}

}  // namespace AppPaths
}  // namespace SCMiner
