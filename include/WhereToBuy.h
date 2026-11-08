#pragma once

#include <map>
#include <string>
#include <vector>

namespace SCMiner {

// A point in a system: where a place stands (NavPlaces.h), in metres with
// the star at the origin
struct NavPoint {
    std::string system;   // "Stanton", "Pyro", "Nyx"
    double x = 0, y = 0, z = 0;
};

// A place's point by any name the game log, GamePlaces or UEX give it:
// "HUR-L1", "HUR-L1 Green Glade Station", "PYR2 L4", "Area 18",
// "Pyro Gateway (Nyx)", "Stanton-Pyro Jump Point". `system` tells two
// gateways of one name apart, and a name that says its system ("(Nyx)")
// says it itself. False for a name that is no one point - a belt, a ring,
// open space - or that is not known.
bool navPoint(const std::string& name, const std::string& system, NavPoint& out);

// How far it is from one to the other, in metres: straight across within a
// system; into another, to the gateway leading there and on from the one
// arrived at. -1 where no gateway joins the two systems.
double travelMetres(const NavPoint& from, const NavPoint& to);

// A shop that sells an item, as UEX's community lists it (items_prices).
// The names are UEX's, and UEX names a laser head, a module or a gadget as
// the game does - as Bedrock does (MiningHardware.h).
struct ShopOffer {
    std::string item;        // "Helix II Mining Laser"
    std::string where;       // the place, as the terminal's name has it: "ARC-L1", "Area 18", "Pyro Gateway"
    std::string shop;        // ...and the shop there: "Refinery Shop", "Dumper's Depot"
    std::string system;      // "Stanton"
    double price = 0;        // what it asks, aUEC
    long long reported = 0;  // when that was reported, Unix seconds
    bool located = false;    // whether `at` is known
    NavPoint at;
};

// The shops of the mining gear: UEX's items_prices, one answer per category
// (28 gadgets, 29 laser heads, 30 modules), read into one list
class UexShops {
public:
    // Reads one answer, {"status":"ok","data":[...]}, and adds its rows. A
    // row without a price is left out; of one item at one terminal twice,
    // the newer report is kept.
    bool read(const std::string& json, std::string& why);
    // ...an item's shops, by its name; empty where UEX lists none
    const std::vector<ShopOffer>& offersFor(const std::string& item) const;
    bool empty() const { return byItem_.empty(); }
    size_t itemCount() const { return byItem_.size(); }

    // "Refinery Shop - ARC-L1" -> "Refinery Shop" and "ARC-L1"; the place is
    // what follows the last " - " ("Tammany and Sons - Metro Center -
    // Lorville": Lorville), less its system in brackets ("Pyro Gateway (Nyx)")
    static void splitTerminal(const std::string& terminal, const std::string& system, std::string& shop,
                              std::string& where);

private:
    std::map<std::string, std::vector<ShopOffer>> byItem_;
};

// A shop in a list of an item's shops, seen from where the player is
struct ShopInOrder {
    ShopOffer offer;
    bool home = false;     // in the player's system
    double metres = -1;    // how far from the player; -1 where that is not known
};

// The order an item's shops are listed in: the player's system first,
// nearest first from where the player is (NavPlaces); then the other
// systems, by the way through the gateway. Where the player is not at a
// place with a point (open space, nothing in the log yet), the player's
// system (where known) first and then by price. Of two as far as each
// other to a tenth of a Gm - as a distance is shown -, the cheaper first.
std::vector<ShopInOrder> nearestFirst(const std::vector<ShopOffer>& offers, const std::string& system,
                                      const std::string& place);

}  // namespace SCMiner
