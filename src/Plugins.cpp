#include "Plugins.h"

#include "MiniJson.h"
#include "PluginKeys.h"
#include "Reputation.h"

// Lua is compiled as C++ here (third_party/lua/LICENSE.txt): no extern "C"
#include "../third_party/lua/lauxlib.h"
#include "../third_party/lua/lua.h"
#include "../third_party/lua/lualib.h"
#include "../third_party/monocypher/monocypher-ed25519.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

namespace SCMiner {
namespace Plugins {

namespace {

// What a signature covers: the manifest, with this in front, so a signature
// of anything else Bedrock may come to sign can never pass for a plugin's
constexpr const char* SIGNED_PREFIX = "Bedrock plugin 1\n";

std::string hexOf(const unsigned char* bytes, size_t size) {
    static const char* figures = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; i++) {
        out += figures[bytes[i] >> 4];
        out += figures[bytes[i] & 15];
    }
    return out;
}


// A file's SHA-512 in hex; "" where it cannot be read
std::string sha512OfFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    std::vector<char> buffer(1 << 16);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) crypto_sha512_update(&ctx, reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(got));
    }
    if (in.bad()) return {};
    uint8_t hash[64];
    crypto_sha512_final(&ctx, hash);
    return hexOf(hash, sizeof hash);
}

std::string readText(const std::filesystem::path& file, size_t limit = 16u << 20) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::string text;
    char buffer[1 << 14];
    while (in && text.size() <= limit) {
        in.read(buffer, sizeof buffer);
        text.append(buffer, static_cast<size_t>(in.gcount()));
    }
    return text.size() > limit ? std::string() : text;
}

std::string pathText(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

// "2.10.1" against "2.9.0": the parts as numbers, from the left
bool olderVersion(const std::string& version, const std::string& needed) {
    auto parts = [](const std::string& v) {
        std::vector<int> out;
        int n = 0;
        bool any = false;
        for (char c : v) {
            if (c >= '0' && c <= '9') {
                n = n * 10 + (c - '0');
                any = true;
            } else if (c == '.') {
                out.push_back(any ? n : 0);
                n = 0;
                any = false;
            } else {
                break;
            }
        }
        out.push_back(any ? n : 0);
        return out;
    };
    const std::vector<int> a = parts(version), b = parts(needed);
    for (size_t i = 0; i < (std::max)(a.size(), b.size()); i++) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y;
    }
    return false;
}

bool plainName(const std::string& name, size_t longest) {
    if (name.empty() || name.size() > longest) return false;
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}

namespace {

// ---- JSON and Lua, both ways -----------------------------------------------

void pushJson(lua_State* L, const JsonValue& v, int depth) {
    luaL_checkstack(L, 4, "too deep");
    if (depth > 64) {
        lua_pushnil(L);
        return;
    }
    switch (v.kind) {
        case JsonValue::Kind::Null: lua_pushnil(L); break;
        case JsonValue::Kind::Bool: lua_pushboolean(L, v.boolean); break;
        case JsonValue::Kind::Number:
            if (std::isfinite(v.number) && v.number == std::floor(v.number) && std::fabs(v.number) < 9.0e15) {
                lua_pushinteger(L, static_cast<lua_Integer>(v.number));
            } else {
                lua_pushnumber(L, v.number);
            }
            break;
        case JsonValue::Kind::String: lua_pushlstring(L, v.text.data(), v.text.size()); break;
        case JsonValue::Kind::Array:
            lua_createtable(L, static_cast<int>(v.items.size()), 0);
            for (size_t i = 0; i < v.items.size(); i++) {
                pushJson(L, v.items[i], depth + 1);
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            break;
        case JsonValue::Kind::Object:
            lua_createtable(L, 0, static_cast<int>(v.members.size()));
            for (const auto& [key, value] : v.members) {
                pushJson(L, value, depth + 1);
                lua_setfield(L, -2, key.c_str());
            }
            break;
    }
}

void quoted(std::string& out, const char* s, size_t n) {
    out += '"';
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

// The value at `index` as JSON: a table with the keys 1 to n alone is an
// array, any other an object (number keys written as text). False, and
// `why`, for what JSON has no word for - a function, a table in itself.
bool toJson(lua_State* L, int index, std::string& out, int depth, std::string& why) {
    if (depth > 64) {
        why = "nested too deep, or a table in itself";
        return false;
    }
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
        case LUA_TNIL: out += "null"; return true;
        case LUA_TBOOLEAN: out += lua_toboolean(L, index) ? "true" : "false"; return true;
        case LUA_TNUMBER: {
            if (lua_isinteger(L, index)) {
                out += std::to_string(lua_tointeger(L, index));
            } else {
                const double d = lua_tonumber(L, index);
                if (!std::isfinite(d)) {
                    out += "null";
                } else {
                    char buffer[40];
                    std::snprintf(buffer, sizeof buffer, "%.17g", d);
                    out += buffer;
                }
            }
            return true;
        }
        case LUA_TSTRING: {
            size_t n = 0;
            const char* s = lua_tolstring(L, index, &n);
            quoted(out, s, n);
            return true;
        }
        case LUA_TTABLE: {
            luaL_checkstack(L, 6, "too deep");
            const lua_Integer length = static_cast<lua_Integer>(lua_rawlen(L, index));
            lua_Integer keys = 0;
            bool array = true;
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                keys++;
                if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > length) array = false;
                lua_pop(L, 1);
            }
            if (array && keys == length) {
                out += '[';
                for (lua_Integer i = 1; i <= length; i++) {
                    if (i > 1) out += ',';
                    lua_rawgeti(L, index, i);
                    const bool ok = toJson(L, -1, out, depth + 1, why);
                    lua_pop(L, 1);
                    if (!ok) return false;
                }
                out += ']';
                return true;
            }
            out += '{';
            bool first = true;
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    size_t n = 0;
                    const char* s = lua_tolstring(L, -2, &n);
                    key.assign(s, n);
                } else if (lua_isinteger(L, -2)) {
                    key = std::to_string(lua_tointeger(L, -2));
                } else {
                    lua_pop(L, 2);
                    why = "a table key that is no text and no whole number";
                    return false;
                }
                if (!first) out += ',';
                first = false;
                quoted(out, key.data(), key.size());
                out += ':';
                if (!toJson(L, -1, out, depth + 1, why)) {
                    lua_pop(L, 2);
                    return false;
                }
                lua_pop(L, 1);
            }
            out += '}';
            return true;
        }
        default:
            why = std::string("a ") + lua_typename(L, lua_type(L, index)) + ", which cannot be saved";
            return false;
    }
}

}  // namespace

