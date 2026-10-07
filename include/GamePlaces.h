#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace SCMiner {

// A place the game log can name, and what it means for the app: the system
// it lies in and which location of the ore tables (MiningLocations.h)
// applies there. Both may be empty - a gateway station is named for the
// system it leads TO and stands in one of two others, and a landing zone
// has no ore under it.
struct GamePlace {
    std::string name;         // as shown to the player: "Monox", "Levski", "HUR-L1"
    std::string system;       // "Stanton", "Pyro", "Nyx" - "" when the name does not say
    std::string oreLocation;  // a `location` of ORE_LOCATIONS, "" when none applies
    int surface = -1;         // 1 on a planet or moon (a city counts), 0 in space, -1 unknown
    bool known() const { return !name.empty(); }
};

// The start location of a route line ("Projected Start Location is X for
// route to ..."): a planet, a moon, a station, a landing zone, a Lagrange
// point - or the system itself, out in space, which comes back with an
// empty name. `systemHint` is the last system known, for the names that
// need one ("Asteroid Cluster" stands in every system).
GamePlace placeByLogName(std::string_view logName, std::string_view systemHint);

// A quantum destination as the log spells it internally: "pyro2",
// "OOC_Stanton_2b_Daymar", "levski_all-001", "LOC_RR_S1_L2",
// "rs_ext_nyx-pyro_jp1". Unknown names come back empty.
GamePlace placeByDestination(std::string_view internalName, std::string_view systemHint);

// Where a terminal was opened, as the game's inventory request names it:
// "Nyx_Levski", "Stanton1_Lorville", "RR_JP_StantonPyro" (a gateway: the
// system it stands in, then where it leads), "RR_HUR_LEO", "RR_HUR_L1",
// "RR_P2_L4". Codes for places the app has no name for - outposts and the
// like - come back empty.
GamePlace placeByLocationCode(std::string_view code, std::string_view systemHint);

// For a jump point destination ("rs_ext_nyx-pyro_jp1"): the system on the
// far side ("Pyro"); "" for anything that is not a jump point.
std::string jumpDestination(std::string_view internalName);

// "Monox" -> "Pyro II (Monox)" etc.: the ore tables' own spelling of a
// place, or "" when the tables know no such place
std::string oreLocationOf(std::string_view placeName);

// Every place that changes what the ore tables say - the bodies, the belts,
// the rings, and Stanton's twenty Lagrange points - for a player who wants
// to set the place by hand because the log has not caught up. Landing zones
// and stations with nothing to mine are left out: picking one of those would
// say nothing the system does not already say. Pyro's Lagrange points stand
// in it as one, "Asteroid clusters": the tables put all of Pyro's clusters
// under one heading, and every point would say the same.
const std::vector<GamePlace>& minablePlaces();

// Whether what the log last named contradicts a place set by hand:
// `namedPlace` a place the tables know, or "" when the log named only a
// system, and `namedSystem` that system. Lyria in Stanton against a
// hand-set Wala says the ship has moved on; "" in Pyro against Wala says
// so too; "" in Stanton says nothing, and neither does a hand place the
// tables cannot put in a system.
bool contradictsHandPlace(std::string_view handPlace, std::string_view namedPlace, std::string_view namedSystem);

}  // namespace SCMiner
