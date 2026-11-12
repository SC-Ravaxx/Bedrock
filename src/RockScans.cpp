#include "RockScans.h"

#include "MiningAdvice.h"
#include "MiningHardware.h"
#include "MiningLocations.h"

#include <algorithm>
#include <cmath>

namespace SCMiner {
namespace RockScans {

bool notScannedYet(const RockStats& rock) {
    if (rock.ore.empty() || rock.ore == "Ice") return false;
    if (rock.mass <= 0 || rock.scu <= 0) return false;
    // both read, and both nothing
    if (rock.resistance < 0 || rock.instability < 0) return false;
    if (rock.resistance >= 0.5f || rock.instability >= 0.005f) return false;
    const float volume = rock.scu * KG_PER_SCU_AT_DENSITY_ONE;
    return std::fabs(rock.mass - volume) <= (std::max)(1.5f, 0.006f * rock.mass);
}

double densestPart(const std::string& rockOre) {
    double densest = 0;
    for (const RockPart& part : ROCK_PARTS) {
        if (rockOre == part.rock) densest = (std::max)(densest, oreDensity(part.ore));
    }
    if (densest > 0) return densest;
    if (const double own = oreDensity(rockOre); own > 0) return own;
    for (const OreDensity& row : ORE_DENSITIES) densest = (std::max)(densest, row.density);
    return densest;
}

std::string impossible(const RockStats& rock) {
    if (rock.mass <= 0 || rock.scu <= 0) return "";
    // the panel prints the SCU to a hundredth and the mass to a kilogram
    if (rock.scu > LARGEST_ROCK_SCU + 0.08f) return "more SCU than the largest rock";
    const double lightest = (rock.mass + 1.0) / (std::max)(0.001, rock.scu - 0.01);
    if (lightest < KG_PER_SCU_AT_DENSITY_ONE * 0.99) return "lighter than its volume";
    const double heaviest = (rock.mass - 1.0) / (rock.scu + 0.01);
    if (heaviest > KG_PER_SCU_AT_DENSITY_ONE * densestPart(rock.ore) * 1.01) return "heavier than its densest part";
    return "";
}

std::string LookCheck::notTaken(const RockStats& rock) {
    if (notScannedYet(rock)) return "not scanned yet";
    const std::string why = impossible(rock);
    if (why.empty()) {
        looks_ = 0;
        return "";
    }
    if (looks_ > 0 && rock.mass == mass_ && rock.scu == scu_) {
        looks_++;
    } else {
        mass_ = rock.mass;
        scu_ = rock.scu;
        looks_ = 1;
    }
    return looks_ >= 8 ? std::string() : why;
}

}  // namespace RockScans
}  // namespace SCMiner
