#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "Reputation.h"
#include "TradeBook.h"

namespace SCMiner {

struct ContractPayout;

// Contracts completed, and whether a blueprint came of them, line by line.
// While one of its blueprints is missing, a contract that pays out blueprints
// gives one every time it is completed; once it has none left, it gives
// nothing. So completing one and getting nothing says everything it could
// have given is owned - which fills in what the logs never showed: a
// blueprint had before the oldest log, or from somewhere that writes no
// line. In 4.10 logs, 42 of 42 such completions paid while one was missing;
// before 4.10 it does not hold, so logs of an older game are not looked at.
// Which contracts, and which blueprints, is CONTRACT_PAYOUTS
// (GameTextMarks.h): only titles every variant of which pays at 100 %, and
// only what all their pools share.
class ContractScan {
public:
    struct Unpaid {
        std::string title;                  // bare, as CONTRACT_PAYOUTS spells it
        const ContractPayout* payout = nullptr;
        std::string stamp;                  // "YYYY-MM-DD HH:MM", the completion's
    };
    // One log line, in order. `out` gets every completion now known to have
    // paid nothing: WINDOW seconds went by without a blueprint.
    void line(std::string_view text, std::vector<Unpaid>& out);
    // The file is over: what its last lines decide goes to `out`, what they
    // do not is dropped, and the scan is ready for the next file
    void finish(std::vector<Unpaid>& out);
    void reset();
    bool ruleHolds() const { return ruleHolds_; }
    // Which game wrote the log, as its header says - "pub" LIVE, "ptu" the
    // PTU and the tech preview - and its version, 41000 for 4.10.0; "" and 0
    // until the header has been read
    const std::string& env() const { return env_; }
    int version() const { return version_; }
    // The player's own contracts, taken and finished, as their lines come in
    // (see RankEvent); the library takes them out as it goes
    std::vector<RankEvent>& rankEvents() { return rankEvents_; }
    // A contract taken in this log and not over yet - the player's own, or
    // one somebody shared with them - by the very contract the objective
    // marker names. Over with the line the game writes for every end,
    //   <EndMission> Ending mission for player. MissionId[<M>] ...
    //       CompletionType[Abandon] Reason[Player left]
    // - of 301 contracts taken in 4.10, 264 ended in their log, every one
    // with that line (214 Complete, 44 Abandon, 13 Fail); given up raises no
    // notification, and the server's "MissionEnded" push comes only for the
    // finished and the failed. The other 37 were open when the game was
    // closed, and none came back in the next log: a new log starts with
    // nothing running.
    struct Running {
        std::string mission;     // MissionId
        std::string contract;    // "generator/debug"
        std::string stamp;       // when it was taken, the log's own time
        bool shared = false;
    };
    const std::vector<Running>& running() const { return running_; }
    int runningChanges() const { return runningChanges_; }   // counts every taking and every end

    // "Ship In Distress [300 Rep]*" and Bedrock's own marks, "Deep space hit
    // <EM4>[BP]</EM4>", as the game's text spells the title
    static std::string bareTitle(std::string_view logTitle);
    // The payout of a bare title, when exactly one entry matches it
    static const ContractPayout* payoutFor(const std::string& bare);
    // A title of the table - {*} where the game fills in a place or a name -
    // against a bare one, case aside
    static bool titleMatches(const char* pattern, const std::string& bare);
    // The line's own time, seconds of the UTC day count; -1 without one
    static std::int64_t secondsOf(std::string_view line);
    static constexpr int WINDOW = 30;        // seconds a blueprint may take to follow

private:
    bool ruleHolds_ = false;                  // the log is of 4.10 or later...
    bool versionSeen_ = false;                // ...as its header says
    struct Pending {
        std::string title;
        const ContractPayout* payout;
        std::string stamp;
        std::int64_t at;
        bool ambiguous;                       // another one completed inside its window
    };
    std::vector<Pending> pending_;
    std::int64_t last_ = -1;                  // the newest time seen
    std::int64_t lastReceived_ = -1;          // ...and the newest blueprint no completion has claimed
    std::string env_;
    int version_ = 0;
    std::map<std::string, std::string> contractOf_;   // MissionId -> "generator/debug", off the objective markers
    std::set<std::string> shared_;                    // MissionIds somebody else shared with the player
    std::vector<RankEvent> rankEvents_;
    std::vector<Running> running_;
    int runningChanges_ = 0;
    void endRunning(std::string_view mission);
    void rankLine(std::string_view text);
};

// What the player already owns, read off the game's own log. Every blueprint
// the HUD ever announced with "Received Blueprint: X" lands in Game.log as
//   <2026-08-30T21:17:49.987Z> [Notice] <SHUDEvent_OnNotification> Added
//   notification "Received Blueprint: Suckerpunch-XL Cannon: " [85] to queue...
// and that is the one text the game has for it (global.ini carries exactly
// one such key). The launcher keeps every old Game.log in logbackups\, so
// the history reaches back months; from then on the live log is tailed.
// Kept in owned_blueprints.txt beside the exe, with what was ticked by hand
// on top of it.
class BlueprintLibrary {
public:
    static constexpr const char* FILE_NAME = "owned_blueprints.txt";
    // Every "Received Blueprint" line the game ever wrote, kept where the
    // game cannot prune it. Not a copy of the logs: measured over a year of
    // them, these lines are 0.015 % of the bytes, and Game.log grows while
    // you play - copying it on every change would write megabytes a minute
    // to save a few hundred meaningful ones. The file is also read back, so
    // with the logs gone entirely the library still rebuilds from it.
    static constexpr const char* ARCHIVE_NAME = "blueprint_log_backup.txt";

