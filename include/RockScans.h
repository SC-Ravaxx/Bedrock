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
#include <map>
#include <string>
#include <vector>

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

// One rock as the view took it: a line of rock_scans.csv, and what goes to
// the service where the player shares (sharerocks=1).
struct ScanPart {
    std::string ore;         // as the signature table spells it; "" for the inert rest
    bool inert = false;
    float percent = 0;       // of the rock
    int quality = 0;         // 0 for the inert rest
};

struct Scan {
    long long observed = 0;  // UTC seconds: the look that took it
    std::string ore;
    float mass = 0, scu = 0;
    float resistance = -1;   // per cent, as printed; -1 not read
    bool inColour = false;   // ...in colour: a laser on the rock had had its say
    float instability = -1;  // -1 not read
    int rangeMetres = 0;     // the laser's range to it; 0 not read
    std::string ship;        // "prospector", "mole" or "golem"; "" for another
    std::string place;       // as the window's header names it; "" unknown
    std::vector<ScanPart> parts;   // the list in the panel's order, where the view read it whole
};

// The rocks of a session, a Scan each. A rock comes in with the first look
// the view takes of it and goes out when another rock comes on view or the
// view empties. Looks of the same rock in between - the same ore, mass and
// SCU - add what the first did not have: the resistance in white, the
// instability, the range, the list. A rock looked at again later counts
// once, or twice where only the second time read its list.
class Recorder {
public:
    // The rock on view - the figures the view shows - and the look that
    // showed them, which says whether the instability was read at all
    void taken(const RockStats& shown, const RockStats& look, long long observed, const std::string& ship,
               const std::string& place);
    // The list the view shows, read at that mass
    void listed(float mass, const std::vector<ScanPart>& parts);
    void gone();
    // The rocks gone out since the last call
    std::vector<Scan> done();

private:
    bool open_ = false;
    Scan scan_;
    std::vector<Scan> done_;
    std::map<std::string, bool> counted_;   // rocks gone out, and whether with their list
};

// "prospector", "mole" or "golem" for the ship the log names; "" for any other
std::string shipKind(const std::string& logShip);
// "Taranite 9.37 957; Taranite 38.51 310; inert 52.11"
std::string partsText(const std::vector<ScanPart>& parts);
// rock_scans.csv: its header, and a rock's line under it - `time` as the
// caller writes it
extern const char* const CSV_HEADER;
std::string csvLine(const Scan& scan, const std::string& time);

}  // namespace RockScans
}  // namespace SCMiner
