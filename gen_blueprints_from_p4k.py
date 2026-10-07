"""Builds the whole crafting table out of the game files - recipes, slots,
stat curves, craft times and the item each one makes.

    gen_blueprints_from_p4k.py <extract-dir> <global.ini> <resource_guids.json> \\
                               <blueprints.json> [reference.json]

Everything comes from the unpacked game database (Data.p4k -> Game2.dcb ->
unforge). The recipe tree is nested rather than flat, so it is walked with a
real XML parser:

  CraftingBlueprintRecord.BP_CRAFT_xyz  __ref=BP-GUID
    blueprint/CraftingBlueprint
      processSpecificData/CraftingProcess_Creation  @entityClass -> the item
      tiers/CraftingBlueprintTier/recipe/CraftingRecipe/costs/CraftingRecipeCosts
        craftTime/TimeValue_Partitioned              @days @hours @minutes @seconds
        mandatoryCost/CraftingCost_Select            the "ASPECTS" group
          options/CraftingCost_Select                one per slot
            context/.../CraftingGameplayPropertyModifierCommon @gameplayPropertyRecord
              valueRanges/..._Linear                 @startQuality @endQuality
                                                     @modifierAtStart @modifierAtEnd
            nameInfo                                 @debugName @displayName
            options/CraftingCost_Resource            @resource @minQuality
              quantity/SStandardCargoUnit            @standardCargoUnits

Item names, slot names and stat names are localisation keys, resolved through
Data/Localization/english/global.ini.

The one thing the database does not carry is which material a resource GUID
means - those live server-side - so resource_guids.json holds that mapping.
It is 37 lines, it lives in this repository, and GUIDs do not change between
patches.
"""
import io, json, os, re, sys, xml.etree.ElementTree as ET

extract, ini_path, guid_file, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
reference = sys.argv[5] if len(sys.argv) > 5 else None
records = os.path.join(extract, "Data", "libs", "foundry", "records")


def walk(sub):
    for base, _dirs, files in os.walk(os.path.join(records, sub)):
        for f in files:
            if f.lower().endswith(".xml"):
                yield os.path.join(base, f)


def parse(path):
    try:
        return ET.parse(path).getroot()
    except (ET.ParseError, OSError):
        return None


# ---- names ---------------------------------------------------------------
loc = {}
# Keys in global.ini sometimes carry a suffix after a comma
# (StatName_GPP_Armor_RadiationCapacity,P=Radiation Capacity), so both the
# plain and the suffixed form are stored - 24 of 29 stat names hid behind it.
for line in io.open(ini_path, encoding="utf-8", errors="replace"):
    k, sep, v = line.partition("=")
    if sep and "," in k:
        loc.setdefault(k.split(",")[0].strip().lower(), v.strip())
    if sep:
        loc[k.strip().lower()] = v.strip()


# Item names in the files carry the game's own component prefix:
# "Cmp/1/C IcePlunge" is size 1, grade C. Size and grade are columns of their
# own here, so the prefix only makes the list harder to read.
PREFIX = re.compile(r"^[A-Za-z]{2,5}/[\dSs]+/[A-Da-d]\s+")


def name_of(key, fallback=""):
    if not key:
        return fallback
    raw = loc.get(str(key).lstrip("@").lower())
    if not raw:
        return fallback
    raw = re.sub(r"<[^>]+>", "", raw)
    raw = raw.replace(" ", " ")          # the files use hard spaces in places
    raw = " ".join(raw.split()).strip()
    return PREFIX.sub("", raw) or fallback


resources = json.load(io.open(guid_file, encoding="utf-8"))

# ---- what each stat is called -------------------------------------------
properties = {}
for path in walk(os.path.join("crafting", "craftedproperties")):
    root = parse(path)
    if root is None:
        continue
    properties[root.get("__ref")] = name_of(root.get("propertyName"), "")
print(f"{len(properties)} crafted properties")

# ---- manufacturers -------------------------------------------------------
makers = {}
for sub in ("scitemmanufacturer", "manufacturers"):
    for path in walk(sub):
        root = parse(path)
        if root is None:
            continue
        code = root.get("Code") or root.get("code")
        if not code:
            # <SCItemManufacturer.AMRS ...> - the tag carries the code
            m = re.match(r"[\w]+\.([\w]+)$", root.tag)
            code = m.group(1).upper() if m else None
        if root.get("__ref") and code:
            makers.setdefault(root.get("__ref"), code)
print(f"{len(makers)} manufacturers")

# ---- the items recipes produce ------------------------------------------
items = {}
for path in walk(os.path.join("entities", "scitem")):
    root = parse(path)
    if root is None or not root.get("__ref"):
        continue
    attach = root.find(".//AttachDef")
    localis = root.find(".//Localization")
    if attach is None and localis is None:
        continue
    size = attach.get("Size") if attach is not None else None
    items[root.get("__ref")] = {
        "type": (attach.get("Type") if attach is not None else "") or "",
        "subType": (attach.get("SubType") if attach is not None else "") or "",
        "size": int(size) if size and size.lstrip("-").isdigit() else -1,
        "grade": (attach.get("Grade") if attach is not None else "") or "",
        "maker": (attach.get("Manufacturer") if attach is not None else "") or "",
        # The short name is what the game shows in lists
        "name": name_of(localis.get("Name") if localis is not None else "", ""),
    }
