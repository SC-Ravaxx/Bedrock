#include "ColourScheme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace SCMiner {
namespace Colours {

namespace {

// Bedrock's own: what every window was painted with before there was a
// choice. The rarities borrow the amber, the green and the grey of the text.
constexpr Scheme BEDROCK = {{
    rgb(238, 178, 70),    // Legendary - the highlight's amber
    rgb(171, 139, 214),   // Epic
    rgb(106, 166, 226),   // Rare
    rgb(96, 202, 132),    // Uncommon - good's green
    rgb(160, 166, 176),   // Common - the quieter text
    rgb(238, 178, 70),    // Highlight
    rgb(96, 202, 132),    // Good
    rgb(224, 186, 92),    // Caution
    rgb(234, 94, 86),     // Bad
    rgb(234, 94, 86),     // Looked for
    rgb(15, 16, 19),      // Background
    rgb(238, 239, 241),   // Text
}};

// The sets for colour blindness. Each was searched for among the colours
// readable on Bedrock's cards - contrast 4.5 and up, to normal sight and to
// the eye it is made for - for the largest difference, CIEDE2000, between
// the pairs that have to be told apart, and between each colour that means
// something and the plain text and greys; seen with that kind of colour
// blindness and with normal sight both, and kept as near Bedrock's own as
// that allows. The self-test checks them (worstDifference,
// plainestDifference). Bedrock's own fall apart there: Epic and Rare are
// 2.3 apart to deuteranopia and 4.7 to protanopia, Good and Caution 7.0 to
// protanopia, Rare and Uncommon 9.7 to tritanopia. The sets keep everything
// 15 apart and more to their own kind and to normal sight, and the two
// red-green sets 13 to the other red-green kind as well: who has which is
// not always known.
//
// Red-green (deuteranopia, protanopia): what is left to tell things apart
// is light against dark and yellow against blue. Epic is a pale lavender
// over a mid blue Rare for deuteranopia, a mid violet under a pale sky Rare
// for protanopia; Good and Uncommon are greens that stand off the reds by
// their lightness, and the reds are light enough to read to an eye that
// sees red darker. (A pale cyan told good from bad best of all, but read as
// white text to these eyes: hence the plain inks in the search.)
constexpr Scheme DEUTERANOPIA = {{
    rgb(235, 173, 46),    // Legendary
    rgb(212, 201, 251),   // Epic
    rgb(98, 158, 219),    // Rare
    rgb(91, 213, 142),    // Uncommon
    rgb(169, 169, 169),   // Common
    rgb(238, 178, 70),    // Highlight
    rgb(117, 207, 161),   // Good
    rgb(247, 204, 75),    // Caution
    rgb(240, 106, 87),    // Bad
    rgb(240, 106, 87),    // Looked for
    rgb(15, 16, 19),      // Background
    rgb(238, 239, 241),   // Text
}};
constexpr Scheme PROTANOPIA = {{
    rgb(223, 153, 24),    // Legendary
    rgb(144, 124, 198),   // Epic
    rgb(129, 190, 253),   // Rare
    rgb(109, 209, 148),   // Uncommon
    rgb(169, 169, 169),   // Common
    rgb(238, 178, 70),    // Highlight
    rgb(142, 191, 158),   // Good
    rgb(234, 197, 92),    // Caution
    rgb(241, 104, 98),    // Bad
    rgb(241, 104, 98),    // Looked for
    rgb(15, 16, 19),      // Background
    rgb(238, 239, 241),   // Text
}};
// Blue-yellow (tritanopia): blue and green run together, and yellow and
// pink, so Rare is a deep azure that stands off a light Uncommon green by
// its lightness, Epic a clear violet, and the warm colours stay much as
// Bedrock's own
constexpr Scheme TRITANOPIA = {{
    rgb(237, 183, 85),    // Legendary
    rgb(178, 112, 223),   // Epic
    rgb(14, 137, 214),    // Rare
    rgb(78, 224, 143),    // Uncommon
    rgb(169, 169, 169),   // Common
    rgb(238, 178, 70),    // Highlight
    rgb(82, 205, 134),    // Good
    rgb(231, 186, 84),    // Caution
    rgb(232, 96, 91),     // Bad
    rgb(232, 96, 91),     // Looked for
    rgb(15, 16, 19),      // Background
    rgb(238, 239, 241),   // Text
}};

const char* const ROLE_KEYS[ROLE_COUNT] = {
    "legendary", "epic", "rare", "uncommon", "common", "highlight",
    "good", "caution", "bad", "lookedfor", "background", "text",
};
const char* const SET_KEYS[(int)Set::COUNT] = {"bedrock", "protanopia", "deuteranopia", "tritanopia", "own"};

std::string trimmed(const std::string& s) {
    size_t from = 0, to = s.size();
    while (from < to && (s[from] == ' ' || s[from] == '\t' || s[from] == '\r')) from++;
    while (to > from && (s[to - 1] == ' ' || s[to - 1] == '\t' || s[to - 1] == '\r')) to--;
    return s.substr(from, to - from);
}

// sRGB to linear light, by table: every colour has one of 256 values a channel
const std::array<double, 256>& linearTable() {
    static const std::array<double, 256> table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; i++) {
            const double c = i / 255.0;
            t[(size_t)i] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
        return t;
    }();
    return table;
}

double linear(int channel) { return linearTable()[(size_t)std::clamp(channel, 0, 255)]; }

int encoded(double v) {
    v = std::clamp(v, 0.0, 1.0);
    const double c = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return (int)std::lround(c * 255.0);
}

// ...and back for a whole picture: 4096 steps of linear light are finer than
// the 256 of the encoded channel everywhere but in the deepest shadows, where
// a step is a 255th of nothing
const std::array<uint8_t, 4097>& encodedTable() {
    static const std::array<uint8_t, 4097> table = [] {
        std::array<uint8_t, 4097> t{};
        for (int i = 0; i <= 4096; i++) t[(size_t)i] = (uint8_t)encoded(i / 4096.0);
        return t;
    }();
    return table;
}

// Machado, Oliveira and Fernandes 2009, severity 1.0, for linear sRGB
const double MACHADO[3][3][3] = {
    // protanopia
    {{0.152286, 1.052583, -0.204868}, {0.114503, 0.786281, 0.099216}, {-0.003882, -0.048116, 1.051998}},
    // deuteranopia
    {{0.367322, 0.860646, -0.227968}, {0.280085, 0.672501, 0.047413}, {-0.011820, 0.042940, 0.968881}},
    // tritanopia
    {{1.255528, -0.076749, -0.178779}, {-0.078411, 0.930809, 0.147602}, {0.004733, 0.691367, 0.303900}},
};

void toLab(Rgb c, double& L, double& a, double& b) {
    const double r = linear(redOf(c)), g = linear(greenOf(c)), bl = linear(blueOf(c));
    const double x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * bl;
    const double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * bl;
    const double z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * bl;
    const auto f = [](double t) {
        return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0;
    };
    const double fx = f(x / 0.95047), fy = f(y / 1.0), fz = f(z / 1.08883);
    L = 116.0 * fy - 16.0;
    a = 500.0 * (fx - fy);
    b = 200.0 * (fy - fz);
}

constexpr double PI = 3.14159265358979323846;
double radians(double degrees) { return degrees * PI / 180.0; }
double degrees(double radiansValue) { return radiansValue * 180.0 / PI; }

}  // namespace

