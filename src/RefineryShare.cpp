#include "RefineryShare.h"

#include "MiniJson.h"
#include "RefineryTerminal.h"
#include "UexReports.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <sstream>

namespace SCMiner {
namespace RefineryShare {

std::string body(const Reading& r, const std::string& bedrockVersion, int gameVersion, const std::string& channel) {
    using UexReports::jsonText;
    std::ostringstream out;
    out << "{\"v\":1,\"bedrock\":" << jsonText(bedrockVersion);
    if (gameVersion > 0) {
        char game[32];
        std::snprintf(game, sizeof(game), "%d.%d.%d", gameVersion / 10000, gameVersion / 100 % 100, gameVersion % 100);
        out << ",\"game\":" << jsonText(game);
    }
    if (!channel.empty()) out << ",\"channel\":" << jsonText(channel);
    out << ",\"observed\":" << r.observed << ",\"station\":" << jsonText(r.station);
    if (r.capacity >= 0) out << ",\"capacity\":" << r.capacity;
    if (r.workload >= 0 && r.workload <= 3) out << ",\"workload\":" << jsonText(refineryWorkloadName(r.workload));
    if (!r.method.empty() && r.cost >= 0 && r.seconds > 0 && !r.items.empty()) {
        out << ",\"quote\":{\"method\":" << jsonText(r.method) << ",\"cost\":" << std::llround(r.cost)
            << ",\"seconds\":" << r.seconds << ",\"items\":[";
        for (size_t i = 0; i < r.items.size(); i++) {
            const Item& it = r.items[i];
            out << (i ? "," : "") << "{\"material\":" << jsonText(it.material) << ",\"quantity\":" << it.quantity;
            if (it.quality >= 0) out << ",\"quality\":" << it.quality;
            if (it.yield >= 0) out << ",\"yield\":" << it.yield;
            out << "}";
        }
        out << "]}";
    }
    out << "}";
    return out.str();
}

bool readStations(const std::string& json, std::map<std::string, StationState>& out, std::string& why) {
    JsonValue v;
    if (!parseJson(json, v, why)) return false;
    const JsonValue* list = v.find("stations");
    if (!list || list->kind != JsonValue::Kind::Array) {
        why = "no stations in the answer";
        return false;
    }
    out.clear();
    for (const JsonValue& s : list->items) {
        const std::string name = s.textOr("station");
        if (name.empty()) continue;
        StationState st;
        st.capacity = static_cast<int>(s.numberOr("capacity", -1));
        const std::string word = s.textOr("workload");
        for (int i = 0; i < 4; i++) {
            if (word == refineryWorkloadName(i)) st.workload = i;
        }
        st.observed = static_cast<long long>(s.numberOr("observed", 0));
        st.sources = static_cast<int>(s.numberOr("sources", 0));
        const JsonValue* confirmed = s.find("confirmed");
        st.confirmed = confirmed && confirmed->kind == JsonValue::Kind::Bool && confirmed->boolean;
        out[name] = st;
    }
    return true;
}

bool worthSending(int capacity, int workload, long long now, int lastCapacity, int lastWorkload, long long lastSent) {
    if (capacity < 0 && workload < 0) return false;
    if (lastSent <= 0) return true;
    const bool levelChanged = workload >= 0 && lastWorkload >= 0 && workload != lastWorkload;
    const bool moved = capacity >= 0 && lastCapacity >= 0 && std::abs(capacity - lastCapacity) >= CHANGE_POINTS;
    if (levelChanged || moved) return true;
    if ((workload < 0 && lastWorkload >= 0) || (capacity < 0 && lastCapacity >= 0)) return false;
    if ((workload >= 0 && lastWorkload < 0) || (capacity >= 0 && lastCapacity < 0)) return true;
    return now - lastSent >= RESEND_SECONDS;
}

int surchargeLevel(int capacity, int workload) {
    if (workload >= 0 && workload <= 3) return workload;
    if (capacity < 0) return -1;
    return capacity < 100 ? 0 : capacity < 200 ? 1 : capacity < 300 ? 2 : 3;
}

bool cheaperFirst(const PriceRank& a, const PriceRank& b, bool cargo) {
    // never read goes after extreme
    const int stepA = a.level < 0 ? 4 : a.level, stepB = b.level < 0 ? 4 : b.level;
    if (stepA != stepB) return stepA < stepB;
    if (!cargo) return a.observed > b.observed;
    // compared in whole cents, so that two stations a rounding apart stay
    // in the order they came in
    auto cents = [](double v) { return std::llround((std::min)((std::max)(v, 0.0), 1e12) * 100.0); };
    return cents(a.perScu) < cents(b.perScu);
}

}  // namespace RefineryShare
}  // namespace SCMiner
