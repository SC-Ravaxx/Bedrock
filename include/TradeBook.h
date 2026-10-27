#pragma once
// What the player bought and sold at the commodity terminals, read off the
// game's own log.
//
// A deal at a terminal goes into Game.log as the request the game sends:
//
//   <2026-09-28T11:37:34.512Z> [Notice] <CEntityComponentCommodityUIProvider::
//   SendCommoditySellRequest> Sending SShopCommoditySellRequest - playerId[..]
//   shopId[..] shopName[SCShop_Trdpst_Warehouse_OTLW_Int_A] kioskId[..]
//   amount[1021440.000000] resourceGUID[ca524406-0e3e-4d5e-baad-8a6c7b42054b]
//   autoLoading[0] quantity[64] transactionMode[ResourceContainer] Cargo Box
//   Data:  [boxSize[2] | unitAmount[32]]
//
//   <...::SendCommodityBuyRequest> Sending SShopCommodityBuyRequest - ...
//   shopName[SCShop_ht_delta_shubin_m_store] kioskId[..] price[1377500.000000]
//   shopPricePerCentiSCU[275.500000] resourceGUID[21825507-7923-4683-9bf3-
//   9cfe316940e3] autoLoading[0] quantity[5000.000000 cSCU] Cargo Box Data: ...
//
// - what, how much, for how much in all, and at which shop. A sale's quantity
// is in SCU; a purchase's is in hundredths of one, and says so. The game
// writes no line for a deal that went through. One the shop refused is
// answered within half a second:
//
//   [Error] <...::RmToken_CommodityTransactionResponse> Commodity Transaction
//   Response Error - playerId[..] result[InvalidQuantityError] type[Selling]
//
// so a request counts until that line follows it (see refused).
//
// The shop's name is the game's own, and one name serves every station of a
// kind - SCShop_Admin_lt_base_g is the admin office of each of them. Where
// the deal was made is the place the same log puts the player at (GameState),
// read along with the deals.

#include "GameState.h"

#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace SCMiner {

struct Trade {
    std::string stamp;      // "2026-09-28T11:37:34.512": the request's own time, UTC
    bool sold = false;      // to the shop; bought from it otherwise
    std::string resource;   // what, by the GUID the log names it with (COMMODITY_NAMES)
    double scu = 0;         // how much of it
    double total = 0;       // aUEC, for all of it
    std::string shop;       // the terminal's shop, as the game names it
    std::string place;      // "HUR-L1 Green Glade Station"; the system alone, or "", where the log had not said more
    bool refused = false;   // the shop answered with an error: nothing changed hands
    double perScu() const { return scu > 0 ? total / scu : 0.0; }
};

class TradeBook {
public:
    static constexpr const char* FILE_NAME = "trades.txt";

    // A request's line as a trade; false for every other line
    static bool parseRequest(std::string_view line, Trade& out);
    // A refusal's line, and whether it is a sale's
    static bool parseRefusal(std::string_view line, bool& sold);
    // The commodity's English name, "" for a GUID this build does not know
    static const char* nameOf(const std::string& resource);

    // Game.log as it grows: every line, in order - the lines that say where
    // the player is are read here too. True when the book changed.
    bool live(std::string_view line);
    void liveLogRestarted();   // the game started again: where the player is has to be said anew

    // A commodity terminal filling its list, as the live log has it: the
    // game lists what the terminal's shop sells, each with the cargo box
    // sizes it comes in, whenever the list fills - when the player opens it,
    // and again after each trade:
    //   <...::LoadShopInventoryData::<lambda_1>::operator ()> AddingCommodityBox
    //   - playerId[..] shopId[..] shopName[SCShop_Admin_lt_base_g]
    //   commodityName[ResourceType.Titanium] Available Box Sizes:  boxSize[8]
    //   boxSize[16] boxSize[24] boxSize[32] [Team_CoreGameplayFeatures]...
    // What the shop buys is not listed (a warehouse that bought Elespo on
    // 26 October 2026 listed Neon, Amioshi Plague and E'tam).
    struct TerminalFill {
        long long ms = 0;          // its first line, in milliseconds since 1970, UTC
        std::string shopId, shop;  // as the log names them
        std::string place;         // where the log had the player then
        // by the resource's name in the game's records ("Titanium",
        // "ShipAmmoSize4", "RMC"): the sizes listed, in SCU ("8,16,24,32")
        std::map<std::string, std::string> boxes;
    };
    // The last ones seen, the newest last
    const std::deque<TerminalFill>& terminalFills() const { return fills_; }
    // A commodity terminal in use, as the live log has it: the time of the
    // last line of one - its list filling, the player's boxes listed for a
    // sale, a trade - in milliseconds since 1970, UTC, 0 none yet; and its
    // shop's name. The Datarunner looks at the game while one is in use.
    long long terminalUsedMs() const { return terminalUsedMs_; }
    const std::string& terminalUsedShop() const { return terminalUsedShop_; }
    // The one filled last at or before `ms`, no longer than `within` ms
    // before it - the terminal a picture taken then shows; nullptr for none
    const TerminalFill* terminalAt(long long ms, long long within) const;
    // One such line: the resource's name and its sizes; false for any other
    static bool parseBoxes(std::string_view line, std::string& resource, std::string& sizes);
    // Whether a resource's name in the game's records is the commodity
    // written so - "ShipAmmoSize4" Ship Ammunition - Size 4, "RMC" Recycled
    // Material Composite, "PlasmaFuel" Hydrogen Fuel, "Etam" E'tam
    static bool resourceIs(const std::string& resource, const std::string& name);
    // "2026-09-28T11:37:34.512", a line's time, in milliseconds since 1970,
    // UTC; -1 for anything else
    static long long millisOf(const std::string& stamp);
    // Where the log has the player, as UEX names it: a gateway with its
    // system ("Pyro Gateway (Stanton)"), the system alone where it said no more
    static std::string placeOf(const GameState& state);
    // A log that is over, all of it at once. Asked only of a log with a
    // request in it (hasRequest), since finding the place means reading
    // every line of it a second time.
    static bool hasRequest(std::string_view log);
    bool replay(std::string_view log);

    // One line with the place the log has the player at; what live() and
    // replay() come down to. Public for the self-test.
    bool line(std::string_view text, const std::string& place);

    // Every request seen, the oldest first
    std::vector<Trade> trades() const;
    int count() const { return (int)byKey_.size(); }
    // Counts every change, so a reader knows when to look again
    int changes() const { return changes_; }

    void load(const std::filesystem::path& file);
    void save();   // only when something changed since the last time

private:
    std::map<std::string, Trade> byKey_;   // stamp, kind and resource: one request, however often its line is read
    std::string lastKey_[2];               // the newest request bought [0] and sold [1], for a refusal to find
    GameState where_;                      // the live log's
    std::deque<TerminalFill> fills_;
    static constexpr size_t FILLS_KEPT = 40;
    long long terminalUsedMs_ = 0;
    std::string terminalUsedShop_;
    std::filesystem::path file_;
    bool dirty_ = false;
    int changes_ = 0;
};

}  // namespace SCMiner