std::string jsonText(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    quoted(out, text.data(), text.size());
    return out;
}

// ---- keys and signatures -----------------------------------------------------

namespace {

constexpr const char* SIG_NAMESPACE = "bedrock-plugin";
constexpr const char* SIG_BEGIN = "-----BEGIN SSH SIGNATURE-----";
constexpr const char* SIG_END = "-----END SSH SIGNATURE-----";

std::string base64Of(std::string_view bytes) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const size_t n = std::min<size_t>(3, bytes.size() - i);
        uint32_t v = (uint32_t)(uint8_t)bytes[i] << 16;
        if (n > 1) v |= (uint32_t)(uint8_t)bytes[i + 1] << 8;
        if (n > 2) v |= (uint8_t)bytes[i + 2];
        out += abc[v >> 18 & 63];
        out += abc[v >> 12 & 63];
        out += n > 1 ? abc[v >> 6 & 63] : '=';
        out += n > 2 ? abc[v & 63] : '=';
    }
    return out;
}

bool bytesOfBase64(std::string_view text, std::string& out) {
    out.clear();
    uint32_t v = 0;
    int bits = 0, pad = 0;
    for (char c : text) {
        if (std::isspace((unsigned char)c)) continue;
        if (c == '=') {
            pad++;
            continue;
        }
        const int d = c >= 'A' && c <= 'Z'   ? c - 'A'
                      : c >= 'a' && c <= 'z' ? c - 'a' + 26
                      : c >= '0' && c <= '9' ? c - '0' + 52
                      : c == '+'             ? 62
                      : c == '/'             ? 63
                                             : -1;
        if (d < 0 || pad) return false;
        v = v << 6 | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)(v >> bits & 0xFF);
        }
    }
    return pad <= 2;
}

// SSH's string: uint32 length, big-endian, then the bytes
void putString(std::string& out, std::string_view s) {
    const uint32_t n = (uint32_t)s.size();
    out += (char)(n >> 24);
    out += (char)(n >> 16 & 0xFF);
    out += (char)(n >> 8 & 0xFF);
    out += (char)(n & 0xFF);
    out.append(s.data(), s.size());
}

bool takeString(std::string_view& in, std::string_view& s) {
    if (in.size() < 4) return false;
    const uint32_t n = (uint32_t)(uint8_t)in[0] << 24 | (uint32_t)(uint8_t)in[1] << 16 | (uint32_t)(uint8_t)in[2] << 8 |
                       (uint32_t)(uint8_t)in[3];
    if (in.size() - 4 < n) return false;
    s = in.substr(4, n);
    in.remove_prefix(4 + (size_t)n);
    return true;
}

