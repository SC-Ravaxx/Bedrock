#pragma once
// The commodity terminal's SHOP INVENTORY, read off a picture of it: each
// commodity's card - its name, the level of the shop's stock in the game's
// words, the stock itself (SHOP QUANTITY) and its price. UEX takes a price
// with the stock and its level the terminal shows beside it (scu_buy,
// status_buy, post_data_submit), and the log has neither: a report with the
// price alone goes to UEX's staff, who turn it down when the picture shows
// other figures (11 October 2026).
//
// The card, at Pyro Gateway (Stanton) and at Checkmate alike - the colours
// differ, the layout does not:
//
//     [icon] | HEPHAESTANITE              SHOP QUANTITY
//            | MAX INVENTORY                  1,050 SCU
//                                            ¤4,167/SCU
//
// The bar left of the name fills with the level; the lines on the right
// stand flush right. A picked card says "/UNITS" for the price of one cargo
// box and opens below with the box sizes, the amount and BUY.

#include <opencv2/core.hpp>

#include <functional>
#include <string>
#include <vector>

namespace SCMiner {

class TextLineReader;

struct CommodityCard {
    std::string name;        // as read, a long name's two lines joined
    int level = -1;          // UEX's scale: 1 out of stock .. 7 max inventory; 0 "Inventory Updating"; -1 not made out
    int stock = -1;          // SCU, SHOP QUANTITY
    int price = -1;          // aUEC, as the card shows it
    bool perUnit = false;    // "/UNITS": a cargo box is picked, the price is one box's
    // How the price was made out: 2 the picture as it is and at 1080 said the
    // same, 1 one of them alone (a 1080 picture is read once); 0 not at all
    int priceReads = 0;
    cv::Rect card;           // where it stands on the picture
};

struct CommodityScreen {
    std::vector<CommodityCard> cards;   // top to bottom
    // The list they are on: 1 the shop's stock, what it sells (the tab BUY,
    // IN STOCK), 2 what it buys (LOCAL MARKET VALUE, IN DEMAND); 0 not made
    // out. Read for the whole list only - one card looked for is a purchase.
    int side = 0;
    int lines = 0;                      // lines of text read, for the tools
    double ms = 0;
};

// Every card of the SHOP INVENTORY list on a picture of the terminal, as
// the player uses it - the terminal filling the middle of the view. With a
// commodity's name only its card, the others' names read no further than
// their first line (a line read costs 50 to 100 ms).
CommodityScreen readCommodityScreen(const cv::Mat& bgr, TextLineReader& reader, const std::string& wanted = "");

// A picture kept from the terminal around a trade: when (ms since 1970,
// UTC) and the picture, a JPEG
struct TerminalPicture {
    long long ms = 0;
    std::vector<unsigned char> jpeg;
};
// The card of a commodity traded at `tradeMs` for `perScu` a SCU, off the
// pictures kept around the trade: two pictures that say the same of it,
// nearest the trade first - the newest before it, else the first after the
// confirmation - its price the trade's to the aUEC (the reader takes the
// currency sign for a 1 now and then), a picked card's box price never;
// `picture` the nearer of the two, for UEX's screenshot
struct TradeCard {
    bool found = false;
    int stock = -1, level = -1, price = -1;
    size_t picture = 0;
    int read = 0;            // pictures read
};
TradeCard readTradeCard(const std::vector<TerminalPicture>& pictures, const std::string& name, long long tradeMs,
                        double perScu, TextLineReader& reader);

// A visit to a terminal's list, picture by picture - read whole as the
// player looks and scrolls, bought or not: the cards two pictures of the
// shop's stock agree on, gathered for UEX into as few of the pictures as
// show them all, as a report takes one picture
class TerminalSession {
public:
    void add(long long ms, const CommodityScreen& screen);
    size_t pictures() const { return seen_.size(); }
    // a picture of what the shop buys among them - not reported yet
    bool sawDemand() const;
    struct Row {
        std::string name;    // as read
        int level = -1, stock = -1, price = -1;
        int priceReads = 0;  // the most its price was read with (CommodityCard)
    };
    struct Report {
        size_t picture = 0;  // which of the pictures added
        long long ms = 0;
        std::vector<Row> rows;
    };
    // `wanted` says which agreed cards go (a known commodity, a price near
    // UEX's last, not sent a moment ago)
    std::vector<Report> reports(const std::function<bool(const Row&)>& wanted) const;

private:
    struct Picture {
        long long ms = 0;
        int side = 0;
        std::vector<CommodityCard> cards;
    };
    std::vector<Picture> seen_;
};

// The pieces, for the tools and the self-test
int commodityLevelIn(const std::string& text);                     // "MAX INVENTORY" -> 7; -1 none
int parseShopQuantity(const std::string& text);                    // "24,000 SCU" -> 24000; -1
int parseCommodityPrice(const std::string& text, bool& perUnit);   // "¤1,879/Units" -> 1879, true; -1
bool commodityNamed(const std::string& read, const std::string& name);   // a card's name is this commodity's

}  // namespace SCMiner