print(f"{len(items)} items")


# ---- the recipes ---------------------------------------------------------
def linears_of(node):
    """EVERY stat curve on a slot, not just the first.

    A slot can move more than one value: in 4.10, 997 of them move two and
    262 move three - 1,521 of the 5,695 curves in the game files. This used
    to return inside the loop, so all but the first were dropped before
    anything was written down, and a "Frame" slot that moves Recoil
    Smoothness, Recoil Handling and Recoil Kick showed only the first.

    A modifier is usually written as TWO ranges - 0..500 running 0.8 to 1.0,
    then 501..1000 running 1.0 to 1.2 - so taking only the first range left
    every slider stuck at stock above quality 500. The pieces of ONE modifier
    are joined into one line from the first start to the last end."""
    out = []
    for common in node.iter("CraftingGameplayPropertyModifierCommon"):
        prop = common.get("gameplayPropertyRecord")
        ranges = common.findall(".//CraftingGameplayPropertyModifierValueRange_Linear")
        if not ranges:
            # unforge writes its exception into the XML where the numbers
            # should be. That is not "this slot does nothing" - it is "this
            # slot could not be read", and the two must not look alike.
            if any("Error reading array property" in (vr.get("value") or "")
                   for vr in common.iter("valueRanges")):
                out.append((prop, "UNREAD"))
            else:
                out.append((prop, None))
            continue
        ranges.sort(key=lambda r: float(r.get("startQuality") or 0))
        first, last = ranges[0], ranges[-1]
        out.append((prop, (float(first.get("startQuality") or 0),
                           float(last.get("endQuality") or 1000),
                           float(first.get("modifierAtStart") or 1),
                           float(last.get("modifierAtEnd") or 1))))
    return out


def input_of(node):
    """What goes into a slot: a resource in SCU, or an item by the piece."""
    for res in node.iter("CraftingCost_Resource"):
        scu = res.find(".//SStandardCargoUnit")
        return {
            "guid": res.get("resource") or "",
            "scu": float(scu.get("standardCargoUnits")) if scu is not None else 0.0,
            "units": 0,
            "minQuality": int(res.get("minQuality") or 1),
        }
    for tag in ("CraftingCost_Item", "CraftingCost_ItemRecord", "CraftingCost_Entity"):
        for itm in node.iter(tag):
            # <CraftingCost_Item entityClass="..." quantity="7" minQuality="1"/>
            # The attribute is "quantity"; reading "count" instead would make every one
            # of these slots ask for a single piece.
            count = itm.find(".//SItemCount")
            n = (itm.get("quantity") or itm.get("count") or
                 (count.get("count") if count is not None else None))
            guid = (itm.get("entityClass") or itm.get("item") or itm.get("itemRecord") or
                    itm.get("resource") or "")
            return {"guid": guid, "scu": 0.0,
                    "units": int(float(n)) if n and str(n).replace(".", "").isdigit() else 1,
                    "minQuality": int(itm.get("minQuality") or 1)}
    return None


blueprints = []
for path in walk(os.path.join("crafting", "blueprints")):
    root = parse(path)
    if root is None:
        continue
    m = re.match(r"CraftingBlueprintRecord\.(BP_CRAFT_[\w]+)$", root.tag)
    if not m:
        continue
    creation = root.find(".//CraftingProcess_Creation")
    entity = creation.get("entityClass") if creation is not None else None

    seconds = 0
    t = root.find(".//craftTime/TimeValue_Partitioned")
    if t is not None:
        d, h, mi, s = (int(t.get(k) or 0) for k in ("days", "hours", "minutes", "seconds"))
        seconds = ((d * 24 + h) * 60 + mi) * 60 + s

    # The slots sit one level under the mandatory "ASPECTS" group
    slots = []
    for group in root.iter("mandatoryCost"):
        for outer in group.findall("./CraftingCost_Select"):
            for slot in outer.findall("./options/CraftingCost_Select"):
                info = slot.find("./nameInfo")
                slot_name = ""
                if info is not None:
                    slot_name = name_of(info.get("displayName"),
                                        (info.get("debugName") or "").title())
                effects = []
                for prop, lin in (linears_of(slot) or [(None, None)]):
                    unread = (lin == "UNREAD")
                    if unread:
                        lin = None
                    effects.append({
                        "prop": prop or "",
                        "unreadable": unread,
                        "stat": properties.get(prop, "") if prop else "",
                        "qMin": int(lin[0]) if lin else 0,
                        "qMax": int(lin[1]) if lin else 1000,
                        "atMin": lin[2] if lin else 1.0,
                        "atMax": lin[3] if lin else 1.0,
                    })
                inp = input_of(slot)
                if not inp or not inp["guid"]:
                    continue
                slots.append({
                    "slot": slot_name,
                    # the raw name, so an unreadable curve can be matched
                    # against the same slot on another blueprint
                    "slotKey": (info.get("debugName") or "") if info is not None else "",
                    "material": resources.get(inp["guid"], ""),
                    "materialGuid": inp["guid"],
                    "scu": inp["scu"],
                    "units": inp["units"],
                    "minQuality": inp["minQuality"],
                    # every value this slot moves; the flat fields below are
                    # the first of them, kept for anything that reads those
                    "effects": effects,
                    "prop": effects[0]["prop"],
                    "unreadable": effects[0]["unreadable"],
                    "stat": effects[0]["stat"],
                    "qMin": effects[0]["qMin"],
                    "qMax": effects[0]["qMax"],
                    "atMin": effects[0]["atMin"],
                    "atMax": effects[0]["atMax"],
                })
        break   # the first mandatory group is the recipe; later tiers repeat it

    item = items.get(entity) or {}
    blueprints.append({
        "tag": m.group(1),
        "guid": root.get("__ref"),
        "seconds": seconds,
        "slots": slots,
        "name": item.get("name") or "",
        "type": item.get("type") or "",
        "subType": item.get("subType") or "",
        "size": item.get("size", -1),
        "grade": item.get("grade") or "",
        "maker": makers.get(item.get("maker") or "", ""),
    })

