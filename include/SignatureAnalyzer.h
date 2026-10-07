#pragma once

#include <vector>
#include <string>
#include "MiningData.h"
#include "OCREngine.h"

namespace SCMiner {

class SignatureAnalyzer;

// The domain knowledge the OCR vote uses: repair misreads against the
// signature table, reward exact hits, and reject values no signature can
// produce. Shared so the app, the self-test and the --ocr tool all judge
// readings the same way.
OCREngine::ReadingPolicy makeReadingPolicy(SignatureAnalyzer& analyzer);

// Matches a scanned radar signature against the per-mineral signature table
// (signature = mineral base value x cluster size, see MiningData.h).
class SignatureAnalyzer {
public:
    SignatureAnalyzer();

    // Restricts ship-mining matches to one system ("STANTON", "PYRO", "NYX").
    // Pass an empty string to disable filtering.
    void setSystemFilter(const std::string& system);
    std::string getSystemFilter() const;
    // Narrower still: only the ores the tables list for one location
    // ("Pyro II (Monox)", a `location` of ORE_LOCATIONS) - where the
    // player is, read off the game log. A candidate the place does not
    // have is marked out of scope exactly like one outside the system,
    // and the mixes are built without it. Empty, or a location the tables
    // do not know, lifts it.
    void setPlaceFilter(const std::string& oreLocation);
    const std::string& placeFilter() const { return placeName_; }
    // Where the ship stands, as far as the ground goes. On a planet or moon
    // the 2,000 family reads "Harvestable/Salvage" (there is no salvage on
    // a body today, and harvestables share the value); out in space it
    // stays "Salvage", and there are no ROC deposits and no hand minables,
    // so those are out of scope there the way an ore of another system is.
    enum class Ground { Unknown, Space, Surface };
    void setGround(Ground ground) { ground_ = ground; }
    Ground ground() const { return ground_; }
    // What a candidate out of scope is out of: "NYX", "Pyro II (Monox)",
    // or "open space"
    std::string scopeName() const {
        if (!placeName_.empty()) return placeName_;
        return ground_ == Ground::Space ? std::string("open space") : systemFilter_;
    }
    // True when the last analysis found nothing the place can explain, and
    // handed back what it had anyway - see the note in analyzeSignature
    bool outOfPlace() const { return outOfPlace_; }

    // Analyzes a scanned signature and returns matching candidates,
    // sorted by: available-in-system first, then probability (highest first).
    std::vector<AnalysisResult> analyzeSignature(int scannedSignature);

    // Returns only the most likely result
    AnalysisResult getMostLikely(int scannedSignature);

    // Sets the full-probability window in signature units (default: 30).
    // Deviations beyond ~2x this window score close to zero.
    void setTolerance(int tolerance);

    // Returns a formatted plain-text report of the analysis (console mode)
    std::string generateReport(int scannedSignature);

    // Checks whether a signature is an exact base x cluster match
    bool isExactMatch(int scannedSignature, std::string& outName, int& outClusterSize);

    // True for any value the game can actually show: a single signature, or
    // a sum of two that can be explained in exactly ONE way. A marker over a
    // spot holding two minables carries their sum (39,350 in game is Iron x5
    // plus harvestables x6); without sums the reader could not repair a
    // hesitant digit towards such a value and the scan would not accept it.
    // Uniqueness keeps it honest: with several splits the arithmetic fits
    // almost anything.
    bool isKnownSignature(int scannedSignature);
    // The stricter tier: a single signature, or one ore beside one ground
    // field. A reading may be REPAIRED towards these - one hesitant digit
    // swapped, a stray end glyph dropped - but never towards a two-ore sum:
    // those are accepted only as read. Measured on 1,646 live frames: with
    // sums in the plain table a "6" read as "8" gave 18,960 where 16,960
    // stood; with sums excluded altogether a real 61,620 (a sum) was
    // "repaired" to 81,620 in six frames. Read plainly, both kinds count;
    // repaired, only the single ones.
    bool isSingleSignature(int scannedSignature);
    // Anything the analysis explains to the unit: a single signature, a
    // pair, or the three-ore sums the pair test leaves out. The second
    // reader's test (TextLineReader): on the labelled frames a real 86,000 -
    // three ores and nothing less - is only reachable this way.
    bool explainsExactly(int scannedSignature);

