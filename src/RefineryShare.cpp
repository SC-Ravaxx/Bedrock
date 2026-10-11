#include "RefineryShare.h"

#include "MiniJson.h"
#include "RefineryTerminal.h"
#include "UexReports.h"

#include <cmath>
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
    if (capacity != lastCapacity || workload != lastWorkload) return true;
    return now - lastSent >= RESEND_SECONDS;
}

}  // namespace RefineryShare
}  // namespace SCMiner
