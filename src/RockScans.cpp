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
    const float volume = rock.scu * KG_PER_SCU_AT_DENSITY_ONE;
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
    // the panel prints the SCU to a hundredth and the mass to a kilogram
    if (rock.scu > LARGEST_ROCK_SCU + 0.08f) return "more SCU than the largest rock";
    const double lightest = (rock.mass + 1.0) / (std::max)(0.001, rock.scu - 0.01);
    if (lightest < KG_PER_SCU_AT_DENSITY_ONE * 0.99) return "lighter than its volume";
    const double heaviest = (rock.mass - 1.0) / (rock.scu + 0.01);
    if (heaviest > KG_PER_SCU_AT_DENSITY_ONE * densestPart(rock.ore) * 1.01) return "heavier than its densest part";
    return "";
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

static bool sameRock(const Scan& scan, const RockStats& rock) {
    return scan.ore == rock.ore && std::fabs(scan.mass - rock.mass) <= 1.0f && std::fabs(scan.scu - rock.scu) <= 0.011f;
}

void Recorder::taken(const RockStats& shown, const RockStats& look, long long observed, const std::string& ship,
                     const std::string& place) {
    if (shown.ore.empty() || shown.mass <= 0 || shown.scu <= 0) return;
    const float instability = look.instability >= 0 ? (std::max)(0.0f, shown.instability) : -1.0f;
    if (open_ && sameRock(scan_, shown)) {
        // the rock's own resistance, printed white, over one a laser changed
        if (shown.resistance >= 0 && (scan_.resistance < 0 || (scan_.inColour && !shown.resistanceAdjusted))) {
            scan_.resistance = shown.resistance;
            scan_.inColour = shown.resistanceAdjusted;
        }
        if (scan_.instability < 0) scan_.instability = instability;
        if (scan_.rangeMetres <= 0 && shown.distance > 0) scan_.rangeMetres = (int)std::lround(shown.distance);
        return;
    }
    gone();
    open_ = true;
    scan_ = Scan();
    scan_.observed = observed;
    scan_.ore = shown.ore;
    scan_.mass = shown.mass;
    scan_.scu = shown.scu;
    scan_.resistance = shown.resistance >= 0 ? shown.resistance : -1.0f;
    scan_.inColour = scan_.resistance >= 0 && shown.resistanceAdjusted;
    scan_.instability = instability;
    scan_.rangeMetres = shown.distance > 0 ? (int)std::lround(shown.distance) : 0;
    scan_.ship = ship;
    scan_.place = place;
}

void Recorder::listed(float mass, const std::vector<ScanPart>& parts) {
    if (open_ && !parts.empty() && std::fabs(scan_.mass - mass) <= 1.0f) scan_.parts = parts;
}

void Recorder::gone() {
    if (!open_) return;
    open_ = false;
    char key[96];
    std::snprintf(key, sizeof(key), "%s|%.0f|%.2f", scan_.ore.c_str(), scan_.mass, scan_.scu);
    const bool listedNow = !scan_.parts.empty();
    const auto it = counted_.find(key);
    if (it != counted_.end() && (it->second || !listedNow)) return;
    counted_[key] = listedNow;
    done_.push_back(scan_);
}

std::vector<Scan> Recorder::done() {
    std::vector<Scan> out;
    out.swap(done_);
    return out;
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
    // "LIVE + HOTFIX" is LIVE's
    std::string kind = channel.rfind("LIVE", 0) == 0 ? std::string("LIVE") : channel;
    if (kind == "LIVE" || kind == "PTU" || kind == "EPTU" || kind == "TECH-PREVIEW" || kind == "HOTFIX") {
        out << ",\"channel\":" << jsonText(kind);
    }
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
