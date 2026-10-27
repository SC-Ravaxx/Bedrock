#pragma once
// UEX Datarunner: a commodity terminal's prices reported to UEX the way
// SC-Datarunner-UEX goes about it. The player stands at the terminal, picks
// the terminal's own inventory with nothing selected, and takes a picture
// with the game's Print Screen; Bedrock finds it in the game's ScreenShots
// folder, reads it (CommodityTerminal.h) and shows each row beside its piece
// of the picture. Nothing goes until the player has looked at every value
// and clicked Send: then one report goes to UEX's data_submit, with that
// very picture. One picture, one report.
//
// What is here is the part without a window and without the network: which
// channel a screenshot folder belongs to, the rows a picture makes and how
// sure each of their values is, UEX's commodity and terminal for them, what
// still stands in the way of Send, and the record of what went - kept over
// a restart, for UEX's five minutes between two reports of a commodity.

#include "CommodityTerminal.h"
#include "TradeBook.h"
#include "UexReports.h"

#include <opencv2/core.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace SCMiner {

class TextLineReader;

namespace Datarunner {

// The game's screenshot folder beside the log it writes:
// <channel>\Game.log -> <channel>\ScreenShots
std::filesystem::path screenshotsBeside(const std::filesystem::path& gameLog);
// The channel a screenshot folder belongs to, as UEX's game versions go:
// "LIVE" for LIVE and HOTFIX (the same servers as LIVE), "PTU" for PTU,
// EPTU and TECH-PREVIEW; "" for a folder that says neither
std::string channelOf(const std::filesystem::path& screenshots);

// UEX's five minutes: a commodity reported at a terminal is not taken
// again from the same player within them (duplicated_report)
constexpr long long COOLDOWN_SECONDS = 5 * 60;
// ...and how old a report's date may be
constexpr long long OLDEST_SECONDS = 30ll * 24 * 3600;
// The cargo box sizes UEX takes, in SCU
bool boxesUexTakes(const std::string& sizes);

// One row of a picture: a commodity's card, as the reader made it out and
// the player has it since
struct Row {
    std::string read;               // the name as read off the picture
    int commodity = 0;              // UEX's id; 0 not known
    std::string name;               // ...and UEX's name for it
    int level = -1;                 // 1 out of stock .. 7 max inventory
    int stock = -1;                 // SCU
    int price = -1;                 // aUEC per SCU
    bool perUnit = false;           // a cargo box was picked: the picture shows one box's price
    // How sure each value is: Sure as read, or once the player has set or
    // confirmed it; Check to be compared with the picture; Missing to be
    // filled in
    Certainty nameSure = Certainty::Missing;
    Certainty levelSure = Certainty::Missing;
    Certainty stockSure = Certainty::Missing;
    Certainty priceSure = Certainty::Missing;
    std::string boxes;              // the cargo box sizes the log listed for it, "8,16,24,32"; "" none
    cv::Rect card;                  // where it stands on the picture
    cv::Mat snippet;                // ...that piece of the picture, to show beside it
    bool removed = false;           // taken out by the player
};

// A picture and what came of it
struct Shot {
    enum class State { Check, Sending, Sent, Partly, Refused, Failed };
    int id = 0;
    std::filesystem::path file;     // the game's own picture: read, never changed
    // Star Citizen can save its screenshots black (seen with HDR on). Then
    // the rows are read off the picture Bedrock took of the game the moment
    // the game's came, and `own` is set; `shown` is where that picture was
    // written for the player to open (debug\uex_pictures), "" where it was
    // not. `black`: the picture read was black - Bedrock had none of its own
    bool own = false;
    bool black = false;
    std::filesystem::path shown;
    long long taken = 0;            // when it was written, Unix seconds
    int width = 0, height = 0;
    std::string channel;            // "LIVE", "PTU"; "" not known
    // The list it shows: 1 BUY (what the terminal sells), 2 LOCAL MARKET
    // VALUE (what it buys); 0 not made out. Sure where read off its tab.
    int side = 0;
    bool sideSure = false;
    std::string shop, place;        // from the game's log, where it had them
    bool matched = false;           // UEX's lists and the log have been asked (match)
    std::vector<std::pair<int, std::string>> terminals;   // UEX's terminals that fit: id, name
    int terminal = 0;               // the one: the only one that fits, or the player's pick
    std::vector<Row> rows;
    std::vector<unsigned char> jpeg;   // the picture as UEX gets it (pictureForUex)
    State state = State::Check;
    std::string note;               // what UEX said, in the window's words
    std::vector<std::string> ids;   // the report ids UEX gave
};

// A picture as UEX gets it with a report: at most 1080 pixels tall - a
// 5120x1440 game comes out 3840x1080, its terminal readable - a JPEG well
// under UEX's 10 MB. Empty for an empty picture.
std::vector<unsigned char> pictureForUex(cv::Mat frame);

// A picture read: its rows as the reader made them out, each with its piece
// of the picture, the list it shows, and the picture as UEX gets it. No rows
// where it shows no commodity terminal - or cannot be read. Where the game's
// picture is black (blackPicture), `own` - Bedrock's picture of the game,
// taken when the game's came - is read instead, if there is one.
Shot readPicture(const std::filesystem::path& file, TextLineReader& reader, const cv::Mat& own = cv::Mat());
// ...the same, of a picture in hand (the self-test, the screenshots)
Shot readPicture(const cv::Mat& bgr, TextLineReader& reader);
// A picture with nothing on it, as Star Citizen saved its screenshots with
// HDR on: nought throughout. A dark scene still has its lights and its HUD.
bool blackPicture(const cv::Mat& bgr);

// The commodity among UEX's a name read off a terminal is: Sure where it is
// one of their names letter for letter, Check where it is one with a slip or
// two (commodityNamed) and no other comes as near - one the log listed for
// the terminal before one it did not -, Missing otherwise. Its id, 0 none.
int matchCommodity(const std::string& read, const std::vector<std::pair<std::string, int>>& names,
                   const std::vector<int>& listed, std::string& name, Certainty& sure);
// UEX's commodities nearest a name read off a terminal, the nearest first:
// what the player picks from when it is not one of them
std::vector<std::pair<std::string, int>> nearestCommodities(const std::string& read,
                                                            const std::vector<std::pair<std::string, int>>& names,
                                                            size_t count);
// Everything about a picture that UEX's lists and the game's log can say:
// each row's commodity, the terminal (the only one that fits, or the ones
// to pick from), and the cargo box sizes the log listed for what the
// terminal sells - they go with the BUY list only (reportBody). `fill` is
// the terminal the log saw fill before the picture was taken; null where
// it saw none.
void match(Shot& shot, const UexDirectory& directory, const TradeBook::TerminalFill* fill);

// What went to UEX, row by row - kept in uex\reported.txt with the refinery
// jobs, a line each: "price|<terminal>|<side>|<commodity>|<sent>|<taken>"
struct Sent {
    int terminal = 0, side = 0, commodity = 0;
    long long sent = 0;             // when it went, Unix seconds
    long long taken = 0;            // when its picture was taken
};
std::string sentKey(const Sent& sent);
bool readSentKey(const std::string& key, Sent& sent);

// Why a row of a picture cannot go now: not at all, as another picture
// taken within UEX's five minutes of this one already carried it to UEX
// (Hold::Sent), or not yet, as it went less than five minutes ago
// (Hold::Wait, `seconds` until it may)
enum class Hold { None, Sent, Wait };
Hold holdOf(const Shot& shot, const Row& row, const std::vector<Sent>& sent, long long now, long long* seconds = nullptr);
// What still stands in the way of Send, in the window's words - "" where
// nothing does
std::string whyNotSend(const Shot& shot, const std::vector<Sent>& sent, long long now);
// The rows that go with Send: neither removed nor held
std::vector<const Row*> rowsToSend(const Shot& shot, const std::vector<Sent>& sent, long long now);
// The report itself: the rows to send, the game version of the picture's
// channel, the picture's time and the picture
std::string reportBody(const Shot& shot, const std::vector<Sent>& sent, long long now, const std::string& gameVersion,
                       bool production);

}  // namespace Datarunner
}  // namespace SCMiner
