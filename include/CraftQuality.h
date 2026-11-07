#pragma once

// The quality a crafted item comes out at, as one number. The game draws
// none: the bench shows each slot's quality and what it moves, and the item
// keeps the qualities it was made of (CraftingGlobalParams'
// defaultCompositionQuality, 500, stands in for an item nobody made). The
// community tools print one all the same ("958/1000"), and players compare
// against it.
//
// Theirs is the slots' qualities averaged by the amount each slot takes, as
// the blueprint states the amount: SCU for a resource, a count for an item.
// The one published case - a Parallax from Carinite 959, Gold 916 and
// Tungsten 964, which is 958 - comes out at 958.4 that way. Averaged plainly
// it is 946, by the item's own volume (Carinite 1000 microSCU) 948, by the
// values each slot moves 953 and by how far it moves them 960.
#include <cmath>

#include "CraftingData.h"

namespace SCMiner {

// `quality` holds one value per slot of the blueprint, in its slot order,
// `have` how many it holds. -1 when that is too few for the blueprint or
// none of its slots takes an amount.
inline int craftAverageQuality(const CraftBlueprint& bp, const int* quality, int have) {
    if (bp.count > have) return -1;
    double sum = 0.0, weight = 0.0;
    for (int s = 0; s < bp.count; s++) {
        const CraftSlot& slot = CRAFT_SLOTS[bp.first + s];
        const double amount = slot.scu > 0 ? (double)slot.scu : (double)slot.units;
        if (amount <= 0) continue;
        sum += amount * quality[s];
        weight += amount;
    }
    if (weight <= 0) return -1;
    return (int)std::lround(sum / weight);
}

} // namespace SCMiner