const Scheme& preset(Set set) {
    switch (set) {
        case Set::Protanopia: return PROTANOPIA;
        case Set::Deuteranopia: return DEUTERANOPIA;
        case Set::Tritanopia: return TRITANOPIA;
        default: return BEDROCK;
    }
}

const char* roleKey(Role role) {
    return (role >= 0 && role < ROLE_COUNT) ? ROLE_KEYS[role] : "";
}

const char* setKey(Set set) {
    const int i = (int)set;
    return (i >= 0 && i < (int)Set::COUNT) ? SET_KEYS[i] : "";
}

Settings defaults() {
    Settings s;
    s.set = Set::Bedrock;
    s.ownKept = false;
    s.own = BEDROCK;
    return s;
}

Scheme active(const Settings& settings) {
    if (settings.set == Set::Own) return settings.ownKept ? settings.own : BEDROCK;
    return preset(settings.set);
}

std::string write(const Settings& settings) {
    std::ostringstream out;
    out << "# Bedrock: the colours picked in its settings (the gear). Delete the\n";
    out << "# file and Bedrock's own come back.\n";
    out << "set=" << setKey(settings.set) << "\n";
    if (settings.ownKept) {
        for (int r = 0; r < ROLE_COUNT; r++) {
            out << "own." << ROLE_KEYS[r] << "=" << hex(settings.own[r]) << "\n";
        }
    }
    return out.str();
}

