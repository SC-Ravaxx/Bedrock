#pragma once
// Reporting to UEX (uexcorp.space), under the player's own UEX account - and
// only once they have opted in; without that it stays off. Two kinds:
//
//   - the player's refinery jobs, into the refinery list of their account
//     (user_refineries_jobs_add) - read off the REFINEMENT CENTER by the
//     terminal reader, handed over as a UexRefineryJob;
//   - a commodity terminal's prices as a UEX Datarunner reports them
//     (data_submit) - read off a picture the player took on purpose with the
//     game's Print Screen, checked by the player, sent on their click with
//     that very picture (Datarunner.h).
//
// What is here: UEX's ids for what Bedrock knows by name, the requests'
// bodies, what UEX's answers mean, and where the player's key is kept.

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

// UEX's ids for the terminals, commodities and refining methods Bedrock
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
    // The commodity terminals a picture of one can have been taken at, from
    // the terminal's shop as the game's log names it (SCShop_Levski_Cargo
    // Office_Commodities names its town) and else the place the log had the
    // player at: the one the shop's name points to, else every one at that
    // place - several at a city or a moon with more than one (Lorville's
    // CBD and L19). Empty where none fits. A refinery's ore sales are UEX's
    // raw terminals.
    std::vector<int> tradeTerminals(const std::string& shop, const std::string& place) const;
    // Every commodity terminal the game has, for the player to pick from:
    // id and name ("Admin - Checkmate"), by name
    std::vector<std::pair<int, std::string>> commodityTerminals() const;
    // A terminal's name ("Admin - Checkmate"); "" for an id it does not know
    std::string terminalName(int id) const;
    // Every commodity as UEX writes its name ("Ship Ammunition - Size 2"),
    // with its id - what a name read off a terminal is matched against
    const std::vector<std::pair<std::string, int>>& commodityNames() const { return commodityNames_; }

private:
    struct Terminal {
        int id = 0;
        std::string type;                 // "commodity", "commodity_raw", "refinery", ...
        std::string name;                 // as UEX writes it: "Admin - Checkmate"
        std::vector<std::string> names;   // its own names, folded
        std::vector<std::string> around;  // ...and the moon, planet and orbit it is at, folded
    };
    std::vector<Terminal> terminals_;
    std::vector<std::pair<std::string, int>> commodities_;      // folded name, id
    std::vector<std::pair<std::string, int>> commodityNames_;   // ...as UEX writes it, id
    std::vector<std::pair<std::string, int>> methods_;          // letters, id
};

// One commodity's row in a price report: what the terminal shows of it
struct UexPriceRow {
    int commodity = 0;       // UEX's id
    double price = 0;        // aUEC per SCU, as the terminal shows it
    int stock = -1;          // SCU the terminal shows; -1 left out
    int level = -1;          // 1 out of stock .. 7 max inventory; -1 left out
    std::string boxes;       // the cargo box sizes it is offered in, "8,16,24,32"; "" left out
};

namespace UexReports {

// The body of a data_submit: the prices at one terminal off one picture of
// it, one side of its list - what the terminal sells (`sold` false: UEX's
// price_buy, scu_buy and status_buy, the player's side of it) or what it
// buys (true: the _sell fields), never both in one report. With the game
// version the picture is of ("4.10.2"; "" leaves it to UEX), when the
// picture was taken (Unix seconds; 0 leaves it to UEX), and the picture
// itself, a JPEG in base64 ("" none)
std::string priceReport(int terminal, bool sold, const std::vector<UexPriceRow>& rows, long long when,
                        const std::string& gameVersion, const std::string& screenshotBase64, bool production);
// The body of a user_refineries_jobs_add: the commodities' ids beside their items
std::string refineryJob(int terminal, int method, double cost, int minutes,
                        const std::vector<std::pair<int, UexRefineryJob::Item>>& items, bool production);
// What UEX said to a report, in the window's words: "" when it took it.
// `username` gets the account's name UEX answers with, where it does.
std::string verdict(int httpStatus, const std::string& body, std::string* username = nullptr);
// The ids UEX gave the rows of a price report it took, in their order ("0"
// for a test report); empty where it gave none
std::vector<std::string> reportIds(const std::string& body);
// Of a price report turned down as duplicated_report, the entry UEX names
// (its `report`): the rows before that one were stored. "" where it names
// none.
std::string duplicateNamed(const std::string& body);
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
std::string base64(const unsigned char* bytes, size_t size);
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
