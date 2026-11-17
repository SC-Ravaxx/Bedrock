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
// asteroids, scaled down). Nyx I's rocks in 5.0 have 13.141 m3 to the SCU
// (RockModel in MiningAdvice.h): the same rules by their own numbers.
//
// Of the looks Bedrock took at rock panels from 16 September to 8 November
// 2026, 13,631 read an ore, a mass and SCU: 151 showed a rock the game had
// not finished scanning (below), 221 a figure misread - an SCU of 1.00, 7.00
// or 474.54 - and the median over five looks passed some of both to the view.
#include <climits>
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

// Where the place does not say which rocks are around (in Nyx, before the
// log names the body): whether the figures fit a Nyx I rock and not the
// usual kind - too light for the usual rule, and nothing wrong by the 5.0
// one. A heavy Nyx I rock (Savrilium, Gold) fits both and is not told.
bool v5Fits(const RockStats& rock);

// The rock's kg per SCU as a rock of the usual numbers with the same parts
// would weigh it, for the rules written for those (nameContentsByRock); 0
// where the mass or the SCU was not read
double usualKgPerScu(const RockStats& rock);

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

// Whether two readings of a figure can be one: the same as the panel prints
// it (`places` after the point), or one numeral apart - 21,460 and 21,464 on
// one Copper rock, look by look. False where either is not above 0.
bool oneNumeralApart(float a, float b, int places);

// Whether a look is another rock's than the readings the view holds, whose
// middle is `held`: its mass more than one numeral off, and another figure
// besides - the SCU, the instability, or the resistance printed the same way.
// A misread puts one figure wrong; another rock has them all different. Where
// no other figure was read on both, a mass a quarter or more off says it.
//
// Only a quarter off, the readings of two rocks went into one middle: in the
// MOLE's turret, 15,223 kg showed with the SCU of the 12,229 kg rock before
// it, and of his 1,079 looks of 17 September 2026 by the face reader, 16
// showed figures no look had read together.
bool anotherRock(const RockStats& held, const RockStats& look);

// A look of the rock on view (`held`, the same mass to the kilogram) that read
// no SCU takes the view's. The line and face readers read no SCU, and the
// shapes miss it on some looks: in the MOLE's turret on 17 November 2026 the
// view showed 25.27 SCU and none a look apart, the composition's SCU with it.
// A mass one numeral off may be another rock and takes nothing.
void keepScu(const RockStats& held, RockStats& look);

// Whether one mass is another as a misread makes it: one numeral more, one
// numeral less, or one numeral other (89,667 or 9,966 for 8,966). False
// where they are the same to the kilogram or either is not above 0.
bool numeralOff(float a, float b);

// The view's guard for the looks the second reader and the face reader
// read, which come to it one by one: a look whose mass is the view's
// misread (numeralOff) waits for a second look that reads the same. In the
// MOLE's pilot seat on 17 November 2026 the line reader read 11,294 as
// 112,943 and 8,966 as 89,667, one look each; every shape look empties the
// window the view takes the middle of, so the look after one stood alone,
// and the view showed those masses and lost the SCU with them. The other
// figures are not asked: the instability the shapes settled for that rock
// was 513.11 for 513.44, so they could not tell the same rock either.
class MassCheck {
public:
    // Whether the view takes the look now, the view showing `shown`
    bool takes(const RockStats& shown, const RockStats& look);

private:
    float waiting_ = 0;   // the mass of the look held back
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
    bool v5 = false;         // a Nyx I rock of 5.0 (RockModel): kept, not sent
    std::vector<ScanPart> parts;   // the list in the panel's order, where the view read it whole
    // How Bedrock saw it, for telling why some HUDs give no list: the height
    // of the screen picture (0 not known), the size the panel's label column
    // was found at (0: the figures were read without it), and whether a
    // reading of its list began at all
    int rows = 0;
    float labelScale = 0;
    bool listTried = false;
};