Settings read(const std::string& text) {
    Settings s = defaults();
    std::istringstream in(text);
    std::string line;
    bool anyOwn = false;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trimmed(line.substr(0, eq));
        const std::string value = trimmed(line.substr(eq + 1));
        if (key == "set") {
            for (int i = 0; i < (int)Set::COUNT; i++) {
                if (value == SET_KEYS[i]) s.set = (Set)i;
            }
            continue;
        }
        if (key.rfind("own.", 0) == 0) {
            const std::string role = key.substr(4);
            for (int r = 0; r < ROLE_COUNT; r++) {
                Rgb c = 0;
                if (role == ROLE_KEYS[r] && parseHex(value, c)) {
                    s.own.colour[(size_t)r] = c;
                    anyOwn = true;
                }
            }
        }
    }
    s.ownKept = anyOwn;
    // ...and readable, whatever was written into the file by hand
    s.own.colour[Background] = keepBackgroundDark(s.own[Background]);
    s.own.colour[Text] = keepTextReadable(s.own[Text], s.own[Background]);
    if (s.set == Set::Own && !s.ownKept) s.set = Set::Bedrock;
    return s;
}

std::string hex(Rgb c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", redOf(c), greenOf(c), blueOf(c));
    return buf;
}

bool parseHex(const std::string& text, Rgb& out) {
    std::string t = trimmed(text);
    if (!t.empty() && t[0] == '#') t = t.substr(1);
    if (t.size() != 6) return false;
    for (char ch : t) {
        if (!std::isxdigit((unsigned char)ch)) return false;
    }
    out = (Rgb)std::strtoul(t.c_str(), nullptr, 16) & 0xFFFFFF;
    return true;
}

double luminance(Rgb c) {
    return 0.2126 * linear(redOf(c)) + 0.7152 * linear(greenOf(c)) + 0.0722 * linear(blueOf(c));
}

double contrast(Rgb a, Rgb b) {
    const double la = luminance(a), lb = luminance(b);
    return ((std::max)(la, lb) + 0.05) / ((std::min)(la, lb) + 0.05);
}

Rgb keepBackgroundDark(Rgb background) {
    if (luminance(background) <= BACKGROUND_MOST) return background;
    // darker in the same hue: every channel by the same share
    const auto scaled = [&](double f) {
        return rgb((int)std::floor(redOf(background) * f), (int)std::floor(greenOf(background) * f),
                   (int)std::floor(blueOf(background) * f));
    };
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 30; i++) {
        const double mid = (lo + hi) / 2;
        if (luminance(scaled(mid)) <= BACKGROUND_MOST) lo = mid;
        else hi = mid;
    }
    return scaled(lo);
}

Rgb keepTextReadable(Rgb text, Rgb background) {
    if (contrast(text, background) >= TEXT_CONTRAST_LEAST) return text;
    // lighter in the same hue: towards white until it reads
    const auto mixed = [&](double t) {
        return rgb((int)std::ceil(redOf(text) + (255 - redOf(text)) * t),
                   (int)std::ceil(greenOf(text) + (255 - greenOf(text)) * t),
                   (int)std::ceil(blueOf(text) + (255 - blueOf(text)) * t));
    };
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 30; i++) {
        const double mid = (lo + hi) / 2;
        if (contrast(mixed(mid), background) >= TEXT_CONTRAST_LEAST) hi = mid;
        else lo = mid;
    }
    return mixed(hi);
}

Scheme withColour(const Scheme& scheme, Role role, Rgb colour) {
    Scheme s = scheme;
    if (role < 0 || role >= ROLE_COUNT) return s;
    s.colour[(size_t)role] = colour & 0xFFFFFF;
    s.colour[Background] = keepBackgroundDark(s[Background]);
    s.colour[Text] = keepTextReadable(s[Text], s[Background]);
    return s;
}

