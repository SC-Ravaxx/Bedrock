#pragma once

// The quality a crafted item comes out at, as one number. The game shows
// none: the bench shows each slot's quality and what it moves, and the item
// keeps the qualities it was made of (CraftingGlobalParams'
// defaultCompositionQuality, 500, stands in for an item nobody made).
//
// Each value of the item counts by what the item is for
// (craftStatImportance): a weapon's damage and fire rate 3, each of its
// recoil values 1. The slots that move a value share its count by how far
// each moves it. How far quality moves a value does not raise its count -
// damage moves 7.5 % either way and recoil 40 %, and a weapon is still
// taken for its damage. A Lumin V SMG of Riccite 965 (recoil), Copper 958
// (fire rate) and Aphorite 523 (damage) is 815. By the amount each slot
// takes it was 532, two Aphorite pieces counted as 2 against 0.03 and
// 0.01 SCU; by how far each slot moves the item 941.
#include <cmath>
#include <initializer_list>
#include <string_view>

#include "CraftingData.h"

namespace SCMiner {

// How much a value counts toward the item's quality: 3 what the item is
// made for, 2 what comes second, 1 the rest - Integrity, temperatures,
// recoil. A weapon is taken for its damage per second, a quantum drive for
// its speed and fuel, a shield for its strength, armour for what it stops.
inline int craftStatImportance(const CraftBlueprint& bp, int stat) {
    const std::string_view type = CRAFT_TYPES[bp.type];
    const std::string_view name = CRAFT_STAT_NAMES[stat];
    const auto is = [&](std::initializer_list<std::string_view> names) {
        for (const std::string_view n : names) {
            if (n == name) return true;
        }
        return false;
    };
    if (type.find("(Armor)") != std::string_view::npos || type == "Jacket" || type == "Shirt" ||
        type == "Shoes" || type == "Trousers")
        return is({"Damage Mitigation"}) ? 3 : 1;
    if (type == "FPS Weapon") return is({"Impact Force", "Fire Rate"}) ? 3 : 1;
    if (type == "Weapon Gun" || type == "Mining Laser") return is({"Impact Force"}) ? 3 : 1;
    if (type == "Quantum Drive") return is({"Quantum Speed", "Quantum Fuel Burn"}) ? 3 : 1;
    if (type == "Shield") return is({"Max. Shield Strength"}) ? 3 : 1;
    if (type == "Cooler" || type == "Other") return is({"Coolant Rating"}) ? 3 : 1;
    if (type == "Radar") return is({"Min. Assist Distance", "Max. Assist Distance"}) ? 3 : 1;
    if (type == "Tractor Beam") return is({"Beam Force", "Max. Volume"}) ? 3 : is({"Integrity"}) ? 1 : 2;
    if (type == "Salvage Modifier") return is({"Efficiency", "Speed"}) ? 3 : 2;
    return 1;
}

// `quality` holds one value per slot of the blueprint, in its slot order,
// `have` how many it holds. -1 when that is too few for the blueprint, when
// no slot moves anything, and when a value's curve is not known or not to be
// trusted: counted at nothing, its slot would weigh as if any quality did.
inline int craftAverageQuality(const CraftBlueprint& bp, const int* quality, int have) {
    if (bp.count > have) return -1;
    // how far the slots together move each value...
    double moved[CRAFT_STAT_COUNT] = {};
    for (int s = 0; s < bp.count; s++) {
        const CraftSlot& slot = CRAFT_SLOTS[bp.first + s];
        for (int k = 0; k < slot.effCount; k++) {
            const CraftEffect& e = CRAFT_EFFECTS[slot.effFirst + k];
            if (effectDataBroken(e) || effectCurveUnknown(e)) return -1;
            if (effectMovesStat(e)) moved[e.stat] += std::fabs((double)e.atMax - (double)e.atMin);
        }
    }
    // ...and each slot's share of every value it moves
    double sum = 0.0, weight = 0.0;
    for (int s = 0; s < bp.count; s++) {
        const CraftSlot& slot = CRAFT_SLOTS[bp.first + s];
        double w = 0.0;
        for (int k = 0; k < slot.effCount; k++) {
            const CraftEffect& e = CRAFT_EFFECTS[slot.effFirst + k];
            if (!effectMovesStat(e)) continue;
            w += craftStatImportance(bp, e.stat) * std::fabs((double)e.atMax - (double)e.atMin) / moved[e.stat];
        }
        sum += w * quality[s];
        weight += w;
    }
    if (weight <= 0) return -1;
    return (int)std::lround(sum / weight);
}

// The same as a letter, S to F, along the game's quality bands - 0-399,
// 400-599, 600-699, 700-799, 800-899, 900-949, 950-998 and 999-1000 for
// every material (records/crafting/qualityquantization), the top two as S.
// The game shows no letter; the bands are the steps it hands quality out in.
inline char craftQualityGrade(int quality) {
    if (quality >= 950) return 'S';
    if (quality >= 900) return 'A';
    if (quality >= 800) return 'B';
    if (quality >= 700) return 'C';
    if (quality >= 600) return 'D';
    if (quality >= 400) return 'E';
    return 'F';
}

} // namespace SCMiner
