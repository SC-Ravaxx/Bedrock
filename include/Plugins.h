#pragma once
// Plugins: features that come apart from Bedrock - Wikelo's trades the first
// - each a folder of Lua and data in the data folder's plugins\<id>, written
// and signed by Ravaxx. Bedrock runs no plugin that is not signed with a key
// it carries (PluginKeys.h): a folder copied in from anywhere else stays
// shut, whatever it holds. Plugins come in through Bedrock alone.
//
// A plugin runs in a Lua of its own, on Bedrock's own thread, with the
// language and none of the world: no files, no programs, no network - only
// what the `bedrock` table below hands it. A mistake in it, or a plugin that
// runs too long or eats too much memory, stops that plugin and nothing else.
//
//   bedrock.data(name)           its own data/<name>.json, as tables
//   bedrock.load()               what it saved last, or nil
//   bedrock.save(t)              ...the table, kept in plugin_data\<id>.json
//   bedrock.watch(text, f, old)  f(line, old) for every line of Game.log with
//                                `text` in it - with `old` also the lines of
//                                the older logs, read once at start
//   bedrock.now()                the time, Unix milliseconds
//   bedrock.note(text)           a line at the foot of Bedrock's window
//   bedrock.redraw()             its window drawn again
//   bedrock.require(name)        its own <name>.lua
//   function draw(ui) ... end    its window: see UiItem
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace SCMiner {
namespace Plugins {

using Key = std::array<unsigned char, 32>;

// The keys a plugin's signature is taken from (PluginKeys.h); a developer's
// build takes the test key besides
std::vector<Key> trustedKeys();
// The test key's secret seed - known to all (PluginKeys.h)
Key testSeed();

// A plugin folder's manifest: a line for every file in it but plugin.sig -
// its SHA-512 in hex, two spaces, its path in the folder with '/' between
// the folders - sorted by path. What a signature covers. "" where a file
// cannot be read.
std::string manifestOf(const std::filesystem::path& folder);
// Whether `signatureHex` (128 hex figures) is the Ed25519 signature of the
// manifest by one of `keys`
bool signedBy(const std::string& manifest, const std::string& signatureHex, const std::vector<Key>& keys);
// ...the signature with a secret key (its 32-byte seed): for the developer's
// signing tool and the self-test
std::string signatureOf(const std::string& manifest, const Key& seed);
Key publicKeyOf(const Key& seed);

// A plugin folder, as Bedrock finds it
struct Info {
    std::string id, name, version, description;
    std::string needs;                  // the oldest Bedrock it runs on; "" any
    std::filesystem::path folder;
    bool trusted = false;               // signed with one of the keys
    std::string problem;                // "" where it can run; else why not, in the window's words
};

// Every plugin folder under `root`, its plugin.json read and its signature
// checked; `acceptUnsigned` for a developer's own plugins
std::vector<Info> discover(const std::filesystem::path& root, const std::vector<Key>& keys, bool acceptUnsigned,
                           const std::string& bedrockVersion);

// ---- the window ------------------------------------------------------------
// A plugin draws its window anew whenever something in it changes: draw(ui)
// calls the ui functions top to bottom, and each adds a piece. What the
// player did in the window since - a click, a pick, a field typed into, a
// card opened - comes back as the answer of the piece it was done in:
//
//   ui.heading(text)                       a section's heading
//   ui.text(text, tone)                    a paragraph
//   ui.tabs(id, {labels}, picked)          -> the tab picked, 1 up
//   ui.chips(id, {labels}, picked)         -> the chip picked, 1 up
//   ui.search(id, hint)                    -> what is typed in it
//   ui.card(id, title, {subtitle, chip, tone}) -> whether it is open; then
//       its rows, and ui.close()
//   ui.row(label, value, tone)             a line of a card: left and right
//   ui.counter(id, label, value)           -> the value, changed by - and +
//                                             or typed
//   ui.buttons({{id, text, kind}, ...})    -> the id of the button clicked, or nil
//   ui.progress(fraction, text)            a bar
//   ui.gap()                               a little room
//
// tone: "good", "warn", "bad", "dim" or nil; kind: "primary", "plain" or
// "danger".
struct UiItem {
    enum class Kind { Heading, Text, Tabs, Chips, Search, Card, CardEnd, Row, Counter, Buttons, Progress, Gap };
    enum class Tone { Plain, Good, Warn, Bad, Dim };
    Kind kind = Kind::Text;
    std::string id;
    std::string text;                   // heading, paragraph, card title, row and counter label
    std::string value;                  // row's right side, card subtitle, counter's value, search's hint
    std::string chip;                   // a card's
    Tone tone = Tone::Plain;
    std::vector<std::string> options;   // tabs', chips' and buttons' labels
    std::vector<std::string> optionIds; // buttons' ids
    std::vector<std::string> kinds;     // buttons' kinds
    int picked = 0;                     // tabs and chips: which, 1 up
    bool open = false;                  // a card's
    double fraction = 0;                // a bar's
};

// What a plugin's window keeps between two draws
struct UiState {
    std::string clicked;                        // a button's id, a card's, or "<counter id>:-" and ":+"
    std::map<std::string, int> picks;           // tabs and chips
    std::map<std::string, std::string> typed;   // search fields
    std::map<std::string, int> numbers;         // counters typed into, until the next draw
    std::set<std::string> open;                 // cards open
};

// A plugin running
class Plugin {
public:
    Plugin(Info info, std::filesystem::path stateFile);
    ~Plugin();
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    // Its main.lua run: false, and error(), where it could not start
    bool start();
    bool running() const { return lua_ != nullptr; }
    const Info& info() const { return info_; }
    const std::string& error() const { return error_; }
    std::string title() const;

    // A line of Game.log - `old`: of an older log, read at start
    void logLine(std::string_view line, bool old);
    // Whether any of its watches wants this line at all (cheap)
    bool wants(std::string_view line, bool old) const;

    // Its window: the pieces, the player's doings taken in; empty, and the
    // plugin stopped, where draw went wrong
    std::vector<UiItem> draw(UiState& state);

    // What it asked for since: a line at the foot of the window, a new draw
    std::string takeNote();
    bool takeRedraw();

    // Limits: memory and time a call may take
    static constexpr size_t MEMORY_LIMIT = 64u << 20;
    static constexpr int CALL_MS = 250;
    static constexpr int START_MS = 2000;

private:
    struct Watch {
        std::string text;
        int function = 0;   // its reference in the registry
        bool old = false;
    };
    friend struct Api;
    bool call(int args, int results, int budgetMs);
    void stop(const std::string& why);

    Info info_;
    std::filesystem::path stateFile_;
    lua_State* lua_ = nullptr;
    size_t used_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
    std::vector<Watch> watches_;
    std::string error_, note_;
    bool redraw_ = false;
    // while draw runs: the window's state and the pieces so far
    UiState* drawing_ = nullptr;
    std::vector<UiItem>* items_ = nullptr;

    static void* allocate(void* self, void* block, size_t oldSize, size_t newSize);
};

}  // namespace Plugins
}  // namespace SCMiner
