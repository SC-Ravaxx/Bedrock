#pragma once
// What the Gear page says under an item no shop is known for (WhereToBuy.h).
// UEX lists the shops; where it knows none and the game has a blueprint of
// the item, the item is made, not bought - Clearcut, Deluge and Overrun, and
// the Golem's Pitman, in 4.10.3 - and the line says so instead of that no
// shop is known.
#include "CraftingData.h"

#include <string>

namespace SCMiner {

// Whether the game has a blueprint of the item, by its name as the Gear page
// and UEX have it ("Clearcut Module")
inline bool hasBlueprint(const std::string& item) {
    for (int i = 0; i < CRAFT_BLUEPRINT_COUNT; i++) {
        if (item == CRAFT_BLUEPRINTS[i].name) return true;
    }
    return false;
}

// ...the line under an item UEX knows no shop for
inline const wchar_t* noShopWords(const std::string& item) {
    return hasBlueprint(item) ? L"Craftable only" : L"No shop known to UEX";
}

}  // namespace SCMiner