// The rocks of a session, a Scan each. A rock - its ore and its mass - comes
// in with the first look the view takes of it. Looks of it after that add
// what the first did not have: the resistance in white, the instability, the
// range, the list. Its SCU is the one most of its looks read in the same
// picture as its mass: a misread SCU, or one the view still held from the
// rock before, does not make it a second rock.
//
// Off view, a rock waits HOLD_SECONDS before it is done: a list whose second
// reading comes back after the next rock came on view is still its own, and
// a rock looked at again within that time is the same rock. A rock looked at
// again later counts once, or twice where only the second time read its list.
class Recorder {
public:
    static constexpr long long HOLD_SECONDS = 30;
    // The rock on view - the figures the view shows - and the look that
    // showed them: whether the instability was read at all, and the SCU read
    // with the mass
    void taken(const RockStats& shown, const RockStats& look, long long observed, const std::string& ship,
               const std::string& place, int rows = 0, float labelScale = 0);
    // A reading of the list began while the view showed a rock of that mass
    void tried(float mass);
    // The list read twice alike while the view showed a rock of that mass:
    // that rock's, on view or waiting
    void listed(float mass, const std::vector<ScanPart>& parts);
    // The rock on view went off it
    void gone();
    // The rocks done by `now` (UTC seconds), the first seen first; by
    // default every rock off view, waiting or not
    std::vector<Scan> done(long long now = LLONG_MAX);
    // Whether a rock is on view or waiting
    bool holding() const { return open_ || !waiting_.empty(); }

private:
    struct Rock {
        Scan scan;
        long long last = 0;           // its last look
        std::map<int, int> masses;    // the masses the view showed for it, kg: looks
        std::map<int, int> read;      // SCU in hundredths: looks that read it with the rock's mass
        std::map<int, int> shown;     // ...and looks the view showed it on
    };
    // The rock the view shows: the same ore, and a mass the view showed for
    // it - or one numeral off that, with an SCU it showed (a settled 21,460
    // read again as 21,464)
    static bool sameRock(const Rock& rock, const RockStats& shown);
    static bool ofMass(const Rock& rock, float mass);
    void release(long long now);
    void finish(Rock& rock);
    bool open_ = false;
    Rock rock_;
    std::vector<Rock> waiting_;
    std::vector<Scan> done_;
    struct Counted {
        std::string ore;
        float mass = 0;
        float scu = 0;
        bool listed = false;
    };
    std::vector<Counted> counted_;   // rocks done, and whether with their list
};

// Which rock a list is of. A whole reading of the list says the same as
// another of the same rock's: two pictures that read alike for one mass make
// it that rock's list, whichever rock the view has moved on to by the time
// the second reading comes back. The last eight rocks' first readings are
// kept.
class ListReadings {
public:
    // A whole reading of the list, read while the view showed `mass`. True:
    // it says what the kept one for that mass said.
    bool confirms(float mass, const std::vector<ScanPart>& parts);

private:
    struct Kept {
        float mass = 0;
        std::string key;
    };
    std::vector<Kept> kept_;
};

// "prospector", "mole" or "golem" for the ship the log names; "" for any other
std::string shipKind(const std::string& logShip);
// "Taranite 9.37 957; Taranite 38.51 310; inert 52.11"
std::string partsText(const std::vector<ScanPart>& parts);
// rock_scans.csv: its header, and a rock's line under it - `time` as the
// caller writes it
extern const char* const CSV_HEADER;
std::string csvLine(const Scan& scan, const std::string& time);

// Sharing them (server/refinery/README.md, "Rock scans"): sent unless the
// player takes the tick off in Settings (sharerocks=0), to work out rock
// sizes, densities and how the game sets resistance and instability.
// Nothing of the player goes along; the session's number - sixteen hex
// digits drawn at each start - groups one session's rocks, a rock and the
// pieces it broke into, and says nothing more.
constexpr size_t ROCKS_PER_REPORT = 50;
std::string newSession();
// Whether the service takes the rock as it stands: a ship ore, its figures
// in the ranges the service checks, and the usual rocks' numbers - not a
// Nyx I rock of 5.0. A report with one it does not take is turned down whole.
bool sendable(const Scan& scan);
// The channel as a report to the service names it: "LIVE + HOTFIX" is
// LIVE's; "" for one the service does not know, which a report leaves out
std::string channelKind(const std::string& channel);
// The report: the game's version as Game.log's header gives it, 41002 for
// 4.10.2, 0 leaves it out; a channel the service does not know is left out,
// and so is a place with a character the service does not take
std::string body(const std::vector<Scan>& scans, const std::string& bedrockVersion, int gameVersion,
                 const std::string& channel, const std::string& session);

}  // namespace RockScans
}  // namespace SCMiner
