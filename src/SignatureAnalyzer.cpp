#include "SignatureAnalyzer.h"
#include "MiningLocations.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <map>
#include <set>

namespace SCMiner {

// How many ways a value may split before the splits explain nothing. Four,
// like the list the player gets to pick from: the rock in view shows what it
// holds, and four options are a choice; more is a lottery.
constexpr size_t MAX_KNOWN_SPLITS = 4;

OCREngine::ReadingPolicy makeReadingPolicy(SignatureAnalyzer& analyzer) {
    OCREngine::ReadingPolicy policy;
    policy.canonicalize = [&analyzer](int v) { return analyzer.canonicalize(v); };
    policy.bonus = [&analyzer](int v) -> double {
        // No signature exists outside this range, so such a reading is noise -
        // penalise it hard instead of letting it outvote a plausible one
        if (v < 1500 || v > MAX_VALID_SIGNATURE) {
            return -60.0;
        }
        std::string name;
        int cluster = 0;
        return analyzer.isExactMatch(v, name, cluster) ? 40.0 : 0.0;
    };
    return policy;
}

SignatureAnalyzer::SignatureAnalyzer() = default;

void SignatureAnalyzer::setSystemFilter(const std::string& system) {
    systemFilter_ = system;
}

std::string SignatureAnalyzer::getSystemFilter() const {
    return systemFilter_;
}

void SignatureAnalyzer::setPlaceFilter(const std::string& oreLocation) {
    placeName_.clear();
    placeMinerals_.clear();
    if (oreLocation.empty()) return;
    for (const auto& row : ORE_LOCATIONS) {
        if (row.family != MiningFamily::Ship || oreLocation != row.location) continue;
        if (std::find(placeMinerals_.begin(), placeMinerals_.end(), row.mineral) == placeMinerals_.end()) {
            placeMinerals_.push_back(row.mineral);
        }
    }
    // A place the tables list no ship ore for is no filter at all
    if (!placeMinerals_.empty()) placeName_ = oreLocation;
}

bool SignatureAnalyzer::atPlace(const std::string& mineral) const {
    if (placeMinerals_.empty()) return true;
    return std::find(placeMinerals_.begin(), placeMinerals_.end(), mineral) != placeMinerals_.end();
}


uint8_t SignatureAnalyzer::systemMask(const std::string& system) {
    if (system == "STANTON") return SYS_STANTON;
    if (system == "PYRO")    return SYS_PYRO;
    if (system == "NYX")     return SYS_NYX;
    return SYS_ALL;
}

double SignatureAnalyzer::scoreCandidate(double difference, int expectedSignature) const {
    if (difference < 1.0) {
        return 100.0;  // exact match
    }

    // Neighboring signatures are only ~15 units apart at cluster size 1,
    // so the window must stay tight. It grows slightly with the expected
    // value because cluster multiplication also multiplies the spacing.
    double window = (std::max)(tolerance_, expectedSignature * 0.006);
    double factor = 1.0 - difference / (2.0 * window);

    if (factor <= 0.0) return 0.0;
    return factor * factor * 100.0;
}

std::vector<AnalysisResult> SignatureAnalyzer::analyzeSignature(int scannedSignature) {
    std::vector<AnalysisResult> results;
    const uint8_t filterMask = systemMask(systemFilter_);

    // Ship mining: every mineral, every cluster size its rarity allows
    for (const auto& mineral : MINERAL_SIGNATURES) {
        // Only the cluster size closest to the scanned value matters
        int bestN = 1;
        double bestDiff = std::abs(scannedSignature - mineral.baseSignature);
        for (int n = 2; n <= maxClusterSize(mineral.rarity); n++) {
            double diff = std::abs(scannedSignature - mineral.baseSignature * n);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestN = n;
            }
        }

        const int expected = mineral.baseSignature * bestN;
        const double probability = scoreCandidate(bestDiff, expected);

        if (probability > 0.1) {
            AnalysisResult result;
            result.name = mineral.name;
            result.category = SignalCategory::ShipMining;
            result.rarity = mineral.rarity;
            result.baseSignature = mineral.baseSignature;
            result.clusterSize = bestN;
            result.expectedSignature = expected;
            result.probability = probability;
            result.difference = bestDiff;
            result.inSelectedSystem = (mineral.systems & filterMask) != 0 && atPlace(mineral.name);
            results.push_back(result);
        }
    }