    // The name the way the log and the table agree on it: HUD markup gone,
    // the class/size/grade prefix of ship parts gone ("Ind/1/B Guardian" is
    // "Guardian" in the table), one case, one kind of space
    static std::string normalise(const std::string& name);
    // Reads the blueprint name out of one log line; false for every line
    // that is not the event itself (the queue dumps repeat the text and must
    // not count). stamp gets "YYYY-MM-DD HH:MM".
    static bool parseReceived(const std::string& line, std::string& name, std::string& stamp);

    void load();          // owned_blueprints.txt, when there is one
    void save();
    // How many evidence lines are being kept, for the app to say so
    int archived() const { return (int)archived_.size(); }

    // The log this session reads; its backups are queued for reading. LIVE
    // and HOTFIX are one server and one character, so with either the other
    // folder's logs are read too (not twice when HOTFIX is a junction onto
    // LIVE, as it usually is). PTU and TECH-PREVIEW are copies and keep a
    // file of their own.
    void setGameLog(const std::filesystem::path& gameLog);
    const std::filesystem::path& gameLog() const { return gameLog_; }
    const std::string& channel() const { return channel_; }      // "LIVE", "LIVE + HOTFIX", "PTU"...
    const std::string& fileName() const { return fileName_; }

    // One step of work: the next unread backup, or whatever Game.log has
    // grown by. True when the library changed.
    bool poll();
    bool historyPending() const { return !backlog_.empty(); }
    int historyRead() const { return historyRead_; }

    bool owned(int blueprint) const { return owned_.count(blueprint) > 0; }
    // "YYYY-MM-DD HH:MM" from the log, "YYYY-MM-DD HH:MM (by hand)" when
    // ticked, "YYYY-MM-DD HH:MM (assumed)" when a contract that pays it out
    // paid nothing (see ContractScan), "" when not owned - one per
    // blueprint, for the window
    std::vector<std::string> stamps() const;
    void setByHand(int blueprint, bool owned);
    int ownedCount() const { return (int)owned_.size(); }
    int byHandCount() const;
    int assumedCount() const;
    // A contract completed without a blueprint: what it pays out of is
    // owned. Returns how many that made new. Public for the self-test.
    int assume(const ContractScan::Unpaid& unpaid);
    // Titles the rule failed for - a blueprint assumed owned came later
    // after all - which assume nothing any more
    const std::set<std::string>& doubted() const { return doubted_; }
    // One line of a game log, the way the logs are read: a blueprint received
    // goes in, and a contract that has now paid nothing adds what it pays out
    // of. Returns how many blueprints were new. finishFile() at the end of a
    // log that will not grow. Public for the self-test, which reads no file.
    int feedLine(std::string_view line, ContractScan& scan);
    int finishFile(ContractScan& scan);
    // Where an owned one came from: "log", "hand", "assumed: <contract>"; ""
    std::string sourceOf(int blueprint) const;
    // Received according to the log, but no row in this build's table
    // (renamed or removed since) - kept, so nothing is silently lost
    const std::map<std::string, std::string>& unknownNames() const { return unknown_; }

    // The player's rank with each outfit, out of the same logs: the contracts
    // they took and finished themselves, kept the way the blueprints' lines
    // are - in contract_log_backup.txt, one line a contract, where the game
    // cannot prune them - and the ranks they set in the Grind Planner, in
    // my_ranks.txt. Only logs of the game this library is for count: a PTU
    // log the launcher left in LIVE's folder is not LIVE's, for ranks or for
    // blueprints.
    static constexpr const char* RANK_ARCHIVE_NAME = "contract_log_backup.txt";
    static constexpr const char* RANK_FILE_NAME = "my_ranks.txt";
    std::vector<Standing> standings() const;
    // The game version the counting starts at: LIVE's last wipe; on the PTU,
    // which is wiped all the time, the newest build its logs come from
    int countsFrom() const;
    // A rank set by hand for an outfit's ladder, from now on; -1 goes back
    // to what the logs say
    void setRank(int faction, int ladder, int rank);
    // True once after the ranks changed - a contract taken or finished, a
    // rank set, a library read again
    bool ranksChanged() {
        const bool was = ranksChanged_;
        ranksChanged_ = false;
        return was;
    }
    int contractsKept() const { return (int)rankEvents_.size(); }
    // The contracts running in the game right now: taken in the Game.log
    // being written and not over (see ContractScan::Running)
    const std::vector<ContractScan::Running>& runningContracts() const { return liveScan_.running(); }
    int runningChanges() const { return liveScan_.runningChanges(); }
    // When the game last wrote to its log, by the last line's own time
    // (seconds since 1970, UTC; 0 before one was read). A game closed takes
    // its contracts with it, and its log says nothing of that - it only
    // stops. The file's own date is no help: Windows keeps it back while the
    // game holds the file open.
    long long liveLogEpoch() const { return liveEpoch_; }
    const std::map<std::string, RankEvent>& contracts() const { return rankEvents_; }
    // Whether every log has been read for the contracts and the game it came
    // from (see finishLogCheck)
    bool logsChecked() const { return ranksRead_; }
    int foreignDropped() const { return foreignDropped_; }

