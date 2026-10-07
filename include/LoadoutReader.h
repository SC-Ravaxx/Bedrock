#pragma once
// What a one-seat ship has fitted, read off its mining HUD.
//
// Bottom left of the mining HUD the game names the laser over a table of
// what it does - "HELIX I MINING LASER", then FRACTURE PWR, RESISTANCE and
// the rest - and above that it lists the modules, each under its own name:
// "FOCUS III MODULE", "PASSIVE", what it does. That is the loadout, without
// a log line and whichever hull it is: two Prospectors with different heads
// are told apart by what they show. The MOLE's pilot never sees its heads,
// so this is for the Prospector and the Golem.

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace SCMiner {

struct MiningLaser;
struct MiningModule;
class TextLineReader;

struct SeenLoadout {
    // the laser's table: its first word's left edge and top, at what size
    bool tableFound = false;
    cv::Point table;
    double scale = 0;
    double score = -1;
    // the line over the table, as the second reader gave it, and the laser
    // it names
    std::string laserText;
    const MiningLaser* laser = nullptr;
    // the lines above that name a module, and the modules they name - none
    // when nothing is fitted
    std::vector<std::string> moduleTexts;
    std::vector<const MiningModule*> modules;
    // how many icons stand over the laser's table, one over each module's
    // column (-1: not looked at) - more than there are over a busy ground -
    // and whether one stands where the list's next module's would
    int moduleColumns = -1;
    bool iconPastList = false;
    // the table's Head column as read, FRACTURE PWR to WINDOW SIZE in per
    // cent (-1: not read), and whether it is what the laser and the modules
    // read make of it
    int head[6] = {-1, -1, -1, -1, -1, -1};
    double headSure[6] = {0, 0, 0, 0, 0, 0};   // the reader's mean certainty
    bool headAgrees = false;
    // the list was looked at, every line that says MODULE named one the game
    // has, the Head column agrees, and no icon stands past the list's
    bool modulesRead = false;

    bool complete() const { return laser != nullptr && modulesRead; }
};

// The laser's table, left of the middle of the screen. False when the mining
// HUD is not up.
bool findLaserTable(const cv::Mat& frame, SeenLoadout& seen);

// ...and the laser and its modules, read by the second reader. Costs a line
// read for the laser, one for each line of the module list and one for each
// figure of the Head column (about 50 ms each), so the app asks now and then,
// not on every look, and off the window's thread.
bool readSeenLoadout(const cv::Mat& frame, TextLineReader& lines, SeenLoadout& seen);

// The HUD's lettering in a stretch of the picture, white on black: what
// stands out of the ground under it, in whichever colour channel it stands
// out most, less its glow and the rules under headings. `scale` is the HUD's
// size against its size at 1440 lines. For any reader of the HUD's lists -
// the rock's composition (RockContents.h) is one too.
cv::Mat hudLetteringInk(const cv::Mat& bgr, double scale);

// A line as the HUD writes it matched to the game's names: the laser's name
// before MINING LASER, a module's before MODULE. nullptr unless exactly one
// fits.
const MiningLaser* laserByHudLine(const std::string& line);
const MiningModule* moduleByHudLine(const std::string& line);
// A module's block in the HUD's list - its name, and the lines under it that
// say what it does - matched as readSeenLoadout matches it. Public for the
// self-test.
const MiningModule* moduleByHudBlock(const std::string& nameLine, const std::vector<std::string>& below);

// Whether the table's Head column, as read (SeenLoadout::head, -1 where
// nothing was), is what this laser and these modules make of it
bool headColumnAgrees(const MiningLaser& laser, const std::vector<const MiningModule*>& modules, const int head[6],
                      const double sure[6]);

// Whose mining HUD a picture shows, by a ship's id - "prospector" or
// "golem", the two whose HUD lists what they carry - and nullptr for any
// other: by the label column of the scan panel (`glyph`, 0 where no panel
// was seen), and with no panel up at all by the ship the game's log has the
// player sitting in. The HUD lists the laser and its modules as soon as the
// mining mode is on, before any rock is scanned.
const char* hudShipOf(char glyph, const std::string& logShip, bool inSeat);

}  // namespace SCMiner