    // Special families: ground deposits (vehicle / hand) and salvage panels.
    // These only occur at specific node counts - a lone deposit or a field.
    for (const auto& special : SPECIAL_SIGNATURES) {
        const auto sizes = clusterSizesFor(special);
        int bestN = sizes.front();
        double bestDiff = std::abs(scannedSignature - special.baseSignature * bestN);
        for (int n : sizes) {
            double diff = std::abs(scannedSignature - special.baseSignature * n);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestN = n;
            }
        }

        const int expected = special.baseSignature * bestN;
        const double probability = scoreCandidate(bestDiff, expected);

        if (probability > 0.1) {
            AnalysisResult result;
            result.name = special.name;
            result.category = special.category;
            result.rarity = Rarity::None;
            result.baseSignature = special.baseSignature;
            result.clusterSize = bestN;
            result.expectedSignature = expected;
            result.probability = probability;
            result.difference = bestDiff;
            result.inSelectedSystem = familyPossible(special.name);   // not system-bound, but ground-bound
            result.unusualCluster = isUnusualFieldSize(special, bestN);
            results.push_back(result);
        }
    }

    // In-game values are always exactly base x cluster size (each node/rock
    // carries a fixed signature; a cluster is a plain multiple). An exact hit
    // therefore rules out fuzzy neighbors - those only serve as OCR-misread
    // suggestions when no exact match is available in the selected system.
    bool anyExact = false;
    bool anyExactInSystem = false;
    for (const auto& r : results) {
        if (r.difference < 1.0) {
            anyExact = true;
            if (r.inSelectedSystem) anyExactInSystem = true;
        }
    }

    // No single signature spells the value: the sums do, when they are few.
    // Each split is offered as its own exact row - "Silicon ×4 + Riccite" -
    // and the player, who sees what the rock holds, picks. Fuzzy single
    // matches would only mislead beside an exact sum, so they go.
    if (!anyExact) {
        const auto constellations = decomposeAll(scannedSignature, 6);
        if (!constellations.empty()) {
            results.clear();
            double total = 0.0;
            for (const auto& c : constellations) total += c.weight;
            for (const auto& c : constellations) {
                // The multiplication sign as UTF-8, like every other name: toW() reads
                // UTF-8 first and widens byte-wise only as a fallback, so a Latin-1
                // 0xD7 byte would show as U+FFD7 in every match row.
                auto part = [this](const Term& t) {
                    const std::string shown = shownName(t.name);
                    return t.count > 1 ? shown + " \xC3\x97" + std::to_string(t.count) : shown;
                };
                std::string name;
                // The rarest ore names the find (Common < ... < Legendary in
                // the enum; None sits above them all and means "no ore")
                Rarity rarest = Rarity::Common;
                bool anyOre = false;
                for (size_t i = 0; i < c.terms.size(); i++) {
                    if (i) name += " + ";
                    name += part(c.terms[i]);
                    if (c.terms[i].isMineral && c.terms[i].rarity != Rarity::None &&
                        (!anyOre || static_cast<int>(c.terms[i].rarity) > static_cast<int>(rarest))) {
                        rarest = c.terms[i].rarity;
                        anyOre = true;
                    }
                }
                AnalysisResult result;
                result.name = name;
                result.category = SignalCategory::ShipMining;
                result.rarity = anyOre ? rarest : Rarity::None;
                result.baseSignature = scannedSignature;
                result.clusterSize = 1;
                result.expectedSignature = scannedSignature;
                result.probability = total > 0.0 ? 100.0 * c.weight / total
                                                 : 100.0 / static_cast<double>(constellations.size());
                result.difference = 0.0;
                result.inSelectedSystem = c.inSelectedSystem;
                result.combination = true;
                result.terms = (int)c.terms.size();
                results.push_back(result);
            }
            anyExact = true;
            for (const auto& r : results) if (r.inSelectedSystem) anyExactInSystem = true;
        }
    }
    if (anyExact) {
        std::vector<AnalysisResult> kept;
        for (const auto& r : results) {
            if (r.difference < 1.0) {
                kept.push_back(r);
            } else if (!anyExactInSystem && r.inSelectedSystem) {
                // exact hit exists only outside the selected system - keep the
                // in-system near-misses as "in case the OCR misread" options
                kept.push_back(r);
            }
        }
        results = std::move(kept);
    }

    // Nothing here explains it: a 119,000 read at Monox is no misreading -
    // the marker says so - but no ore the tables list for Monox adds up to
    // it with any other. The tables name the ores a place is known for, not
    // every rock it can carry, so the honest answer is the list of readings
    // with a word about the place, not six rows greyed out of the way.
    outOfPlace_ = false;
    if ((!placeName_.empty() || ground_ == Ground::Space) && !results.empty()) {
        bool anyHere = false;
        for (const auto& r : results) anyHere = anyHere || r.inSelectedSystem;
        if (!anyHere) outOfPlace_ = true;
    }

    // Normalize probabilities over the candidates available in the selected
    // system. Matches outside it keep their raw match score (0..100) - they
    // are informational only and excluded from the 100% pool.
    double total = 0.0;
    for (const auto& r : results) {
        if (r.inSelectedSystem) total += r.probability;
    }
    if (total > 0.0) {
        for (auto& r : results) {
            if (r.inSelectedSystem) {
                r.probability = (r.probability / total) * 100.0;
            }
        }
    }

    // Sort: available where you stand first, then the story with the fewest
    // deposits in it, then plausibility. The count of deposits has to be in
    // here and not left to the weight: once a value can be read every way it
    // can be read, "Hephaestanite + Copper x2 + Salvage x6" outweighs the
    // plain "Riccite + Silicon x5" for 24,660 simply because common ores in
    // ordinary cluster sizes score better than one legendary rock - and two
    // things in a rock are commoner than three whatever they are.
    // Stable, not plain sort: the comparison leaves ties, and std::sort
    // settles them differently per library (MSVC and libstdc++ list the last
    // two constellations for 31,185 in opposite order), so two builds would
    // list the same answer differently.
    std::stable_sort(results.begin(), results.end(), [](const AnalysisResult& a, const AnalysisResult& b) {
        if (a.inSelectedSystem != b.inSelectedSystem) return a.inSelectedSystem;
        if (a.terms != b.terms) return a.terms < b.terms;
        return a.probability > b.probability;
    });
    for (auto& r : results) r.name = shownName(r.name);


    return results;
}

