#pragma once
// Reporting to UEX (uexcorp.space), under the player's own UEX account - and
// only once they have opted in; without that it stays off. What goes is the
// player's refinery jobs, into the refinery list of their account
// (user_refineries_jobs_add) - read off the REFINEMENT CENTER by the terminal
// reader, handed over as a UexRefineryJob.
//
// What is here: UEX's ids for what Bedrock knows by name, the request's body,
// what UEX's answers mean, and where the player's key is kept.

#include <string>
#include <utility>
#include <vector>

namespace SCMiner {

// A refinery job as the REFINEMENT CENTER showed it
struct UexRefineryJob {
    std::string station;      // as the refinery tables name it: "Levski", "HUR-L1 Green Glade Station"
    int number = -1;          // WORK ORDER n on its terminal
    std::string method;       // as REFINING_METHODS names it
    double cost = -1;         // aUEC
    int seconds = -1;         // the processing time as quoted
    long long placed = 0;     // UTC, seconds since 1970
    struct Item {
        std::string material; // as REFINERY_MATERIALS spells it: "Iron (Ore)"
        int quantity = -1;    // cSCU put in
        int yield = -1;       // cSCU that comes out
    };
    std::vector<Item> items;
    // The same job read twice is one job
    std::string key() const;
};

// UEX's ids for the refineries, commodities and refining methods Bedrock
// names, out of UEX's lists terminals, commodities and refineries_methods
class UexDirectory {
public:
    bool read(const std::string& terminalsJson, const std::string& commoditiesJson, const std::string& methodsJson,
              std::string& why);
    bool empty() const { return terminals_.empty(); }

    // The commodity UEX lists under this name, case aside (Aluminium is its
    // Aluminum); 0 where there is none
    int commodity(const std::string& name) const;
    // The refining method, by its letters alone; 0 where there is none
    int method(const std::string& name) const;
    // The refinery of a station as the refinery tables name it; 0 unless
    // exactly one fits: a report against the wrong one is worse than none
    int refineryTerminal(const std::string& station, std::string& why) const;

private:
    struct Terminal {
        int id = 0;
        std::string type;               // "commodity", "commodity_raw", "refinery", ...
        std::vector<std::string> names;   // its own names, folded
    };
    std::vector<Terminal> terminals_;
    std::vector<std::pair<std::string, int>> commodities_;   // folded name, id
    std::vector<std::pair<std::string, int>> methods_;       // letters, id
};

namespace UexReports {

// The body of a user_refineries_jobs_add: the commodities' ids beside their items
std::string refineryJob(int terminal, int method, double cost, int minutes,
                        const std::vector<std::pair<int, UexRefineryJob::Item>>& items, bool production);
// What UEX said to a report, in the window's words: "" when it took it.
// `username` gets the account's name UEX answers with, where it does.
std::string verdict(int httpStatus, const std::string& body, std::string* username = nullptr);
// Whether an answer is about the key - wrong or unknown: no more reports
// until it changes
bool keyTrouble(int httpStatus, const std::string& body);
// Whether it is about the account behind a good key: "datarunner" where UEX
// takes reports from data runners only and the account is not one yet,
// "blocked" where UEX has barred it, "unverified" where it wants the account
// verified with RSI first; "" otherwise. The next job is reported again.
std::string accountTrouble(int httpStatus, const std::string& body);
// A key as UEX hands them out: 40 characters, nothing but letters and digits
bool keyLooksRight(const std::string& key);
// Bedrock's app token out of uex_token.dat (CMakeLists.txt writes it): one
// line of decimal numbers, byte i mixed with (i * 37 + 91) % 256. "" for
// anything that does not come out as one word of printable characters.
std::string unmixAppToken(const std::string& line);
// A string as JSON writes it, quotes included
std::string jsonText(const std::string& text);

}  // namespace UexReports

// The player's UEX secret key, beside their data and nowhere else: sealed to
// the Windows account with DPAPI (under Linux, readable by its owner alone),
// never in a backup, never in the debug folder, never in a log
namespace UexKey {
bool save(const std::string& key);
std::string load();        // "" where there is none, or it cannot be opened
void forget();
const char* const FILE_NAME = "uex_key.dat";
}  // namespace UexKey

}  // namespace SCMiner
