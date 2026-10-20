"""Generates include/CraftingData.h from the blueprint dump of the Star
Citizen wiki API. Usage:

    gen_crafting.py blueprints_all.json detail_dir item_dir bands.json contracts.json \
                    include/CraftingData.h [reputation.json]

The list endpoint gives names, times and ingredients; the per-blueprint detail
adds what the crafting bench actually shows: the SLOTS a recipe has ("Frame",
"Emitter"), which stat each slot moves, and how far the material's quality
moves it. Both come from api.star-citizen.wiki and carry the game version.

The table is flat on purpose: one array of materials, one of slots, one of
blueprints indexing into the slots. 1,600 recipes stay well under a megabyte
of header that way, and "what needs this ore" is a scan over an array.
"""
import json, io, os, re, sys, collections, datetime


def game_text(s):
    """The game stores its line breaks as the two characters backslash and n
    inside the string itself. Passed through, they printed as "\\n" in the
    middle of a sentence; turned into real breaks, they lay a briefing out
    the way the game does."""
    s = (s or "").replace("\\r\\n", "\n").replace("\\n", "\n").replace("\\r", "\n")
    s = re.sub(r"[ \t]+\n", "\n", s)
    s = re.sub(r"\n{3,}", "\n\n", s)
    return s.strip()


def briefing_and_rep(raw, rep):
    """The briefing as the game writes it, and what completing the contract
    awards (gen_contracts_from_p4k.py reads it out of the contract). The
    game's own text has no "Reputation Awarded" tail; only community
    language packs add one."""
    return game_text(raw).strip(), rep or ""



src, detail_dir, item_dir, bands_file, contracts_file, out = (
    sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6])
rep_file = sys.argv[7] if len(sys.argv) > 7 else ""
# {"BP_CRAFT_...": {"contracts": 16, "sources": [{title, faction, systems, ...}]}}
contracts = (json.load(io.open(contracts_file, encoding="utf-8"))
             if os.path.exists(contracts_file) else {})
# {"Iron": {"bands": [325, 521, ...], "rarity": "common"}, ...}
_raw_bands = json.load(io.open(bands_file, encoding="utf-8")) if os.path.exists(bands_file) else {}


def _band_key(name):
    """Material names differ by source: the game files spell them from their
    file name (quantization_saldynium_ore -> "Saldynium Ore"), the wiki writes
    "Saldynium (Ore)" and "Aluminum". Compared without the bracketed part and
    with the one spelling difference folded, they line up."""
    n = re.sub(r"\s*\(.*\)$", "", str(name)).strip().lower()
    n = n.replace("aluminum", "aluminium")
    n = re.sub(r"\s+(ore|gem|raw)$", "", n)
    return re.sub(r"[^a-z]", "", n)


bands = {}
for _k, _v in _raw_bands.items():
    bands.setdefault(_band_key(_k), _v)

# The reputation ladders and every contract's rank, out of the game's records
# (gen_contracts_from_p4k.py, REPUTATION_JSON): what the planner reads the
# player's ranks with. Without the file the tables are empty, and every row
# asks for no rank.
reputation = (json.load(io.open(rep_file, encoding="utf-8"))
              if rep_file and os.path.exists(rep_file) else {})
rep_ladders = reputation.get("ladders", {})
rep_standings = reputation.get("standings", {})
rep_outfits = reputation.get("factions", {})
ladder_order = sorted(rep_ladders, key=lambda s: (rep_ladders[s]["name"], s))
ladder_index = {s: i for i, s in enumerate(ladder_order)}
outfit_order = sorted(rep_outfits, key=lambda f: (rep_outfits[f], f))
outfit_index = {f: i for i, f in enumerate(outfit_order)}
assert len(ladder_order) < 128 and len(outfit_order) < 32768


def rank_on(scope, standing, default):
    """The rank of the scope's ladder a standing falls in. Not every standing
    a contract names is a rung: Neutral up to "FactionRep_Allied_Border"
    (750) is a range inside Neutral, and it falls in Neutral."""
    if not standing or standing not in rep_standings:
        return default
    value = rep_standings[standing]
    rank = 0
    for i, r in enumerate(rep_ladders[scope]["ranks"]):
        if r["min"] <= value:
            rank = i
    return rank


def rep_fields(need, awards):
    """(outfit, ladder, rank, highest rank, gain) of a contract: the rank it
    is offered in and what completing it adds on that ladder. A contract
    that asks for no rank still counts where its reputation goes."""
    outfit = scope = None
    if need and need[0] in outfit_index and need[1] in ladder_index:
        outfit, scope = need[0], need[1]
    else:
        for f, s, _a in awards or []:
            if f in outfit_index and s in ladder_index:
                outfit, scope = f, s
                break
    if outfit is None:
        return -1, -1, -1, -1, 0
    top = len(rep_ladders[scope]["ranks"]) - 1
    if need and need[0] == outfit and need[1] == scope:
        rank, highest = rank_on(scope, need[2], 0), rank_on(scope, need[3], top)
    else:
        rank = highest = -1
    gain = sum(a for f, s, a in awards or [] if f == outfit and s == scope)
    return outfit_index[outfit], ladder_index[scope], rank, highest, gain
blueprints = json.load(io.open(src, encoding="utf-8"))
blueprints = [b for b in blueprints if b.get("output_name")]
versions = sorted({b.get("game_version") for b in blueprints if b.get("game_version")})

# --- the ore finder spells a few of these differently ----------------------
# Pressurized Ice is what a rock's ice refines into, as Agricium is what
# Agricium (Ore) does: Shubin's ship-mining orders ask for it beside Quartz,
# Iron and Titanium. The ore finder and the signature table call that rock
# Ice.
LOCATION_NAME = {"Aluminum": "Aluminium", "Saldynium (Ore)": "Saldynium", "Pressurized Ice": "Ice"}
# ...and the quality bands a refined material keeps are those of what it is
# refined from, filed under the mined name: a rock's ice is the game's Raw
# Ice (mining/mineableelements/ice_raw.xml, quantization_rawice.xml), as the
# rock view has it (gen_mining_hardware.py, BAND_STEM)
BAND_NAME = {"Pressurized Ice": "Raw Ice"}