AnalysisResult SignatureAnalyzer::getMostLikely(int scannedSignature) {
    auto results = analyzeSignature(scannedSignature);
    if (results.empty()) {
        AnalysisResult unknown;
        unknown.name = "UNKNOWN";
        unknown.difference = static_cast<double>(scannedSignature);
        return unknown;
    }
    return results[0];
}

void SignatureAnalyzer::setTolerance(int tolerance) {
    tolerance_ = static_cast<double>(tolerance);
}

bool SignatureAnalyzer::isExactMatch(int scannedSignature, std::string& outName, int& outClusterSize) {
    for (const auto& mineral : MINERAL_SIGNATURES) {
        for (int n = 1; n <= maxClusterSize(mineral.rarity); n++) {
            if (scannedSignature == mineral.baseSignature * n) {
                outName = mineral.name;
                outClusterSize = n;
                return true;
            }
        }
    }
    for (const auto& special : SPECIAL_SIGNATURES) {
        for (int n : clusterSizesFor(special)) {
            if (scannedSignature == special.baseSignature * n) {
                outName = special.name;
                outClusterSize = n;
                return true;
            }
        }
    }
    return false;
}

bool SignatureAnalyzer::snapToExact(int scannedSignature, int& outCorrected) {
    // Digits the game font renders similarly enough for Tesseract to swap,
    // especially with the HUD's chromatic-aberration fringing
    static const std::map<char, std::string> CONFUSIONS = {
        {'0', "689"}, {'1', "47"},  {'2', "7"},   {'3', "895"},
        {'4', "19"},  {'5', "635"}, {'6', "805"}, {'7', "12"},
        {'8', "6035"},{'9', "038"},
    };

    std::string name;
    int cluster = 0;

    // Signatures are 4-6 digits; anything outside that is OCR garbage.
    // The bound also keeps digit substitution inside the int range.
    if (scannedSignature < 1000 || scannedSignature > 999999) {
        return false;
    }

    // Never rewrite a value that is already a table entry: neighboring
    // entries are often one digit apart (8480 -> 8450), so "repairing" a
    // correct read would corrupt it
    if (isExactMatch(scannedSignature, name, cluster)) {
        return false;
    }

    const std::string original = std::to_string(scannedSignature);

    // Single substitution first - the least invasive repair
    std::set<int> matches;
    for (size_t i = 0; i < original.size(); i++) {
        auto it = CONFUSIONS.find(original[i]);
        if (it == CONFUSIONS.end()) continue;

        for (char replacement : it->second) {
            std::string candidate = original;
            candidate[i] = replacement;
            if (candidate[0] == '0') continue;  // no leading zero

            const int value = std::stoi(candidate);
            if (isExactMatch(value, name, cluster)) {
                matches.insert(value);
            }
        }
    }
    if (matches.size() == 1) {
        outCorrected = *matches.begin();
        return true;
    }

    // Systematic misread: the HUD glow closes a digit's counter the same way
    // wherever it appears, so "16,960" turns into "18,980" - one confusion
    // applied to EVERY occurrence. That is far more constrained than editing
    // digits independently (which reaches so many values that it "uniquely"
    // matches genuine garbage), so it repairs this common case safely.
    std::set<int> systematic;
    for (const auto& [from, replacements] : CONFUSIONS) {
        if (std::count(original.begin(), original.end(), from) < 2) {
            continue;  // a single occurrence was already covered above
        }
        for (char to : replacements) {
            std::string candidate = original;
            std::replace(candidate.begin(), candidate.end(), from, to);
            if (candidate[0] == '0') continue;

            const int value = std::stoi(candidate);
            if (isExactMatch(value, name, cluster)) {
                systematic.insert(value);
            }
        }
    }
    if (systematic.size() == 1) {
        outCorrected = *systematic.begin();
        return true;
    }

    // Deliberately no free two-digit repair: with ~200 table values, allowing
    // two independent substitutions finds a "unique" match for genuine garbage
    // often enough to make results worse (measured: 16/20 -> 14/20).
    return false;
}

