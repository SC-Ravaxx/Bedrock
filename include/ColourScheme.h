#pragma once

// The colours Bedrock paints with, as the player has them: what each one is
// for, the sets made for colour blindness, and the file they are kept in.
//
// Nothing here draws. MinerGUI paints with what comes out of it, and the
// self-test checks the sets the way they are meant to be used: every pair
// of colours that has to be told apart, seen as someone with each kind of
// colour blindness sees it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace SCMiner {
namespace Colours {

// What a colour is for. The order is the file's and the settings window's.
enum Role : int {
    // the rarities: the rarity's name wherever an ore's is given, and a
    // rock part's quality by the band it rolled into
    Legendary,
    Epic,
    Rare,
    Uncommon,
    Common,
    // what something means
    Highlight,   // the figures that count, what was picked, the main button
    Good,        // it breaks, the best match, done
    Caution,     // only with help, slow, nearly full
    Bad,         // it will not hold, something went wrong
    LookedFor,   // the ores looked for - what blinks
    // the windows themselves; the surfaces and the quieter inks are worked
    // out from these two (see MinerGUI's applyColours)
    Background,
    Text,
    ROLE_COUNT
};

using Rgb = uint32_t;   // 0xRRGGBB

constexpr Rgb rgb(int r, int g, int b) {
    return ((Rgb)(r & 0xFF) << 16) | ((Rgb)(g & 0xFF) << 8) | (Rgb)(b & 0xFF);
}
constexpr int redOf(Rgb c) { return (int)((c >> 16) & 0xFF); }
constexpr int greenOf(Rgb c) { return (int)((c >> 8) & 0xFF); }
constexpr int blueOf(Rgb c) { return (int)(c & 0xFF); }

struct Scheme {
    std::array<Rgb, ROLE_COUNT> colour{};
    Rgb operator[](int role) const { return colour[(size_t)role]; }
    bool operator==(const Scheme& other) const { return colour == other.colour; }
};

// The sets: Bedrock's own, one for each kind of colour blindness that has a
// name in every game's settings, and the player's own once a colour has
// been changed by hand
enum class Set : int { Bedrock, Protanopia, Deuteranopia, Tritanopia, Own, COUNT };

// A set's colours. Own has none of its own here - Settings keeps them - and
// gives Bedrock's.
const Scheme& preset(Set set);

const char* roleKey(Role role);   // the file's word: "legendary"
const char* setKey(Set set);      // "deuteranopia"

// What colours.txt holds: the set in use, and the player's own colours,
// kept while another set is in use so that they can be gone back to
struct Settings {
    Set set = Set::Bedrock;
    bool ownKept = false;
    Scheme own;
};
Settings defaults();
Scheme active(const Settings& settings);
std::string write(const Settings& settings);
// Whatever the file says, something usable: a line that cannot be read is
// passed over, a colour missing is Bedrock's
Settings read(const std::string& text);

std::string hex(Rgb c);                           // "#EEB246"
bool parseHex(const std::string& text, Rgb& out); // "#EEB246", "eeb246"

// What keeps every window readable whatever is picked: the background
// stays dark enough for light words on it, and the words light enough on it
constexpr double BACKGROUND_MOST = 0.03;   // relative luminance, at the most
constexpr double TEXT_CONTRAST_LEAST = 10.0;
Rgb keepBackgroundDark(Rgb background);
Rgb keepTextReadable(Rgb text, Rgb background);
// The scheme with one colour changed, and the background and the words
// still readable against each other
Scheme withColour(const Scheme& scheme, Role role, Rgb colour);

// Hue in degrees, saturation and lightness 0 to 1 - for the sliders
struct Hsl {
    double h = 0, s = 0, l = 0;
};
Hsl toHsl(Rgb c);
Rgb fromHsl(const Hsl& c);

// How colours look to someone with each kind of colour blindness, at full
// strength: Machado, Oliveira and Fernandes (2009), in linear sRGB. For the
// settings' preview and for the self-test's check of the sets.
enum class Vision : int { Normal, Protanopia, Deuteranopia, Tritanopia, COUNT };
Rgb seenWith(Rgb c, Vision vision);
// ...a picture's pixels in place: four bytes each, blue, green, red, alpha
// (GDI+'s 32-bit order), the alpha left as it is
void seenWith(uint8_t* bgra, size_t pixels, Vision vision);

// How far apart two colours look: CIEDE2000. Under about 10 two small
// coloured words are hard to tell apart at a glance; from 20 they are not.
double difference(Rgb a, Rgb b);
double differenceLab(double l1, double a1, double b1, double l2, double a2, double b2);
// WCAG 2 contrast ratio, 1 to 21
double contrast(Rgb a, Rgb b);
double luminance(Rgb c);

// The pairs a player has to tell apart at a glance: the rarities from one
// another, good, caution and bad from one another, and an ore looked for
// from the best match beside it. The smallest difference among them, as
// someone with the given vision sees them, and which pair it is.
double worstDifference(const Scheme& scheme, Vision vision, Role* first = nullptr, Role* second = nullptr);

// The two quieter inks every window writes with besides the text, between
// the background and the words as far as Bedrock's own lie: dim for what is
// said in passing, faint for what is hardly said. Bedrock's scheme gives
// (160,166,176) and (108,115,127).
Rgb dimInk(const Scheme& scheme);
Rgb faintInk(const Scheme& scheme);
// ...and how near a colour that means something comes to those plain inks
// or the text, as someone with the given vision sees it, and which colour
// comes nearest: one that reads as plain grey or white says nothing. Common
// is left out - it is grey on purpose.
double plainestDifference(const Scheme& scheme, Vision vision, Role* which = nullptr);

}  // namespace Colours
}  // namespace SCMiner