def material_bands(name):
    return bands.get(_band_key(BAND_NAME.get(name, name))) or {}

# Which planets each of Pyro's four contract regions covers. Read out of
# missiondata/pu_missionlocality/pyro_regions/*.xml, whose location GUIDs
# resolve against the starmap records: region C is Pyro IV plus Pyro V and
# all six of its moons, region D is Terminus alone, and so on.
PYRO_REGION_BODIES = {
    "A": "Pyro I, Monox",
    "B": "Bloom",
    "C": "Pyro IV, Pyro V",
    "D": "Terminus",
}

# --- materials ------------------------------------------------------------
mats = {}
for b in blueprints:
    for ing in b.get("ingredients", []):
        entry = mats.setdefault(ing["name"], {"kind": ing["kind"], "count": 0})
        entry["count"] += 1
        if ing["kind"] == "resource":
            entry["kind"] = "resource"
material_names = sorted(mats)
material_index = {n: i for i, n in enumerate(material_names)}

# --- output types ---------------------------------------------------------
# The wiki's type_label is wrong for two families, and the blueprint key says
# so plainly: everything under "Docking Collar" is BP_CRAFT_Nozzle_FuelGiver_*
# (refuelling nozzles - the game has no craftable docking collar), and the
# four "Salvage Head" entries are BP_CRAFT_*_TractorBeam_* and belong with the
# other eight tractor beams. Checked against every key in the dump.
KEY_TYPE = [
    ("Nozzle_FuelGiver", "Fuel Nozzle"),
    ("TractorBeam", "Tractor Beam"),
]


def type_label(b):
    key = b.get("key") or ""
    for needle, label in KEY_TYPE:
        if needle in key:
            return label
    label = b["output"].get("type_label") or "Other"
    # One lone "Cargo" (a carryable mission item) reads better with the other
    # carryables under Misc than as a category of its own
    if label == "Cargo" and "Carryable" in key:
        return "Misc"
    return label


for b in blueprints:
    b["_type"] = type_label(b)
types = sorted({b["_type"] for b in blueprints})
type_index = {t: i for i, t in enumerate(types)}


# The item a recipe builds carries what a player filters by: how big it is,
# what grade it comes out at, and which class of component it is. None of
# that is on the blueprint, so it is pulled per item and joined here.
def item_for(uuid):
    path = os.path.join(item_dir, str(uuid) + ".json")
    if not uuid or not os.path.exists(path):
        return {}
    try:
        return json.load(io.open(path, encoding="utf-8"))
    except ValueError:
        return {}


def detail_for(uuid):
    path = os.path.join(detail_dir, uuid + ".json")
    if not os.path.exists(path):
        return None
    try:
        return json.load(io.open(path, encoding="utf-8"))
    except ValueError:
        return None


