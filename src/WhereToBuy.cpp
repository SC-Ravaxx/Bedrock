#include "WhereToBuy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "MiniJson.h"
#include "NavPlaces.h"

namespace SCMiner {

namespace {

bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string trimmed(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

// "Pyro Gateway (Nyx)", "Pyro Gateway (Nyx system)": the place, and the
// system it stands in - which the name says better than anything around it
void bracketSystem(std::string& name, std::string& system) {
    if (name.empty() || name.back() != ')') return;
    const size_t open = name.rfind(" (");
    if (open == std::string::npos) return;
    std::string inner = name.substr(open + 2, name.size() - open - 3);
    static const std::string_view WORD = " system";
    if (inner.size() > WORD.size() && sameName(std::string_view(inner).substr(inner.size() - WORD.size()), WORD)) {
        inner.resize(inner.size() - WORD.size());
    }
    for (const char* known : {"Stanton", "Pyro", "Nyx"}) {
        if (!sameName(inner, known)) continue;
        system = known;
        name = trimmed(std::string_view(name).substr(0, open));
        return;
    }
}

// After `at`: a '-' or a space or nothing, an L and 1-5, then the end or a space
bool pointAfter(const std::string& name, size_t at, char& digit) {
    if (at < name.size() && (name[at] == '-' || name[at] == ' ')) at++;
    if (at + 2 > name.size() || std::toupper(static_cast<unsigned char>(name[at])) != 'L') return false;
    const char d = name[at + 1];
    if (d < '1' || d > '5') return false;
    if (at + 2 < name.size() && name[at + 2] != ' ') return false;
    digit = d;
    return true;
}

// "HUR-L1 Green Glade Station", "hur l1" -> "HUR-L1"; "PYR2 L4", "PYR2-L4"
// -> "PYR2 L4": a Lagrange point by the name NavPlaces has it under
bool lagrangeName(const std::string& name, std::string& out) {
    char digit = 0;
    if (name.size() >= 5) {
        for (const char* code : {"HUR", "CRU", "ARC", "MIC"}) {
            if (sameName(std::string_view(name).substr(0, 3), code) && pointAfter(name, 3, digit)) {
                out = std::string(code) + "-L" + digit;
                return true;
            }
        }
    }
    if (name.size() >= 6 && sameName(std::string_view(name).substr(0, 3), "PYR") && name[3] >= '1' &&
        name[3] <= '6' && pointAfter(name, 4, digit)) {
        out = "PYR" + std::string(1, name[3]) + " L" + digit;
        return true;
    }
    return false;
}

// "Stanton-Pyro Jump Point", "Nyx - Castra Jump Point": the near side's
// gateway, which stands at it - "Pyro Gateway" in Stanton
bool jumpPointName(std::string& name, std::string& system) {
    static const std::string_view TAIL = " Jump Point";
    if (name.size() <= TAIL.size() || !sameName(std::string_view(name).substr(name.size() - TAIL.size()), TAIL)) {
        return false;
    }
    const std::string pair = name.substr(0, name.size() - TAIL.size());
    const size_t dash = pair.find('-');
    if (dash == std::string::npos) return false;
    const std::string from = trimmed(std::string_view(pair).substr(0, dash));
    const std::string to = trimmed(std::string_view(pair).substr(dash + 1));
    if (from.empty() || to.empty()) return false;
    name = to + " Gateway";
    system = from;
    return true;
}

double across(const NavPoint& a, const NavPoint& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool navPoint(const std::string& nameIn, const std::string& systemIn, NavPoint& out) {
    std::string name = trimmed(nameIn), system = trimmed(systemIn);
    if (name.empty()) return false;
    bracketSystem(name, system);
    if (!jumpPointName(name, system)) {
        std::string point;
        if (lagrangeName(name, point)) name = point;
    }
    const NavPlace* found = nullptr;
    for (const NavPlace& place : NAV_PLACES) {
        if (!sameName(place.name, name) || (!system.empty() && !sameName(place.system, system))) continue;
        // two places of that name - a gateway in each of two systems - and
        // nothing to tell them apart
        if (found && (found->x != place.x || found->y != place.y || found->z != place.z ||
                      !sameName(found->system, place.system))) {
            return false;
        }
        found = &place;
    }
    if (!found) return false;
    out.system = found->system;
    out.x = found->x;
    out.y = found->y;
    out.z = found->z;
    return true;
}

double travelMetres(const NavPoint& from, const NavPoint& to) {
    if (sameName(from.system, to.system)) return across(from, to);
    // Gateways are named for where they lead: "Pyro Gateway" in Stanton
    // leads to Pyro, arriving at "Stanton Gateway" there
    NavPoint out, in;
    if (!navPoint(to.system + " Gateway", from.system, out) || !navPoint(from.system + " Gateway", to.system, in)) {
        return -1;
    }
    return across(from, out) + across(in, to);
}

void UexShops::splitTerminal(const std::string& terminal, const std::string& system, std::string& shop,
                             std::string& where) {
    const size_t dash = terminal.rfind(" - ");
    if (dash == std::string::npos) {
        shop = trimmed(terminal);
        where.clear();
        return;
    }
    shop = trimmed(std::string_view(terminal).substr(0, dash));
    where = trimmed(std::string_view(terminal).substr(dash + 3));
    std::string named = system;
    bracketSystem(where, named);
}

bool UexShops::read(const std::string& json, std::string& why) {
    JsonValue answer;
    if (!parseJson(json, answer, why)) {
        why = "Shop list unreadable: " + why;
        return false;
    }
    const std::string status = answer.textOr("status");
    if (status != "ok") {
        why = status.empty() ? "Response without status" : "UEX responded: " + status;
        return false;
    }
    const JsonValue* data = answer.find("data");
    if (!data || data->kind != JsonValue::Kind::Array) {
        why = "Response contained no data";
        return false;
    }
    for (const JsonValue& row : data->items) {
        ShopOffer offer;
        offer.item = trimmed(row.textOr("item_name"));
        offer.price = row.numberOr("price_buy");
        if (offer.item.empty() || offer.price <= 0) continue;
        offer.system = trimmed(row.textOr("star_system_name"));
        offer.reported = (long long)row.numberOr("date_modified");
        splitTerminal(row.textOr("terminal_name"), offer.system, offer.shop, offer.where);
        // where it is: the place's own name, then what it is at or on - an
        // outpost by its moon or planet, which is as near as a tenth of a
        // Gm can tell
        std::vector<std::string> names = {offer.where};
        for (const char* key : {"space_station_name", "city_name", "outpost_name", "orbit_name", "moon_name",
                                "planet_name"}) {
            names.push_back(trimmed(row.textOr(key)));
        }
        if (offer.where.empty()) {
            for (size_t i = 1; i <= 3 && offer.where.empty(); i++) offer.where = names[i];
        }
        for (const std::string& name : names) {
            if (!name.empty() && navPoint(name, offer.system, offer.at)) {
                offer.located = true;
                break;
            }
        }
        // one item at one terminal once: the newer report
        std::vector<ShopOffer>& shops = byItem_[offer.item];
        auto same = std::find_if(shops.begin(), shops.end(), [&](const ShopOffer& s) {
            return s.where == offer.where && s.shop == offer.shop && s.system == offer.system;
        });
        if (same == shops.end()) {
            shops.push_back(std::move(offer));
        } else if (offer.reported > same->reported) {
            *same = std::move(offer);
        }
    }
    return true;
}

const std::vector<ShopOffer>& UexShops::offersFor(const std::string& item) const {
    static const std::vector<ShopOffer> none;
    const auto found = byItem_.find(item);
    return found == byItem_.end() ? none : found->second;
}

std::vector<ShopInOrder> nearestFirst(const std::vector<ShopOffer>& offers, const std::string& system,
                                      const std::string& place) {
    NavPoint me;
    const bool placed = !trimmed(place).empty() && navPoint(place, system, me);
    const std::string mine = placed ? me.system : trimmed(system);
    std::vector<ShopInOrder> out;
    out.reserve(offers.size());
    for (const ShopOffer& offer : offers) {
        ShopInOrder shop;
        shop.offer = offer;
        shop.home = !mine.empty() && sameName(offer.system, mine);
        if (placed && offer.located) shop.metres = travelMetres(me, offer.at);
        out.push_back(std::move(shop));
    }
    std::stable_sort(out.begin(), out.end(), [](const ShopInOrder& a, const ShopInOrder& b) {
        if (a.home != b.home) return a.home;
        const bool aKnown = a.metres >= 0, bKnown = b.metres >= 0;
        if (aKnown != bKnown) return aKnown;
        if (aKnown) {
            // to the tenth of a Gm, as a distance is shown
            const long long aTenths = std::llround(a.metres / 1e8), bTenths = std::llround(b.metres / 1e8);
            if (aTenths != bTenths) return aTenths < bTenths;
        }
        if (a.offer.price != b.offer.price) return a.offer.price < b.offer.price;
        if (a.offer.system != b.offer.system) return a.offer.system < b.offer.system;
        return a.offer.where < b.offer.where;
    });
    return out;
}

std::vector<ShopInOrder> onePerPlace(const std::vector<ShopInOrder>& shops) {
    std::vector<ShopInOrder> out;
    for (const ShopInOrder& shop : shops) {
        const bool listed = std::any_of(out.begin(), out.end(), [&](const ShopInOrder& o) {
            return sameName(o.offer.where, shop.offer.where) && sameName(o.offer.system, shop.offer.system);
        });
        if (!listed) out.push_back(shop);
    }
    return out;
}

std::string shopPlace(const ShopOffer& offer) {
    static const std::string_view GATEWAY = " Gateway";
    const std::string& where = offer.where;
    const bool gateway = where.size() > GATEWAY.size() &&
                         sameName(std::string_view(where).substr(where.size() - GATEWAY.size()), GATEWAY);
    return gateway && !offer.system.empty() ? where + " (" + offer.system + ")" : where;
}

std::string gigametres(double metres) {
    if (metres < 0) return std::string();
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f Gm", metres / 1e9);
    return text;
}

std::string shopLine(const std::vector<ShopInOrder>& shops, const std::function<bool(const std::string&)>& fits,
                     size_t* named) {
    if (named) *named = 0;
    if (shops.empty()) return std::string();
    static const std::string SEP = "  \xC2\xB7  ";
    const auto words = [](const ShopInOrder& shop) {
        const std::string way = gigametres(shop.metres);
        return shopPlace(shop.offer) + (way.empty() ? "" : " " + way);
    };
    for (size_t take = shops.size(); take >= 1; take--) {
        std::string line;
        for (size_t i = 0; i < take; i++) line += (i ? SEP : std::string()) + words(shops[i]);
        if (take < shops.size()) line += SEP + std::to_string(shops.size() - take) + " more";
        if (take == 1 || !fits || fits(line)) {
            if (named) *named = take;
            return line;
        }
    }
    return std::string();
}

}  // namespace SCMiner
