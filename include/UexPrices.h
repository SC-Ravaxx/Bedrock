#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace SCMiner {

// A terminal that buys a refined ore, as UEX's community reports it - or,
// from cheapestBuy, one that sells it
struct UexOffer {
    std::string terminal;    // "TDD Orison"
    std::string place;       // "Stanton  ·  Crusader", the Ore Finder's separator
    std::string system;      // "Stanton"
    double price = 0;        // aUEC it pays for one SCU (asks, from cheapestBuy)
    long long reported = 0;  // when that was reported, Unix seconds
    // How much of it the terminal holds, in UEX's inventory states: 1 empty
    // ... 7 full; 0 where nobody said
    int status = 0;
    // High or very high: it takes some more, but maybe not a whole hold
    bool nearlyFull() const { return status == 5 || status == 6; }
};

// What the refined ores fetch at the terminals: UEX's price list
// (commodities_prices_all) read against its terminal list (terminals), for
// the places. uexcorp.space is community-run, its prices players' reports.
class UexPrices {
public:
    static constexpr int OFFERS_KEPT = 5;
    // A report older than this is no price any more
    static constexpr long long REPORT_DAYS = 30;

    // Reads the two answers; `now` is what the reports' age is counted from.
    // The terminal list may be empty - the offers then go without places.
    bool read(const std::string& pricesJson, const std::string& terminalsJson, long long now, std::string& why);

    // The terminals that pay most for one SCU of an ore once refined, best
    // first, OFFERS_KEPT at most - by Bedrock's name of the ore; empty where
    // UEX knows no buyer
    const std::vector<UexOffer>& offersFor(const std::string& ore) const;
    // ...the best of them, 0 where there is none
    double bestPrice(const std::string& ore) const;
    // ...the best in one system ("Pyro", in any case); nullptr where UEX
    // knows no buyer there
    const UexOffer* bestIn(const std::string& ore, const std::string& system) const;
    // The terminal that sells it cheapest - what buying it costs; nullptr
    // where none is reported to have any
    const UexOffer* cheapestBuy(const std::string& ore) const;
    // The ore among `ores` whose refined form fetches most; "" where none has a price
    std::string mostValuable(const std::vector<std::string>& ores) const;
    bool empty() const { return byCommodity_.empty(); }

    // UEX's name of an ore's refined form: Bedrock's, but Aluminium is
    // "Aluminum" there
    static std::string commodityOf(const std::string& ore);

    // What a terminal asked for one SCU of a commodity (`sold` false) or
    // paid for it (true) as UEX last had it, by UEX's ids; 0 where nobody
    // reported it in REPORT_DAYS - what a figure read off a terminal is held
    // against before it is reported
    double lastPrice(int terminal, int commodity, bool sold) const;

private:
    std::map<std::string, std::vector<UexOffer>> byCommodity_;              // by UEX's name
    std::map<std::string, std::map<std::string, UexOffer>> bestBySystem_;   // ...and system, lower case
    std::map<std::string, UexOffer> cheapestBuy_;
    std::unordered_map<long long, std::pair<double, double>> byTerminal_;   // terminal, commodity -> asks, pays
};

}  // namespace SCMiner
