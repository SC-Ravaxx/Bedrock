#pragma once
// The player's refinery work orders, as the REFINEMENT CENTER showed them.
//
// The game logs nothing of an order (RefineryTerminal.h), so the book is
// kept from what the terminal's panels say whenever the player stands at
// one: a PROCESSING panel names the order's number, what comes out of it
// and how long it still runs, which is all a tracker needs to tell where
// the ore is and when it is done. A SETUP panel read just before it adds
// what the order cost and the method it runs with. What is collected is
// the player's to say - the terminal shows nothing once it is.

#include "RefineryTerminal.h"

#include <filesystem>
#include <string>
#include <vector>

namespace SCMiner {

struct RefineryOrder {
    struct Part {
        std::string material;    // as REFINERY_MATERIALS spells it
        int quality = -1;
        int yield = -1;          // cSCU it comes out at
        int quantity = -1;       // cSCU put in - off the quote's QTY column; -1 unknown
    };
    std::string station;         // as the refinery tables name it: "Levski", "HUR-L1 Green Glade Station"
    int number = -1;             // WORK ORDER n on its terminal
    std::vector<Part> parts;
    std::string method;          // "" where no SETUP was read for it
    double cost = -1;            // aUEC; -1 unknown
    int seconds = -1;            // its processing time as quoted; -1 unknown
    long long placed = 0;        // UTC, seconds since 1970; 0 unknown
    long long due = 0;           // ...when it is done
    long long seen = 0;          // ...when a terminal last showed it
    long long completed = 0;     // ...when a terminal showed it COMPLETED; 0 not yet
    long long handed = 0;        // ...when it went to the UEX report (Application); 0 not yet
    int missing = 0;             // looks since "WORK ORDER DELIVERED" that did not show it (not saved)
    int yieldTotal() const;      // cSCU
    // All a report to UEX needs: the method, the bill, the time, when it was
    // placed, and every part's quantity and yield - off its quote
    bool complete() const;
};

// The refinery at the place the log puts the player - "Levski", or "Pyro
// Gateway" in Nyx as the tables' "Pyro Gateway (Nyx)" - or "" where there is
// none
std::string refineryStationAt(const std::string& place, const std::string& system);
// The refinery a terminal's header names - "LEVSKI", "HUR-L1 GREEN GLADE
// STATION" - as the tables spell it; a gateway's by the system the player is
// in. "" when the read is near none of them.
std::string refineryStationNamed(const std::string& text, const std::string& system);

class RefineryOrderBook {
public:
    // A reading of the terminal at `station`, `now` being when the picture
    // was taken. Its PROCESSING panels go into the book - new ones added,
    // known ones given the time they still run and any part not seen before
    // (a long list scrolls); its SETUP panel is kept for the order it turns
    // into. True when the book changed.
    bool apply(const std::string& station, const RefineryScreen& screen, long long now);
    // A reading as it comes off the screen: only the panels the reading
    // before it said the same about are applied - a panel caught half drawn,
    // or with the game's "WORK ORDER ACCEPTED" box over it, says something
    // else a second later. A quote is kept off a single reading: it stands
    // on the screen for a second or two before CONFIRM takes it away, and it
    // only ever goes to an order whose parts and time it matches. True when
    // the book changed.
    bool take(const std::string& station, const RefineryScreen& read, long long taken);
    // Collected, or not wanted in the list any more
    bool remove(size_t index);
    // Gone to the UEX report: not again
    bool markHanded(size_t index, long long when);
    // Whether an order is done by now: only then is "WORK ORDER DELIVERED"
    // worth looking for
    bool anyDone(long long now) const;
    const std::vector<RefineryOrder>& orders() const { return orders_; }
    // Running first, the soonest done at the top
    std::vector<RefineryOrder> sorted() const;

    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;

    // Whether two readings of a panel are of the same order: number and
    // parts alike, and done at the same moment give or take `slack` seconds
    static bool samePanel(const RefineryPanel& a, long long aTaken, const RefineryPanel& b, long long bTaken, int slack = 5);
    // A PROCESSING panel the book can take: a number, a time, and every
    // part named with its quality and yield; a COMPLETED one: its parts
    static bool usable(const RefineryPanel& panel);

private:
    std::vector<RefineryOrder> orders_;
    // The reading before, which the next has to agree with
    RefineryScreen last_;
    std::string lastStation_;
    long long lastTaken_ = 0;
    void noteQuote(const std::string& station, const RefineryPanel& panel, long long now);
    // "WORK ORDER DELIVERED": where and since when it stands, whether the
    // last look saw it and when - and the done order no look since shows,
    // taken off: one for each time it comes up
    std::string deliveredStation_;
    long long deliveredAt_ = 0;
    long long noticeSeen_ = 0;
    bool noticeUp_ = false;
    int toCollect_ = 0;
    bool noteCollected(const std::string& station, const RefineryScreen& read, long long now);
    // The last SETUP that had a quote on it, for the order it becomes
    struct Quote {
        std::string station, method;
        double cost = -1;
        int seconds = -1;
        std::vector<RefineryOrder::Part> parts;   // the rows with a yield: the ones refined
        long long taken = 0;                      // last seen
        bool used = false;                        // gone to an order already
    } quote_;
};

}  // namespace SCMiner