Hsl toHsl(Rgb c) {
    const double r = redOf(c) / 255.0, g = greenOf(c) / 255.0, b = blueOf(c) / 255.0;
    const double hi = (std::max)({r, g, b}), lo = (std::min)({r, g, b});
    Hsl out;
    out.l = (hi + lo) / 2;
    const double d = hi - lo;
    if (d < 1e-9) return out;
    out.s = out.l > 0.5 ? d / (2.0 - hi - lo) : d / (hi + lo);
    double h;
    if (hi == r) h = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (hi == g) h = (b - r) / d + 2.0;
    else h = (r - g) / d + 4.0;
    out.h = h * 60.0;
    return out;
}

Rgb fromHsl(const Hsl& c) {
    const double s = std::clamp(c.s, 0.0, 1.0), l = std::clamp(c.l, 0.0, 1.0);
    double h = std::fmod(c.h, 360.0);
    if (h < 0) h += 360.0;
    if (s < 1e-9) {
        const int v = (int)std::lround(l * 255.0);
        return rgb(v, v, v);
    }
    const double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    const double p = 2.0 * l - q;
    const auto channel = [&](double t) {
        if (t < 0) t += 1.0;
        if (t > 1) t -= 1.0;
        if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
        return p;
    };
    const double hh = h / 360.0;
    return rgb((int)std::lround(channel(hh + 1.0 / 3.0) * 255.0), (int)std::lround(channel(hh) * 255.0),
               (int)std::lround(channel(hh - 1.0 / 3.0) * 255.0));
}

Rgb seenWith(Rgb c, Vision vision) {
    if (vision == Vision::Normal || vision == Vision::COUNT) return c;
    const auto& m = MACHADO[(int)vision - 1];
    const double r = linear(redOf(c)), g = linear(greenOf(c)), b = linear(blueOf(c));
    return rgb(encoded(m[0][0] * r + m[0][1] * g + m[0][2] * b), encoded(m[1][0] * r + m[1][1] * g + m[1][2] * b),
               encoded(m[2][0] * r + m[2][1] * g + m[2][2] * b));
}

void seenWith(uint8_t* bgra, size_t pixels, Vision vision) {
    if (!bgra || vision == Vision::Normal || vision == Vision::COUNT) return;
    const auto& m = MACHADO[(int)vision - 1];
    const auto& lin = linearTable();
    const auto& enc = encodedTable();
    const auto out = [&](double v) {
        return enc[(size_t)std::lround(std::clamp(v, 0.0, 1.0) * 4096.0)];
    };
    for (size_t i = 0; i < pixels; i++) {
        uint8_t* p = bgra + i * 4;
        const double b = lin[p[0]], g = lin[p[1]], r = lin[p[2]];
        p[2] = out(m[0][0] * r + m[0][1] * g + m[0][2] * b);
        p[1] = out(m[1][0] * r + m[1][1] * g + m[1][2] * b);
        p[0] = out(m[2][0] * r + m[2][1] * g + m[2][2] * b);
    }
}