    // What was bought and sold at the commodity terminals, out of the same
    // logs (TradeBook) - kept in trades.txt, where the game cannot prune it.
    // A PTU log's trades are the PTU's, like its blueprints.
    const TradeBook& trades() const { return trades_; }

private:
    struct Entry {
        std::string stamp;
        std::string source;   // "log" or "hand"
        std::string name;     // as the log or the table wrote it
    };
    std::map<int, Entry> owned_;                   // blueprint index -> when, from where
    std::set<std::string> denied_;                 // normalised names ticked off by hand
    std::set<std::string> doubted_;                // contract titles, lower case, see doubted()
    void doubt(const std::string& title);
    // The game's old logs are read once more for the contracts in them: a
    // library saved by an older version has never seen a "Contract Complete"
    bool payoutsRead_ = false;
    // ...and once more for the contracts the player took and the game each
    // log came from: an older library took blueprints out of PTU logs that
    // lay in LIVE's folder, and kept no contracts. While that reading goes
    // on, the lines of the other game's logs are gathered, and at its end
    // they leave the evidence file and the library.
    bool ranksRead_ = false;
    bool checkingLogs_ = false;
    std::set<std::string> foreignLines_;
    int foreignDropped_ = 0;
    void finishLogCheck();
    bool foreign(const ContractScan& scan) const;
    std::map<std::string, RankEvent> rankEvents_;  // 'a' or 'c', then the MissionId
    std::vector<std::string> toRankArchive_;
    std::vector<RankSetting> rankSettings_;
    bool ranksChanged_ = false;
    void takeRankEvents(ContractScan& scan);
    bool keepRankEvent(const RankEvent& e);
    void loadRanks();
    void flushRankArchive();
    void saveRankSettings();
    std::string channelFile(const char* base) const;   // "my_ranks.txt" -> "my_ranks_PTU.txt" on the PTU
    TradeBook trades_;
    // The game's old logs are read once more for them: a library saved by an
    // older version passed those lines by
    bool tradesRead_ = false;
    ContractScan historyScan_;                     // a backup, start to end
    ContractScan liveScan_;                        // Game.log, as it grows
    std::map<std::string, std::string> unknown_;   // name -> stamp
    std::map<std::string, int> byName_;            // normalised name -> blueprint index
    std::set<std::string> archived_;               // evidence lines already kept
    std::vector<std::string> toArchive_;           // ...and the ones still to be appended
    void loadArchive();
    void flushArchive();
    std::string archiveName() const;
    std::filesystem::path gameLog_;
    std::string channel_;
    std::string fileName_ = FILE_NAME;
    std::vector<std::filesystem::path> folders_;   // the channel folders that make up this library
    std::vector<std::filesystem::path> extraLogs_; // the other folder's Game.log, read once per start
    struct Pending {
        std::filesystem::path file;
        std::string folderKey;                     // whose watermark to move, "" for none
    };
    std::vector<Pending> backlog_;                 // not read yet, oldest first
    std::uintmax_t offset_ = 0;                    // how far Game.log has been read
    long long liveEpoch_ = 0;                      // see liveLogEpoch()
    std::map<std::string, std::int64_t> historyUntil_;   // per folder: write time of the newest backup read
    std::int64_t legacyUntil_ = 0;                 // a 3.6.0 file's single watermark, until a folder claims it
    int historyRead_ = 0;
    int ticks_ = 0;
    bool dirty_ = false;

    void buildIndex();
    void queueHistory();
    bool importLine(const std::string& line);
    // Reads a file from an offset; the last unfinished line is left for the
    // next round. Returns how many blueprints were new. `whole`: the file
    // is read to its end and will not grow (a backup), so its scan finishes.
    int readFile(const std::filesystem::path& path, std::uintmax_t from, std::uintmax_t* endOffset,
                 ContractScan& scan, bool whole);
    static std::int64_t writeTime(const std::filesystem::path& path);
};

}  // namespace SCMiner
