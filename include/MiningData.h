#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace SCMiner {

// ---------------------------------------------------------------------------
// Signature model (Star Citizen 4.10, per-mineral signatures)
//
// Every signature stands on its own: one reading is ONE resource type times
// the number of nodes in that one cluster. Signatures are never mixed and
// never summed across separate objects.
//
//   reading = base signature x node count
//
// Node counts differ per family:
//   - ship mining:  1 node up to the rarity's maximum (Legendary 2, Epic 3,
//                   Rare 4, Uncommon 5, Common 6). These caps are real: a
//                   Legendary ore such as Savrilium never appears as more
//                   than a pair.
//   - FPS / ground: base 3000 / 4000. Deposit fields get large: values like
//                   54000 (3000 x 18) and 84000 (4000 x 21) were captured in
//                   game, so the published charts only show their axis range,
//                   not a cap.
//   - salvage:      base 2000 per panel.
//
// Sources, cross-checked:
//   https://scmdb.net           (build 4.10.0-live, base values and rarities)
//   https://strata.celd.space   (per-family node counts incl. ground/FPS)
// ---------------------------------------------------------------------------

enum class Rarity {
    Common,
    Uncommon,
    Rare,
    Epic,
    Legendary,
    None        // special categories (ROC / FPS / salvage)
};

enum class SignalCategory {
    ShipMining,
    ROCMining,
    FPSMining,
    Salvage
};

// System availability flags
constexpr uint8_t SYS_STANTON = 1;
constexpr uint8_t SYS_PYRO    = 2;
constexpr uint8_t SYS_NYX     = 4;
constexpr uint8_t SYS_ALL     = SYS_STANTON | SYS_PYRO | SYS_NYX;

// One ship-minable mineral with its base signature
struct MineralSignature {
    std::string name;
    int baseSignature;
    Rarity rarity;
    uint8_t systems;    // where this mineral spawns (bitmask of SYS_*)
};

// Hard maximum cluster size per rarity, from SCMDB's game-file data and
// confirmed in game: a Legendary ore never spawns more than 2 nodes, so a
// "Savrilium x10" reading does not exist. Strata's uniform 1x-10x grid is a
// generic multiplication table, not evidence of larger clusters.
inline int maxClusterSize(Rarity rarity) {
    switch (rarity) {
        case Rarity::Legendary: return 2;
        case Rarity::Epic:      return 3;
        case Rarity::Rare:      return 4;
        case Rarity::Uncommon:  return 5;
        case Rarity::Common:    return 6;
        default:                return 1;
    }
}

inline const char* rarityName(Rarity rarity) {
    switch (rarity) {
        case Rarity::Legendary: return "Legendary";
        case Rarity::Epic:      return "Epic";
        case Rarity::Rare:      return "Rare";
        case Rarity::Uncommon:  return "Uncommon";
        case Rarity::Common:    return "Common";
        default:                return "-";
    }
}

