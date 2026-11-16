#pragma once
// The Ore Finder's second list (By place): every place the ore tables know,
// in the system on the tabs, by what kind of place it is - planets, moons,
// asteroid fields, caves and Nyx's events, each kind A to Z. Over all
// systems each system's kinds come in turn: Stanton, Pyro, Nyx. Asked for
// on Spectrum: the places by planet and system, the way the Blueprints
// window lists by material and by component.
#include "MiningLocations.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace SCMiner {

struct PlaceGroup {
    std::string system;               // "Stanton", over all systems; "" for one system's list
    std::string kind;                 // "PLANETS", "MOONS", "ASTEROID FIELDS", "CAVES", "EVENTS"
    std::vector<std::string> places;  // as the tables spell them
};

// A belt and a Lagrange point are both an asteroid field to a miner
inline int placeKindRank(const char* kind) {
    const std::string k = kind ? kind : "";
    if (k == "planet") return 0;
    if (k == "moon") return 1;
    if (k == "belt" || k == "lagrange" || k == "cluster") return 2;
    if (k == "cave") return 3;
    if (k == "event") return 4;
    return 5;
}

inline const char* placeKindHeading(int rank) {
    static const char* const words[6] = {"PLANETS", "MOONS", "ASTEROID FIELDS", "CAVES", "EVENTS", "OTHER"};
    return words[(std::max)(0, (std::min)(rank, 5))];
}

// Stanton, Pyro, Nyx, and a system the tables come to name later after them
inline int placeSystemRank(const char* system) {
    static const char* const known[3] = {"Stanton", "Pyro", "Nyx"};
    for (int i = 0; i < 3; i++) {
        if (_stricmp(system, known[i]) == 0) return i;
    }
    return 3;
}

// `system`: "ALL" for every system, else one of them in any case
inline std::vector<PlaceGroup> placeGroups(const std::string& system) {
    const bool all = _stricmp(system.c_str(), "ALL") == 0;
    struct Place { std::string name, system; int systemRank, kindRank; };
    std::vector<Place> places;
    for (const OreLocation& row : ORE_LOCATIONS) {
        if (!all && _stricmp(row.system, system.c_str()) != 0) continue;
        const bool seen = std::any_of(places.begin(), places.end(), [&](const Place& p) {
            return p.name == row.location;
        });
        if (!seen) places.push_back({row.location, row.system, placeSystemRank(row.system), placeKindRank(row.kind)});
    }
    auto folded = [](const std::string& s) {
        std::string out = s;
        for (char& c : out) c = (char)std::tolower((unsigned char)c);
        return out;
    };
    std::stable_sort(places.begin(), places.end(), [&](const Place& a, const Place& b) {
        if (a.systemRank != b.systemRank) return a.systemRank < b.systemRank;
        if (a.system != b.system) return a.system < b.system;
        if (a.kindRank != b.kindRank) return a.kindRank < b.kindRank;
        return folded(a.name) < folded(b.name);   // microTech among the H's and the P's
    });
    std::vector<PlaceGroup> groups;
    for (const Place& p : places) {
        const std::string sys = all ? p.system : std::string();
        const std::string kind = placeKindHeading(p.kindRank);
        if (groups.empty() || groups.back().system != sys || groups.back().kind != kind) {
            groups.push_back({sys, kind, {}});
        }
        groups.back().places.push_back(p.name);
    }
    return groups;
}

inline bool placeListed(const std::vector<PlaceGroup>& groups, const std::string& place) {
    for (const PlaceGroup& group : groups) {
        for (const std::string& p : group.places) {
            if (p == place) return true;
        }
    }
    return false;
}

// The system a place is in, as the tables spell it; "" for a place they do
// not know
inline std::string placeSystem(const std::string& place) {
    for (const OreLocation& row : ORE_LOCATIONS) {
        if (_stricmp(row.location, place.c_str()) == 0) return row.system;
    }
    return "";
}

// How many ores a place has, the ship's, the ROC's and the hand-mined ones:
// the figure the card says after the place's name
inline int oresAtCount(const std::string& place) {
    int count = 0;
    for (const OreLocation& row : ORE_LOCATIONS) {
        if (_stricmp(row.location, place.c_str()) == 0) count++;
    }
    return count;
}

}  // namespace SCMiner
