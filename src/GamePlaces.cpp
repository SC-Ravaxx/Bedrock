#include "GamePlaces.h"
#include "MiningLocations.h"
#include <cctype>
#include <cstring>
#include <regex>
#include <vector>

namespace SCMiner {
namespace {

std::string trimmed(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool startsWith(std::string_view s, std::string_view head) {
    return s.size() >= head.size() && sameName(s.substr(0, head.size()), head);
}

// The named places. Where the ore tables list the place, its spelling
// there; where they do not (a city, a station, a gas giant), no ore
// location - the filter then falls back to the system.
// surface: 1 on the body (a city counts - there is ground under it), 0 in
// space. On a body the 2,000 family of signatures is harvestables as much
// as salvage; out in space it is salvage alone.
struct Named { const char* name; const char* system; const char* ore; int surface; };
const Named NAMED[] = {
    // Stanton: planets and moons
    {"Hurston", "Stanton", "Hurston", 1},       {"Aberdeen", "Stanton", "Aberdeen", 1},
    {"Arial", "Stanton", "Arial", 1},           {"Ita", "Stanton", "Ita", 1},
    {"Magda", "Stanton", "Magda", 1},           {"Crusader", "Stanton", "", 0},
    {"Cellin", "Stanton", "Cellin", 1},         {"Daymar", "Stanton", "Daymar", 1},
    {"Yela", "Stanton", "Yela", 1},             {"ArcCorp", "Stanton", "", 1},
    {"Lyria", "Stanton", "Lyria", 1},           {"Wala", "Stanton", "Wala", 1},
    {"microTech", "Stanton", "microTech", 1},   {"Calliope", "Stanton", "Calliope", 1},
    {"Clio", "Stanton", "Clio", 1},             {"Euterpe", "Stanton", "Euterpe", 1},
    // Stanton: cities and stations. Grim HEX sits in Yela's belt, where
    // the rocks are; the rest have nothing to mine within reach.
    {"Lorville", "Stanton", "", 1},             {"Area18", "Stanton", "", 1},
    {"Orison", "Stanton", "", 1},               {"New Babbage", "Stanton", "", 1},
    {"Everus Harbor", "Stanton", "", 0},        {"Seraphim Station", "Stanton", "", 0},
    {"Port Tressler", "Stanton", "", 0},        {"Baijini Point", "Stanton", "", 0},
    {"Grim HEX", "Stanton", "Yela Asteroid Belt", 0},
    {"Aaron Halo", "Stanton", "Aaron Halo", 0}, {"Yela Asteroid Belt", "Stanton", "Yela Asteroid Belt", 0},
    // Pyro: the ore tables name the bodies by their number
    {"Pyro I", "Pyro", "Pyro I", 1},            {"Monox", "Pyro", "Pyro II (Monox)", 1},
    {"Bloom", "Pyro", "Pyro III (Bloom)", 1},   {"Pyro IV", "Pyro", "Pyro IV", 1},
    {"Pyro V", "Pyro", "", 0},                  {"Ignis", "Pyro", "Pyro V-a (Ignis)", 1},
    {"Vatra", "Pyro", "Pyro V-b (Vatra)", 1},   {"Adir", "Pyro", "Pyro V-c (Adir)", 1},
    {"Fairo", "Pyro", "Pyro V-d (Fairo)", 1},   {"Fuego", "Pyro", "Pyro V-e (Fuego)", 1},
    {"Vuur", "Pyro", "Pyro V-f (Vuur)", 1},     {"Terminus", "Pyro", "Pyro VI (Terminus)", 1},
    {"Checkmate", "Pyro", "", 0},               {"Ruin Station", "Pyro", "", 0},
    {"Orbituary", "Pyro", "", 0},               {"Gaslight", "Pyro", "", 0},
    {"Patch City", "Pyro", "", 0},              {"Rod's Fuel 'N Supplies", "Pyro", "", 0},
    {"Dudley & Daughters", "Pyro", "", 0},      {"Starlight Service Station", "Pyro", "", 0},
    {"Megumi Refueling", "Pyro", "", 0},        {"Endgame", "Pyro", "", 0},
    {"Rustbucket", "Pyro", "", 0},              {"Shepherd's Rest", "Pyro", "", 0},
    {"Terminus Ring", "Pyro", "Terminus Ring", 0},
    // ...and its asteroid clusters at the Lagrange points, the RABs and the
    // RMBs, which the tables have under one heading: one entry for them all,
    // for picking by hand. The log names them otherwise - "PYR2 L4",
    // "Asteroid Cluster" - see below.
    {"Asteroid clusters", "Pyro", "Pyro Deep Space Asteroids", 0},
    // Nyx
    {"Levski", "Nyx", "", 1},                   {"Delamar", "Nyx", "", 1},
    {"QV Breaker Station", "Nyx", "", 0},       {"Glaciem Ring", "Nyx", "Glaciem Ring", 0},
    {"Keeger Belt", "Nyx", "Keeger Belt", 0},
};

// "HUR-L1", "HUR L1", "HUR-L1 Green Glade Station" -> the ore tables'
// Lagrange group, read off their own notes ("HUR-L1, HUR-L4" under
// "Lagrange A"), so the data stays the one authority
GamePlace lagrange(std::string_view name) {
    static const std::regex form(R"(^(HUR|CRU|ARC|MIC)[- ]?L([1-5])\b)", std::regex::icase);
    std::cmatch m;
    const std::string text(name);
    if (!std::regex_search(text.c_str(), m, form)) return {};
    std::string key = m[1].str();
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    key += "-L" + m[2].str();
    GamePlace place{key, "Stanton", "", 0};
    for (const auto& row : ORE_LOCATIONS) {
        if (std::strcmp(row.kind, "lagrange") != 0 || std::strcmp(row.system, "Stanton") != 0) continue;
        // the note lists the points: "HUR-L1, HUR-L4"
        const std::string note = row.note;
        size_t at = 0;
        while ((at = note.find(key, at)) != std::string::npos) {
            const size_t end = at + key.size();
            if (end == note.size() || note[end] == ',' || note[end] == ' ') {
                place.oreLocation = row.location;
                return place;
            }
            at = end;
        }
    }
    return place;
}

// Pyro's Lagrange points hold asteroid clusters, and the ore tables put
// every Pyro cluster, RAB and RMB under one heading
const char* const PYRO_CLUSTERS = "Pyro Deep Space Asteroids";

std::string systemOfAbbreviation(std::string_view s) {
    if (sameName(s, "stan") || sameName(s, "stanton")) return "Stanton";
    if (sameName(s, "pyro")) return "Pyro";
    if (sameName(s, "nyx")) return "Nyx";
    if (sameName(s, "castra")) return "Castra";
    if (sameName(s, "magnus")) return "Magnus";
    if (sameName(s, "terra")) return "Terra";
    return "";
}

// "rs_ext_nyx-pyro_jp1" / "LOC_rs_ext_stan-magnus_jp1" -> (from, to)
bool jumpPoint(std::string_view name, std::string* from, std::string* to) {
    static const std::regex form(R"(rs_ext_([a-z]+)-([a-z]+)_jp)", std::regex::icase);
    std::cmatch m;
    const std::string text(name);
    if (!std::regex_search(text.c_str(), m, form)) return false;
    const std::string a = systemOfAbbreviation(m[1].str()), b = systemOfAbbreviation(m[2].str());
    if (a.empty() || b.empty()) return false;
    if (from) *from = a;
    if (to) *to = b;
    return true;
}

}  // namespace

const std::vector<GamePlace>& minablePlaces() {
    static const std::vector<GamePlace> LIST = [] {
        std::vector<GamePlace> out;
        for (const Named& n : NAMED) {
            if (!n.ore || !*n.ore) continue;
            out.push_back({n.name, n.system, n.ore, n.surface});
        }
        // ...and Stanton's Lagrange points, each with its group of the
        // tables (HUR-L3 is Lagrange E), so one can be picked by hand
        for (const char* planet : {"HUR", "CRU", "ARC", "MIC"}) {
            for (int point = 1; point <= 5; point++) {
                const GamePlace place = lagrange(std::string(planet) + "-L" + std::to_string(point));
                if (place.known() && !place.oreLocation.empty()) out.push_back(place);
            }
        }
        return out;
    }();
    return LIST;
}

std::string oreLocationOf(std::string_view placeName) {
    for (const auto& n : NAMED) {
        if (sameName(placeName, n.name)) return n.ore;
    }
    return "";
}

GamePlace placeByLogName(std::string_view logName, std::string_view systemHint) {
    const std::string name = trimmed(logName);
    if (name.empty() || name == "<= UNINITIALIZED =>") return {};
    // The system itself, out in space: no place
    for (const char* sys : {"Stanton", "Pyro", "Nyx"}) {
        if (sameName(name, sys) || sameName(name, std::string(sys) + " System")) return {};
    }
    for (const auto& n : NAMED) {
        if (sameName(name, n.name)) return {n.name, n.system, n.ore, n.surface};
    }
    if (GamePlace lg = lagrange(name); lg.known()) return lg;
    // "PYR2 L4", "PYR2-L4": a Pyro Lagrange point
    {
        static const std::regex form(R"(^PYR([1-6])[- ]?L([1-5])\b)", std::regex::icase);
        std::cmatch m;
        if (std::regex_search(name.c_str(), m, form)) {
            return {"PYR" + m[1].str() + " L" + m[2].str(), "Pyro", PYRO_CLUSTERS, 0};
        }
    }
    // Remote asteroid / mining bases: "RAB-CHARLIE", "RMB-..."
    if (startsWith(name, "RAB-") || startsWith(name, "RMB-")) return {name, "Pyro", PYRO_CLUSTERS, 0};
    if (startsWith(name, "People's Service Station")) return {name, "Nyx", "", 0};
    if (sameName(name, "Asteroid Cluster")) {
        // One in every system; only Pyro's share one table
        const std::string sys = trimmed(systemHint);
        return {name, sys, sameName(sys, "Pyro") ? PYRO_CLUSTERS : "", 0};
    }
    // "Stanton Gateway" stands in Pyro AND in Nyx: named for where it leads
    if (name.size() > 8 && sameName(name.substr(name.size() - 8), " Gateway")) return {name, "", "", 0};
    // "Stanton-Pyro Jump Point", "Nyx - Castra Jump Point": the near side
    {
        static const std::regex form(R"(^([A-Za-z]+)\s*-\s*([A-Za-z]+) Jump Point)", std::regex::icase);
        std::cmatch m;
        if (std::regex_search(name.c_str(), m, form)) {
            return {name, systemOfAbbreviation(m[1].str()), "", 0};
        }
    }
    // Known by the ore tables under this very name?
    for (const auto& row : ORE_LOCATIONS) {
        if (sameName(name, row.location)) {
            const bool body = std::strcmp(row.kind, "planet") == 0 || std::strcmp(row.kind, "moon") == 0 ||
                              std::strcmp(row.kind, "cave") == 0;
            return {row.location, row.system, row.location, body ? 1 : 0};
        }
    }
    // A name the app has never heard of: shown as it is, system unknown
    return {name, "", "", -1};
}

// A terminal opened - "<RequestLocationInventory> Player[...] requested
// inventory for Location[RR_JP_StantonPyro]" - and the place it stands in.
// The codes seen in game logs: the cities as system, body and name
// (Nyx_Levski, Stanton1_Lorville), Grim HEX, the planets' stations in low
// orbit (RR_HUR_LEO), the rest stops at Lagrange points (RR_HUR_L1,
// RR_P2_L4), the gateways (RR_JP_StantonPyro: in Stanton, to Pyro) - and
// outposts, which say nothing here. Pyro's rest stops by the point they
// stand at, as UEX lists them ("Monox Lagrange Point 4" is Checkmate,
// Ruin Station orbits Terminus).
GamePlace placeByLocationCode(std::string_view code, std::string_view systemHint) {
    const std::string name = trimmed(code);
    if (name.empty() || sameName(name, "INVALID_LOCATION_ID")) return {};
    std::cmatch m;
    {
        static const std::regex form(R"(^RR_JP_([A-Z][a-z]+)([A-Z][a-z]+)$)");
        if (std::regex_match(name.c_str(), m, form)) {
            const std::string from = systemOfAbbreviation(m[1].str()), to = systemOfAbbreviation(m[2].str());
            if (from.empty() || to.empty()) return {};
            return {to + " Gateway", from, "", 0};
        }
    }
    {
        static const std::regex form(R"(^RR_(HUR|CRU|ARC|MIC)_(LEO|L([1-5]))$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, form)) {
            std::string planet = m[1].str();
            for (char& c : planet) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (m[3].matched) return placeByLogName(planet + "-L" + m[3].str(), systemHint);
            static const std::pair<const char*, const char*> LOW_ORBIT[] = {{"HUR", "Everus Harbor"},
                                                                            {"CRU", "Seraphim Station"},
                                                                            {"ARC", "Baijini Point"},
                                                                            {"MIC", "Port Tressler"}};
            for (const auto& [at, station] : LOW_ORBIT) {
                if (planet == at) return placeByLogName(station, systemHint);
            }
            return {};
        }
    }
    {
        static const std::regex form(R"(^RR_P([1-6])_(LEO|L([1-5]))$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, form)) {
            struct Stop { int planet; const char* point; const char* name; };
            static const Stop STOPS[] = {{2, "L4", "Checkmate"},          {3, "L1", "Starlight Service Station"},
                                         {3, "L3", "Patch City"},         {3, "LEO", "Orbituary"},
                                         {5, "L2", "Gaslight"},           {5, "L4", "Rod's Fuel 'N Supplies"},
                                         {5, "L5", "Rat's Nest"},         {6, "L3", "Endgame"},
                                         {6, "L4", "Dudley & Daughters"}, {6, "L5", "Megumi Refueling"},
                                         {6, "LEO", "Ruin Station"}};
            const int planet = m[1].str()[0] - '0';
            for (const Stop& s : STOPS) {
                if (s.planet == planet && sameName(m[2].str(), s.point)) {
                    const GamePlace known = placeByLogName(s.name, systemHint);
                    return {known.known() ? known.name : std::string(s.name), "Pyro", known.oreLocation,
                            known.surface < 0 ? 0 : known.surface};
                }
            }
            // a point no station of UEX's is known at: the point itself
            if (m[3].matched) return placeByLogName("PYR" + m[1].str() + " L" + m[3].str(), systemHint);
            return {};
        }
    }
    {
        static const std::regex form(R"(^(?:Stanton|Pyro|Nyx)[0-9]*[a-z]?_(.+)$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, form)) {
            const std::string rest = m[1].str();
            static const std::pair<const char*, const char*> CITIES[] = {{"Levski", "Levski"},
                                                                         {"Lorville", "Lorville"},
                                                                         {"Area18", "Area18"},
                                                                         {"Orison", "Orison"},
                                                                         {"NewBabbage", "New Babbage"}};
            for (const auto& [code, city] : CITIES) {
                if (sameName(rest, code)) return placeByLogName(city, systemHint);
            }
            return {};
        }
    }
    if (startsWith(name, "GrimHEX")) return placeByDestination(name, systemHint);
    return {};
}

GamePlace placeByDestination(std::string_view internalName, std::string_view systemHint) {
    const std::string name = trimmed(internalName);
    if (name.empty()) return {};
    std::cmatch m;
    // pyro2, pyro5c: Pyro's bodies by number and moon letter
    {
        static const std::regex form(R"(^pyro([1-6])([a-f]?)$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, form)) {
            const int planet = m[1].str()[0] - '0';
            const std::string moon = m[2].str();
            if (planet == 5 && !moon.empty()) {
                static const char* const MOONS[] = {"Ignis", "Vatra", "Adir", "Fairo", "Fuego", "Vuur"};
                return placeByLogName(MOONS[std::tolower(static_cast<unsigned char>(moon[0])) - 'a'], systemHint);
            }
            static const char* const PLANETS[] = {"Pyro I", "Monox", "Bloom", "Pyro IV", "Pyro V", "Terminus"};
            return placeByLogName(PLANETS[planet - 1], systemHint);
        }
    }
    // OOC_Stanton2_L2: a Lagrange point itself, out by its asteroids rather
    // than at its station
    {
        static const std::regex form(R"(^OOC_Stanton_?([1-4])_L([1-5])$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, form)) {
            static const char* const PLANETS[] = {"HUR", "CRU", "ARC", "MIC"};
            return placeByLogName(std::string(PLANETS[m[1].str()[0] - '1']) + "-L" + m[2].str(), systemHint);
        }
    }
    // OOC_Stanton_2b_Daymar, OOC_Stanton_4_Microtech: the name is at the end
    {
        static const std::regex form(R"(^OOC_Stanton_?[1-4][a-d]?_([A-Za-z]+)$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, form)) return placeByLogName(m[1].str(), systemHint);
    }
    // LOC_RR_S1_L2: the rest stop at Stanton planet 1's Lagrange point 2
    {
        static const std::regex form(R"(^LOC_RR_S([1-4])_L([1-5])$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, form)) {
            static const char* const PLANETS[] = {"HUR", "CRU", "ARC", "MIC"};
            return placeByLogName(std::string(PLANETS[m[1].str()[0] - '1']) + "-L" + m[2].str(), systemHint);
        }
    }
    // rs_ext_pyro3_leo, rs_ext_cru-leo1: the station in a planet's low orbit
    // - Orbituary, Ruin Station, Seraphim Station - and no Lagrange point: in
    // the logs every arrival at one opens that station's own terminals next
    // (RR_P3_LEO, RR_P6_LEO, RR_CRU_LEO).
    {
        static const std::regex pyro(R"(^rs_ext_pyro([1-6])_leo$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, pyro)) return placeByLocationCode("RR_P" + m[1].str() + "_LEO", systemHint);
        static const std::regex stanton(R"(^rs_ext_(hur|cru|arc|mic)-leo[0-9]*$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, stanton)) return placeByLocationCode("RR_" + m[1].str() + "_LEO", systemHint);
    }
    // rs_ext_pyro2_l4, P2_L1: Pyro's Lagrange points
    {
        static const std::regex form(R"(^(?:rs_ext_pyro|P)([1-6])_l([1-5])$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, form)) {
            return placeByLogName("PYR" + m[1].str() + " L" + m[2].str(), systemHint);
        }
    }
    // rs_ext_arc-l001: Stanton's
    {
        static const std::regex form(R"(^rs_ext_(hur|cru|arc|mic)-l0*([1-5])$)", std::regex::icase);
        if (std::regex_match(name.c_str(), m, form)) {
            return placeByLogName(m[1].str() + "-L" + m[2].str(), systemHint);
        }
    }
    // Jump points: the near side, named the way the game names it. A gateway
    // is called after where it LEADS, so the one serving the Pyro jump out of
    // Nyx is "Pyro Gateway" - which is exactly how it reads in the log. The
    // system shown in front of it says which side it is; a wording such as
    // "jump to Pyro" would read as an instruction rather than a place.
    {
        std::string from, to;
        if (jumpPoint(name, &from, &to)) return {to + " Gateway", from, "", 0};
    }
    if (startsWith(name, "levski")) return placeByLogName("Levski", systemHint);
    if (startsWith(name, "GrimHEX")) return placeByLogName("Grim HEX", systemHint);
    if (startsWith(name, "ObjectContainer_Lorville")) return placeByLogName("Lorville", systemHint);
    if (startsWith(name, "Area18")) return placeByLogName("Area18", systemHint);
    if (startsWith(name, "Orison")) return placeByLogName("Orison", systemHint);
    if (startsWith(name, "NewBabbage")) return placeByLogName("New Babbage", systemHint);
    const std::string lower = [&] {
        std::string l = name;
        for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return l;
    }();
    if (lower.find("keeger") != std::string::npos) return placeByLogName("Keeger Belt", systemHint);
    if (lower.find("glaciem") != std::string::npos) return placeByLogName("Glaciem Ring", systemHint);
    if (lower.find("pyro") != std::string::npos &&
        (lower.find("cluster") != std::string::npos || lower.find("ab_mine") != std::string::npos ||
         lower.find("encounter") != std::string::npos || lower.find("regiona") != std::string::npos ||
         lower.find("regionb") != std::string::npos)) {
        return {"Asteroid Cluster", "Pyro", PYRO_CLUSTERS, 0};
    }
    // Stanton's mining bases are the Aaron Halo's (ore tables: "all Mining
    // Base locations"); its dynamic clusters could be anywhere
    if (lower.find("miningbase_stanton") != std::string::npos || startsWith(lower, "ab_mine_stanton")) {
        return placeByLogName("Aaron Halo", systemHint);
    }
    if (lower.find("cluster") != std::string::npos) {
        return {"Asteroid Cluster", trimmed(systemHint), "", 0};
    }
    // ObjectContainer_RestStop (which one?), mission beacons, nav points,
    // party markers: nothing to say
    return {};
}

std::string jumpDestination(std::string_view internalName) {
    std::string to;
    return jumpPoint(trimmed(internalName), nullptr, &to) ? to : "";
}

bool contradictsHandPlace(std::string_view handPlace, std::string_view namedPlace, std::string_view namedSystem) {
    const GamePlace hand = placeByLogName(handPlace, "");
    if (!hand.known() || hand.system.empty()) return false;
    const std::string named = trimmed(namedPlace);
    if (!named.empty()) return !sameName(named, hand.name);
    const std::string system = trimmed(namedSystem);
    if (system.empty()) return false;
    return !sameName(system, hand.system);
}

}  // namespace SCMiner