# --- slots ----------------------------------------------------------------
# A slot is one row at the bench: a named place, the material that goes in it
# and (usually) one stat that the material's quality moves.
slots = []
effects = []      # every value a slot moves; a slot points at a run of these
sources = []
pool_items = []
rows = []
missing_detail = 0
for b in sorted(blueprints, key=lambda x: (x["_type"], x["output_name"])):
    start = len(slots)
    detail = detail_for(b["uuid"])
    aspects = (detail or {}).get("aspects") or []

    if aspects:
        for asp in aspects:
            inp = asp.get("input") or {}
            name = inp.get("name")
            if name is None or name not in material_index:
                continue
            # EVERY modifier on the slot, not just modifiers[0]: a "Frame"
            # moves recoil smoothness, recoil handling and recoil kick, and
            # 1,521 of the game's 5,692 curves sit beyond the first.
            eff_start = len(effects)
            for mod in (asp.get("modifiers") or []):
                rng = mod.get("modifier_range") or {}
                qrng = mod.get("quality_range") or {}
                at_min = rng.get("at_min_quality")
                at_max = rng.get("at_max_quality")
                q_min = qrng.get("min")
                q_max = qrng.get("max")

                # Some modifiers give no straight range but a list of
                # segments, and 476 of them give neither. Measured over the
                # whole dump: the segmented ones are either the same straight
                # line by another name (0-500: 0.8->1.0, 501-1000: 1.0->1.2)
                # or flat at 1.0 - a stat the bench names but quality does not
                # move. A range still missing is taken as flat at 1.0; read as
                # zero it would show "x0.000  -100 %".
                segments = mod.get("value_segments") or []
                if (at_min is None or at_max is None) and segments:
                    at_min = segments[0].get("modifier_at_start")
                    at_max = segments[-1].get("modifier_at_end")
                    if q_min is None:
                        q_min = segments[0].get("quality_min")
                    if q_max is None:
                        q_max = segments[-1].get("quality_max")
                if at_min is None or at_max is None:
                    at_min = at_max = 1.0        # quality changes nothing here
                if q_min is None:
                    q_min = 0
                if not q_max:
                    q_max = 1000
                effects.append({
                    "stat": mod.get("label") or "",
                    "atMin": at_min,
                    "atMax": at_max,
                    # "higher"/"lower"/"neutral": whether a bigger number is
                    # the good one. Recoil and quantum fuel burn read the
                    # other way.
                    "better": (4 if mod.get("unreadable")
                               else {"higher": 0, "lower": 1}.get(mod.get("better_when"), 2)),
                    "prop": mod.get("property_key") or "",
                    "qMin": q_min,
                    "qMax": q_max,
                })
            slots.append({
                "slot": asp.get("name") or "",
                "material": material_index[name],
                "scu": inp.get("quantity_scu") or 0.0,
                "units": inp.get("quantity") or 0,
                "minQuality": inp.get("min_quality") or 1,
                "effFirst": eff_start,
                "effCount": len(effects) - eff_start,
            })
    else:
        # No detail on disk: fall back to the plain ingredient list, so the
        # recipe is still listed - only without its slot names and stats.
        missing_detail += 1
        for ing in b.get("ingredients", []):
            slots.append({
                "slot": "", "material": material_index[ing["name"]],
                "scu": ing.get("quantity_scu") or 0.0, "units": ing.get("quantity") or 0,
                "minQuality": 1, "effFirst": len(effects), "effCount": 0,
            })

    # Contracts that drop this blueprint, matched on the game's own tag
    entry = contracts.get(b.get("key") or "") or {}
    src_start = len(sources)
    for row in (entry.get("sources") or []):
        systems = row.get("systems") or []
        standing = (row.get("standing") or {})
        # Where it can be handed in. Pyro's four regions each have their own
        # reward pool, so "Pyro A" and "Pyro B" are not the same contract run.
        places = [x for x in (row.get("regions") or []) if x]
        systems_only = [x for x in places if x not in ("A", "B", "C", "D")] or systems
        regions_only = sorted(x for x in places if x in ("A", "B", "C", "D"))
        # Every system by name, never "Multi": one word for more than one system
        # would stand for three different sets - Pyro and Stanton (23 contracts),
        # all three (21), Nyx and Stanton (8) - and the app could only read it as
        # all three. Written out, "Stanton, Pyro" is read by the same substring
        # match that reads "Pyro: Bloom".
        order = {"Stanton": 0, "Pyro": 1, "Nyx": 2}
        where = ", ".join(sorted(set(systems_only), key=lambda x: order.get(x, 9)))
        if regions_only:
            # Name the planets, not the letter: "Pyro C/D" is no place on a map.
            # The regions ARE defined in the game files -
            # libs/foundry/records/missiondata/pu_missionlocality/pyro_regions
            # lists the locations of each - and resolved through the starmap
            # records they come out as whole planets:
            #   A  Pyro I and Monox        C  Pyro IV and Pyro V (with its
            #   B  Bloom                      six moons)
            #                              D  Terminus
            if len(regions_only) >= 4:
                where = (where or "Pyro") + ": everywhere"
            else:
                named = ", ".join(PYRO_REGION_BODIES.get(r, r) for r in regions_only)
                where = (where + ": " if where else "") + named
        brief, rep = briefing_and_rep(row.get("description"), row.get("reputation"))
        rep_outfit, rep_ladder, rep_rank, rep_highest, rep_gain = rep_fields(row.get("repNeed"),
                                                                            row.get("repAwardList"))
        pool_start = len(pool_items)
        # The whole pool: the files hold pools of up to eighteen, all of
        # them real blueprints. poolCount is a byte; eighteen fits.
        pool_items.extend([x for x in (row.get("poolItems") or []) if x])
        sources.append({
            "poolFirst": pool_start,
            "poolCount": len(pool_items) - pool_start,
            # some titles carry a trailing line break of their own
            "title": " ".join(game_text(row.get("title")).split()),
            "faction": row.get("faction") or "",
            # The card grows to fit, so the briefing goes in whole: there are
            # only 302 distinct ones and the linker pools the duplicates
            "description": brief[:3000],
            "minutes": int(row.get("minutes") or 0),
            "difficulty": int(row.get("difficulty") or 0),
            "chance": int(round((row.get("chance") or 1) * 100)),
            "poolSize": int(row.get("poolSize") or 0),
            "axes": row.get("axes") or {},
            # one figure in tenths, the wording behind it, and what the run puts up
            "score": int(round(float(row.get("score") or 0) * 10)),
            "risk": row.get("risk") or "",
            "ships": int(row.get("ships") or 0),
            # how many players the contract lets in; 1 means solo
            "party": int(row.get("groupMax") or 0),
            "illegal": bool(row.get("illegal")),
            "reputation": rep,
            "poolItems": [x for x in (row.get("poolItems") or []) if x],
            "cooldown": int(row.get("cooldown") or 0),
            "abandonCooldown": int(row.get("abandonCooldown") or 0),
            "onceOnly": bool(row.get("onceOnly")),
            "respawn": int(row.get("respawn") or 0),
            "respawnVar": int(row.get("respawnVar") or 0),
            "intro": bool(row.get("intro")),
            "otherPools": row.get("otherPools") or [],
            "system": where,
            "standing": standing.get("name") if isinstance(standing, dict) else "",
            "type": row.get("type") or "",
            "share": int(round((row.get("share") or 0) * 100)),
            "repFaction": rep_outfit,
            "repLadder": rep_ladder,
            "repRank": rep_rank,
            "repRankMax": rep_highest,
            "repGain": rep_gain,
        })

    item = item_for(b.get("output_item_uuid"))
    # "grade" on the blueprint is the recipe's own tier and is not what the
    # bench shows; the item's A-D grade is. Kept apart on purpose.
    grade = (item.get("grade") or "").strip().upper()
    if grade in ("NONE", "0"):
        grade = ""
    size = item.get("size")
    try:
        size = int(size)
    except (TypeError, ValueError):
        size = -1
    cls = (item.get("class") or "").strip()
    if cls.lower() in ("none", "null"):
        cls = ""
    rows.append({
        # a few item names carry a line break of their own, which shows up
        # in the list and breaks the match against the game log
        "name": " ".join(game_text(b["output_name"]).split()),
        "type": type_index[b["_type"]],
        "grade": grade,
        "size": size,
        "cls": cls,
        "maker": (item.get("manufacturer") or "").strip(),
        "seconds": int(b.get("craft_time_seconds") or 0),
        "start": start,
        "count": len(slots) - start,
        "default": bool(b.get("is_available_by_default")),
        # the wiki's own count, and the named ones from the game data
        "missions": int(entry.get("contracts") or b.get("unlocking_missions_count") or 0),
        "employers": int(entry.get("factions") or 0),
        "srcFirst": src_start,
        "srcCount": len(sources) - src_start,
    })

# --- two sanity passes over the modifiers ---------------------------------
# 1. Direction by majority per property. The dump disagrees with itself on
#    armor_damagemitigation: 865 slots say higher is better, 470 say neutral,
#    3 say lower - and those three are the same three with the impossible
#    factor below. One property cannot point both ways, so the majority wins
#    and the exceptions are counted rather than believed.
direction = collections.defaultdict(collections.Counter)
for sl in effects:
    # an unreadable curve votes on nothing
    if sl["stat"] and sl["prop"] and sl["better"] != 4:
        direction[sl["prop"]][sl["better"]] += 1