// What the key signs (PROTOCOL.sshsig)
std::string sshsigData(std::string_view ns, std::string_view reserved, const std::string& message) {
    uint8_t hash[64];
    crypto_sha512(hash, reinterpret_cast<const uint8_t*>(message.data()), message.size());
    std::string data = "SSHSIG";
    putString(data, ns);
    putString(data, reserved);
    putString(data, "sha512");
    putString(data, std::string_view(reinterpret_cast<const char*>(hash), sizeof hash));
    return data;
}

}  // namespace

Key testSeed() {
    uint8_t hash[64];
    crypto_sha512(hash, reinterpret_cast<const uint8_t*>(PLUGIN_TEST_SEED_TEXT), std::strlen(PLUGIN_TEST_SEED_TEXT));
    Key seed{};
    std::memcpy(seed.data(), hash, seed.size());
    return seed;
}

Key publicKeyOf(const Key& seed) {
    uint8_t copy[32], secret[64];
    Key pub{};
    std::memcpy(copy, seed.data(), sizeof copy);   // the key pair wipes the seed it is given
    crypto_ed25519_key_pair(secret, pub.data(), copy);
    crypto_wipe(secret, sizeof secret);
    return pub;
}

std::vector<Key> trustedKeys() {
    std::vector<Key> keys = RAVAXX_PLUGIN_KEYS;
#ifdef BEDROCK_DEV_TOOLS
    keys.push_back(publicKeyOf(testSeed()));
#endif
    return keys;
}

std::string manifestOf(const std::filesystem::path& folder) {
    std::error_code ec;
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string relative = pathText(std::filesystem::relative(it->path(), folder, ec));
        if (ec || relative.empty() || relative == "plugin.sig") continue;
        files.push_back({relative, it->path()});
    }
    if (ec) return {};
    std::sort(files.begin(), files.end());
    std::string manifest;
    for (const auto& [relative, file] : files) {
        const std::string hash = sha512OfFile(file);
        if (hash.empty()) return {};
        manifest += hash + "  " + relative + "\n";
    }
    return manifest;
}

std::string signedText(const std::string& manifest) { return SIGNED_PREFIX + manifest; }

bool signedBy(const std::string& manifest, const std::string& signature, const std::vector<Key>& keys) {
    if (manifest.empty()) return false;
    const size_t begin = signature.find(SIG_BEGIN), end = signature.find(SIG_END);
    if (begin == std::string::npos || end == std::string::npos || end < begin) return false;
    const size_t from = begin + std::strlen(SIG_BEGIN);
    std::string blob;
    if (!bytesOfBase64(std::string_view(signature).substr(from, end - from), blob)) return false;
    std::string_view in(blob), pub, ns, reserved, hash, sig, keyType, keyBytes, sigType, sigBytes;
    if (in.size() < 10 || in.substr(0, 6) != "SSHSIG" || in.substr(6, 4) != std::string_view("\0\0\0\1", 4)) return false;
    in.remove_prefix(10);
    if (!takeString(in, pub) || !takeString(in, ns) || !takeString(in, reserved) || !takeString(in, hash) ||
        !takeString(in, sig) || !in.empty() || ns != SIG_NAMESPACE || hash != "sha512") {
        return false;
    }
    if (!takeString(pub, keyType) || !takeString(pub, keyBytes) || !pub.empty() || keyType != "ssh-ed25519" ||
        keyBytes.size() != 32 || !takeString(sig, sigType) || !takeString(sig, sigBytes) || !sig.empty() ||
        sigType != "ssh-ed25519" || sigBytes.size() != 64) {
        return false;
    }
    Key key{};
    std::memcpy(key.data(), keyBytes.data(), key.size());
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) return false;
    const std::string data = sshsigData(ns, reserved, signedText(manifest));
    return crypto_ed25519_check(reinterpret_cast<const uint8_t*>(sigBytes.data()), key.data(),
                                reinterpret_cast<const uint8_t*>(data.data()), data.size()) == 0;
}