int SignatureAnalyzer::canonicalize(int scannedSignature) {
    int corrected = 0;
    if (snapToExact(scannedSignature, corrected)) {
        return corrected;
    }
    return scannedSignature;
}

std::string SignatureAnalyzer::generateReport(int scannedSignature) {
    std::stringstream ss;

    ss << "\n========================================\n";
    ss << "  STAR CITIZEN SIGNATURE ANALYSIS\n";
    ss << "========================================\n\n";
    ss << "  Scanned signature: " << scannedSignature;
    if (!systemFilter_.empty()) {
        ss << "  (system: " << systemFilter_ << ")";
    }
    ss << "\n\n";

    auto results = analyzeSignature(scannedSignature);

    if (results.empty()) {
        ss << "  No matching signature found.\n";
        ss << "  Possibly an OCR misread or an unknown value.\n";
    } else {
        if (results[0].difference >= 1.0) {
            ss << "  NOTE: no exact match - in-game values are always\n";
            ss << "  base x cluster, so this is likely an OCR misread.\n\n";
        }
        ss << "  PROBABLE MATCHES:\n";
        ss << "  ----------------------------------------\n";

        int rank = 1;
        for (const auto& result : results) {
            ss << "  " << rank << ". " << std::left << std::setw(14) << result.name;
            ss << " x" << result.clusterSize;
            if (result.inSelectedSystem) {
                ss << " | " << std::right << std::setw(5) << std::fixed << std::setprecision(1)
                   << result.probability << "%";
            } else {
                ss << " |   -  ";
            }
            ss << " | " << result.expectedSignature;

            if (result.difference < 1.0) {
                ss << " [EXACT]";
            } else {
                ss << " [~" << static_cast<int>(result.difference) << "]";
            }
            if (result.category == SignalCategory::ShipMining) {
                ss << " " << rarityName(result.rarity);
                if (!result.inSelectedSystem) {
                    ss << " (not in " << systemFilter_ << ")";
                }
            }
            ss << "\n";

            if (++rank > 5) break;
        }

        ss << "\n  ----------------------------------------\n";
        ss << "  BEST MATCH: " << results[0].name
           << " x" << results[0].clusterSize
           << " (" << results[0].expectedSignature << ")\n";
    }

    ss << "\n========================================\n";

    return ss.str();
}