double differenceLab(double L1, double a1, double b1, double L2, double a2, double b2) {
    // Sharma, Wu and Dalal (2005), as written there
    const double C1 = std::hypot(a1, b1), C2 = std::hypot(a2, b2);
    const double Cb = (C1 + C2) / 2.0;
    const double Cb7 = std::pow(Cb, 7.0);
    const double G = 0.5 * (1.0 - std::sqrt(Cb7 / (Cb7 + std::pow(25.0, 7.0))));
    const double a1p = (1.0 + G) * a1, a2p = (1.0 + G) * a2;
    const double C1p = std::hypot(a1p, b1), C2p = std::hypot(a2p, b2);
    const auto hue = [](double b, double ap) {
        if (b == 0 && ap == 0) return 0.0;
        double h = degrees(std::atan2(b, ap));
        return h < 0 ? h + 360.0 : h;
    };
    const double h1p = hue(b1, a1p), h2p = hue(b2, a2p);
    const double dLp = L2 - L1, dCp = C2p - C1p;
    double dhp = 0;
    if (C1p * C2p != 0) {
        dhp = h2p - h1p;
        if (dhp > 180) dhp -= 360;
        else if (dhp < -180) dhp += 360;
    }
    const double dHp = 2.0 * std::sqrt(C1p * C2p) * std::sin(radians(dhp / 2.0));
    const double Lbp = (L1 + L2) / 2.0, Cbp = (C1p + C2p) / 2.0;
    double hbp = h1p + h2p;
    if (C1p * C2p != 0) {
        if (std::abs(h1p - h2p) <= 180) hbp = (h1p + h2p) / 2.0;
        else hbp = (h1p + h2p < 360) ? (h1p + h2p + 360) / 2.0 : (h1p + h2p - 360) / 2.0;
    }
    const double T = 1.0 - 0.17 * std::cos(radians(hbp - 30)) + 0.24 * std::cos(radians(2 * hbp)) +
                     0.32 * std::cos(radians(3 * hbp + 6)) - 0.20 * std::cos(radians(4 * hbp - 63));
    const double dTheta = 30.0 * std::exp(-std::pow((hbp - 275.0) / 25.0, 2.0));
    const double Cbp7 = std::pow(Cbp, 7.0);
    const double Rc = 2.0 * std::sqrt(Cbp7 / (Cbp7 + std::pow(25.0, 7.0)));
    const double Sl = 1.0 + (0.015 * std::pow(Lbp - 50.0, 2.0)) / std::sqrt(20.0 + std::pow(Lbp - 50.0, 2.0));
    const double Sc = 1.0 + 0.045 * Cbp;
    const double Sh = 1.0 + 0.015 * Cbp * T;
    const double Rt = -std::sin(radians(2.0 * dTheta)) * Rc;
    return std::sqrt(std::pow(dLp / Sl, 2.0) + std::pow(dCp / Sc, 2.0) + std::pow(dHp / Sh, 2.0) +
                     Rt * (dCp / Sc) * (dHp / Sh));
}

double difference(Rgb a, Rgb b) {
    double L1, a1, b1, L2, a2, b2;
    toLab(a, L1, a1, b1);
    toLab(b, L2, a2, b2);
    return differenceLab(L1, a1, b1, L2, a2, b2);
}

double worstDifference(const Scheme& scheme, Vision vision, Role* first, Role* second) {
    static const Role rarities[] = {Legendary, Epic, Rare, Uncommon, Common};
    static const Role meanings[] = {Good, Caution, Bad};
    double worst = 1e9;
    const auto pair = [&](Role a, Role b) {
        const double d = difference(seenWith(scheme[a], vision), seenWith(scheme[b], vision));
        if (d < worst) {
            worst = d;
            if (first) *first = a;
            if (second) *second = b;
        }
    };
    for (size_t i = 0; i < 5; i++) {
        for (size_t j = i + 1; j < 5; j++) pair(rarities[i], rarities[j]);
    }
    for (size_t i = 0; i < 3; i++) {
        for (size_t j = i + 1; j < 3; j++) pair(meanings[i], meanings[j]);
    }
    pair(LookedFor, Good);
    return worst;
}

namespace {
// r/223 of the way from the background to the words in red, as Bedrock's
// own inks lie (160 between 15 and 238), g/223 in green, b/222 in blue
Rgb between(const Scheme& scheme, int r, int g, int b) {
    const Rgb bg = scheme[Background], text = scheme[Text];
    return rgb((int)std::lround(redOf(bg) + (redOf(text) - redOf(bg)) * r / 223.0),
               (int)std::lround(greenOf(bg) + (greenOf(text) - greenOf(bg)) * g / 223.0),
               (int)std::lround(blueOf(bg) + (blueOf(text) - blueOf(bg)) * b / 222.0));
}
}  // namespace

Rgb dimInk(const Scheme& scheme) { return between(scheme, 145, 150, 157); }
Rgb faintInk(const Scheme& scheme) { return between(scheme, 93, 99, 108); }

double plainestDifference(const Scheme& scheme, Vision vision, Role* which) {
    static const Role coloured[] = {Legendary, Epic, Rare, Uncommon, Highlight, Good, Caution, Bad, LookedFor};
    const Rgb plain[3] = {seenWith(scheme[Text], vision), seenWith(dimInk(scheme), vision),
                          seenWith(faintInk(scheme), vision)};
    double nearest = 1e9;
    for (Role r : coloured) {
        const Rgb seen = seenWith(scheme[r], vision);
        for (Rgb p : plain) {
            const double d = difference(seen, p);
            if (d < nearest) {
                nearest = d;
                if (which) *which = r;
            }
        }
    }
    return nearest;
}

}  // namespace Colours
}  // namespace SCMiner