# ---- the curves unforge could not read ----------------------------------
# Same slot name, same gameplay property, one single curve across every
# blueprint that does read: then it is that curve. Anything less than
# unanimous stays unknown - a plausible number is still an invented one.
twins = {}
for b in blueprints:
    for s in b["slots"]:
        for e in s["effects"]:
            if e["unreadable"] or not e["prop"] or not e["stat"]:
                continue
            twins.setdefault((s["slotKey"], e["prop"]), set()).add(
                (e["qMin"], e["qMax"], e["atMin"], e["atMax"]))
filled = unknown = 0
for b in blueprints:
    for s in b["slots"]:
        for e in s["effects"]:
            if not e["unreadable"]:
                continue
            seen = twins.get((s["slotKey"], e["prop"]))
            if seen and len(seen) == 1:
                e["qMin"], e["qMax"], e["atMin"], e["atMax"] = next(iter(seen))
                e["unreadable"] = False
                e["fromTwin"] = True
                filled += 1
            else:
                unknown += 1
        # the flat fields follow the first effect wherever it went
        s["unreadable"] = s["effects"][0]["unreadable"]
        s["qMin"] = s["effects"][0]["qMin"]
        s["qMax"] = s["effects"][0]["qMax"]
        s["atMin"] = s["effects"][0]["atMin"]
        s["atMax"] = s["effects"][0]["atMax"]
        if s["effects"][0].get("fromTwin"):
            s["fromTwin"] = True
print(f"unreadable curves: {filled} recovered from an identical slot, "
      f"{unknown} left unknown")

named = sum(1 for b in blueprints if b["name"])
slot_rows = sum(len(b["slots"]) for b in blueprints)
with_stat = sum(1 for b in blueprints for s in b["slots"] if s["stat"])
no_material = sum(1 for b in blueprints for s in b["slots"] if not s["material"])
print(f"{len(blueprints)} recipes, {named} named, {slot_rows} slots "
      f"({with_stat} with a stat, {no_material} without a known material)")

json.dump(blueprints, io.open(out, "w", encoding="utf-8"), indent=1)
print("wrote", out)

if reference and os.path.exists(reference):
    ref = {b["key"]: b for b in json.load(io.open(reference, encoding="utf-8")) if b.get("key")}
    mine = {b["tag"]: b for b in blueprints}
    both = set(ref) & set(mine)
    same_name = sum(1 for t in both if (ref[t].get("output_name") or "") == mine[t]["name"])
    same_time = sum(1 for t in both if (ref[t].get("craft_time_seconds") or 0) == mine[t]["seconds"])
    same_slots = sum(1 for t in both if len(ref[t].get("ingredients") or []) == len(mine[t]["slots"]))
    same_mats = 0
    for t in both:
        a = [i.get("name") for i in (ref[t].get("ingredients") or [])]
        b = [s["material"] for s in mine[t]["slots"]]
        if a == b:
            same_mats += 1
    print(f"against the reference: {len(both)} in both, {same_name} same name, {same_time} same craft time, "
          f"{same_slots} same slot count, {same_mats} identical material lists "
          f"({len(set(ref) - set(mine))} only there, {len(set(mine) - set(ref))} only here)")
    bad = [t for t in sorted(both) if (ref[t].get("output_name") or "") != mine[t]["name"]][:3]
    for t in bad:
        print(f"  name differs {t}: mine {mine[t]['name']!r} / ref {ref[t].get('output_name')!r}")
