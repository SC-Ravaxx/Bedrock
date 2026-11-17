#include "RockScans.h"

#include "MiningAdvice.h"
#include "MiningHardware.h"
#include "MiningLocations.h"
#include "UexReports.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <random>
#include <sstream>

namespace SCMiner {
namespace RockScans {

bool notScannedYet(const RockStats& rock) {
    if (rock.ore.empty() || rock.ore == "Ice") return false;
    if (rock.mass <= 0 || rock.scu <= 0) return false;
    // both read, and both nothing
    if (rock.resistance < 0 || rock.instability < 0) return false;
    if (rock.resistance >= 0.5f || rock.instability >= 0.005f) return false;
    const float volume = rock.scu * modelOf(rock).kgPerScuAtDensityOne;
    return std::fabs(rock.mass - volume) <= (std::max)(1.5f, 0.006f * rock.mass);
}

double densestPart(const std::string& rockOre) {
    double densest = 0;
    for (const RockPart& part : ROCK_PARTS) {
        if (rockOre == part.rock) densest = (std::max)(densest, oreDensity(part.ore));
    }
    if (densest > 0) return densest;
    if (const double own = oreDensity(rockOre); own > 0) return own;
    for (const OreDensity& row : ORE_DENSITIES) densest = (std::max)(densest, row.density);
    return densest;
}

std::string impossible(const RockStats& rock) {
    if (rock.mass <= 0 || rock.scu <= 0) return "";
    // A Nyx I rock has ten times the SCU to its volume (RockModel), so the
    // largest rock's SCU and the weights per SCU go by its model
    const double perScu = modelOf(rock).kgPerScuAtDensityOne;
    const double largest = LARGEST_ROCK_SCU * (KG_PER_SCU_AT_DENSITY_ONE / perScu);
    // the panel prints the SCU to a hundredth and the mass to a kilogram
    if (rock.scu > largest + 0.08) return "more SCU than the largest rock";
    const double lightest = (rock.mass + 1.0) / (std::max)(0.001, rock.scu - 0.01);
    if (lightest < perScu * 0.99) return "lighter than its volume";
    const double heaviest = (rock.mass - 1.0) / (rock.scu + 0.01);
    if (heaviest > perScu * densestPart(rock.ore) * 1.01) return "heavier than its densest part";
    return "";
}

bool v5Fits(const RockStats& rock) {
    RockStats usual = rock, v5 = rock;
    usual.v5 = false;
    v5.v5 = true;
    return impossible(usual) == "lighter than its volume" && impossible(v5).empty();
}

double usualKgPerScu(const RockStats& rock) {
    if (rock.mass <= 0 || rock.scu <= 0) return 0.0;
    return rock.mass / rock.scu * (KG_PER_SCU_AT_DENSITY_ONE / modelOf(rock).kgPerScuAtDensityOne);
}

std::string LookCheck::notTaken(const RockStats& rock) {
    if (notScannedYet(rock)) return "not scanned yet";
    const std::string why = impossible(rock);
    if (why.empty()) {
        looks_ = 0;
        return "";
    }
    if (looks_ > 0 && rock.mass == mass_ && rock.scu == scu_) {
        looks_++;
    } else {
        mass_ = rock.mass;
        scu_ = rock.scu;
        looks_ = 1;
    }
    return looks_ >= 8 ? std::string() : why;
}

bool oneNumeralApart(float a, float b, int places) {
    if (a <= 0 || b <= 0) return false;
    char x[32], y[32];
    std::snprintf(x, sizeof(x), "%.*f", places, a);
    std::snprintf(y, sizeof(y), "%.*f", places, b);
    const size_t n = std::strlen(x);
    if (n != std::strlen(y)) return false;
    int differ = 0;
    for (size_t i = 0; i < n; i++) differ += x[i] != y[i] ? 1 : 0;
    return differ <= 1;
}

bool anotherRock(const RockStats& held, const RockStats& look) {
    if (held.mass <= 0 || look.mass <= 0) return false;
    if (std::fabs(held.mass - look.mass) <= 1.0f || oneNumeralApart(held.mass, look.mass, 0)) return false;
    // ...and another figure: each where both have it
    const bool scus = held.scu > 0 && look.scu > 0;
    const bool instabilities = held.instability > 0 && look.instability > 0;
    const bool resistances =
        held.resistance >= 0 && look.resistance >= 0 && held.resistanceAdjusted == look.resistanceAdjusted;
    if (scus && std::fabs(held.scu - look.scu) > 0.011f && !oneNumeralApart(held.scu, look.scu, 2)) return true;
    if (instabilities && std::fabs(held.instability - look.instability) > 0.011f &&
        !oneNumeralApart(held.instability, look.instability, 2)) {
        return true;
    }
    // (a resistance of 0 is a reading: one numeral off it is 1 to 9)
    if (resistances && std::fabs(held.resistance - look.resistance) > 0.5f &&
        !oneNumeralApart(held.resistance + 1000, look.resistance + 1000, 0)) {
        return true;
    }
    if (scus || instabilities || resistances) return false;
    return std::fabs(look.mass - held.mass) > held.mass * 0.25f;
}

void keepScu(const RockStats& held, RockStats& look) {
    if (look.scu > 0 || held.scu <= 0 || held.mass <= 0 || look.mass <= 0) return;
    if (std::fabs(held.mass - look.mass) > 1.0f) return;
    look.scu = held.scu;
}

bool numeralOff(float a, float b) {
    if (a <= 0 || b <= 0 || std::fabs(a - b) <= 1.0f) return false;
    if (oneNumeralApart(a, b, 0)) return true;
    char x[32], y[32];
    std::snprintf(x, sizeof(x), "%.0f", a);
    std::snprintf(y, sizeof(y), "%.0f", b);
    std::string longer = x, shorter = y;
    if (longer.size() < shorter.size()) std::swap(longer, shorter);
    if (longer.size() != shorter.size() + 1) return false;
    for (size_t i = 0; i < longer.size(); i++) {
        if (longer.substr(0, i) + longer.substr(i + 1) == shorter) return true;
    }
    return false;
}

bool MassCheck::takes(const RockStats& shown, const RockStats& look) {
    if (!numeralOff(shown.mass, look.mass)) {
        waiting_ = 0;
        return true;
    }
    if (waiting_ > 0 && std::fabs(waiting_ - look.mass) <= 1.0f) {
        waiting_ = 0;
        return true;
    }
    waiting_ = look.mass;
    return false;
}

static int hundredths(float scu) {
    return (int)std::lround(scu * 100.0f);
}

bool Recorder::ofMass(const Rock& rock, float mass) {
    for (const auto& seen : rock.masses) {
        if (std::fabs((float)seen.first - mass) <= 1.0f) return true;
    }
    return false;
}

bool Recorder::sameRock(const Rock& rock, const RockStats& shown) {
    if (rock.scan.ore != shown.ore) return false;
    if (ofMass(rock, shown.mass)) return true;
    if (!oneNumeralApart(rock.scan.mass, shown.mass, 0)) return false;
    for (const auto& seen : rock.shown) {
        if (seen.first == hundredths(shown.scu)) return true;
    }
    return false;
}

void Recorder::taken(const RockStats& shown, const RockStats& look, long long observed, const std::string& ship,
                     const std::string& place, int rows, float labelScale) {
    if (shown.ore.empty() || shown.mass <= 0 || shown.scu <= 0) return;
    release(observed);
    if (!open_ || !sameRock(rock_, shown)) {
        gone();
        // a rock still waiting is the same rock, back on view
        const auto back =
            std::find_if(waiting_.rbegin(), waiting_.rend(), [&](const Rock& r) { return sameRock(r, shown); });
        if (back != waiting_.rend()) {
            rock_ = *back;
            waiting_.erase(std::next(back).base());
        } else {
            rock_ = Rock();
            rock_.scan.observed = observed;
            rock_.scan.ore = shown.ore;
            rock_.scan.mass = shown.mass;
            rock_.scan.scu = shown.scu;
            rock_.scan.ship = ship;
            rock_.scan.place = place;
            rock_.scan.v5 = shown.v5;
        }
        open_ = true;
    }
    Scan& scan = rock_.scan;
    rock_.last = observed;
    // the rock's own resistance, printed white, over one a laser changed
    if (shown.resistance >= 0 && (scan.resistance < 0 || (scan.inColour && !shown.resistanceAdjusted))) {
        scan.resistance = shown.resistance;
        scan.inColour = shown.resistanceAdjusted;
    }
    if (scan.instability < 0 && look.instability >= 0) scan.instability = (std::max)(0.0f, shown.instability);
    if (scan.rangeMetres <= 0 && shown.distance > 0) scan.rangeMetres = (int)std::lround(shown.distance);
    if (rows > 0) scan.rows = rows;
    if (scan.labelScale <= 0 && labelScale > 0) scan.labelScale = labelScale;
    rock_.masses[(int)std::lround(shown.mass)]++;
    rock_.shown[hundredths(shown.scu)]++;
    if (look.scu > 0 && look.mass > 0 && ofMass(rock_, look.mass)) rock_.read[hundredths(look.scu)]++;
}

void Recorder::listed(float mass, const std::vector<ScanPart>& parts) {
    if (parts.empty()) return;
    if (open_ && ofMass(rock_, mass)) {
        rock_.scan.parts = parts;
        return;
    }
    for (auto it = waiting_.rbegin(); it != waiting_.rend(); ++it) {
        if (ofMass(*it, mass)) {
            it->scan.parts = parts;
            return;
        }
    }
}

void Recorder::tried(float mass) {
    if (open_ && ofMass(rock_, mass)) {
        rock_.scan.listTried = true;
        return;
    }
    for (auto it = waiting_.rbegin(); it != waiting_.rend(); ++it) {
        if (ofMass(*it, mass)) {
            it->scan.listTried = true;
            return;
        }
    }
}

void Recorder::gone() {
    if (!open_) return;
    open_ = false;
    waiting_.push_back(rock_);
}

void Recorder::release(long long now) {
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        if (now - it->last >= HOLD_SECONDS) {
            finish(*it);
            it = waiting_.erase(it);
        } else {
            ++it;
        }
    }
}

void Recorder::finish(Rock& rock) {
    Scan& scan = rock.scan;
    // The mass the view showed most, the first on a tie
    int mostMass = 0;
    for (const auto& seen : rock.masses) mostMass = (std::max)(mostMass, seen.second);
    const auto first = rock.masses.find((int)std::lround(scan.mass));
    if (first == rock.masses.end() || first->second < mostMass) {
        for (const auto& seen : rock.masses) {
            if (seen.second == mostMass) {
                scan.mass = (float)seen.first;
                break;
            }
        }
    }
    // The SCU most looks read with the rock's mass; where none did, the one
    // the view showed most. A tie goes to the one the view showed more.
    const std::map<int, int>& votes = rock.read.empty() ? rock.shown : rock.read;
    int best = 0, most = 0, mostShown = 0;
    for (const auto& [scu, looks] : votes) {
        const auto shown = rock.shown.find(scu);
        const int onView = shown == rock.shown.end() ? 0 : shown->second;
        if (looks > most || (looks == most && onView > mostShown)) {
            best = scu;
            most = looks;
            mostShown = onView;
        }
    }
    if (best > 0) scan.scu = best / 100.0f;
    // A rock back on view after its wait counts once, its SCU read one
    // numeral off or not (39.56 and 39.58 for one 54,501 kg rock) - a second
    // time only with a list it did not have the first time
    const bool listedNow = !scan.parts.empty();
    for (Counted& seen : counted_) {
        if (seen.ore != scan.ore || std::fabs(seen.mass - scan.mass) > 1.0f) continue;
        if (std::fabs(seen.scu - scan.scu) > 0.011f && !oneNumeralApart(seen.scu, scan.scu, 2)) continue;
        if (seen.listed || !listedNow) return;
        seen.listed = true;
        done_.push_back(scan);
        return;
    }
    counted_.push_back({scan.ore, scan.mass, scan.scu, listedNow});
    done_.push_back(scan);
}

std::vector<Scan> Recorder::done(long long now) {
    release(now);
    std::vector<Scan> out;
    out.swap(done_);
    std::stable_sort(out.begin(), out.end(), [](const Scan& a, const Scan& b) { return a.observed < b.observed; });
    return out;
}

static std::string listKey(const std::vector<ScanPart>& parts) {
    std::string key;
    char text[96];
    for (const ScanPart& part : parts) {
        std::snprintf(text, sizeof(text), "%s:%.2f:%d;", part.inert ? "-" : part.ore.c_str(), part.percent,
                      part.quality);
        key += text;
    }
    return key;
}

bool ListReadings::confirms(float mass, const std::vector<ScanPart>& parts) {
    if (mass <= 0 || parts.empty()) return false;
    const std::string key = listKey(parts);
    for (Kept& kept : kept_) {
        if (std::fabs(kept.mass - mass) > 1.0f && !oneNumeralApart(kept.mass, mass, 0)) continue;
        if (kept.key == key) return true;
        // another reading of that rock's list: the next has to say this one
        kept.key = key;
        return false;
    }
    kept_.push_back({mass, key});
    if (kept_.size() > 8) kept_.erase(kept_.begin());
    return false;
}

std::string shipKind(const std::string& logShip) {
    std::string lower;
    for (char c : logShip) lower += (char)std::tolower((unsigned char)c);
    for (const char* kind : {"prospector", "mole", "golem"}) {
        if (lower.find(kind) != std::string::npos) return kind;
    }
    return "";
}

std::string partsText(const std::vector<ScanPart>& parts) {
    std::string out;
    char text[96];
    for (const ScanPart& part : parts) {
        if (part.inert) std::snprintf(text, sizeof(text), "inert %.2f", part.percent);
        else std::snprintf(text, sizeof(text), "%s %.2f %d", part.ore.c_str(), part.percent, part.quality);
        out += (out.empty() ? "" : "; ") + std::string(text);
    }
    return out;
}

const char* const CSV_HEADER = "time,ore,mass,scu,res,res_printed,inst,range_m,ship,place,parts";

std::string csvLine(const Scan& scan, const std::string& time) {
    // a name may hold a comma: quoted, and a quote inside made an apostrophe
    auto quoted = [](std::string text) {
        for (char& c : text) {
            if (c == '"') c = '\'';
            if (c == '\n' || c == '\r') c = ' ';
        }
        return "\"" + text + "\"";
    };
    char text[64];
    std::string line = time + "," + quoted(scan.ore);
    std::snprintf(text, sizeof(text), ",%.0f,%.2f,", scan.mass, scan.scu);
    line += text;
    if (scan.resistance >= 0) {
        std::snprintf(text, sizeof(text), "%.0f,%s", scan.resistance, scan.inColour ? "colour" : "white");
        line += text;
    } else {
        line += ",";
    }
    line += ",";
    if (scan.instability >= 0) {
        std::snprintf(text, sizeof(text), "%.2f", scan.instability);
        line += text;
    }
    return line + "," + std::to_string(scan.rangeMetres) + "," + quoted(scan.ship) + "," + quoted(scan.place) + "," +
           quoted(partsText(scan.parts));
}

std::string newSession() {
    std::random_device device;
    std::mt19937_64 draw(((std::uint64_t)device() << 32) ^ device());
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)draw());
    return text;
}