std::string signatureOf(const std::string& manifest, const Key& seed) {
    uint8_t copy[32], secret[64], pub[32], signature[64];
    std::memcpy(copy, seed.data(), sizeof copy);
    crypto_ed25519_key_pair(secret, pub, copy);
    const std::string data = sshsigData(SIG_NAMESPACE, "", signedText(manifest));
    crypto_ed25519_sign(signature, secret, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    crypto_wipe(secret, sizeof secret);
    std::string keyBlob, sigBlob, blob = std::string("SSHSIG") + std::string("\0\0\0\1", 4);
    putString(keyBlob, "ssh-ed25519");
    putString(keyBlob, std::string_view(reinterpret_cast<const char*>(pub), sizeof pub));
    putString(sigBlob, "ssh-ed25519");
    putString(sigBlob, std::string_view(reinterpret_cast<const char*>(signature), sizeof signature));
    putString(blob, keyBlob);
    putString(blob, SIG_NAMESPACE);
    putString(blob, "");
    putString(blob, "sha512");
    putString(blob, sigBlob);
    const std::string text = base64Of(blob);
    std::string out = std::string(SIG_BEGIN) + "\n";
    for (size_t i = 0; i < text.size(); i += 70) out += text.substr(i, 70) + "\n";
    return out + SIG_END + "\n";
}

// ---- the folders -------------------------------------------------------------

std::vector<Info> discover(const std::filesystem::path& root, const std::vector<Key>& keys, bool acceptUnsigned,
                           const std::string& bedrockVersion) {
    std::vector<Info> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        if (pathText(entry.path().filename()).rfind('.', 0) == 0) continue;
        Info info;
        info.folder = entry.path();
        info.id = pathText(entry.path().filename());
        JsonValue json;
        std::string why;
        const std::string text = readText(entry.path() / "plugin.json", 1 << 16);
        if (!plainName(info.id, 32) || text.empty() || !parseJson(text, json, why) || json.kind != JsonValue::Kind::Object ||
            json.textOr("id") != info.id) {
            info.name = info.id;
            info.problem = "BROKEN - plugin.json missing or wrong";
            found.push_back(info);
            continue;
        }
        info.name = json.textOr("name", info.id);
        info.version = json.textOr("version");
        info.description = json.textOr("description");
        info.needs = json.textOr("bedrock");
        const std::string signature = readText(entry.path() / "plugin.sig", 4096);
        info.trusted = !signature.empty() && signedBy(manifestOf(entry.path()), signature, keys);
        if (!info.trusted && !acceptUnsigned) {
            info.problem = signature.empty() ? "NOT SIGNED - Install it again"
                                             : "FILES CHANGED - Install it again";
        } else if (!info.needs.empty() && olderVersion(bedrockVersion, info.needs)) {
            info.problem = "NEEDS BEDROCK " + info.needs + " - Update Bedrock to run it";
        }
        found.push_back(info);
    }
    std::sort(found.begin(), found.end(), [](const Info& a, const Info& b) { return a.id < b.id; });
    return found;
}

// ---- a plugin running --------------------------------------------------------

// The `bedrock` table and the window's `ui`: functions the plugin calls,
// each finding its plugin through the Lua's extra space
struct Api {
    static Plugin* of(lua_State* L) { return *static_cast<Plugin**>(lua_getextraspace(L)); }

    static void hook(lua_State* L, lua_Debug*) {
        Plugin* p = of(L);
        if (p && std::chrono::steady_clock::now() > p->deadline_) luaL_error(L, "took too long - it is stopped");
    }

    static int print(lua_State* L) {
        std::string line;
        const int n = lua_gettop(L);
        for (int i = 1; i <= n; i++) {
            size_t size = 0;
            const char* s = luaL_tolstring(L, i, &size);
            if (i > 1) line += ' ';
            line.append(s, size);
            lua_pop(L, 1);
        }
        std::cout << "\nPlugin " << of(L)->info_.id << ": " << line << std::endl;
        return 0;
    }

    static int data(lua_State* L) {
        Plugin* p = of(L);
        const std::string name = luaL_checkstring(L, 1);
        if (!plainName(name, 64)) return luaL_error(L, "bedrock.data: '%s' is no plain name", name.c_str());
        const std::string text = readText(p->info_.folder / "data" / (name + ".json"));
        JsonValue json;
        std::string why;
        if (text.empty() || !parseJson(text, json, why)) {
            return luaL_error(L, "bedrock.data: data/%s.json %s", name.c_str(), text.empty() ? "is missing" : why.c_str());
        }
        pushJson(L, json, 0);
        return 1;
    }

    static int load(lua_State* L) {
        Plugin* p = of(L);
        const std::string text = readText(p->stateFile_);
        JsonValue json;
        std::string why;
        if (text.empty() || !parseJson(text, json, why)) {
            lua_pushnil(L);
            return 1;
        }
        pushJson(L, json, 0);
        return 1;
    }

