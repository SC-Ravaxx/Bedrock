#pragma once
// Whether the figures a scan panel shows can be one rock's, before the rock
// view takes them.
//
// A rock's mass is its volume times what its parts weigh - each part's share
// times its ore's density, the inert rest at 1.0 (ORE_DENSITIES) - and its
// SCU is its volume as well, 142.857 m3 to the SCU. So its mass per SCU lies
// between 142.857 kg, all of it inert, and 142.857 kg times the densest ore
// the rock can hold, and no rock is bigger than the largest rock the game
// spawns: mining_rock_01_breakable at full size, 8,460 m3 or 59.22 SCU
// (game build 4.10.2; every ship rock is that model or one of three smaller
// asteroids, scaled down).
//
// Of the looks Bedrock took at rock panels from 16 September to 8 November
// 2026, 13,631 read an ore, a mass and SCU: 151 showed a rock the game had
// not finished scanning (below), 221 a figure misread - an SCU of 1.00, 7.00
// or 474.54 - and the median over five looks passed some of both to the view.
#include <string>

namespace SCMiner {

struct RockStats;

namespace RockScans {

inline constexpr float KG_PER_SCU_AT_DENSITY_ONE = 142.857f;
inline constexpr float LARGEST_ROCK_SCU = 59.22f;

// The panel's first picture of a rock: before the game has worked out what
// the rock holds it shows the rock's volume as its mass, and no resistance
// and no instability - for a second or two, at times for half a minute. A
// Lindinium of 44,059 kg and 53 % stood on view as 7,822 kg and 0 % for
// seven seconds (30 October 2026). An Ice rock really is like that; a rock
// of any other ore is not.
bool notScannedYet(const RockStats& rock);

// Why the figures cannot be one rock's - more SCU than the largest rock,
// lighter than its volume, heavier than its densest part - or "" where they
// can. Says nothing where the mass or the SCU was not read. Allows for the
// panel rounding the mass to a kilogram and the SCU to a hundredth.
std::string impossible(const RockStats& rock);

// The densest ore a rock of that ore holds, by its parts (ROCK_PARTS); for
// an ore not known, the densest of all
double densestPart(const std::string& rockOre);

// Why a look's figures are not taken for the rock on view, or "" where they
// are: notScannedYet, or impossible. The same impossible figures on eight
// looks in a row are taken all the same: a misread does not hold that long,
// and a patch may have made rocks bigger or heavier than the game data
// Bedrock carries.
class LookCheck {
public:
    std::string notTaken(const RockStats& rock);

private:
    float mass_ = 0, scu_ = 0;
    int looks_ = 0;
};

}  // namespace RockScans
}  // namespace SCMiner
