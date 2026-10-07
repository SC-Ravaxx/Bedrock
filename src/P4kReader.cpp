#include "P4kReader.h"

#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace SCMiner {
namespace {

uint16_t u16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t u32(const unsigned char* p) { return (uint32_t)u16(p) | ((uint32_t)u16(p + 2) << 16); }
uint64_t u64(const unsigned char* p) { return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32); }

// One spelling for names: lower case, forward slashes
std::string plain(const char* p, size_t n) {
    std::string s(p, n);
    for (auto& c : s) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
    return s;
}

bool readAt(std::ifstream& in, uint64_t at, size_t n, std::vector<unsigned char>& buf) {
    buf.resize(n);
    in.clear();
    in.seekg((std::streamoff)at);
    in.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)n);
    return (size_t)in.gcount() == n;
}

}  // namespace

bool P4kReader::read(const std::filesystem::path& archive, const std::string& name,
                     std::string& out, std::string& error) {
    out.clear();
    std::ifstream in(archive, std::ios::binary);
    if (!in.is_open()) {
        error = "cannot open " + archive.filename().string();
        return false;
    }
    in.seekg(0, std::ios::end);
    const uint64_t size = (uint64_t)in.tellg();
    if (size < 22) {
        error = archive.filename().string() + " is too short to be an archive";
        return false;
    }

    // The end records: the plain one last, the ZIP64 locator 20 bytes before
    // it, pointing at the ZIP64 record that says where the directory is
    std::vector<unsigned char> tail;
    const size_t tailLen = (size_t)std::min<uint64_t>(size, 128 * 1024);
    if (!readAt(in, size - tailLen, tailLen, tail)) {
        error = "cannot read the end of " + archive.filename().string();
        return false;
    }
    size_t loc = std::string::npos;
    for (size_t i = tailLen - 20; i-- > 0;) {
        if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 6 && tail[i + 3] == 7) {
            loc = i;
            break;
        }
    }
    if (loc == std::string::npos) {
        error = archive.filename().string() + " has no ZIP64 end record";
        return false;
    }
    std::vector<unsigned char> end64;
    if (!readAt(in, u64(&tail[loc + 8]), 56, end64) || u32(end64.data()) != 0x06064b50) {
        error = "the ZIP64 end record of " + archive.filename().string() + " is not where it says";
        return false;
    }
    const uint64_t entries = u64(&end64[32]);
    const uint64_t dirSize = u64(&end64[40]);
    const uint64_t dirStart = u64(&end64[48]);

    // The directory, in pieces: an entry cut by the end of a piece is carried
    // into the next
    const std::string want = plain(name.data(), name.size());
    const size_t PIECE = 16u << 20;
    std::vector<unsigned char> buf;
    std::vector<unsigned char> piece;
    uint64_t readSoFar = 0;
    size_t pos = 0;
    uint64_t seen = 0;
    bool found = false;
    uint16_t flags = 0, method = 0;
    uint64_t packed = 0, unpacked = 0, local = 0;
    while (seen < entries && !found) {
        // at least a whole fixed header and its variable parts must be there
        if (buf.size() - pos < 46 || buf.size() - pos < 46u + u16(&buf[pos + 28]) + u16(&buf[pos + 30]) +
                                                          u16(&buf[pos + 32])) {
            if (readSoFar >= dirSize) {
                error = "the directory of " + archive.filename().string() + " ends early";
                return false;
            }
            const size_t n = (size_t)std::min<uint64_t>(PIECE, dirSize - readSoFar);
            if (!readAt(in, dirStart + readSoFar, n, piece)) {
                error = "cannot read the directory of " + archive.filename().string();
                return false;
            }
            readSoFar += n;
            buf.erase(buf.begin(), buf.begin() + (std::ptrdiff_t)pos);
            buf.insert(buf.end(), piece.begin(), piece.end());
            pos = 0;
            continue;
        }
        const unsigned char* e = &buf[pos];
        if (u32(e) != 0x02014b50) {
            error = "the directory of " + archive.filename().string() + " is not what it should be";
            return false;
        }
        const uint16_t nameLen = u16(e + 28), extraLen = u16(e + 30), commentLen = u16(e + 32);
        if (nameLen == want.size() && plain(reinterpret_cast<const char*>(e + 46), nameLen) == want) {
            found = true;
            flags = u16(e + 8);
            method = u16(e + 10);
            packed = u32(e + 20);
            unpacked = u32(e + 24);
            local = u32(e + 42);
            // ZIP64: what does not fit in 32 bits is in extra field 1, in
            // this order and only when the short field says 0xFFFFFFFF
            const unsigned char* x = e + 46 + nameLen;
            const unsigned char* xEnd = x + extraLen;
            while (x + 4 <= xEnd) {
                const uint16_t id = u16(x), len = u16(x + 2);
                if (id == 1) {
                    const unsigned char* v = x + 4;
                    const unsigned char* vEnd = std::min(v + len, xEnd);
                    if (unpacked == 0xFFFFFFFFu && v + 8 <= vEnd) { unpacked = u64(v); v += 8; }
                    if (packed == 0xFFFFFFFFu && v + 8 <= vEnd) { packed = u64(v); v += 8; }
                    if (local == 0xFFFFFFFFu && v + 8 <= vEnd) { local = u64(v); v += 8; }
                }
                x += 4 + len;
            }
        }
        pos += 46u + nameLen + extraLen + commentLen;
        seen++;
    }
    if (!found) {
        error = name + " is not in " + archive.filename().string();
        return false;
    }
    if (flags & 1) {
        error = name + " is encrypted in " + archive.filename().string();
        return false;
    }
    if (unpacked > (512u << 20) || packed > (512u << 20)) {
        error = name + " is too large to be what was asked for";
        return false;
    }

    // The local header in front of the data has its own name and extra lengths
    std::vector<unsigned char> head;
    if (!readAt(in, local, 30, head) || u32(head.data()) != 0x04034b50) {
        error = "the entry of " + name + " is not where the directory says";
        return false;
    }
    const uint64_t dataAt = local + 30 + u16(&head[26]) + u16(&head[28]);
    std::vector<unsigned char> data;
    if (!readAt(in, dataAt, (size_t)packed, data)) {
        error = "cannot read " + name + " out of " + archive.filename().string();
        return false;
    }
    if (method == 0) {
        out.assign(reinterpret_cast<const char*>(data.data()), data.size());
        return true;
    }
    if (method != 100 && method != 93) {
        error = name + " is packed a way this reader does not know (" + std::to_string(method) + ")";
        return false;
    }
    // A zstd frame begins 28 B5 2F FD; anything else here is encrypted
    // or damaged, and is not handed to the decoder
    if (data.size() < 4 || u32(data.data()) != 0xFD2FB528u) {
        error = name + " does not hold a zstd frame - encrypted?";
        return false;
    }
    out.resize((size_t)unpacked);
    const size_t got = ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(got) || got != out.size()) {
        error = std::string("zstd: ") + (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "the size does not match");
        out.clear();
        return false;
    }
    return true;
}

}  // namespace SCMiner