    static int save(lua_State* L) {
        Plugin* p = of(L);
        luaL_checktype(L, 1, LUA_TTABLE);
        std::string json, why;
        if (!toJson(L, 1, json, 0, why)) return luaL_error(L, "bedrock.save: %s", why.c_str());
        if (json.size() > (4u << 20)) return luaL_error(L, "bedrock.save: more than 4 MB");
        std::error_code ec;
        std::filesystem::create_directories(p->stateFile_.parent_path(), ec);
        std::filesystem::path partial = p->stateFile_;
        partial += ".new";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out.write(json.data(), static_cast<std::streamsize>(json.size()));
            if (!out.good()) return luaL_error(L, "bedrock.save: could not be written");
        }
        std::filesystem::rename(partial, p->stateFile_, ec);
        if (ec) return luaL_error(L, "bedrock.save: could not be written");
        return 0;
    }

    static int watch(lua_State* L) {
        Plugin* p = of(L);
        const std::string text = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        const bool old = lua_toboolean(L, 3) != 0;
        if (text.size() < 4) return luaL_error(L, "bedrock.watch: say what to watch for in four letters or more");
        if (p->watches_.size() >= 32) return luaL_error(L, "bedrock.watch: 32 watches at most");
        lua_pushvalue(L, 2);
        Plugin::Watch w;
        w.text = text;
        w.function = luaL_ref(L, LUA_REGISTRYINDEX);
        w.old = old;
        p->watches_.push_back(std::move(w));
        return 0;
    }

    static int now(lua_State* L) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        lua_pushinteger(L, static_cast<lua_Integer>(ms));
        return 1;
    }

    // "<2026-10-19T15:42:02.986Z> ..." - the stamp a log line starts with,
    // as Unix milliseconds
    static int time(lua_State* L) {
        size_t n = 0;
        const char* s = luaL_checklstring(L, 1, &n);
        std::string text(s, (std::min)(n, size_t(40)));
        if (!text.empty() && text[0] == '<') text.erase(0, 1);
        const long long seconds = Reputation::epochOf(text);
        if (seconds < 0) {
            lua_pushnil(L);
            return 1;
        }
        long long ms = 0;
        if (text.size() >= 23 && text[19] == '.' && std::isdigit((unsigned char)text[20]) &&
            std::isdigit((unsigned char)text[21]) && std::isdigit((unsigned char)text[22])) {
            ms = (text[20] - '0') * 100 + (text[21] - '0') * 10 + (text[22] - '0');
        }
        lua_pushinteger(L, static_cast<lua_Integer>(seconds * 1000 + ms));
        return 1;
    }

    // Unix milliseconds in the player's own time, the way Bedrock writes a day
    static int date(lua_State* L) {
        const std::time_t t = static_cast<std::time_t>(luaL_checkinteger(L, 1) / 1000);
        std::tm local{};
#ifdef _WIN32
        if (localtime_s(&local, &t) != 0) return luaL_error(L, "bedrock.date: no such time");
#else
        if (!localtime_r(&t, &local)) return luaL_error(L, "bedrock.date: no such time");
#endif
        static const char* const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char text[32];
        std::snprintf(text, sizeof text, "%d %s %02d:%02d", local.tm_mday, MONTHS[local.tm_mon % 12], local.tm_hour,
                      local.tm_min);
        lua_pushstring(L, text);
        return 1;
    }

    static int note(lua_State* L) {
        of(L)->note_ = luaL_checkstring(L, 1);
        return 0;
    }

    static int copy(lua_State* L) {
        Plugin* p = of(L);
        if (!p->drawing_) return luaL_error(L, "bedrock.copy: from a click in the window only");
        size_t n = 0;
        const char* s = luaL_checklstring(L, 1, &n);
        p->copy_.assign(s, (std::min)(n, size_t(64) << 10));
        return 0;
    }

    static int redraw(lua_State* L) {
        of(L)->changed_ = true;
        return 0;
    }

    static int require(lua_State* L) {
        Plugin* p = of(L);
        const std::string name = luaL_checkstring(L, 1);
        if (!plainName(name, 64)) return luaL_error(L, "bedrock.require: '%s' is no plain name", name.c_str());
        lua_getfield(L, LUA_REGISTRYINDEX, "bedrock.modules");
        lua_getfield(L, -1, name.c_str());
        if (!lua_isnil(L, -1)) return 1;
        lua_pop(L, 1);
        const std::string text = readText(p->info_.folder / (name + ".lua"));
        if (text.empty()) return luaL_error(L, "bedrock.require: %s.lua is missing", name.c_str());
        const std::string chunk = "@" + name + ".lua";
        if (luaL_loadbufferx(L, text.data(), text.size(), chunk.c_str(), "t") != LUA_OK) return lua_error(L);
        lua_call(L, 0, 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name.c_str());
        return 1;
    }

    // ---- ui ----
    static UiItem& add(lua_State* L, UiItem::Kind kind) {
        Plugin* p = of(L);
        if (!p->items_ || !p->drawing_) luaL_error(L, "the window is drawn in draw(ui) only");
        if (p->items_->size() >= 20000) luaL_error(L, "more than 20000 pieces in one window");
        p->items_->push_back(UiItem());
        p->items_->back().kind = kind;
        return p->items_->back();
    }

    static UiItem::Tone toneOf(lua_State* L, int index) {
        const char* t = lua_isstring(L, index) ? lua_tostring(L, index) : "";
        if (!std::strcmp(t, "good")) return UiItem::Tone::Good;
        if (!std::strcmp(t, "warn")) return UiItem::Tone::Warn;
        if (!std::strcmp(t, "bad")) return UiItem::Tone::Bad;
        if (!std::strcmp(t, "dim")) return UiItem::Tone::Dim;
        return UiItem::Tone::Plain;
    }

    static std::vector<std::string> labels(lua_State* L, int index) {
        luaL_checktype(L, index, LUA_TTABLE);
        std::vector<std::string> out;
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, index));
        for (lua_Integer i = 1; i <= n && i <= 64; i++) {
            lua_rawgeti(L, index, i);
            out.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
            lua_pop(L, 1);
        }
        return out;
    }

    static int heading(lua_State* L) {
        add(L, UiItem::Kind::Heading).text = luaL_checkstring(L, 1);
        return 0;
    }

    static int text(lua_State* L) {
        UiItem& item = add(L, UiItem::Kind::Text);
        item.text = luaL_checkstring(L, 1);
        item.tone = toneOf(L, 2);
        return 0;
    }

    static int picker(lua_State* L, UiItem::Kind kind) {
        const std::string id = luaL_checkstring(L, 1);
        std::vector<std::string> options = labels(L, 2);
        int pick = static_cast<int>(luaL_optinteger(L, 3, 1));
        UiItem& item = add(L, kind);
        const auto& picks = of(L)->drawing_->picks;
        const auto it = picks.find(id);
        if (it != picks.end()) pick = it->second;
        pick = (std::max)(1, (std::min)(pick, static_cast<int>(options.size())));
        item.id = id;
        item.options = std::move(options);
        item.picked = pick;
        lua_pushinteger(L, pick);
        return 1;
    }
    static int tabs(lua_State* L) { return picker(L, UiItem::Kind::Tabs); }
    static int chips(lua_State* L) { return picker(L, UiItem::Kind::Chips); }

    static int search(lua_State* L) {
        const std::string id = luaL_checkstring(L, 1);
        UiItem& item = add(L, UiItem::Kind::Search);
        item.id = id;
        item.value = luaL_optstring(L, 2, "");
        const auto& typed = of(L)->drawing_->typed;
        const auto it = typed.find(id);
        item.text = it != typed.end() ? it->second : std::string();
        lua_pushlstring(L, item.text.data(), item.text.size());
        return 1;
    }

    static int card(lua_State* L) {
        const std::string id = luaL_checkstring(L, 1);
        UiItem& item = add(L, UiItem::Kind::Card);
        item.id = id;
        item.text = luaL_checkstring(L, 2);
        if (lua_istable(L, 3)) {
            lua_getfield(L, 3, "subtitle");
            if (lua_isstring(L, -1)) item.value = lua_tostring(L, -1);
            lua_getfield(L, 3, "chip");
            if (lua_isstring(L, -1)) item.chip = lua_tostring(L, -1);
            lua_getfield(L, 3, "tone");
            item.tone = toneOf(L, -1);
            lua_pop(L, 3);
        }
        item.open = of(L)->drawing_->open.count(id) > 0;
        lua_pushboolean(L, item.open);
        return 1;
    }

    static int close(lua_State* L) {
        add(L, UiItem::Kind::CardEnd);
        return 0;
    }

    static int row(lua_State* L) {
        UiItem& item = add(L, UiItem::Kind::Row);
        item.text = luaL_checkstring(L, 1);
        item.value = luaL_optstring(L, 2, "");
        item.tone = toneOf(L, 3);
        return 0;
    }

    static int counter(lua_State* L) {
        Plugin* p = of(L);
        const std::string id = luaL_checkstring(L, 1);
        const std::string label = luaL_checkstring(L, 2);
        lua_Integer value = luaL_checkinteger(L, 3);
        UiState& state = *p->drawing_;
        if (state.clicked == id + ":-") value = (std::max<lua_Integer>)(0, value - 1);
        if (state.clicked == id + ":+") value = (std::min<lua_Integer>)(999999, value + 1);
        const auto typed = state.numbers.find(id);
        if (typed != state.numbers.end()) value = (std::max)(0, (std::min)(typed->second, 999999));
        UiItem& item = add(L, UiItem::Kind::Counter);
        item.id = id;
        item.text = label;
        item.value = std::to_string(value);
        item.tone = toneOf(L, 4);
        lua_pushinteger(L, value);
        return 1;
    }

    static int buttons(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        UiItem& item = add(L, UiItem::Kind::Buttons);
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
        for (lua_Integer i = 1; i <= n && i <= 8; i++) {
            lua_rawgeti(L, 1, i);
            if (lua_istable(L, -1)) {
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                lua_rawgeti(L, -3, 3);
                item.optionIds.push_back(lua_isstring(L, -3) ? lua_tostring(L, -3) : "");
                item.options.push_back(lua_isstring(L, -2) ? lua_tostring(L, -2) : "");
                item.kinds.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "plain");
                lua_pop(L, 3);
            }
            lua_pop(L, 1);
        }
        const std::string& clicked = of(L)->drawing_->clicked;
        for (const std::string& id : item.optionIds) {
            if (!id.empty() && id == clicked) {
                lua_pushlstring(L, id.data(), id.size());
                return 1;
            }
        }
        lua_pushnil(L);
        return 1;
    }

    static int progress(lua_State* L) {
        UiItem& item = add(L, UiItem::Kind::Progress);
        item.fraction = (std::max)(0.0, (std::min)(1.0, static_cast<double>(luaL_checknumber(L, 1))));
        item.text = luaL_optstring(L, 2, "");
        item.tone = toneOf(L, 3);
        return 0;
    }

    static int gap(lua_State* L) {
        add(L, UiItem::Kind::Gap);
        return 0;
    }

    // A message handler: the error with where it happened
    static int traceback(lua_State* L) {
        const char* message = lua_tostring(L, 1);
        luaL_traceback(L, L, message ? message : "(an error that is no text)", 1);
        return 1;
    }
};