bool sendable(const Scan& scan) {
    // The service weighs every rock as made of the usual numbers; a Nyx I
    // rock would read there as a tenth of its weight
    if (scan.v5) return false;
    if (oreDensity(scan.ore) <= 0) return false;   // a ship ore
    if (scan.mass < 1 || scan.mass > 1000000 || scan.scu < 0.01f || scan.scu > 100) return false;
    if (scan.resistance > 100 || scan.instability > 100000) return false;
    if (scan.rangeMetres < 0 || scan.rangeMetres > 2000) return false;
    if (scan.parts.size() > 8) return false;
    for (const ScanPart& part : scan.parts) {
        if (part.percent < 0 || part.percent > 100) return false;
        if (!part.inert && (oreDensity(part.ore) <= 0 || part.quality < 0 || part.quality > 1000)) return false;
    }
    return true;
}

// A place as the service takes it: letters, digits, spaces and . ' ( ) -,
// up to 64, beginning with a letter or digit
static bool placeTaken(const std::string& place) {
    if (place.empty() || place.size() > 64 || !std::isalnum((unsigned char)place[0])) return false;
    for (char c : place) {
        if (!std::isalnum((unsigned char)c) && c != ' ' && c != '.' && c != '\'' && c != '(' && c != ')' && c != '-') {
            return false;
        }
    }
    return true;
}

