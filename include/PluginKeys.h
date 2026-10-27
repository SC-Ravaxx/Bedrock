#pragma once
// The keys Bedrock takes a plugin's signature from. A plugin is signed
// with the secret half of one of them (Bedrock's --sign-plugin, in a
// developer's build); the secret never comes near this repository.
//
// RAVAXX_PLUGIN_KEYS: Ravaxx's public keys, 32 bytes each - empty until his
// key is made. A release runs no plugin before then.
//
// A developer's build also takes the test key: its seed is the first 32
// bytes of the SHA-512 of PLUGIN_TEST_SEED_TEXT - known to all, so what it
// signs runs in a developer's build and nowhere else.
#include <array>
#include <vector>

namespace SCMiner {
namespace Plugins {

inline const std::vector<std::array<unsigned char, 32>> RAVAXX_PLUGIN_KEYS = {};

inline constexpr const char* PLUGIN_TEST_SEED_TEXT = "Bedrock plugin test key - known to all, for developer builds only";

}  // namespace Plugins
}  // namespace SCMiner
