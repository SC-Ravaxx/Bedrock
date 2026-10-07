#include "Reputation.h"

#include "CraftingData.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace SCMiner {
namespace Reputation {

static const RepContract* exactly(std::string_view key) {
    // REP_CONTRACTS is sorted byte by byte (gen_crafting.py)
    int lo = 0, hi = REP_CONTRACT_COUNT;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        const int c = std::string_view(REP_CONTRACTS[mid].key).compare(key);
        if (c == 0) return &REP_CONTRACTS[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return nullptr;
}

const RepContract* contract(std::string_view key) {
    if (const RepContract* c = exactly(key)) return c;
    // The game numbers what it makes of a contract - the log's
    // "Adaigo_Stanton1_Lawful_Salvage_Hard_0" and "..._Hard_2" are the files'
    // "Adaigo_Stanton1_Lawful_Salvage_Hard" - on 177 of 1,857 contracts taken
    // or finished on LIVE since 4.8, and on every one of 4.10 the files have
    // by name. One name of the files ends in a number of its own
    // (Luminalia_1), and is found as it is above.
    size_t cut = key.size();
    while (cut > 0 && key[cut - 1] >= '0' && key[cut - 1] <= '9') cut--;
    if (cut == key.size() || cut < 2 || key[cut - 1] != '_') return nullptr;
    return exactly(key.substr(0, cut - 1));
}

int rankOf(int ladder, long long reputation) {
    if (ladder < 0 || ladder >= REP_LADDER_COUNT) return 0;
    const RepLadder& l = REP_LADDERS[ladder];
    int rank = 0;
    for (int i = 0; i < l.count; i++) {
        if (REP_RANKS[l.first + i].min <= reputation) rank = i;
    }
    return rank;
}

long long rankMin(int ladder, int rank) {
    if (ladder < 0 || ladder >= REP_LADDER_COUNT) return 0;
    const RepLadder& l = REP_LADDERS[ladder];
    if (rank < 0) return 0;
    if (rank >= l.count) rank = l.count - 1;
    return REP_RANKS[l.first + rank].min;
}

int factionByGuid(std::string_view guid) {
    for (int i = 0; i < REP_FACTION_COUNT; i++) {
        if (guid == REP_FACTIONS[i].guid) return i;
    }
    return -1;
}

int ladderByGuid(std::string_view guid) {
    for (int i = 0; i < REP_LADDER_COUNT; i++) {
        if (guid == REP_LADDERS[i].guid) return i;
    }
    return -1;
}

long long epochOf(const std::string& stamp) {
    if (stamp.size() < 19 || stamp[4] != '-' || stamp[7] != '-' || stamp[10] != 'T' || stamp[13] != ':' ||
        stamp[16] != ':') {
        return -1;
    }
    auto num = [&](size_t at, size_t n) {
        int v = 0;
        for (size_t i = at; i < at + n; i++) {
            if (stamp[i] < '0' || stamp[i] > '9') return -1;
            v = v * 10 + (stamp[i] - '0');
        }
        return v;
    };
    int y = num(0, 4);
    const int mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2), s = num(17, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || h < 0 || mi < 0 || s < 0) return -1;
    // days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil)
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + (long long)doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

std::string versionName(int version) {
    return std::to_string(version / 10000) + "." + std::to_string(version / 100 % 100) + "." +
           std::to_string(version % 100);
}

// Per outfit and ladder, a lower bound on what the player holds. Each piece
// of evidence gives one:
//   - a contract they took that asked for rank R: they held at least R's
//     minimum then, plus whatever they finished after it;
//   - the rank they set, likewise from when they set it;
//   - the wipe: nothing, plus everything they finished since.
// The highest of them is the figure. Not the sum of everything ever earned:
// the game's own figure is not in the log, contracts of 4.8 and 4.9 are not
// all in today's tables, and a rank proven by a contract taken is worth more
// than a count that is missing pieces.
std::vector<Standing> standings(const std::vector<RankEvent>& events, const std::vector<RankSetting>& settings,
                                int fromVersion) {
    struct Proof {
        std::string stamp;
        int rank;
        long long min;
    };
    struct Gain {
        std::string stamp;
        long long amount;
    };
    struct Track {
        bool set = false;
        long long base = 0;
        std::string baseStamp;     // "" counts from the beginning
        std::vector<Proof> proofs;
        std::vector<Gain> gains;
    };
    std::map<std::pair<int, int>, Track> tracks;

    for (const RankSetting& s : settings) {
        const int f = factionByGuid(s.faction), l = ladderByGuid(s.ladder);
        if (f < 0 || l < 0) continue;
        Track& t = tracks[{f, l}];
        // the newest setting is the one that counts
        if (t.set && t.baseStamp > s.stamp) continue;
        t.set = true;
        t.base = s.reputation;
        t.baseStamp = s.stamp;
    }

    std::vector<const RankEvent*> order;
    order.reserve(events.size());
    for (const RankEvent& e : events) {
        if (e.version >= fromVersion) order.push_back(&e);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const RankEvent* a, const RankEvent* b) { return a->stamp < b->stamp; });
    for (const RankEvent* e : order) {
        const RepContract* c = contract(e->contract);
        if (!c) continue;
        if (!e->completed) {
            if (c->rank >= 0 && c->faction >= 0 && c->ladder >= 0) {
                tracks[{c->faction, c->ladder}].proofs.push_back(
                    Proof{e->stamp, c->rank, rankMin(c->ladder, c->rank)});
            }
            continue;
        }
        for (int k = 0; k < c->awardCount; k++) {
            const RepAward& a = REP_AWARDS[c->awardFirst + k];
            tracks[{a.faction, a.ladder}].gains.push_back(Gain{e->stamp, a.amount});
        }
    }

    std::vector<Standing> out;
    for (const auto& [key, t] : tracks) {
        Standing s;
        s.faction = key.first;
        s.ladder = key.second;
        s.set = t.set;
        s.setStamp = t.set ? t.baseStamp : std::string();
        // what was finished after a moment, and how many
        auto after = [&](const std::string& from, int* count) {
            long long sum = 0;
            int n = 0;
            for (const Gain& g : t.gains) {
                if (g.stamp > from) {
                    sum += g.amount;
                    n++;
                }
            }
            if (count) *count = n;
            return sum;
        };
        s.reputation = t.base + after(t.baseStamp, &s.completions);
        s.gained = s.reputation - t.base;
        for (const Proof& p : t.proofs) {
            if (p.stamp <= t.baseStamp) continue;     // before the player set their rank
            s.proven = (std::max)(s.proven, p.rank);
            int n = 0;
            const long long sum = after(p.stamp, &n);
            if (p.min + sum > s.reputation) {
                s.reputation = p.min + sum;
                s.anchorRank = p.rank;
                s.anchorStamp = p.stamp;
                s.gained = sum;
                s.completions = n;
            }
        }
        s.rank = rankOf(s.ladder, s.reputation);
        out.push_back(s);
    }
    return out;
}

}  // namespace Reputation
}  // namespace SCMiner
