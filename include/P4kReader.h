#pragma once
#include <filesystem>
#include <string>

namespace SCMiner {

// One file out of the game's Data.p4k. The archive is a ZIP64 of 160 GB and
// 1.4 million entries whose directory alone is 460 MB (4.10); the
// directory is streamed in pieces rather than held, and the reading stops at
// the entry asked for. Entries are stored (method 0) or zstd frames (method
// 100, CIG's number for it); an encrypted entry is refused, not guessed at.
// Read only - the game may be running and reading the same file.
class P4kReader {
public:
    // name: the archive's own spelling, "Data\\Localization\\english\\global.ini";
    // case and slash direction do not matter. False with a reason in error.
    static bool read(const std::filesystem::path& archive, const std::string& name,
                     std::string& out, std::string& error);
};

}  // namespace SCMiner