    // Every value a single signature can show, for readers that need to
    // ENUMERATE rather than test: the grid reader scores each of these
    // against the picture and keeps the one that explains it best.
    std::vector<int> knownSignatures() const;

    struct Combination {
        std::string first;    // "4x Silicon"
        std::string second;   // "1x Riccite"
        // ...and the same, taken apart, for building a result row
        std::string firstName, secondName;
        int firstCount = 0, secondCount = 0;
        bool firstIsMineral = false, secondIsMineral = false;
        Rarity rarity = Rarity::None;   // the rarer of two ores
        bool inSelectedSystem = true;   // both parts spawn in the filtered system
    };

    // Ways to explain a value as TWO signatures at the same spot. A marker
    // over a rock holding more than one minable carries their sum - 39,350
    // read in game turned out to be Iron x5 (21,350) plus harvestables x6
    // (18,000). The arithmetic alone rarely settles it (39,350 also splits
    // three other ways), so every possibility is offered and the player, who
    // can see what the rock holds, picks. Empty when the value needs no
    // explaining or when so many splits fit that they explain nothing.
    std::vector<Combination> decomposePairs(int scannedSignature, size_t maxResults = 4);

    // One deposit in a constellation: an ore cluster or a ground field
    struct Term {
        std::string name;
        int count = 0;
        bool isMineral = false;
        Rarity rarity = Rarity::None;
    };
    struct Decomposition {
        std::vector<Term> terms;      // their signatures sum to the value
        double weight = 0.0;          // plausibility, relative within one call
        bool inSelectedSystem = true;
    };
    // Every constellation of deposits whose signatures add up to the value:
    // one ore cluster, two different ores, an ore beside a ground field,
    // three different ores, two ores beside a field. A plausible number - a
    // multiple of 5 in range - is never left unexplained; the player, who
    // sees the rock, picks. Ranked by plausibility: fewer deposits, common
    // ores, cluster sizes the game spawns. This is for SHOWING; it does not
    // change what the reader accepts.
    std::vector<Decomposition> decomposeAll(int scannedSignature, size_t maxResults = 6);

    // In-game values are always exact table entries, so a read that misses
    // can often be repaired by substituting visually confusable digits
    // (6/8, 1/7, 0/8, ...). Tries one substitution first, then two; succeeds
    // only when exactly one candidate lands on a table value, which keeps
    // the correction unambiguous.
    bool snapToExact(int scannedSignature, int& outCorrected);

    // Returns the repaired value, or the input when it is already exact or
    // cannot be repaired unambiguously.
    int canonicalize(int scannedSignature);

private:
    std::string systemFilter_;  // empty = no filter
    std::string placeName_;     // the place filter, "" when none
    std::vector<std::string> placeMinerals_;   // the ores listed at that place
    bool atPlace(const std::string& mineral) const;
    Ground ground_ = Ground::Unknown;
    bool outOfPlace_ = false;
    // Whether a ground family can be where the ship stands - see setGround
    bool familyPossible(const std::string& family) const {
        return !(ground_ == Ground::Space && (family == "ROC Deposit" || family == "FPS Minable"));
    }
    // The name a special family is shown under here - see setGround
    std::string shownName(const std::string& name) const {
        return (ground_ == Ground::Surface && name == "Salvage") ? "Harvestable/Salvage" : name;
    }
    double tolerance_ = 30.0;

    // Score for one candidate given its deviation (0..100, not normalized)
    double scoreCandidate(double difference, int expectedSignature) const;

    static uint8_t systemMask(const std::string& system);
};

} // namespace SCMiner
