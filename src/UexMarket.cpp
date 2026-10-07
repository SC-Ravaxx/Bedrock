#include "UexMarket.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "MiniJson.h"
#include "UexPrices.h"

namespace SCMiner {

namespace {

std::string lowered(std::string text) {
    for (char& c : text) c = (char)std::tolower((unsigned char)c);
    return text;
}

// The marketplace writes its figures as numbers or as strings ("81000000"),
// as it pleases
double figureOf(const JsonValue& row, const char* key, double fallback = 0) {
    const JsonValue* value = row.find(key);
    if (!value) return fallback;
    if (value->kind == JsonValue::Kind::Number) return value->number;
    if (value->kind == JsonValue::Kind::String && !value->text.empty()) {
        char* end = nullptr;
        const double parsed = std::strtod(value->text.c_str(), &end);
        if (end && end != value->text.c_str()) return parsed;
    }
    return fallback;
}

}  // namespace

std::string UexThreads::threadUrl(long long listing) const {
    const auto hash = hashByListing.find(listing);
    if (hash == hashByListing.end()) return "";
    std::string path;
    const auto known = pathByHash.find(hash->second);
    if (known != pathByHash.end()) {
        path = known->second;
    } else if (!pathPattern.empty()) {
        path = pathPattern;
        const size_t at = path.find("{hash}");
        if (at != std::string::npos) path.replace(at, 6, hash->second);
    }
    if (path.empty()) return "";
    if (path.rfind("https://", 0) == 0) return path;
    return "https://uexcorp.space" + std::string(path.front() == '/' ? "" : "/") + path;
}

double UexListing::perScu() const {
    const std::string u = lowered(unit);
    if (u == "scu") return price;
    if (u == "cscu") return price * 100.0;
    return 0;
}

namespace UexMarket {

bool readItems(const std::string& averagesJson, std::map<std::string, int>& items, std::string& why) {
    JsonValue answer;
    if (!parseJson(averagesJson, answer, why)) {
        why = "the marketplace's items: " + why;
        return false;
    }
    const JsonValue* rows = answer.find("data");
    if (answer.textOr("status") != "ok" || !rows || rows->kind != JsonValue::Kind::Array) {
        why = "the item list was empty";
        return false;
    }
    std::map<std::string, int> read;
    for (const JsonValue& row : rows->items) {
        const int id = (int)figureOf(row, "id_item");
        const std::string name = lowered(row.textOr("item_name"));
        if (id > 0 && !name.empty()) read.emplace(name, id);
    }
    if (read.empty()) {
        why = "the marketplace has no items";
        return false;
    }
    items = std::move(read);
    return true;
}

int itemFor(const std::map<std::string, int>& items, const std::string& ore) {
    const auto found = items.find(lowered(UexPrices::commodityOf(ore)));
    return found == items.end() ? 0 : found->second;
}

bool readListings(const std::string& json, long long now, std::vector<UexListing>& out, std::string& why) {
    out.clear();
    JsonValue answer;
    if (!parseJson(json, answer, why)) {
        why = "the listings: " + why;
        return false;
    }
    const JsonValue* rows = answer.find("data");
    if (answer.textOr("status") != "ok") {
        why = "UEX said " + answer.textOr("status", "nothing");
        return false;
    }
    if (!rows || rows->kind != JsonValue::Kind::Array) return true;   // none open
    for (const JsonValue& row : rows->items) {
        if (figureOf(row, "is_sold_out") != 0) continue;
        UexListing listing;
        listing.expires = (long long)figureOf(row, "date_expiration");
        if (listing.expires > 0 && now > 0 && listing.expires < now) continue;
        listing.currency = row.textOr("currency");
        // aUEC alone: the marketplace also deals in other currencies
        if (lowered(listing.currency) != "uec") continue;
        listing.price = figureOf(row, "price");
        if (!(listing.price > 0)) continue;
        listing.id = (long long)figureOf(row, "id");
        listing.slug = row.textOr("slug");
        listing.title = row.textOr("title");
        listing.buying = lowered(row.textOr("operation")) == "buy";
        listing.unit = row.textOr("unit");
        const double quality = figureOf(row, "quality", -1);
        // 0 and 1 stand for "not said" in most listings
        listing.quality = quality > 1 && quality <= 1000 ? (int)quality : -1;
        listing.location = row.textOr("location");
        listing.user = row.textOr("user_username");
        listing.added = (long long)figureOf(row, "date_added");
        listing.renewed = (std::max)(listing.added, (long long)figureOf(row, "date_approved"));
        if (listing.slug.empty()) continue;
        out.push_back(std::move(listing));
    }
    std::sort(out.begin(), out.end(), [](const UexListing& a, const UexListing& b) {
        return a.price != b.price ? a.price > b.price : a.added > b.added;
    });
    return true;
}

bool readNegotiations(const std::string& json, UexThreads& threads, std::string& why) {
    JsonValue answer;
    if (!parseJson(json, answer, why)) {
        why = "the conversations: " + why;
        return false;
    }
    if (answer.textOr("status") != "ok") {
        why = "UEX said " + answer.textOr("status", "nothing");
        return false;
    }
    threads.hashByListing.clear();
    const JsonValue* rows = answer.find("data");
    if (!rows || rows->kind != JsonValue::Kind::Array) return true;   // none
    // the most lately touched conversation about a listing wins
    std::map<long long, long long> touched;
    for (const JsonValue& row : rows->items) {
        const long long listing = (long long)figureOf(row, "id_listing");
        const std::string hash = row.textOr("hash");
        const long long modified = (long long)figureOf(row, "date_modified");
        if (listing <= 0 || hash.empty()) continue;
        if (touched.count(listing) && touched[listing] >= modified) continue;
        touched[listing] = modified;
        threads.hashByListing[listing] = hash;
    }
    return true;
}

void readNotifications(const std::string& json, UexThreads& threads) {
    JsonValue answer;
    std::string why;
    if (!parseJson(json, answer, why) || answer.textOr("status") != "ok") return;
    const JsonValue* rows = answer.find("data");
    if (!rows || rows->kind != JsonValue::Kind::Array) return;
    for (const JsonValue& row : rows->items) {
        const std::string path = row.textOr("redir");
        if (path.empty()) continue;
        for (const auto& known : threads.hashByListing) {
            const size_t at = path.find(known.second);
            if (at == std::string::npos) continue;
            threads.pathByHash[known.second] = path;
            // the shape of a conversation's page, for the ones no
            // notification has named yet
            std::string pattern = path;
            pattern.replace(at, known.second.size(), "{hash}");
            threads.pathPattern = pattern;
        }
    }
}

std::vector<UexListing> buyersFor(const std::vector<UexListing>& listings, int quality) {
    std::vector<UexListing> out;
    for (const UexListing& listing : listings) {
        if (!listing.buying) continue;
        if (listing.quality > quality) continue;   // asks for better than the part is
        out.push_back(listing);
    }
    return out;
}

}  // namespace UexMarket

}  // namespace SCMiner
