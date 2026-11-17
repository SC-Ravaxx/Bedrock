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
    // ...and which of the game's two tables it is (PanelShapes.h): 'F' as
    // the HUD drew it up to Star Citizen 4.10.2, 'S' since 4.10.3 - each has
    // its line, its list, its Head column and its icons at places of its own
    char tableShape = 0;
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
    // read make of it. SHATTER DMG, the seventh row since 4.10.3, is not read.
    int head[6] = {-1, -1, -1, -1, -1, -1};
    double headSure[6] = {0, 0, 0, 0, 0, 0};   // the reader's mean certainty
    bool headAgrees = false;
    // the list was looked at, every line that says MODULE named one the game
    // has, the Head column agrees, and no icon stands past the list's
    bool modulesRead = false;

    bool complete() const { return laser != nullptr && modulesRead; }
};

// The laser's table, left of the middle of the screen, in either of the
// shapes the game has drawn it in (SeenLoadout::tableShape). False when the
// mining HUD is not up.
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
// ...the laser of a MOLE's turret: size 2, the one size the MOLE takes
const MiningLaser* turretLaserByHudLine(const std::string& line);

// The laser a MOLE's turret fires, off the turret's mining HUD: the line
// "HOFSTEDE-S2 MINING LASER" over its table, low and left of the middle.
// That table stands over the turret's own gauges and does not match the
// one-seat ships' by its shape, so the line is looked for row by row. Null
// when no row there names a laser a turret takes; `read` gets the line.
const MiningLaser* readTurretLaser(const cv::Mat& frame, TextLineReader& lines, std::string* read = nullptr);
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

// The ship a scan panel's label column belongs to: the Prospector's (L, M),
// the MOLE's pilot seat (K, P, Q) and turrets (T, W), the Golem's (G, H).
// Null for any other column and for none.
const char* panelShipOf(char glyph);

// Whether to look for a scan panel at all: not while the log has the player
// in the seat of a ship that cannot mine. Out of the seat the player may
// have boarded a mining ship the log did not name (PanelShipWitness).
bool panelsWanted(const std::string& logShip, bool inSeat);

// The ship the scan panels show the player in, where the log has no seat
// to say it. The log names a ship boarded only by its crew channel ("You
// have joined channel"), and on 16 November 2026 the game stopped writing
// that line three hours into a session: a MOLE and a Golem were boarded
// without it, and Bedrock kept the Prospector's fit. Two sure panels of one
// ship in a row name it, once: a fit picked by hand afterwards stays. A
// seat the log names starts it over.
class PanelShipWitness {
public:
    // One look: the label column of a panel read for sure, 0 for none, and
    // whether the log has the player in a seat. The ship to switch the fit
    // to, by id, or null.
    const char* look(char glyph, bool logSeat);

private:
    const char* seen_ = nullptr;   // the ship the last sure panel named
    int running_ = 0;              // ...and sure panels in a row that named it
    const char* said_ = nullptr;   // the ship last switched to
};

}  // namespace SCMiner
