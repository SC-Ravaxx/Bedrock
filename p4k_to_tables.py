"""Turns the p4k extraction into the three inputs gen_crafting.py reads, so
the header can be built from the game files alone.

    p4k_to_tables.py <blueprints_from_p4k.json> <item_classes.json> <out-dir>

Writes <out-dir>/blueprints_all.json, <out-dir>/detail/*.json and
<out-dir>/items/*.json in the shape gen_crafting.py already understands.

item_classes.json carries the one field the game database does not spell out:
whether a component is Military, Civilian, Industrial, Competition or Stealth.
It is keyed by blueprint tag, lives in this repository, and is only consulted
for that one column.
"""
import io, json, os, sys

src, classes_file, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
blueprints = json.load(io.open(src, encoding="utf-8"))
classes = json.load(io.open(classes_file, encoding="utf-8")) if os.path.exists(classes_file) else {}

detail_dir = os.path.join(out_dir, "detail")
items_dir = os.path.join(out_dir, "items")
os.makedirs(detail_dir, exist_ok=True)
os.makedirs(items_dir, exist_ok=True)

# The game stores grades as 1..4; everything a player sees says A..D.
GRADE = {"1": "A", "2": "B", "3": "C", "4": "D"}

# The item records carry the engine's own type names. These are the words the
# game and its players use instead.
TYPE_LABEL = {
    "Char_Armor_Helmet": "Helmet (Armor)",
    "Char_Armor_Torso": "Torso (Armor)",
    "Char_Armor_Arms": "Arms (Armor)",
    "Char_Armor_Legs": "Legs (Armor)",
    "Char_Armor_Backpack": "Backpack (Armor)",
    "Char_Armor_Undersuit": "Undersuit (Armor)",
    "Char_Clothing_Torso_0": "Jacket",
    "Char_Clothing_Torso_1": "Shirt",
    "Char_Clothing_Legs": "Trousers",
    "Char_Clothing_Feet": "Shoes",
    "WeaponPersonal": "FPS Weapon",
    "WeaponAttachment": "Weapon Attachment",
    "WeaponGun": "Weapon Gun",
    "WeaponMining": "Mining Laser",
    "MiningModifier": "Mining Modifier",
    "SalvageModifier": "Salvage Modifier",
    "PowerPlant": "Power Plant",
    "QuantumDrive": "Quantum Drive",
    "Cooler": "Cooler",
    "Shield": "Shield",
    "Radar": "Radar",
    "TractorBeam": "Tractor Beam",
    "Container": "Container",
    "Fuel Nozzle": "Fuel Nozzle",
    "Misc": "Misc",
    "Other": "Other",
}


def type_label(raw):
    return TYPE_LABEL.get(raw, raw or "Other")

# Which way a stat should go. The property records name a stat and give its
# unit, but not whether more of it is good - so the handful that read the
# other way are named here. Recoil and spread are error terms, fuel burn is a
# cost: on all of them a smaller number is the better item. Everything else
# counts up. (Cross-checked against 5,692 modifier rows: exactly these.)
LOWER_IS_BETTER = ("recoil", "spread", "fuel burn", "fuel requirement")


def direction(stat):
    low = (stat or "").lower()
    return "lower" if any(w in low for w in LOWER_IS_BETTER) else "higher"

listing = []
for b in blueprints:
    tag, guid = b["tag"], b["guid"]
    ingredients = []
    aspects = []
    for s in b["slots"]:
        by_units = s["units"] > 0
        ingredients.append({
            "name": s["material"],
            "kind": "item" if by_units else "resource",
            "resource_type_uuid": None if by_units else s["materialGuid"],
            "item_uuid": s["materialGuid"] if by_units else None,
            "quantity_scu": None if by_units else s["scu"],
            "quantity": s["units"] if by_units else None,
        })
        # One entry per value the slot moves, not only the first: most move one,
        # 997 of them move two and 262 move three.
        modifiers = []
        for e in s.get("effects") or ([s] if s.get("stat") else []):
            if not e.get("stat"):
                continue
            modifiers.append({
                "label": e["stat"],
                # The direction is not in the files; it is filled in by the
                # majority rule in gen_crafting.py, which only needs a value here
                "better_when": direction(e["stat"]),
                "quality_range": {"min": e["qMin"], "max": e["qMax"]},
                "modifier_range": {"at_min_quality": e["atMin"], "at_max_quality": e["atMax"]},
                "value_range_type": "linear",
                "value_segments": None,
                "unreadable": bool(e.get("unreadable")),
                "from_twin": bool(e.get("fromTwin")),
                "property_key": e["stat"],
            })
        aspects.append({
            "key": s["slot"].upper(),
            "name": s["slot"],
            "input": {
                "kind": "item" if by_units else "resource",
                "name": s["material"],
                "quantity": s["units"] if by_units else None,
                "quantity_scu": None if by_units else s["scu"],
                "min_quality": s["minQuality"],
            },
            "modifiers": modifiers,
        })

    listing.append({
        "uuid": guid,
        "key": tag,
        "output_name": b["name"] or tag,
        "output_item_uuid": guid,          # the item table below is keyed the same way
        "craft_time_seconds": b["seconds"],
        "is_available_by_default": False,
        "game_version": "4.10.0-LIVE (game files)",
        "ingredient_count": len(ingredients),
        "unlocking_missions_count": 0,
        "ingredients": ingredients,
        "output": {
            "name": b["name"] or tag,
            "type": b["type"],
            "type_label": type_label(b["type"]),
            "sub_type": b["subType"],
            "grade": b["grade"],
        },
    })

    json.dump({"uuid": guid, "aspects": aspects, "summary_properties": []},
              io.open(os.path.join(detail_dir, guid + ".json"), "w", encoding="utf-8"))
    json.dump({
        "uuid": guid,
        "size": b["size"],
        "grade": GRADE.get(str(b["grade"]), ""),
        "class": classes.get(tag, ""),
        "classification": type_label(b["type"]),
        "manufacturer": b["maker"],
    }, io.open(os.path.join(items_dir, guid + ".json"), "w", encoding="utf-8"))

json.dump(listing, io.open(os.path.join(out_dir, "blueprints_all.json"), "w", encoding="utf-8"))
named = sum(1 for b in listing if b["output_name"] and not b["output_name"].startswith("BP_CRAFT"))
print(f"{len(listing)} blueprints ({named} named), {len(os.listdir(detail_dir))} detail files, "
      f"{len(os.listdir(items_dir))} item files -> {out_dir}")