// Ship mining signatures (SCMDB, patch 4.9.0)
inline const std::vector<MineralSignature> MINERAL_SIGNATURES = {
    // Legendary (max cluster: 2)
    {"Quantainium", 3170, Rarity::Legendary, SYS_ALL},
    {"Stileron",    3185, Rarity::Legendary, SYS_PYRO},
    {"Savrilium",   3200, Rarity::Legendary, SYS_NYX},
    // Epic (max cluster: 3)
    {"Ouratite",    3370, Rarity::Epic, SYS_STANTON | SYS_PYRO},
    {"Riccite",     3385, Rarity::Epic, SYS_PYRO},
    {"Lindinium",   3400, Rarity::Epic, SYS_NYX},
    // Rare (max cluster: 4)
    {"Beryl",       3540, Rarity::Rare, SYS_ALL},
    {"Taranite",    3555, Rarity::Rare, SYS_ALL},
    {"Borase",      3570, Rarity::Rare, SYS_ALL},
    {"Gold",        3585, Rarity::Rare, SYS_ALL},
    {"Bexalite",    3600, Rarity::Rare, SYS_ALL},
    // Uncommon (max cluster: 5)
    {"Laranite",    3825, Rarity::Uncommon, SYS_ALL},
    {"Aslarite",    3840, Rarity::Uncommon, SYS_ALL},
    {"Titanium",    3855, Rarity::Uncommon, SYS_ALL},
    {"Tungsten",    3870, Rarity::Uncommon, SYS_ALL},
    {"Agricium",    3885, Rarity::Uncommon, SYS_ALL},
    {"Torite",      3900, Rarity::Uncommon, SYS_ALL},
    // Common (max cluster: 6)
    {"Hephaestanite", 4180, Rarity::Common, SYS_ALL},
    {"Tin",           4195, Rarity::Common, SYS_ALL},
    {"Quartz",        4210, Rarity::Common, SYS_ALL},
    {"Corundum",      4225, Rarity::Common, SYS_ALL},
    {"Copper",        4240, Rarity::Common, SYS_ALL},
    {"Silicon",       4255, Rarity::Common, SYS_ALL},
    {"Iron",          4270, Rarity::Common, SYS_ALL},
    {"Aluminium",     4285, Rarity::Common, SYS_ALL},
    {"Ice",           4300, Rarity::Common, SYS_ALL},
};

// Special (non-ship) signature families. Ground deposits carry a SIZE-ONLY
// signature (hand/FPS = 3000, vehicle/ROC = 4000); which mineral it is depends
// on the location, and each deposit is 100% one mineral.
struct SpecialSignature {
    std::string name;
    SignalCategory category;
    int baseSignature;
    int maxSize;          // largest published node count
    int typicalFieldMin;  // fields normally start here; smaller groups are rare
};

inline const std::vector<SpecialSignature> SPECIAL_SIGNATURES = {
    // The two sources disagree on which node counts occur: SCMDB lists every
    // multiple, Strata lists a lone deposit or a field of 6..14. Accepting the
    // union avoids discarding a real reading (readings like 32000 = 4000 x 8
    // only exist in Strata's range, and SCMDB's chart stops at its 30000 axis),
    // while counts between the two are flagged as unusual.
    // Node counts observed in game go far beyond what either site plots:
    // live captures over a planet surface show 54000 (= 3000 x 18) and
    // 84000 (= 4000 x 21), both clean multiples. Both charts simply stop at
    // their axis, so their ranges are display limits, not caps.
    {"ROC Deposit",  SignalCategory::ROCMining, 4000, 30, 6},
    {"FPS Minable",  SignalCategory::FPSMining, 3000, 50, 6},   // 123,000 = x41 seen in game
    {"Salvage",      SignalCategory::Salvage,   2000, 30, 2},
};

// Every node count a family can show, smallest first
inline std::vector<int> clusterSizesFor(const SpecialSignature& family) {
    std::vector<int> sizes;
    for (int n = 1; n <= family.maxSize; n++) {
        sizes.push_back(n);
    }
    return sizes;
}

// True for group sizes that only one of the two sources lists
inline bool isUnusualFieldSize(const SpecialSignature& family, int size) {
    return size > 1 && size < family.typicalFieldMin;
}

// A combination only counts when the ground part is a FIELD, not a single
// deposit: confirmed sums in game pair one ore cluster with 6 or more ground
// units, while allowing single units made arbitrary misreads look valid.
inline constexpr int MIN_FIELD_IN_COMBINATION = 6;

// Below this the OCR fallback is guessing rather than reading. One
// constant for the live scan AND the offline benchmark: a benchmark with a
// softer floor counts as recognised readings the player never sees, and a
// benchmark that flatters the app is worse than none.
//
// 65 because Tesseract only ever sees what the template reader could not
// segment, and must not be the softer of the two doors: at 45 it let
// through readings like 7901 off HUD text that had nothing to do with a
// marker.
inline constexpr double MIN_OCR_CONFIDENCE = 65.0;