flipped = 0
for sl in effects:
    # 4 is not a direction but a missing curve, and the majority must not
    # overwrite it back into an ordinary slot
    if not (sl["stat"] and sl["prop"]) or sl["better"] == 4:
        continue
    votes = direction[sl["prop"]]
    # "neutral" is not an opinion about direction, so it does not get a vote
    opinions = {k: v for k, v in votes.items() if k in (0, 1)}
    if not opinions:
        continue
    winner = max(opinions, key=lambda k: opinions[k])
    if sl["better"] in (0, 1) and sl["better"] != winner:
        flipped += 1
    sl["better"] = winner

# 2. A factor no armour piece can carry. Every value of a property is between
#    0.8 and 1.2 except three that read 9.0 where 0.9 would fit the pattern -
#    a lost decimal point somewhere upstream. Guessing at the right number
#    would be inventing data, so the slot keeps its name and says the value
#    cannot be trusted (atMin == atMax marks it as "quality changes nothing").
IMPLAUSIBLE_HIGH = 3.0
IMPLAUSIBLE_LOW = 0.33
implausible = 0
for sl in effects:
    if not sl["stat"] or sl["better"] == 4:
        continue
    if sl["atMin"] > IMPLAUSIBLE_HIGH or sl["atMax"] > IMPLAUSIBLE_HIGH or \
       sl["atMin"] < IMPLAUSIBLE_LOW or sl["atMax"] < IMPLAUSIBLE_LOW:
        sl["atMin"] = sl["atMax"] = 1.0
        sl["better"] = 3          # marks "the game data here is not usable"
        implausible += 1

class_names = sorted({r["cls"] for r in rows if r["cls"]})
class_index = {c: i for i, c in enumerate(class_names)}
grade_letters = sorted({r["grade"] for r in rows if r["grade"]})
stat_names = sorted({e["stat"] for e in effects if e["stat"]})
stat_index = {s: i for i, s in enumerate(stat_names)}
slot_names = sorted({s["slot"] for s in slots if s["slot"]})
slot_index = {s: i for i, s in enumerate(slot_names)}


def esc(s):
    # A real line break cannot stand inside a C++ string literal, so it
    # becomes the escape - after the backslashes, or it would be doubled.
    return (s.replace("\\", "\\\\").replace('"', '\\"')
             .replace("\r\n", "\\n").replace("\n", "\\n").replace("\r", "\\n"))


