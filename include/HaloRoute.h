#pragma once

#include <string>
#include <vector>

#include "WhereToBuy.h"

namespace SCMiner {

// The Aaron Halo, Stanton's asteroid belt, and the way into it. The game
// has no marker in it: a pilot plots a quantum route through it and leaves
// quantum travel where the HUD's distance left to the destination says the
// ship is in the band wanted.
//
// The game's files hold the belt as one ring round the Stanton marker - the
// system's origin, which every place's point is measured from -, 19,597,600
// to 21,392,800 km out and 10,000 km deep (aaronhalo.socpak, the same in
// 4.10.2 and 4.10.3). The bands in it come out of noise in the game's code,
// not out of a file. Their edges, densest points and heights are the
// Cornerstone survey's (CaptSheppard, alpha 3.16.1, March 2022, cstone.space
// knowledge base 65): 1,746 pictures a thousand km apart, alike on every
// server and all round the star. Laid on the places' points of 4.10.2
// (NavPlaces.h), it gives back the survey's own route charts to within
// 2.4 Mm: ARC-L1 to CRU-L4, band 5 at 14,290,792 km before CRU-L4 against
// the chart's 14,292,609.
struct HaloBand {
    int number;               // 1, the innermost, to 10
    double innerKm, outerKm;  // from the Stanton marker
    double densestKm;         // ...where it is densest
    double aboveKm, belowKm;  // how far it reaches over and under the planets' plane there
};
inline constexpr int HALO_BAND_COUNT = 10;
extern const HaloBand HALO_BANDS[HALO_BAND_COUNT];
// Half again as dense as the others, and its rocks bigger; the first has
// next to none
inline constexpr int HALO_DENSEST_BAND = 5;
inline constexpr int HALO_SPARSE_BAND = 1;

// One band on a quantum route, by the distance left to the destination the
// HUD counts down
struct HaloPass {
    int band = 0;
    double enterKm = 0;   // where the route goes into it
    double leaveKm = 0;   // ...and out of it again, further on: less left
    double exitKm = 0;    // where to leave quantum travel: its densest point, or as near it as the route comes
    double heightKm = 0;  // how far over (+) or under (-) the planets' plane the route is there
    // ...over or under the band itself there, which a band only a few
    // hundred km deep is from a station that stands that far off the plane
    bool overIt() const;
    bool underIt() const;
    bool inIt() const { return !overIt() && !underIt(); }
};

// Each band the straight route from `from` to `to` goes through, the first
// time it does, in band order. Empty where it does not cross the belt: both
// ends inside it, both outside and wide of it, or not both in Stanton.
std::vector<HaloPass> haloPasses(const NavPoint& from, const NavPoint& to);

// The one band to name for a route where there is room for one: the densest
// band where the route runs through it within its height, else the widest
// one it does; null where it runs through none
const HaloPass* bestHaloPass(const std::vector<HaloPass>& passes);

// How long the route is, km
double routeKm(const NavPoint& from, const NavPoint& to);

// ...of those only the bands it runs through within their height: the ones
// a ship that leaves quantum travel there actually reaches
std::vector<HaloPass> haloBandsReached(const NavPoint& from, const NavPoint& to);

// Whether a route runs through the belt: reaches at least one band
bool runsThroughHalo(const NavPoint& from, const NavPoint& to);

// The places a route can be planned from and to: Stanton's planets with
// their moons, then its Lagrange points, a planet to a row
struct HaloPlace {
    const char* name;   // as NavPlaces has it
    int row;
};
inline constexpr int HALO_PLACE_COUNT = 36;
inline constexpr int HALO_PLACE_ROWS = 8;
inline constexpr int HALO_BODY_ROWS = 4;   // the rows of planets and moons; the Lagrange points' after them
extern const HaloPlace HALO_PLACES[HALO_PLACE_COUNT];

// The places a route from `from` (a place NavPlaces knows in Stanton) runs
// through the belt to, in HALO_PLACES' order; and the one of them to
// suggest: the nearest whose route runs through the densest band, else the
// nearest; "" where there is none
std::vector<const HaloPlace*> haloDestinations(const std::string& from);
std::string nearestHaloDestination(const std::string& from);

}  // namespace SCMiner
