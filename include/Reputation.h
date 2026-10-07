#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace SCMiner {

struct RepContract;

// One contract of the player's own, as their log tells of it: taken, or
// finished. The log names the very contract -
//   <CLocalMissionPhaseMarker::CreateMarker> Creating objective marker:
//       missionId [M], generator name [Covalex_Hauling], contract [HaulCargo_...]
// comes a moment before
//   <SHUDEvent_OnNotification> Added notification "Contract Accepted: <title>: " ...
//       MissionId: [M]
// in 3,461 of 3,462 logged acceptances - and the title alone would not
// do: "They Will Be Missed" is a Neutral contract without blueprints and
// a Veteran one with them. A contract shared by somebody else
//   Received share push message: ownerId[..] - missionId[M]
// is not the player's own: it can be taken at any rank, and says nothing
// of theirs (see ContractScan). The marker carries the record's id as well
// (contractDefinitionId), but a variant the game numbers - "..._Hard_0"
// for the files' "..._Hard" - has an id of its own (122 of 637 markers in
// 4.10); by name all of them are found.
struct RankEvent {
    std::string stamp;       // the log's own time, "2026-10-02T16:39:32.688" (UTC) - sorts as time does
    bool completed = false;  // false: accepted
    std::string mission;     // MissionId: the same contract read twice is one
    std::string contract;    // "generator/debug", a key of REP_CONTRACTS
    int version = 0;         // the game's, 41000 for 4.10.0 - what a wipe is told by
};

// A rank the player said they hold, in the Grind Planner: counting goes on
// from it. Kept by the records' GUIDs, which outlast the order of the tables.
struct RankSetting {
    std::string faction;     // REP_FACTIONS[].guid
    std::string ladder;      // REP_LADDERS[].guid
    long long reputation = 0;   // where the rank begins, when it was set
    std::string stamp;       // when, as a RankEvent's
};

// The player's rank on one outfit's ladder, and what it rests on
struct Standing {
    int faction = -1, ladder = -1;   // REP_FACTIONS, REP_LADDERS
    int rank = 0;                    // of the ladder's ranks
    long long reputation = 0;        // about this much, at least
    // What the figure is counted from: a contract taken that asked for a rank
    // (anchorRank, the first time one at that rank was taken), else the rank
    // the player set (set), else the last wipe - and what the contracts
    // finished since then added, and how many
    int anchorRank = -1;
    std::string anchorStamp;
    long long gained = 0;
    int completions = 0;
    bool set = false;
    std::string setStamp;
    int proven = -1;                 // the highest rank a contract taken proves, -1 none
};

namespace Reputation {

// The game version that last reset reputation on LIVE: 4.8.0. Everybody
// started again at Neutral; blueprints were kept. A wipe to come goes
// here, or every rank stays where the old game had it.
constexpr int LIVE_WIPE = 40800;

// REP_CONTRACTS by "generator/debug"; nullptr for one this build does not know
const RepContract* contract(std::string_view key);
// The rank a reputation falls in on a ladder
int rankOf(int ladder, long long reputation);
// REP_FACTIONS and REP_LADDERS by GUID; -1 when this build has no such record
int factionByGuid(std::string_view guid);
int ladderByGuid(std::string_view guid);
// Where a rank of a ladder begins
long long rankMin(int ladder, int rank);
// Every outfit and ladder the events and settings say anything about. Events
// of a game older than fromVersion do not count: the wipe took them away.
std::vector<Standing> standings(const std::vector<RankEvent>& events, const std::vector<RankSetting>& settings,
                                int fromVersion);
// "4.10.0" of 41000
std::string versionName(int version);
// A log stamp, "2026-10-02T16:39:32" with or without its milliseconds, as
// seconds since 1970 (UTC); -1 for anything else
long long epochOf(const std::string& stamp);

}  // namespace Reputation

}  // namespace SCMiner
