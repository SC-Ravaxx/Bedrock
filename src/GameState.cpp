#include "GameState.h"
#include "GamePlaces.h"
#include "HaloRoute.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace SCMiner {
namespace {

// How far back a fresh attach reads: 64 MB, and the whole file below that.
// The lines that say where the player is are written once, on arrival
// (the loading screen names the system, a plotted route the place), and
// then slide back as the log fills with everything else - in a 15.5 MB
// log the last loading screen sat 7.8 MB from the end. Parsing costs
// microseconds a line and happens once per attach; the lines are read in
// order, so the newest of each kind still wins.
constexpr std::uintmax_t TAIL_BYTES = 64 * 1024 * 1024;

bool systemWord(std::string_view word, std::string* out) {
    static const char* const NAMES[] = {"Stanton", "Pyro", "Nyx"};
    for (const char* name : NAMES) {
        const size_t len = std::strlen(name);
        if (word.size() != len) continue;
        bool same = true;
        for (size_t i = 0; i < len && same; i++) {
            same = std::tolower(static_cast<unsigned char>(word[i])) ==
                   std::tolower(static_cast<unsigned char>(name[i]));
        }
        if (same) {
            if (out) *out = name;
            return true;
        }
    }
    return false;
}

// Two stamps of the parse ("2026-09-27T09:27:53") no more than `seconds`
// apart. A stamp that is missing counts as close: a line without a time
// cannot be told apart by it.
bool withinSeconds(const std::string& a, const std::string& b, int seconds) {
    if (a.size() != 19 || b.size() != 19) return true;
    if (a.compare(0, 11, b, 0, 11) != 0) return false;   // another day
    auto secs = [](const std::string& s) {
        return (std::atoi(s.c_str() + 11) * 60 + std::atoi(s.c_str() + 14)) * 60 + std::atoi(s.c_str() + 17);
    };
    return std::abs(secs(a) - secs(b)) <= seconds;
}

// "2026-11-21T23:49:11" -> seconds since 1970, UTC; -1 for no stamp
long long stampSeconds(const std::string& s) {
    if (s.size() != 19) return -1;
    const int y = std::atoi(s.c_str()), m = std::atoi(s.c_str() + 5), d = std::atoi(s.c_str() + 8);
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    // days since 1970-01-01 of a date in the proleptic Gregorian calendar
    const int yy = y - (m <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const int yoe = yy - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + doe - 719468;
    return days * 86400 + std::atoi(s.c_str() + 11) * 3600LL + std::atoi(s.c_str() + 14) * 60LL +
           std::atoi(s.c_str() + 17);
}

// Open space in Stanton, as the game's inventory names where the player is
// (<Update Inventory Location>): the id while a route starts at "Stanton
// System", the same in every build since about 11733560
constexpr const char* STANTON_OPEN_SPACE = "1902223495";
// How long after an arrival line on a route through the Aaron Halo the ship
// may still stand in open space before it counts as stopped in the Halo
// (GameState::settle)
constexpr long long HALO_WAIT_SECONDS = 30;

// "MISC_Prospector_824000000606" -> "MISC_Prospector": the entity id at the
// end goes, and what is left has to look like MAKER_Model
std::string shipClass(std::string_view name) {
    while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    const size_t us = name.rfind('_');
    if (us != std::string_view::npos && us + 1 < name.size()) {
        bool digits = true;
        for (size_t i = us + 1; i < name.size() && digits; i++) {
            digits = std::isdigit(static_cast<unsigned char>(name[i])) != 0;
        }
        if (digits && name.size() - us - 1 >= 6) name = name.substr(0, us);
    }
    const size_t sep = name.find('_');
    if (sep == std::string_view::npos || sep < 3 || sep > 5 || sep + 1 >= name.size()) return "";
    for (size_t i = 0; i < sep; i++) {
        if (!std::isupper(static_cast<unsigned char>(name[i]))) return "";
    }
    return std::string(name);
}

// "MISC_Prospector" is a class, "MISC Prospector" a display name
bool isClass(const std::string& s) {
    const size_t sep = s.find('_');
    if (sep == std::string::npos || sep < 3 || sep > 5 || sep + 1 >= s.size()) return false;
    for (size_t i = 0; i < sep; i++) {
        if (!std::isupper(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

}  // namespace

std::string GameState::maker(const std::string& ship) {
    if (!isClass(ship)) {
        const size_t space = ship.find(' ');
        return space == std::string::npos ? ship : ship.substr(0, space);
    }
    static const std::map<std::string, std::string> MAKERS = {
        {"AEGS", "Aegis"},    {"ANVL", "Anvil"},   {"ARGO", "Argo"},     {"BANU", "Banu"},
        {"CNOU", "Consolidated Outland"},          {"CRUS", "Crusader"}, {"DRAK", "Drake"},
        {"ESPR", "Esperia"},  {"GAMA", "Gatac"},   {"GRIN", "Greycat"},  {"KRIG", "Kruger"},
        {"MISC", "MISC"},     {"MRAI", "Mirai"},   {"ORIG", "Origin"},   {"RSI", "RSI"},
        {"TMBL", "Tumbril"},  {"VNCL", "Vanduul"}, {"XIAN", "Xi'an"},    {"XNAA", "Xi'an"}};
    const size_t sep = ship.find('_');
    const std::string prefix = ship.substr(0, sep);
    const auto it = MAKERS.find(prefix);
    return it == MAKERS.end() ? prefix : it->second;
}

std::string GameState::label(const std::string& ship) {
    if (ship.empty() || !isClass(ship)) return ship;
    const size_t sep = ship.find('_');
    std::string model = ship.substr(sep + 1);
    for (char& c : model) {
        if (c == '_') c = ' ';
    }
    return maker(ship) + " " + model;
}

std::string GameState::model(const std::string& ship) {
    const std::string full = label(ship);
    if (full.empty()) return "";
    // Two-word makers first ("Consolidated Outland Nomad"), then any first word
    std::string rest = full;
    static const char* const LONG_MAKERS[] = {"Consolidated Outland "};
    bool stripped = false;
    for (const char* maker : LONG_MAKERS) {
        const size_t n = std::strlen(maker);
        if (full.size() > n && full.compare(0, n, maker) == 0) {
            rest = full.substr(n);
            stripped = true;
        }
    }
    if (!stripped) {
        const size_t space = full.find(' ');
        if (space != std::string::npos) rest = full.substr(space + 1);
    }
    const size_t next = rest.find(' ');
    return next == std::string::npos ? rest : rest.substr(0, next);
}

bool GameState::parseLine(std::string_view line, std::string* system, std::string* ship, bool* leaving) {
    Event event;
    if (!parse(line, &event)) return false;
    if (system && !event.system.empty()) *system = event.system;
    if (ship && !event.ship.empty()) *ship = event.ship;
    if (leaving && !event.ship.empty()) *leaving = event.leaving;
    return !event.system.empty() || !event.ship.empty();
}

bool GameState::parse(std::string_view line, Event* out) {
    Event event;
    std::string* system = &event.system;
    std::string* ship = &event.ship;
    bool* leaving = &event.leaving;
    bool said = false;

    // The line's own time - "<2026-09-27T09:27:53.737Z> ..." - the nineteen
    // characters inside the brackets, UTC to the second
    if (line.size() > 25 && line[0] == '<' && line[5] == '-' && line[11] == 'T' && line[20] == '.') {
        event.stamp = std::string(line.substr(1, 19));
    }

    // "...|Projected Start Location is Nyx for route to destination ..."
    // The whole name up to " for route": "Nyx" or "Nyx System" out in
    // space, otherwise the place the ship is in - "Monox", "Levski",
    // "Stanton Gateway" (which is NOT in Stanton: the first word alone
    // would set the wrong system).
    static constexpr std::string_view START = "Projected Start Location is ";
    if (const size_t k = line.find(START); k != std::string_view::npos) {
        const size_t b = k + START.size();
        size_t e = line.find(" for route", b);
        if (e == std::string_view::npos) e = line.size();
        std::string_view name = line.substr(b, e - b);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        std::string_view word = name;
        if (word.size() > 7 && word.substr(word.size() - 7) == " System") word = word.substr(0, word.size() - 7);
        std::string sys;
        if (systemWord(word, &sys)) {
            *system = sys;
            event.systemFromRoute = true;
            said = true;
        } else if (!name.empty()) {
            event.place = std::string(name);
            said = true;
        }
        // ...and the destination, the word after "for route to destination "
        static constexpr std::string_view TO = " for route to destination ";
        if (line.compare(e, TO.size(), TO) == 0) {
            const size_t d = e + TO.size();
            const size_t end = line.find(' ', d);
            event.routeTo = std::string(line.substr(d, end == std::string_view::npos ? std::string_view::npos : end - d));
        }
    }
    // "Player has selected point pyro2 as their destination, routing locally"
    static constexpr std::string_view PICKED = "Player has selected point ";
    if (const size_t k = line.find(PICKED); k != std::string_view::npos &&
                                          line.find("] <Player Selected Quantum Target") != std::string_view::npos) {
        const size_t b = k + PICKED.size();
        const size_t e = line.find(" as their destination", b);
        if (e != std::string_view::npos && e > b) {
            event.target = std::string(line.substr(b, e - b));
            said = true;
        }
    }
    // 5.0 picks a body by its entity id - "Player has selected point
    // 876113521728", where 4.10 wrote "nyx1" - and the planet's own lines
    // name the id, at login, long before any pick:
    //   <Planet Update Branch Inputs> Planet 'nyx1' (entity 876113521728) first update: ...
    // In seven 5.0 logs: 29 bodies so, each with the same id in every log;
    // none in 4.10. apply() turns a target by such an id into the name.
    static constexpr std::string_view PLANET = "> Planet '";
    static constexpr std::string_view ENTITY = "' (entity ";
    if (const size_t k = line.find(PLANET); k != std::string_view::npos) {
        const size_t b = k + PLANET.size();
        const size_t e = line.find(ENTITY, b);
        if (e != std::string_view::npos && e > b) {
            const size_t ib = e + ENTITY.size();
            size_t ie = ib;
            while (ie < line.size() && std::isdigit(static_cast<unsigned char>(line[ie]))) ie++;
            if (ie > ib && ie < line.size() && line[ie] == ')') {
                event.body = std::string(line.substr(b, e - b));
                event.bodyId = std::string(line.substr(ib, ie - ib));
                said = true;
            }
        }
    }
    if (line.find("Quantum Drive has arrived at final destination") != std::string_view::npos &&
        line.find("| NOT AUTH |") != std::string_view::npos) {
        event.arrived = true;
        said = true;
    }
    // The one line that says where the ship ARRIVED, without a star map and
    // without a quantum drive:
    //   <ProximitySensorMakingLocalHelper> Proximity sensor [...],
    //   helper is being made in zone [pyro4 - Class(OrbitingObjectContainer)
    //   - Context(Unstreamable Immovable) - Socpak(...pyro4.socpak)]
    // A door coming into range is made local in the zone that holds it, and
    // for anything standing on a moon that zone is the moon's own object
    // container - spelled exactly the way a quantum destination is
    // ("pyro5a", "OOC_Stanton_2b_Daymar"), so placeByDestination reads it.
    //
    // Everything else is departure-side: a route names where the ship SET
    // OFF from, and a destination the map does not know ("NavPoint_Dynamic_
    // 817077013330") says nothing at all. After a route to such a nav point
    // this line named the body four minutes later; the next route came only
    // 33 minutes after that.
    //
    // The class matters: over all 1,195 logs every OrbitingObjectContainer
    // was a planet or moon, and nothing else ever was.
    static constexpr std::string_view ZONE = "helper is being made in zone [";
    static constexpr std::string_view BODY = " - Class(OrbitingObjectContainer)";
    if (const size_t k = line.find(ZONE); k != std::string_view::npos) {
        const size_t b = k + ZONE.size();
        const size_t e = line.find(BODY, b);
        if (e != std::string_view::npos && e > b) {
            event.zone = std::string(line.substr(b, e - b));
            said = true;
        }
    }
    // The other line that says where the ship IS. Every ten minutes the
    // engine prints its physics statistics, and among them every planet and
    // moon with the terrain cells loaded for it:
    //   planet cells:  845 [16384] meshes:  204 [ 2048] name: pyro4
    //   planet cells:    0 [    0] meshes:    0 [    0] name: pyro5c
    // Terrain is loaded near the ground only, so the one body with any is
    // the body the ship is over - which is where the mining happens, and so
    // the moment the place matters. Over 120 logs: at 87 moments the place
    // was certain the list named that body 63 times, no body 24 times (in
    // orbit), another body never. Slow, but it needs neither a route nor a
    // door in range: a marker hop to a bare rock field, a friend's ship, a
    // respawn all end up in it within ten minutes.
    static constexpr std::string_view CELLS = "planet cells:";
    static constexpr std::string_view CELLS_NAME = "name: ";
    if (const size_t k = line.find(CELLS); k != std::string_view::npos) {
        size_t p = k + CELLS.size();
        while (p < line.size() && line[p] == ' ') p++;
        int cells = 0;
        int digits = 0;
        while (p < line.size() && digits < 9 && std::isdigit(static_cast<unsigned char>(line[p]))) {
            cells = cells * 10 + (line[p] - '0');
            digits++;
            p++;
        }
        const size_t n = digits > 0 ? line.find(CELLS_NAME, p) : std::string_view::npos;
        if (n != std::string_view::npos) {
            std::string_view name = line.substr(n + CELLS_NAME.size());
            while (!name.empty() && (name.back() == ' ' || name.back() == '\r' || name.back() == '\n')) {
                name.remove_suffix(1);
            }
            if (!name.empty()) {
                if (cells > 0) event.zone = std::string(name);
                // ...and every body, with terrain or without, for the round
                // with none at all (rockPlace: Yela's belt)
                event.cells = cells;
                event.cellsBody = std::string(name);
                said = true;
            }
        }
    }
    // The round of statistics begins and ends:
    //   PHYSICS INSTANCE STATS BEGIN 2
    //   PHYSICS INSTANCE STATS END
    // with the 28 bodies between the two in every round of eight logs from
    // September to November 2026 - and none in the round before the
    // universe is loaded or after it is left
    if (line.find("PHYSICS INSTANCE STATS BEGIN") != std::string_view::npos) {
        event.statsBegin = true;
        said = true;
    } else if (line.find("PHYSICS INSTANCE STATS END") != std::string_view::npos) {
        event.statsEnd = true;
        said = true;
    }
    // "Loading screen for Nyx : SC_Frontend closed after 44.25 seconds"
    // ...and "Loading screen for Frontend_Main : ...", the main menu: at the
    // start of the game, and whenever the player leaves the universe for it
    // (three times on 17 Nov, out of the MOLE, the Prospector and the
    // Golem). Nobody sits in a ship in the menu, and the way back in is a
    // spawn, so whatever seat the log last gave is left - while the ship
    // stays named, as when its seat is left.
    static constexpr std::string_view LOADING = "Loading screen for ";
    if (const size_t k = line.find(LOADING); k != std::string_view::npos) {
        const size_t b = k + LOADING.size();
        const size_t e = line.find(' ', b);
        const std::string_view word = line.substr(b, e == std::string_view::npos ? std::string_view::npos : e - b);
        std::string name;
        if (systemWord(word, &name)) {
            if (system) *system = name;
            // ...and how long the screen was up: a terminal line written
            // meanwhile is the spawn's own (applyOwn)
            static constexpr std::string_view AFTER = "closed after ";
            if (const size_t a = line.find(AFTER, b); a != std::string_view::npos) {
                event.loadingFor = std::atof(std::string(line.substr(a + AFTER.size(), 12)).c_str());
            }
            said = true;
        } else if (word == "Frontend_Main") {
            event.menu = true;
            said = true;
        }
    }
    // "<Player Selected Quantum Target - Local> ... | NOT AUTH | ANVL_Asgard_822000000505[822000000505]|CSCItemNavigation::..."
    // Only the lines that begin with the PLAYER doing something name the
    // player's own ship. The navigation lines the server replicates -
    // <Local Route Guard - Server Rerouted> above all - carry every ship
    // within range: replayed over one afternoon they named a Mantis, a
    // Starfarer, a Shiv and a Vanduul Stinger the player never sat in.
    static constexpr std::string_view NAV = "|CSCItemNavigation::";
    const bool playerAct = line.find("] <Player Selected Quantum Target") != std::string_view::npos ||
                           line.find("] <Player Requested Fuel to Quantum Target") != std::string_view::npos;
    if (const size_t k = line.find(NAV); k != std::string_view::npos) {
        const size_t open = line.rfind('[', k);
        const size_t bar = open == std::string_view::npos ? std::string_view::npos : line.rfind('|', open);
        // "MISC_Prospector_840000000707[840000000707]|CSCItemNavigation::" -
        // the entity id in the brackets says WHICH ship the line is about,
        // the player's or another's; apply() keeps the others out
        if (open != std::string_view::npos && k > open + 2 && line[k - 1] == ']') {
            event.navShip = std::string(line.substr(open + 1, k - open - 2));
            event.playerAct = playerAct;
            if (playerAct) event.hull = event.navShip;
        }
        if (playerAct && open != std::string_view::npos && bar != std::string_view::npos && bar + 1 < open) {
            const std::string cls = shipClass(line.substr(bar + 1, open - bar - 1));
            if (!cls.empty()) {
                if (ship) *ship = label(cls);
                if (leaving) *leaving = false;
                said = true;
            }
        }
    }
    // "... releasing control token for 'MISC_Prospector_824000000606' ..."
    static constexpr std::string_view RELEASE = "releasing control token for '";
    if (const size_t k = line.find(RELEASE); k != std::string_view::npos) {
        const size_t b = k + RELEASE.size();
        const size_t e = line.find('\'', b);
        if (e != std::string_view::npos) {
            const std::string_view token = line.substr(b, e - b);
            const std::string cls = shipClass(token);
            if (!cls.empty()) {
                if (ship) *ship = label(cls);
                if (leaving) *leaving = true;
                said = true;
            }
            // ...and which one of its kind: the id after the class
            const size_t us = token.rfind('_');
            if (!cls.empty() && us != std::string_view::npos && us + 1 < token.size() &&
                std::all_of(token.begin() + us + 1, token.end(),
                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
                event.hull = std::string(token.substr(us + 1));
            }
        }
    }
    // "<[ActorState] Dead> ... Actor '...' [...] ejected from zone
    //  'ARGO_MOLE_812345678901' [...] to zone '...' [...] due to previous zone
    //  being in a destroyed vehicle with detached interior."
    // The one line 4.10 writes when a ship is destroyed, and only with the
    // player in it - 133 of them in 600 logs, and no other kind.
    // The hull is gone for good; a claim brings the ship back under a new id.
    static constexpr std::string_view EJECTED = "ejected from zone '";
    if (const size_t k = line.find(EJECTED);
        k != std::string_view::npos && line.find("destroyed vehicle") != std::string_view::npos) {
        const size_t b = k + EJECTED.size();
        const size_t e = line.find('\'', b);
        if (e != std::string_view::npos) {
            const std::string_view token = line.substr(b, e - b);
            const size_t us = token.rfind('_');
            if (!shipClass(token).empty() && us != std::string_view::npos && us + 1 < token.size() &&
                std::all_of(token.begin() + us + 1, token.end(),
                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
                event.lost = std::string(token.substr(us + 1));
                said = true;
            }
        }
    }
    // The ship's crew channel, joined on boarding and left on the way out:
    //   Added notification "You have joined channel 'MISC Prospector : <owner>'.
    // Only a channel named "<ship> : <owner>" is a ship's; the display name
    // is what the log gives, so that is what every source hands back.
    // A name can hold a quote of its own - "Drake Golem Teach's Special",
    // "Argo MOLE Teach's Special" - so the channel ends where the sentence
    // does, at "'." (all 516 channel lines of seven 5.0 and two 4.10 logs
    // end so); a handle holds no quote.
    static constexpr std::string_view JOINED = "You have joined channel '";
    static constexpr std::string_view LEFT = "You have left the channel '";
    for (int pass = 0; pass < 2; pass++) {
        const std::string_view key = pass == 0 ? JOINED : LEFT;
        const size_t k = line.find(key);
        if (k == std::string_view::npos) continue;
        const size_t b = k + key.size();
        size_t e = line.find("'.", b);
        if (e == std::string_view::npos) e = line.find('\'', b);
        if (e == std::string_view::npos) continue;
        const std::string_view channel = line.substr(b, e - b);
        const size_t owner = channel.find(" : ");
        if (owner == std::string_view::npos || owner == 0) continue;
        if (ship) *ship = std::string(channel.substr(0, owner));
        if (leaving) *leaving = pass == 1;
        said = true;
    }
    // A mining gadget stuck on a rock:
    //   <[ActorState] Place> ... ::DoPlace] '<who>' [id] placed
    //       'Mining_Gadget_SHIN_Sabir_800000000101' [id] on entity
    //       'MineableRock_AsteroidLegendary_Savrilium_812345675858' [id]
    // (a "placing" line a second earlier says the same), and taken off it
    // again it is back in the player's hand:
    //   <AttachmentReceived> Player[..] Attachment[Mining_Gadget_SHIN_Sabir_800000000101,
    //       Mining_Gadget_SHIN_Sabir, 800000000101] ... Port[weapon_attach_hand_right]
    // - the same line that puts it in the hand before it goes on, which is
    // why the entity id is kept: only the one on the rock coming back counts.
    {
        // "Mining_Gadget_SHIN_Sabir_800000000101" -> class and entity id
        auto classAndId = [](std::string_view token, std::string* cls, std::string* id) {
            const size_t us = token.rfind('_');
            if (us == std::string_view::npos || us + 1 >= token.size() ||
                !std::all_of(token.begin() + us + 1, token.end(),
                             [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; })) {
                return false;
            }
            *cls = std::string(token.substr(0, us));
            *id = std::string(token.substr(us + 1));
            return true;
        };
        static constexpr std::string_view PLACED = " placed 'Mining_Gadget_";
        static constexpr std::string_view PLACING = " placing 'Mining_Gadget_";
        static constexpr std::string_view ON = " on entity '";
        static constexpr std::string_view ATTACHED = "Attachment[Mining_Gadget_";
        if (line.find("<[ActorState] Place>") != std::string_view::npos) {
            size_t k = line.find(PLACED);
            if (k == std::string_view::npos) k = line.find(PLACING);
            if (k != std::string_view::npos) {
                const size_t b = line.find('\'', k) + 1;
                const size_t e = line.find('\'', b);
                if (e != std::string_view::npos &&
                    classAndId(line.substr(b, e - b), &event.gadget, &event.gadgetEntity)) {
                    said = true;
                    if (const size_t on = line.find(ON, e); on != std::string_view::npos) {
                        const size_t rb = on + ON.size();
                        const size_t re = line.find('\'', rb);
                        std::string id;
                        if (re != std::string_view::npos) classAndId(line.substr(rb, re - rb), &event.gadgetRock, &id);
                    }
                }
            }
        } else if (const size_t k = line.find(ATTACHED);
                   k != std::string_view::npos && line.find("<AttachmentReceived>") != std::string_view::npos) {
            const size_t b = k + 11;      // past "Attachment["
            const size_t e = line.find(',', b);
            if (e != std::string_view::npos && classAndId(line.substr(b, e - b), &event.gadget, &event.gadgetEntity)) {
                event.gadgetInHand = true;
                said = true;
            }
        }
    }
    // "<Update Inventory Location> Player [...] is changing location.
    // Landing [a] -> [b]. Location [c] -> [d]. Pending [0]": the player's
    // own, and d where the game streams them in now - a place's id without
    // its name (Event::inventoryTo)
    static constexpr std::string_view MOVED = "<Update Inventory Location>";
    static constexpr std::string_view MOVED_TO = ". Location [";
    if (line.find(MOVED) != std::string_view::npos) {
        if (const size_t k = line.find(MOVED_TO); k != std::string_view::npos) {
            const size_t arrow = line.find("] -> [", k + MOVED_TO.size());
            if (arrow != std::string_view::npos) {
                const size_t b = arrow + 6;
                const size_t e = line.find(']', b);
                if (e != std::string_view::npos && e > b) {
                    event.inventoryTo = std::string(line.substr(b, e - b));
                    said = true;
                }
            }
        }
    }
    // "<RequestLocationInventory> Player[...] requested inventory for
    // Location[RR_JP_StantonPyro]": a terminal opened, and the place it
    // stands in - the player on foot at it, so nothing says where more
    // surely.
    static constexpr std::string_view INVENTORY = "<RequestLocationInventory>";
    static constexpr std::string_view AT_LOCATION = "Location[";
    if (line.find(INVENTORY) != std::string_view::npos) {
        if (const size_t k = line.find(AT_LOCATION); k != std::string_view::npos) {
            const size_t b = k + AT_LOCATION.size();
            const size_t e = line.find(']', b);
            if (e != std::string_view::npos && e > b) {
                event.location = std::string(line.substr(b, e - b));
                said = true;
            }
        }
    }
    if (out) *out = event;
    return said;
}

// A gadget on a rock, or back off it. Whoever placed it: a crew mate's gadget
// on the rock the MOLE is working changes the panel all the same.
bool GameState::applyGadget(const Event& event) {
    if (event.gadgetInHand) {
        if (gadget_.empty() || event.gadgetEntity != gadgetEntity_) return false;
        gadget_.clear();
        gadgetEntity_.clear();
        gadgetRock_.clear();
    } else {
        if (event.gadget == gadget_ && event.gadgetEntity == gadgetEntity_) return false;   // "placing", then "placed"
        gadget_ = event.gadget;
        gadgetEntity_ = event.gadgetEntity;
        gadgetRock_ = event.gadgetRock;
    }
    gadgetStamp_ = event.stamp;
    gadgetChanges_++;
    return true;
}

// Whose line it is comes first. The ship the player last acted in - picked
// a target, asked for fuel - is the player's; a navigation line about any
// other ship says nothing about where the player is, however much it says
// about that ship: otherwise an Idris parked in Nyx puts the header in
// Nyx from a Pyro moon, and a Hornet arriving early puts the player at a
// destination they are still flying to (33 times in 200 logs).
//
// One exception is timing, not ownership: a new ship's first route line is
// written a few hundred milliseconds BEFORE the line that names the player
// picking in it, so a route of a ship not yet known as the player's is held
// for a moment and counts once the player's own line names that ship.
bool GameState::apply(const Event& line) {
    // an arrival held for the Aaron Halo, by the line's own time
    const bool settled = !line.stamp.empty() && settle(stampSeconds(line.stamp));
    if (!line.gadget.empty()) return applyGadget(line) || settled;   // a line of its own, about no ship
    // A body's entity id, for the 5.0 targets that give only the id...
    if (!line.bodyId.empty()) {
        bodies_[line.bodyId] = line.body;
        return settled;
    }
    // ...which are read as the body's name, the way 4.10 gave it and
    // placeByDestination knows it
    const auto targetBody = bodies_.find(line.target);
    const auto routeBody = bodies_.find(line.routeTo);
    Event named;
    if (targetBody != bodies_.end() || routeBody != bodies_.end()) {
        named = line;
        if (targetBody != bodies_.end()) named.target = targetBody->second;
        if (routeBody != bodies_.end()) named.routeTo = routeBody->second;
    }
    const Event& event = targetBody != bodies_.end() || routeBody != bodies_.end() ? named : line;
    bool changed = settled;
    if (event.playerAct && !event.navShip.empty()) {
        if (event.navShip != ownShip_) {
            ownShip_ = event.navShip;
            if (holding_ && heldRoute_.navShip == ownShip_ && withinSeconds(heldRoute_.stamp, event.stamp, 5)) {
                const Event held = heldRoute_;
                holding_ = false;
                changed = applyOwn(held);
            }
        }
        holding_ = false;
    } else if (!event.navShip.empty() && !ownShip_.empty() && event.navShip != ownShip_) {
        if (!event.place.empty() || event.systemFromRoute) {
            heldRoute_ = event;
            holding_ = true;
        }
        return false;
    } else if (!event.navShip.empty()) {
        holding_ = false;   // the player's own ship spoke; nothing held is worth more
    }
    if (holding_ && !withinSeconds(heldRoute_.stamp, event.stamp, 5)) holding_ = false;
    const bool own = applyOwn(event);
    return own || changed;
}

bool GameState::applyOwn(const Event& event) {
    bool changed = false;
    // What this line said about where the ship is, for namedPlace()
    auto name = [&](const std::string& place, const std::string& system) {
        namedStamp_ = event.stamp;
        namedPlace_ = place;
        namedSystem_ = system;
    };
    auto setSystem = [&](const std::string& sys) {
        if (!sys.empty() && sys != system_) {
            system_ = sys;
            changed = true;
            // A place belongs to a system: the ship cannot be at Bloom and in Nyx.
            // A line that names the new system without naming a place must not leave
            // the old place standing beside it.
            //
            // Only a place the tables can place is dropped. A gateway or a
            // rest stop belongs to no system by name, and those are precisely
            // the places a ship is at while the system changes around it.
            if (!place_.empty()) {
                const GamePlace here = placeByLogName(place_, "");
                if (!here.system.empty() && here.system != system_) {
                    place_.clear();
                    rockPlaceFresh();
                }
            }
        }
    };
    const bool wasInSpace = inSpace_;
    auto setPlace = [&](const std::string& place) {
        if (place != place_) {
            place_ = place;
            changed = true;
            rockPlaceFresh();
            inHalo_ = false;
        }
        if (!place.empty()) inSpace_ = false;   // a place, whatever it is, is not open space
    };
    // For rockPlace(), which lines say where the ship is NOW: a route's start
    // - but not that of the route to the target just picked, which the ship
    // is about to leave - a body, a terminal the app knows. Each ends the
    // wait after a pick and after an arrival somewhere without a name.
    const bool setsOff = !event.routeTo.empty() && event.routeTo == target_;
    auto placeSaid = [&](bool leaving) {
        unnamed_ = false;
        if (!leaving) travelling_ = false;
    };
    // ...and which say the place last named is not where the ship is, while
    // naming none the tables know: a loading screen (a spawn), a terminal of
    // a code the app has no name for (an outpost, a QV station)
    auto placeUnnamed = [&]() {
        unnamed_ = true;
        travelling_ = false;
        rockPlaceFresh();
    };
    setSystem(event.system);
    if (!event.system.empty()) name("", event.system);
    // A loading screen is a spawn: the place named before is not where the
    // player is now - unless a terminal named it while the screen was still
    // up, which is the spawn's own line. It comes 0-2 s after the screen
    // as a rule, but in 2 of 10 5.0 logins, both long, before it: 17 s
    // before a screen up 59 s, 54 s before one up 138.73 s (17 Nov: the
    // rocks of the next twenty minutes went without a place).
    if (!event.system.empty() && !event.systemFromRoute) {
        const bool spawnNamed = event.loadingFor > 0 && locationStamp_.size() == 19 && event.stamp.size() == 19 &&
                                locationStamp_ <= event.stamp &&
                                withinSeconds(locationStamp_, event.stamp, static_cast<int>(event.loadingFor) + 1);
        if (!spawnNamed) placeUnnamed();
    }
    if (event.systemFromRoute) {
        // out in space: no place any more - save the Aaron Halo, which a
        // ship stopped in it is out in space in (settle())
        if (!inHalo_) {
            setPlace("");
            inSpace_ = true;
        }
        placeSaid(setsOff);
    }
    if (!event.place.empty()) {
        placeSaid(setsOff);
        const GamePlace place = placeByLogName(event.place, system_);
        if (!place.system.empty()) name(place.name, place.system);
        setPlace(place.known() ? place.name : event.place);
        if (!place.system.empty()) {
            setSystem(place.system);
            jumpTo_.clear();
        } else if (!jumpTo_.empty()) {
            // A gateway or any other place the name cannot put in a system,
            // right after a jump point: usually the far side.
            //
            // But NOT when it is the gateway named after that far side. A
            // gateway is named for where it LEADS, never for where it stands
            // - "Nyx Gateway" is the station in PYRO (placeByDestination says
            // the same: {to + " Gateway", from}). Sitting at it is the last
            // thing before the crossing, not the first thing after it, so the
            // jump stays pending until somewhere else is named.
            const std::string nearSide = jumpTo_ + " Gateway";
            const bool atNearSide =
                place_.size() == nearSide.size() &&
                std::equal(place_.begin(), place_.end(), nearSide.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                });
            if (!atNearSide) {
                setSystem(jumpTo_);
                jumpTo_.clear();
            }
        }
    }
    if (!event.zone.empty()) {
        // Where the ship IS, over anything a route said it set off from.
        if (const GamePlace place = placeByDestination(event.zone, system_); place.known()) {
            name(place.name, place.system);
            setPlace(place.name);
            if (!place.system.empty()) setSystem(place.system);
            jumpTo_.clear();
            placeSaid(false);
            // ...from the statistics: the terrain of the body it is at
            if (event.cells > 0) terrain_ = RockPlaceState::Terrain::Loaded;
        }
    }
    if (!event.location.empty()) {
        // At a terminal, on foot: where the player IS, over anything a route
        // or a zone said - and the code names the system, so a jump that was
        // pending is settled too
        if (const GamePlace place = placeByLocationCode(event.location, system_); place.known()) {
            name(place.name, place.system);
            setPlace(place.name);
            if (!place.system.empty()) setSystem(place.system);
            jumpTo_.clear();
            placeSaid(false);
            locationStamp_ = event.stamp;
        } else if (event.location != "INVALID_LOCATION_ID") {
            placeUnnamed();
        }
    }
    // A pick: under way from here on, as far as a rock's place goes - and
    // an arrival held for the Halo is no longer waited on
    if (!event.target.empty()) {
        travelling_ = true;
        haloTarget_.clear();
        haloSince_.clear();
    }
    // (a new pick counts as a change: what the route crosses - the Aaron
    // Halo - is shown while it stands)
    if (!event.target.empty() && event.target != target_) {
        target_ = event.target;
        routeStart_.clear();
        changed = true;
    }
    // Where the route to it set off: its own line's start, open space ("")
    // included - a line of a route to somewhere else (a mission's beacon the
    // game routes to by itself) is not this route's
    if (!event.routeTo.empty() && event.routeTo == target_) {
        const std::string start = event.place.empty() ? std::string() : place_;
        if (start != routeStart_) {
            routeStart_ = start;
            changed = true;
        }
    }
    // Arrived at a target: the place it is
    auto arriveAt = [&](const std::string& target) {
        const std::string far = jumpDestination(target);
        // For rockPlace(): arrived, and somewhere the app names or not
        travelling_ = false;
        rockPlaceFresh();
        unnamed_ = far.empty() && !placeByDestination(target, system_).known();
        haloTarget_.clear();
        haloSince_.clear();
        if (!far.empty()) {
            // At the jump point, still on this side; the crossing itself
            // is not logged, the next place seen tells (see jumpTo_)
            jumpTo_ = far;
            const GamePlace place = placeByDestination(target, system_);
            name(place.name, place.system);
            setPlace(place.name);
            setSystem(place.system);
        } else if (const GamePlace place = placeByDestination(target, system_); place.known()) {
            if (!place.system.empty()) name(place.name, place.system);
            setPlace(place.name);
            if (!place.system.empty()) setSystem(place.system);
        }
        // ...and otherwise nothing: a destination the map does not know -
        // a mission beacon, a party member, one's own parked vehicle -
        // says nothing about WHERE that is, so the place stands. Clearing it
        // would clear the ore filter with it, even on a hop to one's own ship
        // parked on the same moon. A stale place corrects itself the moment
        // any route names its start; an empty one only corrects itself if
        // the player happens to open the map.
        target_.clear();
        routeStart_.clear();
        changed = true;
    };
    // An arrival on a route through the Aaron Halo - or out of it - with
    // the ship still in Stanton's open space: the drive writes the line
    // when the player leaves quantum travel in the Halo too. Held, the
    // place as it was, until the ship moves or the clock says Halo
    // (settle()). Not at a jump point or a target the map has no point for.
    auto heldForHalo = [&]() {
        if (system_ != "Stanton" || inventoryAt_ != STANTON_OPEN_SPACE || !jumpDestination(target_).empty()) {
            return false;
        }
        const GamePlace to = placeByDestination(target_, system_);
        NavPoint a, b;
        if (!to.known() || !navPoint(to.name, "Stanton", b)) return false;
        if (inHalo_) return true;
        const GamePlace from = placeByLogName(routeStart_, system_);
        return from.known() && navPoint(from.name, "Stanton", a) && runsThroughHalo(a, b);
    };
    if (event.arrived && !target_.empty()) {
        if (heldForHalo()) {
            haloTarget_ = target_;
            haloSince_ = event.stamp;
        } else {
            arriveAt(target_);
        }
    }
    // Where the game streams the player in. Out of open space after an
    // arrival held for the Halo: at its target after all - a moon 2 to 13 s
    // after the line, a Reclaimer at HUR-L1 236 s
    if (!event.inventoryTo.empty()) {
        inventoryAt_ = event.inventoryTo;
        if (!haloTarget_.empty() && inventoryAt_ != STANTON_OPEN_SPACE && inventoryAt_ != "0" &&
            haloTarget_ == target_) {
            arriveAt(target_);
        }
    }
    // The ten-minute statistics, for rockPlace(): a round begun since the
    // place line, with bodies listed and terrain loaded for none, says the
    // ship is out in space (Yela's belt rather than the moon)
    if (event.statsBegin) {
        statsWhole_ = true;
        statsBodies_ = false;
        statsTerrain_ = false;
    }
    if (event.cells >= 0) {
        statsBodies_ = true;
        if (event.cells > 0) statsTerrain_ = true;
    }
    if (event.statsEnd) {
        if (statsWhole_ && statsBodies_ && !statsTerrain_) terrain_ = RockPlaceState::Terrain::None;
        statsWhole_ = false;
    }
    // The main menu: out of whatever seat the log last gave (see parse)
    if (event.menu && inSeat_) {
        inSeat_ = false;
        changed = true;
    }
    // A hull destroyed with the player in it: gone, and said on to whoever
    // keeps fits by hull (lostHulls())
    if (!event.lost.empty()) {
        lost_.push_back(event.lost);
        if (hull_ == event.lost) {
            hull_.clear();
            hullShip_.clear();
        }
        changed = true;
    }
    // Which hull (see hull()). A target picked in it or its seat left names
    // it; boarding names only the kind, and the hull is unknown again.
    const std::string hullBefore = hull();
    if (!event.hull.empty()) {
        hull_ = event.hull;
        hullShip_ = event.ship;
    } else if (!event.ship.empty() && !event.leaving) {
        hull_.clear();
        hullShip_.clear();
    }
    if (!event.ship.empty()) {
        if (event.leaving) {
            if (event.ship != ship_ || inSeat_) changed = true;
            ship_ = event.ship;
            inSeat_ = false;
        } else if (event.ship != ship_ || !inSeat_) {
            ship_ = event.ship;
            inSeat_ = true;
            boardings_++;
            changed = true;
        }
    }
    if (hull() != hullBefore) changed = true;
    if (inSpace_ != wasInSpace) changed = true;
    return changed;
}

std::string GameState::hull() const {
    return !hull_.empty() && hullShip_ == ship_ ? hull_ : std::string();
}

bool GameState::settle(long long nowUtc) {
    if (haloTarget_.empty() || haloSince_.empty()) return false;
    const long long since = stampSeconds(haloSince_);
    if (since < 0 || nowUtc < since + HALO_WAIT_SECONDS) return false;
    // In the Halo: its place, out in space. The target and where its route
    // set off stand until the ship moves - which gives the target back - or
    // the next pick.
    static const std::string HALO = "Aaron Halo";
    const std::string stamp = haloSince_;
    haloSince_.clear();
    if (place_ != HALO) {
        place_ = HALO;
        rockPlaceFresh();
    }
    inHalo_ = true;
    inSpace_ = false;
    travelling_ = false;
    unnamed_ = false;
    namedPlace_ = HALO;
    namedSystem_ = "Stanton";
    namedStamp_ = stamp;
    return true;
}

std::string GameState::rockPlace() const {
    RockPlaceState state;
    state.place = place_;
    state.travelling = travelling_;
    state.unnamed = unnamed_;
    state.terrain = terrain_;
    return SCMiner::rockPlace(state);
}

void GameState::rockPlaceFresh() {
    terrain_ = RockPlaceState::Terrain::Unknown;
    statsWhole_ = false;
}

void GameState::setGameLog(const std::filesystem::path& log) {
    log_ = log;
    offset_ = 0;
    system_.clear();
    ship_.clear();
    inSeat_ = false;
    place_.clear();
    inSpace_ = false;
    target_.clear();
    routeStart_.clear();
    jumpTo_.clear();
    ownShip_.clear();
    hull_.clear();
    hullShip_.clear();
    lost_.clear();
    bodies_.clear();
    if (!gadget_.empty()) gadgetChanges_++;
    gadget_.clear();
    gadgetEntity_.clear();
    gadgetRock_.clear();
    gadgetStamp_.clear();
    boardings_ = 0;
    holding_ = false;
    namedPlace_.clear();
    namedSystem_.clear();
    namedStamp_.clear();
    locationStamp_.clear();
    travelling_ = false;
    unnamed_ = false;
    inventoryAt_.clear();
    haloTarget_.clear();
    haloSince_.clear();
    inHalo_ = false;
    rockPlaceFresh();
    if (log_.empty()) return;
    std::error_code ec;
    const auto size = std::filesystem::file_size(log_, ec);
    if (ec) return;
    const std::uintmax_t from = size > TAIL_BYTES ? size - TAIL_BYTES : 0;
    readFrom(from, &offset_, from > 0);
}

bool GameState::poll() {
    if (log_.empty()) return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(log_, ec);
    if (ec) return false;
    bool changed = false;
    if (size < offset_) {
        // The game started again: a new log and a new session. The ship
        // does not carry over; the system holds until the loading screen
        // says which one it is this time.
        offset_ = 0;
        if (!ship_.empty() || inSeat_ || !place_.empty() || inSpace_) changed = true;
        ship_.clear();
        inSeat_ = false;
        place_.clear();
        inSpace_ = false;
        target_.clear();
        routeStart_.clear();
        jumpTo_.clear();
        ownShip_.clear();
        hull_.clear();
        hullShip_.clear();
        lost_.clear();
        bodies_.clear();
        locationStamp_.clear();
        if (!gadget_.empty()) {
            gadgetChanges_++;
            changed = true;
        }
        gadget_.clear();
        gadgetEntity_.clear();
        gadgetRock_.clear();
        gadgetStamp_.clear();
        // boardings_ counts on: the first boarding after a restart is not the
        // one before it, and a reset would let the loadout read for that seat
        // stand for this one's
        holding_ = false;
        travelling_ = false;
        unnamed_ = false;
        inventoryAt_.clear();
        haloTarget_.clear();
        haloSince_.clear();
        inHalo_ = false;
        rockPlaceFresh();
    }
    if (size > offset_) changed = readFrom(offset_, &offset_, false) > 0 || changed;
    // ...and an arrival held for the Halo, by the wall clock: after one the
    // log often stays quiet for minutes
    const long long now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (settle(now)) changed = true;
    return changed;
}

int GameState::readFrom(std::uintmax_t from, std::uintmax_t* end, bool midLine) {
    std::ifstream in(log_, std::ios::binary);
    if (!in.is_open()) {
        if (end) *end = from;
        return 0;
    }
    in.seekg(static_cast<std::streamoff>(from));
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    if (midLine) {
        const size_t nl = data.find('\n');
        if (nl == std::string::npos) {
            if (end) *end = from;
            return 0;
        }
        pos = nl + 1;
    }
    // The game may be mid-line: everything up to the last newline is whole,
    // the rest waits for the next round
    size_t usable = data.rfind('\n');
    if (usable == std::string::npos || usable < pos) {
        if (end) *end = from + pos;
        return 0;
    }
    usable += 1;
    int changes = 0;
    while (pos < usable) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos || nl >= usable) nl = usable - 1;
        const std::string_view line(data.data() + pos, nl - pos);
        Event event;
        if (parse(line, &event) && apply(event)) changes++;
        pos = nl + 1;
    }
    if (end) *end = from + usable;
    return changes;
}

}  // namespace SCMiner
