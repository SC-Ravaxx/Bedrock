#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace SCMiner {

// Bedrock's marks in the game's own contract texts, when the player asks for
// them (the box in the Blueprints window): a mission that pays out blueprints
// gets [BP] in its title and the list of them in its description - what is
// still missing in blue, what is owned plain; the title's [BP] is blue while
// anything is missing. Blue is all the colour there is: the mobiGlas shows
// the game's <EM4> blue and <EM3> underlined, and prints <font color> and
// every other tag tried as raw text. The game reads its texts once, at
// start, so this is the next start's picture, rewritten whenever the
// library changes.
//
// How: the game reads data\Localization\english\global.ini beside Data.p4k in
// place of its own when user.cfg says g_language = english. The file written
// there is the game's own text, byte for byte, taken out of Data.p4k (and
// kept in Bedrock's data folder until the game's files change), with the
// marks of GameTextMarks.h added at the end of their values and one line of
// Bedrock's own at the very end. Nothing else is changed.
//
// Whose file it is decides everything: one Bedrock wrote (that last line, or
// the list heading an older hand-made one carried) is rewritten and, when the
// box is unticked, taken away again. Anyone else's - a translation, a
// community pack - is left exactly as it is, and the window says so. A game
// set to another language is left alone as well: the marks are English.
//
// Spelling: the game asks Windows for its files, and Windows - and Wine, which
// runs the game under Linux - finds them in any case: data\Localization and
// Data\localization are one folder to the game. Under Linux they are two to
// Bedrock, so a pack in Data\Localization would go unseen and Bedrock's file
// would stand beside it, and a USER.cfg set to German would go unread and a
// user.cfg saying English be put beside it. Every name here is looked up as
// Wine looks it up for the game - as spelt, else the first entry that differs
// from it only in case - and a pack in any spelling keeps Bedrock out.
//
// The marks' tokens, filled in here (gen_game_texts.py writes them):
//     {S}                   a space, unless the game's value ends with one
//     {I<b>|text}           a list line - blue while blueprint b is missing,
//                           plain once owned (b = -1: no row, always missing)
//     {O<n>:<b,b,...>}      ' · 2 missing' / ' · none owned' in blue, or ' · all owned'
//     {A<n>:<b,b,...>}      ' · all owned' when all n are, or nothing
//     {T<n>:<b,b,...>|tag}  the title's tag, blue - or plain when all n are owned
// b is a row of CRAFT_BLUEPRINTS.
class GameTexts {
public:
    using OwnedFn = std::function<bool(int blueprint)>;

    struct Outcome {
        bool written = false;
        std::string note;          // one line for the Blueprints window
    };

    // Writes <channel>\data\Localization\english\global.ini - channel is the
    // folder of the game's Data.p4k and Game.log (LIVE, PTU, ...). owned: one
    // flag per CRAFT_BLUEPRINTS row. Slow the first time per game build (the
    // game's text comes out of the 160 GB archive), quick after.
    static Outcome write(const std::filesystem::path& channel, const std::vector<bool>& owned);
    // What write() would put there, without putting it anywhere - for
    // --gametexts, which compares it with gen_game_texts.py's reference.
    // False with the reason in note.
    static bool build(const std::filesystem::path& channel, const std::vector<bool>& owned,
                      std::string& text, int& marked, std::string& note);
    // Takes Bedrock's file away again, and only Bedrock's
    static Outcome remove(const std::filesystem::path& channel);
    // Bedrock's file in this channel stands on an older build of the game:
    // the channel's Data.p4k was written after it - a patch - and has stood
    // still for `settled` since, so the patch is over. The game reads that
    // file in place of its own text, so until it is written anew it hides
    // what the patch added and changed. The archive's time when it is (to be
    // acted on once), else 0.
    static long long outdatedSince(const std::filesystem::path& channel, std::chrono::seconds settled);
    // The game's channel folders - LIVE, PTU, EPTU... - beside this one, each
    // with a Data.p4k of its own: this one first, and a junction to one of
    // them (HOTFIX to LIVE, the usual case) not again.
    static std::vector<std::filesystem::path> channelsBeside(const std::filesystem::path& channel);

    // The pieces, public for the self-test
    static std::string render(const char* text, const std::string& value, const OwnedFn& owned);
    // The game's text with the marks; returns how many of its values got one
    static int compose(const std::string& base, const OwnedFn& owned, std::string& out);
    // Bedrock's file, by its last line - or the list heading of a hand-made one
    static bool isOurs(const std::string& text);
    // user.cfg as the game needs it for this: g_language english, added when
    // missing. False, with the language in `language`, when another one is set.
    static bool wantEnglish(std::string& cfg, std::string& language, bool& changed);
    // Every file below `base` the game could take for base\steps: each step
    // matched in any case. Under Windows one at most; under Linux there can
    // be several (data\ and Data\ side by side).
    static std::vector<std::filesystem::path> spellings(const std::filesystem::path& base,
                                                        const std::vector<std::string>& steps);
    // The one the game opens, found as Wine finds it: each step as spelt when
    // that is there, else the first entry that differs only in case - and,
    // where nothing is there yet, where Bedrock puts it (the spelling given).
    static std::filesystem::path placeFor(const std::filesystem::path& base, const std::vector<std::string>& steps);

    // <channel>\data\Localization\english\global.ini, in the spelling there is
    static std::filesystem::path target(const std::filesystem::path& channel);
    static constexpr const char* MARKER_KEY = "bedrock_game_texts";
};

}  // namespace SCMiner