// Readings above this are treated as misreads. Set well above the data's
// largest family value (ROC 30 x 4000): markers of 123,000 and 171,xxx (on
// a MOLE) appear in game, and a value the cap refuses is never shown - it
// reads as nothing, or as some shorter number.
inline constexpr int MAX_VALID_SIGNATURE = 250000;

// Possible ground-deposit minerals (which one spawns depends on the location)
inline const std::vector<std::string> ROC_MINERALS = {
    "Beradom", "Carinite", "Feynmaline", "Glacosite"
};
inline const std::vector<std::string> FPS_MINERALS = {
    "Aphorite", "Carinite", "Dolivine", "Hadanite", "Jaclium",
    "Janalite", "Sadaryx", "Saldynium"
};

// Result of a signature analysis: one (mineral, cluster size) candidate
struct AnalysisResult {
    std::string name;           // mineral or category name
    SignalCategory category = SignalCategory::ShipMining;
    Rarity rarity = Rarity::None;
    int baseSignature = 0;
    int clusterSize = 1;        // number of rocks / deposits / panels
    int expectedSignature = 0;  // baseSignature * clusterSize
    double probability = 0.0;   // 0..100, normalized over all candidates
    double difference = 0.0;    // |scanned - expected|
    bool inSelectedSystem = true;
    bool unusualCluster = false; // ground field size only one source lists
    bool combination = false;    // two signatures sharing the spot, name is "A + B"
    int terms = 1;               // deposits the name adds up; 1 unless a combination
};

// Mineral colors (for the UI), format 0xAARRGGBB
inline const std::map<std::string, uint32_t> MINERAL_COLORS = {
    {"Quantainium",   0xFF00FFFF},  // Cyan
    {"Stileron",      0xFF00CED1},  // DarkTurquoise
    {"Savrilium",     0xFFFF00FF},  // Magenta
    {"Ouratite",      0xFFEEE8AA},  // PaleGoldenrod
    {"Riccite",       0xFFFF1493},  // DeepPink
    {"Lindinium",     0xFF9400D3},  // DarkViolet
    {"Beryl",         0xFF7FFFD4},  // Aquamarine
    {"Taranite",      0xFFFFFF00},  // Yellow
    {"Borase",        0xFFFF4500},  // OrangeRed
    {"Gold",          0xFFFFD700},  // Gold
    {"Bexalite",      0xFFFF8C00},  // Orange
    {"Laranite",      0xFFDA70D6},  // Orchid
    {"Aslarite",      0xFF66CDAA},  // MediumAquamarine
    {"Titanium",      0xFFB0C4DE},  // LightSteelBlue
    {"Tungsten",      0xFFA9A9A9},  // DarkGray
    {"Agricium",      0xFF00FF00},  // Green
    {"Torite",        0xFF32CD32},  // LimeGreen
    {"Hephaestanite", 0xFFFF69B4},  // Pink
    {"Tin",           0xFF708090},  // SlateGray
    {"Quartz",        0xFFE6E6FA},  // Lavender
    {"Corundum",      0xFF9370DB},  // MediumPurple
    {"Copper",        0xFFB87333},  // Copper
    {"Silicon",       0xFF4682B4},  // SteelBlue
    {"Iron",          0xFF8B4513},  // SaddleBrown
    {"Aluminium",     0xFFC0C0C0},  // Silver
    {"Ice",           0xFFADD8E6},  // LightBlue
    {"ROC Deposit",   0xFFFFB347},  // Light orange
    {"FPS Minable",   0xFF98FB98},  // PaleGreen
    {"Salvage",       0xFF87CEEB},  // SkyBlue
};

// Screen capture configuration
struct CaptureRegion {
    int x = 0;
    int y = 0;
    int width = 400;
    int height = 300;
    bool autoDetect = true;  // Automatic detection of the scan area
};

// OCR configuration
struct OCRConfig {
    std::string tessdataPath = "./tessdata";
    std::string language = "eng";
    int pageSegMode = 7;     // PSM_SINGLE_LINE for single numbers
    double minConfidence = 60.0;
};

} // namespace SCMiner