namespace {

// One value a single signature can produce, and enough about it to judge
// whether pairing it with another one is physically sensible
struct SingleSignature {
    std::string label;
    std::string name;         // the ore or family itself
    bool isMineral = false;   // ship-minable ore, as opposed to a deposit family
    int count = 0;            // rocks / deposits / panels
    Rarity rarity = Rarity::None;
    uint8_t systems = SYS_ALL;
};

const std::map<int, SingleSignature>& singleSignatureTable() {
    static const std::map<int, SingleSignature> table = [] {
        std::map<int, SingleSignature> values;
        for (const auto& mineral : MINERAL_SIGNATURES) {
            for (int n = 1; n <= maxClusterSize(mineral.rarity); n++) {
                values.emplace(mineral.baseSignature * n,
                               SingleSignature{std::to_string(n) + "x " + mineral.name, mineral.name,
                                               true, n, mineral.rarity, mineral.systems});
            }
        }
        for (const auto& family : SPECIAL_SIGNATURES) {
            for (int n : clusterSizesFor(family)) {
                values.emplace(family.baseSignature * n,
                               SingleSignature{std::to_string(n) + "x " + family.name, family.name,
                                               false, n, Rarity::None, SYS_ALL});
            }
        }
        return values;
    }();
    return table;
}

// A marker shows a sum when a rock and a deposit share the spot. Seen in
// game: 39,350 = Iron x5 + harvestables x6, 68,080 = Iron x4 +
// harvestables x17, 76,975 = Tin x5 + a ROC field x14 - one ore beside
// one field of ground material. Two different ores in one rock occur too
// (Silicon x4/x5 with one Riccite deposit: 20,405 or 24,660).
// Anything wider explains far too much: numbers in chat text would count
// as valid signatures and the tracker would lock onto them. The same ore
// twice is not a pair but a bigger cluster, which the published sizes
// rule out, and two fields never pair.
bool sensiblePair(const SingleSignature& a, const SingleSignature& b) {
    if (a.isMineral && b.isMineral) return a.name != b.name;
    if (!a.isMineral && !b.isMineral) return false;
    const SingleSignature& field = a.isMineral ? b : a;
    return field.count >= MIN_FIELD_IN_COMBINATION;
}


}  // namespace

std::vector<int> SignatureAnalyzer::knownSignatures() const {
    std::vector<int> values;
    for (const auto& [value, entry] : singleSignatureTable()) values.push_back(value);
    return values;
}

bool SignatureAnalyzer::isKnownSignature(int scannedSignature) {
    std::string name;
    int cluster = 0;
    if (isExactMatch(scannedSignature, name, cluster)) return true;
    const auto splits = decomposePairs(scannedSignature, MAX_KNOWN_SPLITS);
    return !splits.empty() && splits.size() <= MAX_KNOWN_SPLITS;
}

bool SignatureAnalyzer::explainsExactly(int scannedSignature) {
    for (const auto& result : analyzeSignature(scannedSignature)) {
        if (result.difference < 1.0) return true;
    }
    return false;
}

bool SignatureAnalyzer::isSingleSignature(int scannedSignature) {
    std::string name;
    int cluster = 0;
    if (isExactMatch(scannedSignature, name, cluster)) return true;
    // One ore beside one field, and only when that split is the only one
    const auto splits = decomposePairs(scannedSignature, 1);
    return splits.size() == 1 && splits.front().firstIsMineral != splits.front().secondIsMineral;
}

