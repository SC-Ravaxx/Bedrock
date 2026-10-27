#pragma once
// The commodity terminal's SHOP INVENTORY, read off a picture of it: each
// commodity's card - its name, the level of the shop's stock in the game's
// words, the stock itself (SHOP QUANTITY) and its price. The picture is one
// the player took on purpose with the game's own Print Screen, for a report
// to UEX they check before it goes: each value comes with how sure the
// reader is of it, and the player is shown the card's piece of the picture
// beside it.
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

#include <string>
#include <vector>

namespace SCMiner {

class TextLineReader;

// How sure a value read off the picture is: Sure where two readings of it
// agree, or it is word for word one the game writes; Check where it was made
// out once, or with a slip; Missing where it was not made out at all
enum class Certainty { Missing = 0, Check = 1, Sure = 2 };

struct CommodityCard {
    std::string name;        // as read, a long name's two lines joined
    int level = -1;          // UEX's scale: 1 out of stock .. 7 max inventory; 0 "Inventory Updating"; -1 not made out
    int stock = -1;          // SCU, SHOP QUANTITY
    int price = -1;          // aUEC, as the card shows it
    bool perUnit = false;    // "/UNITS": a cargo box is picked, the price is one box's
    // How the price and the stock were made out: 2 the picture as it is and
    // a second reading at another size said the same (a picture over 1080
    // at 1080, a smaller one at three quarters), 1 one of them alone, or
    // the two differed (the picture's own then); 0 not at all
    int priceReads = 0;
    int stockReads = 0;
    // The level's words: 0 exactly the game's, more the slips taken to
    // read them as the game's; -1 none - and how many slips more the next
    // nearest level's words would have taken
    int levelSlips = -1;
    int levelMargin = 0;
    cv::Rect card;           // where it stands on the picture: the name, the level and the right column

    Certainty levelSure() const;
    Certainty stockSure() const;
    Certainty priceSure() const;
};

struct CommodityScreen {
    std::vector<CommodityCard> cards;   // top to bottom
    // The list they are on: 1 the shop's stock, what it sells (the tab BUY,
    // IN STOCK), 2 what it buys (LOCAL MARKET VALUE, IN DEMAND); 0 not made
    // out
    int side = 0;
    int lines = 0;                      // lines of text read, for the tools
    double ms = 0;
};

// Every card of the SHOP INVENTORY list on a picture of the terminal, as
// the player uses it - the terminal filling the middle of the view. No
// cards where the picture shows no commodity terminal.
CommodityScreen readCommodityScreen(const cv::Mat& bgr, TextLineReader& reader);

// The pieces, for the tools and the self-test
// "MAX INVENTORY" -> 7; -1 none. `slips`: how far the text is from the level's
// words; `margin`: how much further the next nearest level's are
int commodityLevelIn(const std::string& text, int* slips = nullptr, int* margin = nullptr);
int parseShopQuantity(const std::string& text);                    // "24,000 SCU" -> 24000; -1
int parseCommodityPrice(const std::string& text, bool& perUnit);   // "¤1,879/Units" -> 1879, true; -1
bool commodityNamed(const std::string& read, const std::string& name);   // a card's name is this commodity's
// The level as the game words it, in capitals: 7 "MAX INVENTORY"; "" for
// anything off UEX's scale
const char* commodityLevelWords(int level);

}  // namespace SCMiner
