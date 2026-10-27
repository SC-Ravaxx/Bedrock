#pragma once
// Public keys a plugin's signature is checked against (Plugins::signedBy).
//
// RAVAXX_PLUGIN_KEYS: "Bedrock plugin key", ssh-ed25519,
// SHA256:2vsG/57dB3GD8vKofD4oPTJUHT4yDF9W5ZcC9YQZMvU (plugins/tools/bedrock-plugin-key.pub).
//
// Developer builds also take the test key: seed = first 32 bytes of the
// SHA-512 of PLUGIN_TEST_SEED_TEXT. Known to all.
#include <array>
#include <vector>

namespace SCMiner {
namespace Plugins {

inline const std::vector<std::array<unsigned char, 32>> RAVAXX_PLUGIN_KEYS = {
    {
        0xc0, 0xb8, 0xf5, 0x14, 0x92, 0x4b, 0x44, 0xce,
        0x2f, 0x6a, 0xc4, 0xb9, 0xc5, 0x21, 0x9d, 0x10,
        0x1d, 0x42, 0x59, 0xf7, 0xf8, 0xdd, 0xaf, 0x0e,
        0x5b, 0xdc, 0x86, 0xaa, 0xd2, 0x58, 0xed, 0x7f,
    },
};

inline constexpr const char* PLUGIN_TEST_SEED_TEXT = "Bedrock plugin test key - known to all, for developer builds only";

}  // namespace Plugins
}  // namespace SCMiner