void* Plugin::allocate(void* self, void* block, size_t oldSize, size_t newSize) {
    Plugin* p = static_cast<Plugin*>(self);
    const size_t had = block ? oldSize : 0;
    if (newSize == 0) {
        p->used_ -= had;
        std::free(block);
        return nullptr;
    }
    if (newSize > had && p->used_ + (newSize - had) > MEMORY_LIMIT) return nullptr;
    void* moved = std::realloc(block, newSize);
    if (moved) p->used_ = p->used_ - had + newSize;
    return moved;
}

Plugin::Plugin(Info info, std::filesystem::path stateFile) : info_(std::move(info)), stateFile_(std::move(stateFile)) {}

Plugin::~Plugin() {
    if (lua_) lua_close(lua_);
}

void Plugin::stop(const std::string& why) {
    if (lua_) {
        lua_close(lua_);
        lua_ = nullptr;
    }
    watches_.clear();
    used_ = 0;
    error_ = why;
    std::cout << "\nPlugin " << info_.id << " stopped: " << why << std::endl;
}

bool Plugin::call(int args, int results, int budgetMs) {
    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    // the message handler under the function and its arguments
    const int base = lua_gettop(lua_) - args;
    lua_pushcfunction(lua_, &Api::traceback);
    lua_insert(lua_, base);
    const int status = lua_pcall(lua_, args, results, base);
    if (status != LUA_OK) {
        const char* message = lua_tostring(lua_, -1);
        std::string why = message ? message : "an error";
        std::cout << "\nPlugin " << info_.id << ": " << why << std::endl;
        // the window gets the first line, without the traceback
        const size_t cut = why.find('\n');
        if (cut != std::string::npos) why.resize(cut);
        if (status == LUA_ERRMEM) why = "used more than " + std::to_string(MEMORY_LIMIT >> 20) + " MB of memory";
        stop(why);
        return false;
    }
    lua_remove(lua_, base);
    return true;
}

