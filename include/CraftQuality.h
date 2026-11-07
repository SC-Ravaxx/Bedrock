#pragma once

// The quality a crafted item comes out at, as one number. The game shows
// none: the bench shows each slot's quality and what it moves, and the item
// keeps the qualities it was made of (CraftingGlobalParams'
// defaultCompositionQuality, 500, stands in for an item nobody made).
//
// Each slot's quality counts by how far the slot moves the item: the spans
// of its values, |factor at the top - factor at the bottom|, added up. A
// Frame that moves three recoil values from x1.40 to x0.60 counts 2.4,
// Lenses that move Impact Force from x0.93 to x1.08 count 0.15, and a Lumin
// V SMG of Riccite 965, Copper 958 and Aphorite 523 comes out at 941, as its
// values do: recoil -37 %, fire rate +11 %, impact +0.35 %. By the amount
// each slot takes it was 532 - two Aphorite pieces counted as 2 against
// 0.03 and 0.01 SCU, and one blueprint in six mixes pieces and SCU.
#include <cmath>

#include "CraftingData.h"

namespace SCMiner {

// `quality` holds one value per slot of the blueprint, in its slot order,
// `have` how many it holds. -1 when that is too few for the blueprint, when
// no slot moves anything, and when a value's curve is not known or not to be
// trusted: counted at nothing, its slot would weigh as if any quality did.
inline int craftAverageQuality(const CraftBlueprint& bp, const int* quality, int have) {
    if (bp.count > have) return -1;
    double sum = 0.0, weight = 0.0;
    for (int s = 0; s < bp.count; s++) {
        const CraftSlot& slot = CRAFT_SLOTS[bp.first + s];
        double w = 0.0;
        for (int k = 0; k < slot.effCount; k++) {
            const CraftEffect& e = CRAFT_EFFECTS[slot.effFirst + k];
            if (effectDataBroken(e) || effectCurveUnknown(e)) return -1;
            if (effectMovesStat(e)) w += std::fabs((double)e.atMax - (double)e.atMin);
        }
        sum += w * quality[s];
        weight += w;
    }
    if (weight <= 0) return -1;
    return (int)std::lround(sum / weight);
}

} // namespace SCMiner
