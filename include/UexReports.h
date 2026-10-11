#pragma once
// Reporting to UEX (uexcorp.space), under the player's own UEX account - and
// only once they have opted in, after being shown what is sent and that
// every report carries a picture of the game for 90 days; without that
// it stays off. Two kinds:
//
//   - the prices the player trades at, as a data runner's reports
//     (data_submit): the terminal, the commodity, what it paid or asked per
//     SCU, when - and, for UEX's 90 days of evaluating a new data runner, a
//     screenshot of the game window: the terminal's list, with its stock
//     and prices, from around the trade (pictureFor);
//   - the player's refinery jobs, into the refinery list of their account
//     (user_refineries_jobs_add) - read off the REFINEMENT CENTER by the
//     terminal reader, handed over as a UexRefineryJob.
//
// What is here: UEX's ids for what Bedrock knows by name, the requests' bodies,
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
    // The commodity terminal a trade was made at, from the shop's code name
    // (SCShop_Levski_CargoOffice_Commodities names its town) and else the
    // place the log put the player at. 0, and `why`, unless exactly one
    // terminal fits: a report against the wrong terminal is worse than none
    // - the log's place is often only the moon or the system.
    int tradeTerminal(const std::string& shop, const std::string& place, std::string& why) const;
    // The refinery of a station as the refinery tables name it; 0 unless
    // exactly one fits
    int refineryTerminal(const std::string& station, std::string& why) const;
    // A terminal's short name, for the window; "" for an id it does not know
    std::string terminalName(int id) const;

private:
    struct Terminal {
        int id = 0;
        std::string type;               // "commodity", "commodity_raw", "refinery", ...
        std::string nickname;
        std::vector<std::string> names;   // its own names, folded
        std::vector<std::string> around;  // ...and the moon, planet and orbit it is at, folded
    };
    std::vector<Terminal> terminals_;
    std::vector<std::pair<std::string, int>> commodities_;   // folded name, id
    std::vector<std::pair<std::string, int>> methods_;       // letters, id
};

// One commodity's price at a terminal, as a report to UEX carries it
struct UexPriceRow {
    int commodity = 0;       // UEX's id
    bool sold = false;       // what the terminal pays (it buys) - else what it asks
    double price = 0;        // per SCU
    int stock = -1;          // SCU the terminal shows; -1 left out
    int level = -1;          // 1 out of stock .. 7 max inventory; -1 left out
};

namespace UexReports {

// The body of a data_submit for one trade: what the terminal paid for it (a
// sale) or asked (a purchase), per SCU, at `when`, with the shop's stock
// beside it (SCU) and its level (1 out of stock .. 7 max inventory) where
// they were read off the terminal (-1 leaves them out); a screenshot only
// while UEX's 90 days last (empty otherwise)
std::string priceReport(int terminal, int commodity, bool sold, double perScu, long long when,
                        const std::string& screenshotBase64, bool production, int stock = -1, int level = -1);
// ...and of several commodities at one terminal, off one picture of it
std::string priceReport(int terminal, const std::vector<UexPriceRow>& rows, long long when,
                        const std::string& screenshotBase64, bool production);
// The body of a user_refineries_jobs_add: the commodities' ids beside their items
std::string refineryJob(int terminal, int method, double cost, int minutes,
                        const std::vector<std::pair<int, UexRefineryJob::Item>>& items, bool production);
// What UEX said to a report, in the window's words: "" when it took it.
// `username` gets the data runner's name UEX answers with, where it does.
std::string verdict(int httpStatus, const std::string& body, std::string* username = nullptr);
// Whether an answer is about the key - wrong or unknown: no more reports
// until it changes
bool keyTrouble(int httpStatus, const std::string& body);
// Whether it is about the account behind a good key: "datarunner" where UEX
// takes reports from data runners only and the account is not one yet,
// "blocked" where UEX has barred it, "unverified" where it wants the account
// verified with RSI first; "" otherwise. The next trade is reported again -
// signed up as a data runner on UEX's site, it goes through.
std::string accountTrouble(int httpStatus, const std::string& body);
// A key as UEX hands them out: 40 characters, nothing but letters and digits
bool keyLooksRight(const std::string& key);
// Bedrock's app token out of uex_token.dat (CMakeLists.txt writes it): one
// line of decimal numbers, byte i mixed with (i * 37 + 91) % 256. "" for
// anything that does not come out as one word of printable characters.
std::string unmixAppToken(const std::string& line);
std::string base64(const unsigned char* bytes, size_t size);
// Of the pictures kept at a terminal - when each was taken (ms since 1970,
// UTC) and how much detail it has in the middle of the screen
// (Application::tradeFrameDetail) - the one to go with a trade requested at
// `tradeMs`: the terminal's list, not a dialog over it. CONFIRMATION before
// the request and TRANSACTION COMPLETE after it darken the whole terminal
// and leave less detail than the list, and UEX turns down a picture in which
// the prices cannot be seen. The newest with nearly the most detail
// (LIST_NEAR) from PICTURE_BEFORE_MS before the request - the deal being
// made, its price on it. Where one after it, up to PICTURE_AFTER_MS, has
// AFTER_JUMP times as much, none before was the list: then the first with
// nearly that much after it, the list once the player has accepted - unless
// one before had SURELY_LIST, the most a dialog was ever seen to leave and
// then some. -1 where no picture was taken in all that time.
int pictureFor(const std::vector<long long>& takenMs, const std::vector<double>& detail, long long tradeMs);
constexpr long long PICTURE_BEFORE_MS = 5000;
constexpr long long PICTURE_AFTER_MS = 20000;
constexpr double LIST_NEAR = 0.6;
constexpr double AFTER_JUMP = 1.6;
constexpr double SURELY_LIST = 6.5;   // lists measured 7.7-9.2, dialogs at most 4.3
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