bool Plugin::start() {
    if (lua_) return true;
    error_.clear();
    if (!info_.problem.empty()) {
        error_ = info_.problem;
        return false;
    }
    used_ = 0;
    lua_ = lua_newstate(&Plugin::allocate, this);
    if (!lua_) {
        error_ = "no memory for it";
        return false;
    }
    *static_cast<Plugin**>(lua_getextraspace(lua_)) = this;
    lua_sethook(lua_, &Api::hook, LUA_MASKCOUNT, 1000);

    // the language, and none of the world: no io, os, package or debug
    struct Library {
        const char* name;
        lua_CFunction open;
    } libraries[] = {{LUA_GNAME, luaopen_base},        {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
                     {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8}};
    for (const Library& library : libraries) {
        luaL_requiref(lua_, library.name, library.open, 1);
        lua_pop(lua_, 1);
    }
    // ...nor code from files or as bytecode: its own Lua comes through
    // bedrock.require, read as text
    for (const char* name : {"dofile", "loadfile", "load", "collectgarbage"}) {
        lua_pushnil(lua_);
        lua_setglobal(lua_, name);
    }
    lua_getglobal(lua_, LUA_STRLIBNAME);
    lua_pushnil(lua_);
    lua_setfield(lua_, -2, "dump");
    lua_pop(lua_, 1);
    lua_pushcfunction(lua_, &Api::print);
    lua_setglobal(lua_, "print");

    lua_newtable(lua_);
    lua_setfield(lua_, LUA_REGISTRYINDEX, "bedrock.modules");
    const luaL_Reg bedrock[] = {{"data", &Api::data},     {"load", &Api::load},       {"save", &Api::save},
                                {"watch", &Api::watch},   {"now", &Api::now},         {"time", &Api::time},
                                {"note", &Api::note},     {"redraw", &Api::redraw},   {"require", &Api::require},
                                {"copy", &Api::copy},     {"date", &Api::date},       {nullptr, nullptr}};
    luaL_newlib(lua_, bedrock);
    lua_pushstring(lua_, info_.id.c_str());
    lua_setfield(lua_, -2, "id");
    lua_setglobal(lua_, "bedrock");
    const luaL_Reg ui[] = {{"heading", &Api::heading}, {"text", &Api::text},       {"tabs", &Api::tabs},
                           {"chips", &Api::chips},     {"search", &Api::search},   {"card", &Api::card},
                           {"close", &Api::close},     {"row", &Api::row},         {"counter", &Api::counter},
                           {"buttons", &Api::buttons}, {"progress", &Api::progress}, {"gap", &Api::gap},
                           {nullptr, nullptr}};
    luaL_newlib(lua_, ui);
    lua_setfield(lua_, LUA_REGISTRYINDEX, "bedrock.ui");

    const std::string text = readText(info_.folder / "main.lua");
    if (text.empty()) {
        stop("main.lua is missing");
        return false;
    }
    if (luaL_loadbufferx(lua_, text.data(), text.size(), "@main.lua", "t") != LUA_OK) {
        const char* message = lua_tostring(lua_, -1);
        stop(message ? message : "main.lua cannot be read");
        return false;
    }
    return call(0, 0, START_MS);
}

std::string Plugin::title() const {
    if (!lua_) return info_.name;
    lua_getglobal(lua_, "title");
    std::string t = lua_isstring(lua_, -1) ? lua_tostring(lua_, -1) : info_.name;
    lua_pop(lua_, 1);
    return t;
}

std::string Plugin::subtitle() const {
    if (!lua_) return {};
    lua_getglobal(lua_, "subtitle");
    std::string t = lua_isstring(lua_, -1) ? lua_tostring(lua_, -1) : std::string();
    lua_pop(lua_, 1);
    return t;
}

bool Plugin::wants(std::string_view line, bool old) const {
    for (const Watch& w : watches_) {
        if ((!old || w.old) && line.find(w.text) != std::string_view::npos) return true;
    }
    return false;
}

std::vector<std::string> Plugin::oldWatches() const {
    std::vector<std::string> texts;
    for (const Watch& w : watches_) {
        if (w.old && std::find(texts.begin(), texts.end(), w.text) == texts.end()) texts.push_back(w.text);
    }
    return texts;
}

void Plugin::logLine(std::string_view line, bool old) {
    // (the line without the carriage return a log written on Windows ends in)
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    for (size_t i = 0; lua_ && i < watches_.size(); i++) {
        const Watch& w = watches_[i];
        if ((old && !w.old) || line.find(w.text) == std::string_view::npos) continue;
        lua_rawgeti(lua_, LUA_REGISTRYINDEX, w.function);
        lua_pushlstring(lua_, line.data(), line.size());
        lua_pushboolean(lua_, old);
        if (!call(2, 0, CALL_MS)) {
            changed_ = true;   // stopped: its window says so
            return;
        }
        changed_ = true;
    }
}

std::vector<UiItem> Plugin::draw(UiState& state) {
    std::vector<UiItem> items;
    if (!lua_) return items;
    lua_getglobal(lua_, "draw");
    if (!lua_isfunction(lua_, -1)) {
        lua_pop(lua_, 1);
        return items;
    }
    lua_getfield(lua_, LUA_REGISTRYINDEX, "bedrock.ui");
    drawing_ = &state;
    items_ = &items;
    const bool ok = call(1, 0, CALL_MS);
    drawing_ = nullptr;
    items_ = nullptr;
    // a click and a typed number count once
    state.clicked.clear();
    state.numbers.clear();
    if (!ok) items.clear();
    return items;
}

std::string Plugin::takeNote() {
    std::string n;
    n.swap(note_);
    return n;
}

std::string Plugin::takeCopy() {
    std::string c;
    c.swap(copy_);
    return c;
}

bool Plugin::takeChanged() {
    const bool c = changed_;
    changed_ = false;
    return c;
}

}  // namespace Plugins
}  // namespace SCMiner
