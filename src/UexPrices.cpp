#include "UexPrices.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include "MiniJson.h"

namespace SCMiner {

std::string UexPrices::commodityOf(const std::string& ore) {
    if (ore == "Aluminium") return "Aluminum";
    return ore;
}

namespace {

struct Place {
    std::string system, place;
};

// "Stanton  ·  Crusader": the system and the body the terminal is on or by -
// the moon where there is one, else the planet. The terminal's own name says
// the station or the town. What a station orbits is no body where it has
// neither: a gateway's orbit is the gateway again ("Nyx Gateway (Pyro
// system)"), a People's Service Station's the station - those go by their
// system alone. Levski has no planet and no station, and its orbit is the
// body: Delamar.
Place placeOf(const JsonValue& terminal) {
    Place out;
    out.system = terminal.textOr("star_system_name");
    std::string body = terminal.textOr("moon_name");
    if (body.empty()) body = terminal.textOr("planet_name");
    if (body.empty() && terminal.textOr("space_station_name").empty()) body = terminal.textOr("orbit_name");
    out.place = out.system;
    if (!body.empty() && body != out.system) out.place += (out.place.empty() ? "" : "  \xC2\xB7  ") + body;
    return out;
}

std::string lowered(std::string text) {
    for (char& c : text) c = (char)std::tolower((unsigned char)c);
    return text;
}

// The rows of an answer: {"status":"ok","data":[...]}
const JsonValue* rowsOf(const JsonValue& answer, std::string& why) {
    const std::string status = answer.textOr("status");
    if (status != "ok") {
        why = status.empty() ? "an answer without a status" : "UEX said " + status;
        return nullptr;
    }
    const JsonValue* data = answer.find("data");
    if (!data || data->kind != JsonValue::Kind::Array) {
        why = "the answer had no data";
        return nullptr;
    }
    return data;
}

}  // namespace

bool UexPrices::read(const std::string& pricesJson, const std::string& terminalsJson, long long now,
                     std::string& why) {
    JsonValue prices;
    if (!parseJson(pricesJson, prices, why)) {
        why = "the price list couldn't be read: " + why;
        return false;
    }
    const JsonValue* priceRows = rowsOf(prices, why);
    if (!priceRows) return false;

    std::unordered_map<long long, Place> places;
    if (!terminalsJson.empty()) {
        JsonValue terminals;
        std::string skip;
        if (parseJson(terminalsJson, terminals, skip)) {
            if (const JsonValue* rows = rowsOf(terminals, skip)) {
                for (const JsonValue& t : rows->items) {
                    const long long id = (long long)t.numberOr("id", -1);
                    if (id >= 0) places[id] = placeOf(t);
                }
            }
        }
    }

    std::map<std::string, std::vector<UexOffer>> byCommodity;
    std::map<std::string, std::map<std::string, UexOffer>> bestBySystem;
    std::map<std::string, UexOffer> cheapestBuy;
    for (const JsonValue& row : priceRows->items) {
        const std::string commodity = row.textOr("commodity_name");
        if (commodity.empty()) continue;
        const long long reported = (long long)row.numberOr("date_modified");
        if (now > 0 && reported > 0 && now - reported > REPORT_DAYS * 86400) continue;
        UexOffer offer;
        offer.terminal = row.textOr("terminal_name");
        offer.reported = reported;
        const auto place = places.find((long long)row.numberOr("id_terminal", -1));
        if (place != places.end()) {
            offer.system = place->second.system;
            offer.place = place->second.place;
        }
        // What it pays. 7 is a terminal whose stock is full: it takes nothing now
        const double sell = row.numberOr("price_sell");
        const int sellStatus = (int)row.numberOr("status_sell");
        if (sell > 0 && sellStatus < 7) {
            UexOffer pays = offer;
            pays.price = sell;
            pays.status = sellStatus;
            UexOffer& best = bestBySystem[commodity][lowered(pays.system)];
            if (pays.price > best.price || (pays.price == best.price && pays.reported > best.reported)) best = pays;
            byCommodity[commodity].push_back(std::move(pays));
        }
        // ...and what it asks. 1 is a terminal that has none
        const double buy = row.numberOr("price_buy");
        const int buyStatus = (int)row.numberOr("status_buy");
        if (buy > 0 && buyStatus != 1) {
            UexOffer asks = offer;
            asks.price = buy;
            asks.status = buyStatus;
            const auto cheapest = cheapestBuy.find(commodity);
            if (cheapest == cheapestBuy.end() || asks.price < cheapest->second.price ||
                (asks.price == cheapest->second.price && asks.reported > cheapest->second.reported)) {
                cheapestBuy[commodity] = std::move(asks);
            }
        }
    }
    for (auto& entry : byCommodity) {
        auto& offers = entry.second;
        // best first; of two alike the more recent report
        std::sort(offers.begin(), offers.end(), [](const UexOffer& a, const UexOffer& b) {
            return a.price != b.price ? a.price > b.price : a.reported > b.reported;
        });
        if ((int)offers.size() > OFFERS_KEPT) offers.resize(OFFERS_KEPT);
    }
    if (byCommodity.empty()) {
        why = "the price list was empty";
        return false;
    }
    byCommodity_ = std::move(byCommodity);
    bestBySystem_ = std::move(bestBySystem);
    cheapestBuy_ = std::move(cheapestBuy);
    return true;
}

const std::vector<UexOffer>& UexPrices::offersFor(const std::string& ore) const {
    static const std::vector<UexOffer> none;
    const auto found = byCommodity_.find(commodityOf(ore));
    return found == byCommodity_.end() ? none : found->second;
}

double UexPrices::bestPrice(const std::string& ore) const {
    const std::vector<UexOffer>& offers = offersFor(ore);
    return offers.empty() ? 0.0 : offers.front().price;
}

const UexOffer* UexPrices::bestIn(const std::string& ore, const std::string& system) const {
    const auto bySystem = bestBySystem_.find(commodityOf(ore));
    if (bySystem == bestBySystem_.end() || system.empty()) return nullptr;
    const auto best = bySystem->second.find(lowered(system));
    return best == bySystem->second.end() ? nullptr : &best->second;
}

const UexOffer* UexPrices::cheapestBuy(const std::string& ore) const {
    const auto cheapest = cheapestBuy_.find(commodityOf(ore));
    return cheapest == cheapestBuy_.end() ? nullptr : &cheapest->second;
}

std::string UexPrices::mostValuable(const std::vector<std::string>& ores) const {
    std::string best;
    double top = 0;
    for (const std::string& ore : ores) {
        const double price = bestPrice(ore);
        if (price > top) {
            top = price;
            best = ore;
        }
    }
    return best;
}

}  // namespace SCMiner
