#pragma once
// The UEX marketplace (uexcorp.space/marketplace): what players offer and
// ask for one another, outside the terminals - and for ore, at what quality.
// A terminal pays the same for any quality; a crafter pays for quality,
// and players do: 25,000,000 aUEC for Ouratite of quality 960 where a
// terminal pays 46,000 a SCU. Read only, and anonymous: the
// listings are public. Writing to a player starts on UEX's page of the
// listing - the API has no way to begin a conversation, only to go on with
// one (marketplace_negotiations_messages).

#include <map>
#include <string>
#include <vector>

namespace SCMiner {

struct UexListing {
    long long id = 0;
    std::string slug;          // the listing's page: url()
    std::string title;
    bool buying = false;       // operation "buy": the player wants it - and would buy from you
    double price = 0;          // in `currency`, per `unit`
    std::string unit;          // "scu", "cscu", "unit", "crate"... as the player wrote it
    std::string currency;      // "UEC"
    int quality = -1;          // 0-1000; -1 where none is said
    std::string location;      // where, in the player's words; "" where none
    std::string user;          // the player's handle on UEX
    long long added = 0;       // Unix seconds
    long long expires = 0;
    // When UEX last approved it: its player's last edit or renewal. UEX
    // keeps no "last active" for anyone, and lets a listing lapse 60 days
    // after this - so every open listing was touched within 60 days, and
    // this says how lately. Nothing is hidden by it: an old listing may be
    // the busiest one.
    long long renewed = 0;
    std::string url() const { return "https://uexcorp.space/marketplace/item/info/" + slug + "/"; }
    // What one SCU comes to, where the unit says SCU or cSCU; 0 where it
    // says unit, crate or anything else nobody can count in SCU
    double perScu() const;
};

// The player's own conversations on the marketplace - read with their UEX
// key (marketplace_negotiations), and where on uexcorp.space each one is:
// the API gives a conversation's hash, not its page, and UEX's notifications
// (user_notifications) carry the page they are about. Learnt from those:
// the page of a conversation a notification named, and the shape of such a
// page, for the ones none named yet.
struct UexThreads {
    std::map<long long, std::string> hashByListing;    // listing id -> the player's conversation about it
    std::map<std::string, std::string> pathByHash;     // a conversation's page, as a notification gave it
    std::string pathPattern;                            // ...with its hash as {hash}
    // The page of the player's conversation about a listing; "" where they
    // have none, or its page is not known yet
    std::string threadUrl(long long listing) const;
    bool hasThread(long long listing) const { return hashByListing.count(listing) > 0; }
};

namespace UexMarket {

// The player's conversations, into `threads`
bool readNegotiations(const std::string& json, UexThreads& threads, std::string& why);
// UEX's notifications: the pages they point at, for the conversations known
void readNotifications(const std::string& json, UexThreads& threads);

// The marketplace's items by name, out of marketplace_prices_averages_all
// (every item with listings lately): "ouratite" -> 5943
bool readItems(const std::string& averagesJson, std::map<std::string, int>& items, std::string& why);
// The item an ore or gem is listed as: its refined name, Aluminium as
// UEX's Aluminum; 0 where the marketplace has none
int itemFor(const std::map<std::string, int>& items, const std::string& ore);
// The open listings of one answer of marketplace_listings: in aUEC, not
// sold out, not expired, the dearest first (what a buyer pays most, what a
// seller asks most - the window turns the sellers round)
bool readListings(const std::string& json, long long now, std::vector<UexListing>& out, std::string& why);
// The listings that would buy the quality of a rock's part: buying, at
// that quality or less asked for - none asks less than it gets
std::vector<UexListing> buyersFor(const std::vector<UexListing>& listings, int quality);

}  // namespace UexMarket

}  // namespace SCMiner