std::vector<SignatureAnalyzer::Combination> SignatureAnalyzer::decomposePairs(
        int scannedSignature, size_t maxResults) {
    std::vector<Combination> found;

    const auto& table = singleSignatureTable();
    if (table.count(scannedSignature) > 0) return found;   // explained on its own

    for (const auto& [value, entry] : table) {
        const int rest = scannedSignature - value;
        if (rest < value) break;                 // each pair once, smaller first
        const auto other = table.find(rest);
        if (other == table.end()) continue;
        if (!sensiblePair(entry, other->second)) continue;
        Combination combo;
        combo.first = entry.label;
        combo.second = other->second.label;
        combo.firstName = entry.name;
        combo.secondName = other->second.name;
        combo.firstCount = entry.count;
        combo.secondCount = other->second.count;
        combo.firstIsMineral = entry.isMineral;
        combo.secondIsMineral = other->second.isMineral;
        // The rarer ore names the find; a field has no rarity
        combo.rarity = entry.rarity;
        if (other->second.isMineral &&
            (!entry.isMineral || static_cast<int>(other->second.rarity) > static_cast<int>(entry.rarity))) {
            combo.rarity = other->second.rarity;
        }
        const uint8_t mask = systemMask(systemFilter_);
        auto inScope = [&](const SingleSignature& one) {
            return one.isMineral ? ((one.systems & mask) != 0 && atPlace(one.name)) : familyPossible(one.name);
        };
        combo.inSelectedSystem = inScope(entry) && inScope(other->second);
        found.push_back(combo);
        if (found.size() > maxResults) return {};   // too many to mean anything
    }
    return found;
}

// How plausible one deposit is on its own: common ores spawn more than
// rare ones, and clusters come in the sizes the game's presets lay out
// (Common 4-6, Uncommon 3-5, Rare 2-4, Epic 2-3, Legendary 2); a lone rock
// happens, a field of two hardly.
static double termWeight(const SingleSignature& s) {
    if (!s.isMineral) {
        for (const auto& family : SPECIAL_SIGNATURES) {
            if (family.name == s.name) return s.count >= family.typicalFieldMin ? 1.0 : 0.3;
        }
        return 0.5;
    }
    double rarity = 1.0;
    int typicalMin = 2;
    switch (s.rarity) {
        case Rarity::Common:    rarity = 1.0;  typicalMin = 4; break;
        case Rarity::Uncommon:  rarity = 0.7;  typicalMin = 3; break;
        case Rarity::Rare:      rarity = 0.45; break;
        case Rarity::Epic:      rarity = 0.25; break;
        case Rarity::Legendary: rarity = 0.12; break;
        default: break;
    }
    double cluster = 1.0;
    if (s.count == 1) cluster = 0.6;
    else if (s.count < typicalMin) cluster = 0.8;
    return rarity * cluster;
}

// Every way one value can be read on its own. singleSignatureTable() keeps
// the first reading of a value and drops the rest, which is right where a
// value has to be explained uniquely, but loses real answers when a
// constellation is built: 16,000 is a ROC field of 4 AND harvestables of
// 8, the table keeps the ROC one, and the six-unit rule for fields in a
// mix then throws it out, so 37,350 = Iron x5 + eight harvestables could
// not be offered. This table is used by the decomposition alone; what the
// reader ACCEPTS still goes through the single-reading table.
const std::map<int, std::vector<SingleSignature>>& everyReadingTable() {
    static const std::map<int, std::vector<SingleSignature>> table = [] {
        std::map<int, std::vector<SingleSignature>> values;
        for (const auto& mineral : MINERAL_SIGNATURES) {
            for (int n = 1; n <= maxClusterSize(mineral.rarity); n++) {
                values[mineral.baseSignature * n].push_back(
                    SingleSignature{std::to_string(n) + "x " + mineral.name, mineral.name,
                                    true, n, mineral.rarity, mineral.systems});
            }
        }
        for (const auto& family : SPECIAL_SIGNATURES) {
            for (int n : clusterSizesFor(family)) {
                values[family.baseSignature * n].push_back(
                    SingleSignature{std::to_string(n) + "x " + family.name, family.name,
                                    false, n, Rarity::None, SYS_ALL});
            }
        }
        return values;
    }();
    return table;
}