with io.open(out, "w", encoding="utf-8", newline="\n") as f:
    w = f.write
    w("#pragma once\n")
    w("// Crafting blueprints - GENERATED by gen_crafting.py, do not edit by hand.\n")
    # Where the rows really come from: a build made through p4k_to_tables.py
    # has touched no service at all, so its header names the game files, not
    # the wiki API.
    from_game_files = any("game files" in v for v in versions)
    origin = ("the game files - Data.p4k, Game2.dcb, gen_blueprints_from_p4k.py"
              if from_game_files else "api.star-citizen.wiki/api/blueprints")
    w("// Source: %s (%s)\n" % (origin, ", ".join(versions)))
    w("// Pulled %s: %d blueprints, %d materials, %d slots, %d without detail.\n"
      % (datetime.date.today().isoformat(), len(rows), len(material_names), len(slots), missing_detail))
    w("//\n")
    w("// A blueprint is a list of SLOTS. Each slot takes one material and most\n")
    w("// of them move one stat of the finished item: the material's QUALITY,\n")
    w("// 1 to 1000, is mapped linearly onto a factor - 0.9 to 1.1 for a frame,\n")
    w("// say - so quality 500 leaves the stat at stock, 1000 gives the full\n")
    w("// bonus and 1 the full penalty. That mapping is per slot, not per\n")
    w("// material, and it is what the bench shows when you pick an ore.\n")
    w("#include <cstdint>\n#include <cstdlib>\n#include <string>\n#include <vector>\n\n")
    w("namespace SCMiner {\n\n")

    w("struct CraftMaterial {\n")
    w("    const char* name;\n")
    w("    const char* locationName;   // how the ore finder spells it\n")
    w("    bool asUnits;               // carried as units (gems), not bought in SCU\n")
    w("    int blueprints;             // how many blueprints ask for it\n")
    w("    const char* rarity;         // common .. legendary, \"\" when unknown\n")
    w("    // The eight quality values this material is mined at. A rock never\n")
    w("    // carries anything in between, so a slider that stops anywhere else\n")
    w("    // shows a number no material has. All zero when unknown.\n")
    w("    uint16_t bands[8];\n};\n\n")

    w("// One value a slot moves, and what quality does to it. A slot can move\n")
    w("// more than one: 997 of them move two and 262 move three.\n")
    w("struct CraftEffect {\n")
    w("    int16_t stat;        // index into CRAFT_STAT_NAMES, -1 when unnamed\n")
    w("    uint8_t better;      // 0 higher, 1 lower, 2 neither, 3 not usable, 4 unknown\n")
    w("    float atMin, atMax;  // the factor at qMin and at qMax\n")
    w("    uint16_t qMin, qMax; // the quality range the factors belong to\n};\n\n")

    w("struct CraftSlot {\n")
    w("    int16_t slot;        // index into CRAFT_SLOT_NAMES, -1 when unnamed\n")
    w("    uint8_t material;    // index into CRAFT_MATERIALS\n")
    w("    float scu;           // 0 when the slot is counted in units\n")
    w("    uint16_t units;      // 0 when the slot is measured in SCU\n")
    w("    uint16_t minQuality; // the bench refuses material below this\n")
    w("    uint16_t effFirst;   // first row in CRAFT_EFFECTS\n")
    w("    uint8_t effCount;    // how many values this slot moves, 0 = none\n};\n\n")

    w("// A contract that hands out a blueprint. Titles repeat across dozens of\n")
    w("// contracts (the game uses templates), so identical rows are folded.\n")
    w("struct CraftSource {\n")
    w("    const char* title;\n")
    w("    const char* faction;\n")
    w("    const char* system;    // where it runs - Pyro A/B, Stanton, ... - \"\" when unsaid\n")
    w("    const char* standing;  // the reputation rank it needs\n")
    w("    const char* type;      // Mercenary, Delivery, ...\n")
    w("    const char* briefing;  // what the contract asks for, as the game states it\n")
    w("    uint16_t minutes;      // time limit, 0 when it has none\n")
    w("    uint8_t difficulty;    // hardest of the four difficulty axes, 1..7, 0 unknown\n")
    w("    uint16_t cooldown;     // minutes blocked after completing it\n")
    w("    int16_t abandonCd;     // minutes blocked after abandoning, -1 = never again\n")
    w("    bool onceOnly;         // can only ever be done once\n")
    w("    uint8_t chance;        // the contract's own roll in %, 100 = always\n")
    w("    uint8_t poolSize;      // blueprints sharing this reward pool\n")
    w("    uint8_t skill, load, risk, know;   // the four difficulty axes, 1..7\n")
    w("    bool illegal;\n")
    w("    const char* reputation;   // what completing it awards: '100–200 Standing, 50 Affinity'\n")
    w("    uint8_t score;         // the four weighted the way the game does, in tenths\n")
    w("    const char* riskText;  // what the risk axis says in words\n")
    w("    uint8_t ships;         // enemy ships at once, 0 = the files do not say\n")
    w("    uint8_t party;         // players allowed, 0 = the files do not say\n")
    w("    uint16_t poolFirst;    // first entry in CRAFT_POOL_ITEMS\n")
    w("    uint8_t poolCount;     // how many of the pool's blueprints are named\n")
    w("    uint16_t respawn;      // minutes after a run ends until it is offered again, 0 = at once\n")
    w("    uint8_t respawnVar;    // ...give or take this many\n")
    w("    bool intro;            // the outfit's first rung: offered to newcomers, not to everyone\n")
    w("    // The other pools the same contract pays out of - the game's \"Pool 2\",\n")
    w("    // \"Pool 3\": entries of CRAFT_OTHER_POOLS, each a run of CRAFT_POOL_ITEMS\n")
    w("    uint16_t otherFirst;\n")
    w("    uint8_t otherCount;\n")
    w("    // The rank it is offered at, on one outfit's ladder (REP_FACTIONS,\n")
    w("    // REP_LADDERS), and what completing it adds there. repRank -1: it asks\n")
    w("    // for none; repFaction -1: the files tie it to no ladder at all.\n")
    w("    int16_t repFaction;\n")
    w("    int8_t repLadder;\n")
    w("    int8_t repRank, repRankMax;   // offered from repRank up to repRankMax\n")
    w("    int32_t repGain;\n};\n\n")
    w("// A run of CRAFT_POOL_ITEMS: one reward pool, by the blueprints it names\n")
    w("struct CraftPoolRun {\n")
    w("    uint16_t first;\n")
    w("    uint8_t count;\n};\n\n")

    w("// Reputation, the way the game keeps it: per outfit, on a ladder - the\n")
    w("// outfits' Standing (Neutral .. Elite Contractor), Hauling (Trainee ..\n")
    w("// Master), the guilds' own. A rank begins at its min.\n")
    w("struct RepRank {\n")
    w("    const char* name;\n")
    w("    int32_t min;\n")
    w("    bool gated;            // the game holds a player here until a contract of its own lets them past\n};\n\n")
    w("struct RepLadder {\n")
    w("    const char* name;      // what the game calls the reputation: \"Standing\", \"Hauling\"\n")
    w("    const char* guid;      // the scope's record - what a player's own setting is kept by\n")
    w("    uint16_t first;        // first rank in REP_RANKS\n")
    w("    uint8_t count;\n};\n\n")
    w("struct RepFaction {\n")
    w("    const char* name;\n")
    w("    const char* guid;\n};\n\n")
    w("struct RepAward {\n")
    w("    int16_t faction;\n")
    w("    int8_t ladder;\n")
    w("    int32_t amount;\n};\n\n")
    w("// What a gathering order wants brought: a mined material, spelled the way\n")
    w("// the signature table does where it is a ship's ore, and how much of it -\n")
    w("// in SCU, or by the piece for the gems of hand and ground mining\n")
    w("struct OrderOre {\n")
    w("    const char* name;\n")
    w("    uint16_t least, most;\n")
    w("    bool scu;\n};\n\n")
    w("// Every released contract, by the names Game.log gives it: \"generator/debug\"\n")
    w("// out of \"Creating objective marker: ... generator name [G], contract [D]\".\n")
    w("// Sorted by key, byte by byte.\n")
    w("struct RepContract {\n")
    w("    const char* key;\n")
    w("    int16_t faction;       // the rank it asks for: REP_FACTIONS, REP_LADDERS,\n")
    w("    int8_t ladder;         // the ladder's ranks; -1 when it asks for none\n")
    w("    int8_t rank, rankMax;\n")
    w("    uint16_t awardFirst;   // REP_AWARDS: what completing it awards\n")
    w("    uint8_t awardCount;\n")
    w("    const char* title;     // as CraftSource::title spells it\n")
    w("    uint16_t oreFirst;     // ORDER_ORES: what a gathering order wants brought\n")
    w("    uint8_t oreCount;      // 0: not an order\n")
    w("    bool oreChoice;        // any one of them will do\n};\n\n")

    w("struct CraftBlueprint {\n")
    w("    const char* name;\n")
    w("    uint8_t type;          // index into CRAFT_TYPES\n")
    w("    char grade;            // 'A'..'D', 0 for items the game gives no grade\n")
    w("    int8_t size;           // component size, -1 when the item has none\n")
    w("    int8_t itemClass;      // index into CRAFT_CLASSES, -1 when unclassed\n")
    w("    const char* maker;     // manufacturer code, \"\" when unknown\n")
    w("    uint16_t seconds;      // craft time\n")
    w("    uint16_t first;        // first slot in CRAFT_SLOTS\n")
    w("    uint8_t count;         // how many slots\n")
    w("    bool knownFromStart;   // no mission needed to unlock it\n")
    w("    uint8_t missions;      // contracts known to drop it\n")
    w("    uint16_t srcFirst;     // first row in CRAFT_SOURCES\n")
    w("    uint8_t srcCount;      // how many are named\n")
    w("    uint8_t employers;     // different outfits that hand it over\n};\n\n")

    w("inline const char* const CRAFT_TYPES[] = {\n")
    for t in types:
        w('    "%s",\n' % esc(t))
    w("};\n\n")

    w("inline const char* const CRAFT_SLOT_NAMES[] = {\n")
    for s in slot_names:
        w('    "%s",\n' % esc(s))
    w("};\n\n")

    w("// Component classes as the game labels them\n")
    w("inline const char* const CRAFT_CLASSES[] = {\n")
    for c in class_names:
        w('    "%s",\n' % esc(c))
    w("};\n\n")

    w("inline const char* const CRAFT_STAT_NAMES[] = {\n")
    for s in stat_names:
        w('    "%s",\n' % esc(s))
    w("};\n\n")

    w("inline const CraftMaterial CRAFT_MATERIALS[] = {\n")
    for n in material_names:
        m = mats[n]
        b = material_bands(n)
        row = (b.get("bands") or [0] * 8)[:8]
        row += [0] * (8 - len(row))
        w('    {"%s", "%s", %s, %d, "%s", {%s}},\n'
          % (esc(n), esc(LOCATION_NAME.get(n, n)), "false" if m["kind"] == "resource" else "true",
             m["count"], esc(b.get("rarity") or ""), ", ".join(str(int(x)) for x in row)))
    w("};\n\n")

    w("inline const CraftEffect CRAFT_EFFECTS[] = {\n")
    for e in effects:
        w("    {%d, %d, %.4ff, %.4ff, %d, %d},\n"
          % (stat_index.get(e["stat"], -1), e.get("better", 2), e["atMin"], e["atMax"],
             e["qMin"], e["qMax"]))
    w("};\n")
    w("inline constexpr int CRAFT_EFFECT_COUNT = %d;\n\n" % len(effects))

    w("inline const CraftSlot CRAFT_SLOTS[] = {\n")
    for s in slots:
        w("    {%d, %d, %.4ff, %d, %d, %d, %d},\n"
          % (slot_index.get(s["slot"], -1), s["material"], s["scu"], s["units"], s["minQuality"],
             s["effFirst"], s["effCount"]))
    w("};\n\n")

    # The other pools a row's contract pays out of, each pointed at a run
    # that is already in CRAFT_POOL_ITEMS as some other row's own pool - so
    # nothing is written twice. A pool no row owns (none of its blueprints
    # is named) gets a run of its own at the end.
    run_of = {}
    for src in sources:
        key = tuple(pool_items[src["poolFirst"]:src["poolFirst"] + src["poolCount"]])
        run_of.setdefault(key, (src["poolFirst"], src["poolCount"]))
    other_runs = []
    for src in sources:
        src["otherFirst"] = len(other_runs)
        for names in src.get("otherPools") or []:
            key = tuple(names)
            if key not in run_of:
                run_of[key] = (len(pool_items), len(names))
                pool_items.extend(names)
            other_runs.append(run_of[key])
        src["otherCount"] = len(other_runs) - src["otherFirst"]
    assert len(pool_items) < 65536 and len(other_runs) < 65536

    w("// What else the same reward pool can hand out\n")
    w("inline const char* const CRAFT_POOL_ITEMS[] = {\n")
    for item in pool_items:
        w('    "%s",\n' % esc(item))
    if not pool_items:
        w('    "",\n')
    w("};\n\n")

    w("// The other pools a contract pays out of - see CraftSource::otherFirst\n")
    w("inline const CraftPoolRun CRAFT_OTHER_POOLS[] = {\n")
    for first, count in other_runs:
        w("    {%d, %d},\n" % (first, count))
    if not other_runs:
        w("    {0, 0},\n")
    w("};\n\n")

    w("inline const CraftSource CRAFT_SOURCES[] = {\n")
    for src in sources:
        w('    {"%s", "%s", "%s", "%s", "%s", "%s", %d, %d, %d, %d, %s, %d, %d, '
          '%d, %d, %d, %d, %s, "%s", %d, "%s", %d, %d, %d, %d, %d, %d, %s, %d, %d, %d, %d, %d, %d, %d},\n'
          % (esc(src["title"]), esc(src["faction"]), esc(src["system"]),
             esc(src["standing"] or ""), esc(src["type"]), esc(src.get("description") or ""),
             min(65535, src.get("minutes") or 0), min(7, src.get("difficulty") or 0),
             min(65535, max(0, src.get("cooldown") or 0)),
             max(-1, min(32767, src.get("abandonCooldown") or 0)),
             "true" if src.get("onceOnly") else "false",
             min(100, src.get("chance") or 100), min(255, src.get("poolSize") or 0),
             min(7, (src.get("axes") or {}).get("mechanicalSkill", 0)),
             min(7, (src.get("axes") or {}).get("mentalLoad", 0)),
             min(7, (src.get("axes") or {}).get("riskOfLoss", 0)),
             min(7, (src.get("axes") or {}).get("gameKnowledge", 0)),
             "true" if src.get("illegal") else "false", esc(src.get("reputation") or ""),
             min(255, src.get("score") or 0), esc(src.get("risk") or ""),
             min(255, src.get("ships") or 0), min(255, src.get("party") or 0),
             src.get("poolFirst", 0), min(255, src.get("poolCount", 0)),
             min(65535, src.get("respawn") or 0), min(255, src.get("respawnVar") or 0),
             "true" if src.get("intro") else "false", src["otherFirst"], min(255, src["otherCount"]),
             src.get("repFaction", -1), src.get("repLadder", -1), src.get("repRank", -1),
             src.get("repRankMax", -1), src.get("repGain", 0)))
    w("};\n\n")

    # The reputation tables (see RepContract)
    rep_ranks, rep_awards, rep_rows = [], [], []
    for s in ladder_order:
        first = len(rep_ranks)
        for r in rep_ladders[s]["ranks"]:
            rep_ranks.append(r)
        rep_ladders[s]["first"] = first
    # ...and the gathering orders' materials. The records say Aluminum and
    # Pressurized Ice; the signature table, which a rock is matched against,
    # says Aluminium and Ice.
    order_names = LOCATION_NAME
    order_ores = []
    for c in reputation.get("contracts", []):
        outfit, ladder, rank, highest, _gain = rep_fields(c.get("need"), [])
        first = len(rep_awards)
        for f, s, a in c.get("awards") or []:
            if f in outfit_index and s in ladder_index:
                rep_awards.append((outfit_index[f], ladder_index[s], a))
        order = c.get("order") or {}
        ore_first = len(order_ores)
        for name, least, most, unit in order.get("rows", []):
            order_ores.append((order_names.get(name, name), min(65535, least), min(65535, most), unit == "SCU"))
        # every contract, with or without a rank: the planner tells by the
        # title which mission of its list was just finished
        rep_rows.append((c["key"], outfit, ladder, rank, highest, first, len(rep_awards) - first,
                         " ".join(game_text(c.get("title") or "").split()), ore_first,
                         len(order_ores) - ore_first, bool(order.get("choice"))))
    rep_rows.sort(key=lambda r: r[0].encode("utf-8"))
    assert len(rep_ranks) < 65536 and len(rep_awards) < 65536 and len(order_ores) < 65536

    w("inline const RepRank REP_RANKS[] = {\n")
    for r in rep_ranks:
        w('    {"%s", %d, %s},\n' % (esc(r["name"] or str(r["min"])), r["min"], "true" if r["gated"] else "false"))
    if not rep_ranks:
        w('    {"", 0, false},\n')
    w("};\n\n")
    w("inline const RepLadder REP_LADDERS[] = {\n")
    for s in ladder_order:
        w('    {"%s", "%s", %d, %d},\n' % (esc(rep_ladders[s]["name"]), s, rep_ladders[s]["first"],
                                         len(rep_ladders[s]["ranks"])))
    if not ladder_order:
        w('    {"", "", 0, 0},\n')
    w("};\n\n")
    w("inline const RepFaction REP_FACTIONS[] = {\n")
    for f in outfit_order:
        w('    {"%s", "%s"},\n' % (esc(rep_outfits[f]), f))
    if not outfit_order:
        w('    {"", ""},\n')
    w("};\n\n")
    w("inline const RepAward REP_AWARDS[] = {\n")
    for f, s, a in rep_awards:
        w("    {%d, %d, %d},\n" % (f, s, a))
    if not rep_awards:
        w("    {0, 0, 0},\n")
    w("};\n\n")
    w("inline const OrderOre ORDER_ORES[] = {\n")
    for name, least, most, scu in order_ores:
        w('    {"%s", %d, %d, %s},\n' % (esc(name), least, most, "true" if scu else "false"))
    if not order_ores:
        w('    {"", 0, 0, false},\n')
    w("};\n\n")
    w("inline const RepContract REP_CONTRACTS[] = {\n")
    for key, f, s, rank, highest, first, count, title, ore_first, ore_count, choice in rep_rows:
        w('    {"%s", %d, %d, %d, %d, %d, %d, "%s", %d, %d, %s},\n'
          % (esc(key), f, s, rank, highest, first, count, esc(title), ore_first, min(255, ore_count),
             "true" if choice else "false"))
    if not rep_rows:
        w('    {"", -1, -1, -1, -1, 0, 0, "", 0, 0, false},\n')
    w("};\n\n")

    w("inline const CraftBlueprint CRAFT_BLUEPRINTS[] = {\n")
    for r in rows:
        w('    {"%s", %d, \'%s\', %d, %d, "%s", %d, %d, %d, %s, %d, %d, %d, %d},\n'
          % (esc(r["name"]), r["type"], r["grade"] if r["grade"] else "\\0",
             r["size"], class_index.get(r["cls"], -1), esc(r["maker"]),
             min(65535, r["seconds"]), r["start"], r["count"],
             "true" if r["default"] else "false", min(255, r["missions"]),
             r["srcFirst"], r["srcCount"], min(255, r["employers"])))
    w("};\n\n")

    w("inline constexpr int CRAFT_TYPE_COUNT = %d;\n" % len(types))
    w("inline constexpr int CRAFT_SLOT_NAME_COUNT = %d;\n" % len(slot_names))
    w("inline constexpr int CRAFT_STAT_COUNT = %d;\n" % len(stat_names))
    w("inline constexpr int CRAFT_CLASS_COUNT = %d;\n" % len(class_names))
    w("inline constexpr int CRAFT_MATERIAL_COUNT = %d;\n" % len(material_names))
    w("inline constexpr int CRAFT_BLUEPRINT_COUNT = %d;\n" % len(rows))
    w("inline constexpr int CRAFT_SLOT_COUNT = %d;\n" % len(slots))
    w("inline constexpr int CRAFT_SOURCE_COUNT = %d;\n" % len(sources))
    w("inline constexpr int CRAFT_POOL_ITEM_COUNT = %d;\n" % len(pool_items))
    w("inline constexpr int CRAFT_OTHER_POOL_COUNT = %d;\n" % len(other_runs))
    w("inline constexpr int REP_RANK_COUNT = %d;\n" % len(rep_ranks))
    w("inline constexpr int REP_LADDER_COUNT = %d;\n" % len(ladder_order))
    w("inline constexpr int REP_FACTION_COUNT = %d;\n" % len(outfit_order))
    w("inline constexpr int REP_AWARD_COUNT = %d;\n" % len(rep_awards))
    w("inline constexpr int REP_CONTRACT_COUNT = %d;\n" % len(rep_rows))
    w("inline constexpr int ORDER_ORE_COUNT = %d;\n" % len(order_ores))
    w('inline constexpr const char* CRAFT_DATA_VERSION = "%s";\n\n' % esc(versions[0] if versions else "unknown"))

    w("""// The band a quality value falls in, 1..8, or 0 when the material has none
// on file. A band is named by its top value: Iron's fourth band IS 710.
inline int materialBand(const CraftMaterial& mat, int quality) {
    if (!mat.bands[0]) return 0;
    for (int i = 0; i < 8; i++) {
        if (quality <= (int)mat.bands[i]) return i + 1;
    }
    return 8;
}

// The band value nearest to this quality - what a slider should land on
inline int snapToBand(const CraftMaterial& mat, int quality) {
    if (!mat.bands[0]) return quality;
    int best = mat.bands[0], bestGap = 1 << 30;
    for (int i = 0; i < 8; i++) {
        if (!mat.bands[i]) continue;
        const int gap = std::abs((int)mat.bands[i] - quality);
        if (gap < bestGap) { bestGap = gap; best = mat.bands[i]; }
    }
    return best;
}

// Quality: what the bench calls Q. Mined material carries a value from 1 to
// 1000, handed out in eight bands per material since the 4.8 mining rework.
// The band EDGES are not in any published data, so nothing here invents them.
inline constexpr int CRAFT_QUALITY_MIN = 1;
inline constexpr int CRAFT_QUALITY_BASE = 500;      // stats stay at stock
inline constexpr int CRAFT_QUALITY_MAX = 1000;      // full bonus
inline constexpr int CRAFT_QUALITY_BANDS = 8;

// A slot whose two ends are the same moves nothing: the game names the stat,
// but quality does not change it. Measured on this dump: 621 of the slots.
inline bool effectMovesStat(const CraftEffect& e) {
    return e.stat >= 0 && e.atMin != e.atMax;
}

// Whether a change is an improvement. Recoil, quantum fuel burn and one
// damage-mitigation figure count DOWN as better - 305 slots here - and
// painting those green for going up says the opposite of the truth.
// A slot the game data cannot be trusted on: three armour slots carry a
// factor of 9.0 where every other value of that property sits between 0.8
// and 1.15. Named, but shown as unusable rather than as a number.
inline bool effectDataBroken(const CraftEffect& e) { return e.better == 3; }

// A slot whose curve is in the game files but could not be read out of them:
// the extractor fails on 476 of these arrays. The stat is named and the
// quality does move it - by how much is simply not known here, and saying
// "any quality will do" would be the one answer that is certainly wrong.
inline bool effectCurveUnknown(const CraftEffect& e) { return e.better == 4; }

inline bool changeIsGood(const CraftEffect& e, double factor) {
    if (e.better == 1) return factor < 1.0;
    if (e.better == 0) return factor > 1.0;
    return false;
}

inline bool changeIsBad(const CraftEffect& e, double factor) {
    if (e.better == 1) return factor > 1.0;
    if (e.better == 0) return factor < 1.0;
    return false;
}

// The factor a slot applies at this material quality. Linear between the two
// ends the game gives, and clamped outside them: quality 500 of a 0.9-1.1
// slot returns 1.0, which is the stock value.
inline double effectFactor(const CraftEffect& e, int quality) {
    if (e.stat < 0 || e.qMax <= e.qMin) return 1.0;
    if (quality < (int)e.qMin) quality = e.qMin;
    if (quality > (int)e.qMax) quality = e.qMax;
    const double t = double(quality - e.qMin) / double(e.qMax - e.qMin);
    return e.atMin + (e.atMax - e.atMin) * t;
}

// Blueprints that ask for this material
inline std::vector<int> blueprintsUsing(int material) {
    std::vector<int> hits;
    for (int b = 0; b < CRAFT_BLUEPRINT_COUNT; b++) {
        const CraftBlueprint& bp = CRAFT_BLUEPRINTS[b];
        for (int s = 0; s < bp.count; s++) {
            if (CRAFT_SLOTS[bp.first + s].material == material) { hits.push_back(b); break; }
        }
    }
    return hits;
}

// Material index by name, -1 when the table does not carry it
inline int materialIndex(const std::string& name) {
    for (int i = 0; i < CRAFT_MATERIAL_COUNT; i++) {
        if (name == CRAFT_MATERIALS[i].name) return i;
    }
    return -1;
}

} // namespace SCMiner
""")