std::string channelKind(const std::string& channel) {
    // "LIVE + HOTFIX" is LIVE's
    const std::string kind = channel.rfind("LIVE", 0) == 0 ? std::string("LIVE") : channel;
    if (kind == "LIVE" || kind == "PTU" || kind == "EPTU" || kind == "TECH-PREVIEW" || kind == "HOTFIX") return kind;
    return "";
}

std::string body(const std::vector<Scan>& scans, const std::string& bedrockVersion, int gameVersion,
                 const std::string& channel, const std::string& session) {
    using UexReports::jsonText;
    std::ostringstream out;
    out << "{\"v\":1,\"bedrock\":" << jsonText(bedrockVersion);
    if (gameVersion > 0) {
        char game[32];
        std::snprintf(game, sizeof(game), "%d.%d.%d", gameVersion / 10000, gameVersion / 100 % 100, gameVersion % 100);
        out << ",\"game\":" << jsonText(game);
    }
    const std::string kind = channelKind(channel);
    if (!kind.empty()) out << ",\"channel\":" << jsonText(kind);
    out << ",\"session\":" << jsonText(session) << ",\"rocks\":[";
    char text[96];
    for (size_t i = 0; i < scans.size(); i++) {
        const Scan& s = scans[i];
        out << (i ? "," : "") << "{\"observed\":" << s.observed << ",\"ore\":" << jsonText(s.ore);
        std::snprintf(text, sizeof(text), ",\"mass\":%.0f,\"scu\":%.2f", s.mass, s.scu);
        out << text;
        if (s.resistance >= 0) {
            std::snprintf(text, sizeof(text), ",\"resistance\":%.0f,\"printed\":\"%s\"", s.resistance,
                          s.inColour ? "colour" : "white");
        } else {
            std::snprintf(text, sizeof(text), ",\"resistance\":-1");
        }
        out << text;
        if (s.instability >= 0) std::snprintf(text, sizeof(text), ",\"instability\":%.2f", s.instability);
        else std::snprintf(text, sizeof(text), ",\"instability\":-1");
        out << text << ",\"range\":" << s.rangeMetres;
        if (!s.ship.empty()) out << ",\"ship\":" << jsonText(s.ship);
        if (placeTaken(s.place)) out << ",\"place\":" << jsonText(s.place);
        if (s.rows > 0 && s.rows <= 10000) {
            std::snprintf(text, sizeof(text), ",\"rows\":%d,\"label\":%.2f,\"list\":\"%s\"", s.rows,
                          s.labelScale > 0 && s.labelScale <= 5 ? s.labelScale : 0.0f,
                          !s.parts.empty() ? "read" : s.listTried ? "tried" : "none");
            out << text;
        }
        out << ",\"parts\":[";
        for (size_t p = 0; p < s.parts.size(); p++) {
            const ScanPart& part = s.parts[p];
            if (part.inert) {
                std::snprintf(text, sizeof(text), "{\"inert\":true,\"percent\":%.2f}", part.percent);
                out << (p ? "," : "") << text;
            } else {
                std::snprintf(text, sizeof(text), ",\"percent\":%.2f,\"quality\":%d}", part.percent, part.quality);
                out << (p ? "," : "") << "{\"ore\":" << jsonText(part.ore) << text;
            }
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

}  // namespace RockScans
}  // namespace SCMiner