std::vector<SignatureAnalyzer::Decomposition> SignatureAnalyzer::decomposeAll(
        int scannedSignature, size_t maxResults) {
    std::vector<Decomposition> found;
    // Plausible at all? Every signature is a multiple of 5, and nothing
    // above MAX_VALID_SIGNATURE exists in the published data - a 130,000
    // is not a constellation, it is a misread.
    if (scannedSignature < 1500 || scannedSignature > MAX_VALID_SIGNATURE || scannedSignature % 5 != 0) {
        return found;
    }
    const auto& table = everyReadingTable();
    const uint8_t mask = systemMask(systemFilter_);

    auto add = [&](const std::vector<const SingleSignature*>& parts) {
        // Different ores only (the same ore twice is a bigger cluster, and
        // one the sizes rule out), at most one field, and a field of a size
        // fields come in
        int fields = 0;
        for (size_t i = 0; i < parts.size(); i++) {
            if (!parts[i]->isMineral) {
                fields++;
                if (parts[i]->count < MIN_FIELD_IN_COMBINATION) return;
            }
            for (size_t j = i + 1; j < parts.size(); j++) {
                if (parts[i]->isMineral && parts[j]->isMineral && parts[i]->name == parts[j]->name) return;
            }
        }
        if (fields > 1) return;
        Decomposition d;
        d.weight = 1.0;
        for (const auto* p : parts) {
            d.terms.push_back(Term{p->name, p->count, p->isMineral, p->rarity});
            d.weight *= termWeight(*p);
            if (p->isMineral && ((p->systems & mask) == 0 || !atPlace(p->name))) d.inSelectedSystem = false;
            if (!p->isMineral && !familyPossible(p->name)) d.inSelectedSystem = false;
        }
        // Every deposit beyond the second makes the story less likely
        for (size_t extra = 2; extra < parts.size(); extra++) d.weight *= 0.35;
        found.push_back(std::move(d));
    };

    const auto single = table.find(scannedSignature);
    if (single != table.end()) {
        for (const auto& only : single->second) add({&only});
    }

    std::vector<std::pair<int, const std::vector<SingleSignature>*>> values;
    values.reserve(table.size());
    for (const auto& [v, s] : table) values.push_back({v, &s});
    for (size_t i = 0; i < values.size(); i++) {
        const int a = values[i].first;
        const int rest = scannedSignature - a;
        if (rest < a) break;                       // a <= b <= c: each set once
        const auto b = table.find(rest);
        if (b != table.end()) {
            for (const auto& first : *values[i].second) {
                for (const auto& second : b->second) add({&first, &second});
            }
        }
        for (size_t j = i; j < values.size(); j++) {
            const int c = rest - values[j].first;
            if (c < values[j].first) break;
            const auto third = table.find(c);
            if (third == table.end()) continue;
            for (const auto& first : *values[i].second) {
                for (const auto& second : *values[j].second) {
                    for (const auto& last : third->second) add({&first, &second, &last});
                }
            }
        }
    }

    // What can be where the player stands comes first - before the count of
    // deposits, before plausibility, and before the cut to maxResults.
    // Otherwise a 37,350 at Monox, with nine explanations out of local ores,
    // shows only combinations of ores from elsewhere, all marked "not at".
    // Stable for the same reason as the sort in analyzeSignature: equal weights
    // are common here, and the order they end up in should not depend on which
    // compiler built the binary.
    std::stable_sort(found.begin(), found.end(), [](const Decomposition& x, const Decomposition& y) {
        if (x.inSelectedSystem != y.inSelectedSystem) return x.inSelectedSystem;
        if (x.terms.size() != y.terms.size()) return x.terms.size() < y.terms.size();
        return x.weight > y.weight;
    });
    if (found.size() > maxResults) found.resize(maxResults);
    return found;
}

} // namespace SCMiner