sized = sum(1 for r in rows if r["size"] > 0)
graded = sum(1 for r in rows if r["grade"])
classed = sum(1 for r in rows if r["cls"])
print("wrote %s: %d blueprints, %d materials, %d slots (%d named), %d stats, %d without detail, version %s"
      % (out, len(rows), len(material_names), len(slots), len(slot_names), len(stat_names),
         missing_detail, versions))
print("  sizes on %d, grades on %d (%s), classes on %d (%s)"
      % (sized, graded, "/".join(grade_letters), classed, ", ".join(class_names)))
lower = sum(1 for e in effects if e.get("better") == 1)
flat = sum(1 for e in effects if e["stat"] and e["atMin"] == e["atMax"])
print("  %d values moved across %d slots (%d slots move more than one)"
      % (len(effects), len(slots), sum(1 for s in slots if s["effCount"] > 1)))
print("  where lower is better: %d, where quality changes nothing: %d" % (lower, flat))
with_bands = sum(1 for n in material_names if material_bands(n).get("bands"))
print("  materials with quality bands: %d of %d" % (with_bands, len(material_names)))
named = sum(1 for r in rows if r["srcCount"])
print("  blueprints with named contracts: %d, source rows: %d" % (named, len(sources)))
print("  direction flipped to the property majority: %d, implausible factors dropped: %d"
      % (flipped, implausible))
print("  reputation: %d ladders, %d outfits, %d contracts; source rows with a rank: %d, on a ladder: %d"
      % (len(ladder_order), len(outfit_order), len(reputation.get("contracts", [])),
         sum(1 for s in sources if s.get("repRank", -1) >= 0), sum(1 for s in sources if s.get("repLadder", -1) >= 0)))
print("  gathering orders: %d, naming %d materials"
      % (sum(1 for c in reputation.get("contracts", []) if c.get("order")),
         len({r[0] for c in reputation.get("contracts", []) if c.get("order") for r in c["order"]["rows"]})))
