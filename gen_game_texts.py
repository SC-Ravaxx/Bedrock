"""Generates include/GameTextMarks.h: what Bedrock adds to the game's own
contract texts when the player asks for it - the missions that pay out
blueprints marked, their blueprints listed, what the player still lacks in
blue and what they own plain. GameTexts.cpp puts it together with the game's global.ini at
run time; this script decides everything that does not depend on the player.

    gen_game_texts.py <global.ini> <bp_contract_keys.json> <all_contract_keys.json>
                      <bp.json> include/CraftingData.h include/GameTextMarks.h

The two json files come out of gen_contracts_from_p4k.py (BP_CONTRACT_KEYS,
ALL_CONTRACT_KEYS), bp.json out of the blueprint chain; global.ini is the one
of the same game build, for the spelling of its keys.

What gets added, and only this, at the end of the game's values:
  - a mission TITLE that pays out blueprints gets  <EM4>[BP]</EM4>, or
    <EM4>[BP*]</EM4> when the same title is also used by variants that pay
    out none; plain instead of blue once every blueprint it pays is owned
  - its DESCRIPTION gets the list, below an empty line, one blueprint per
    line - the mobiGlas panel holds about sixty characters, and a line of
    several names would break in the middle of one:
        BLUEPRINTS · depends on the region

        Pyro I · Monox · one of 7 · 6 owned
        • 9-Series Longsword Cannon S1
    Groups are named by what sets them apart in the game data: the Pyro
    region (in planet names, the way CraftingData's system column has them),
    the system, the title, the rank, the ore or item in the contract's name,
    the outfit, or the pool's own name (Easy / Medium / Hard+, Rank 0–1)
  - a variant that pays nothing under a [BP*] title says so, and which
    variant does ('They Will Be Missed': the Neutral contract is not the
    Veteran one)

What depends on the player is left as a token for GameTexts.cpp to fill in:
    {S}                   a space, unless the game's value ends with one
    {I<b>|text}           a list line - blue while blueprint b is missing,
                          plain once owned (b = -1: no row, always missing)
    {O<n>:<b,b,...>}      ' · 3 owned' / ' · all owned' of n lines, or nothing
    {A<n>:<b,b,...>}      ' · all owned' when all n are, or nothing
    {T<n>:<b,b,...>|tag}  the title's tag, blue - or plain when all n are owned
b is the row of CRAFT_BLUEPRINTS whose name BlueprintLibrary counts as the one
owned (the first row of that name); a line with no row has no token and
counts as not owned. The literal text never holds a brace - checked below.

Markup: <EM4> is the highlight the game's English text uses (3,567 times,
light blue in the mobiGlas) - the one colour there is. Tried in the game:
of <EM0> to <EM9> only EM4 (blue) and EM3 (underlined) show;
<b>, <i>, <u>, <span style=color>, <color=> and <font color=...> print as
raw text, tags and all; the font has no tick, cross, dot, star or arrow
beyond the bullets and dashes the game's own text uses. Strikethrough
exists nowhere in the game's text styles. Bullets and dashes are characters
the game's own text uses.
"""
import io
import json
import re
import sys
from collections import OrderedDict, defaultdict

ini_path, bp_path, all_path, names_path, crafting_path, out_path = sys.argv[1:7]

NL = r"\n"   # a line break inside a global.ini value is the two characters \ and n


# --- the game's file, for the spelling of its keys ----------------------------
def read_ini(path):
    raw = open(path, "rb").read()
    text = (raw[3:] if raw.startswith(b"\xef\xbb\xbf") else raw).decode("utf-8")
    values = OrderedDict()
    for line in text.split("\r\n" if "\r\n" in text else "\n"):
        key, sep, value = line.partition("=")
        if sep:
            values[key] = value
    return values


game = read_ini(ini_path)
lower = {k.lower(): k for k in game}


def real_key(key):
    """The spelling the file uses for a key the contract files name: case and
    the ',P' some keys carry differ between the two"""
    if not key:
        return ""
    return lower.get(key.lower()) or lower.get((key + ",P").lower()) or ""


# --- what the game data says ------------------------------------------------
bp_records = json.load(open(bp_path, encoding="utf-8"))
all_records = [r for r in json.load(open(all_path, encoding="utf-8")) if not r.get("unreleased")]
blueprints = {b["tag"]: b for b in json.load(open(names_path, encoding="utf-8"))}

usage = defaultdict(lambda: {"bp": 0, "none": 0})   # per title or description key
for r in all_records:
    for field in ("titleKey", "descKey"):
        k = real_key(r[field])
        if k:
            usage[k]["bp" if r["bp"] else "none"] += 1


# --- which CRAFT_BLUEPRINTS row stands for a blueprint -------------------------
def normalise(raw):
    """BlueprintLibrary::normalise: markup and [BP] gone, one case, letters and
    digits only, one space; 'Ind/1/B Guardian' is 'guardian'"""
    s = re.sub(r"<[^>]*>", "", raw).replace("[BP]", "")
    out = " ".join(re.findall(r"[0-9A-Za-z]+", s)).lower()
    if (len(out) > 8 and out[:3].isalpha() and out[3] == " " and out[4].isdigit() and out[5] == " "
            and out[6] in "abcd" and out[7] == " "):
        out = out[8:]
    return out


def crafting_names(path):
    text = io.open(path, encoding="utf-8").read()
    body = text[text.index("inline const CraftBlueprint CRAFT_BLUEPRINTS[] = {"):]
    body = body[:body.index("\n};")]
    rows = re.findall(r'^\s*\{"((?:[^"\\]|\\.)*)",', body, re.M)
    return [re.sub(r'\\(.)', r'\1', r) for r in rows]


row_of = {}
for i, name in enumerate(crafting_names(crafting_path)):
    row_of.setdefault(normalise(name), i)          # the first row wins, as in BlueprintLibrary


def row(tag):
    name = (blueprints.get(tag) or {}).get("name")
    return row_of.get(normalise(name)) if name else None


def rows_token(rows):
    return "%d:%s" % (len(rows), ",".join(str(r) for r in rows if r is not None))


# --- one line per blueprint ----------------------------------------------------
COMPONENT = {"PowerPlant": "Power Plant", "Cooler": "Cooler", "Shield": "Shield",
             "QuantumDrive": "Quantum Drive", "Radar": "Radar"}
COMPONENT_ORDER = ["PowerPlant", "Cooler", "Shield", "QuantumDrive", "Radar"]
ARMOR_PARTS = ["Helmet", "Helm", "Core", "Arms", "Legs", "Backpack"]
LATER = ["WeaponMining", "MiningModifier", "SalvageModifier", "TractorBeam", "DockingCollar", "Container", "Cargo"]


def size_of(b):
    s = b.get("size")
    return int(s) if s not in (None, "") else None


def shown_name(b):
    """The name as the list shows it: ship weapons and components with their
    size, components with what they are ('HeatSafe Cooler S1'), and no size
    twice ('Singe Cannon (S1)' is 'Singe Cannon S1')"""
    name, t, size = b["name"], b.get("type") or "", size_of(b)
    if t == "WeaponGun":
        base = re.sub(r"\s*\(S\d+\)", "", name).strip()
        return base + (" S%d" % size if size is not None else "")
    if t in COMPONENT:
        kind = COMPONENT[t]
        base = name if kind.lower() in name.lower() else name + " " + kind
        return base + (" S%d" % size if size is not None else "")
    if t == "WeaponMining" and not re.match(r"S\d", name):
        return name + (" S%d" % size if size is not None else "")
    return name


def armor_set(name):
    """'Antium Helmet Jet' -> ('Antium Jet', 0): the set it belongs to, and the
    part's place in helmet-core-arms-legs order"""
    words = name.split()
    for i, w in enumerate(words):
        if w in ARMOR_PARTS:
            return " ".join(words[:i] + words[i + 1:]), ARMOR_PARTS.index(w)
    return name, 9


def item_lines(tags):
    """The list's lines, one per blueprint, in an order that reads: personal
    weapons with their variants and magazines, armor set by set, ship weapons
    by size, components by kind and size, then everything else. Returns the
    lines and the CRAFT_BLUEPRINTS row of each (None for a line with none)."""
    items = []
    seen = set()
    for tag in tags:
        b = blueprints.get(tag)
        if not b or not b.get("name") or tag in seen:
            continue
        seen.add(tag)
        items.append((tag, b))
    weapons = OrderedDict()
    for tag, b in items:
        if (b.get("type") or "") == "WeaponPersonal":
            base = re.sub(r'\s*"[^"]+"', "", b["name"]).strip()
            weapons.setdefault(base, []).append((tag, b))
    keyed = []
    for tag, b in items:
        t, sub, name = b.get("type") or "", b.get("subType") or "", b["name"]
        size = size_of(b) or 0
        if t == "WeaponPersonal":
            base = re.sub(r'\s*"[^"]+"', "", name).strip()
            key = (0, list(weapons).index(base), 0 if name == base else 1, name)
        elif t == "WeaponAttachment" and sub == "Magazine":
            owner = next((i for i, base in enumerate(weapons) if name.startswith(base)), None)
            key = (0, owner, 2, name) if owner is not None else (1, 0, 0, name)
        elif t.startswith("Char_Armor_") and t not in ("Char_Armor_Undersuit", "Char_Armor_Backpack"):
            group, part = armor_set(name)
            key = (2, group, part, name)
        elif t in ("Char_Armor_Undersuit", "Char_Armor_Backpack") or t.startswith("Char_Clothing"):
            key = (3, t, 0, name)
        elif t == "WeaponGun":
            key = (4, size, 0, shown_name(b))
        elif t in COMPONENT:
            key = (5, COMPONENT_ORDER.index(t), size, name)
        elif t in LATER:
            key = (6, LATER.index(t), size, name)
        else:
            key = (7, t, size, name)
        keyed.append((tuple(str(k) if not isinstance(k, int) else "%06d" % k for k in key), tag, b))
    lines, rows = [], []
    for _k, tag, b in sorted(keyed, key=lambda x: x[0]):
        text = "• " + shown_name(b)
        r = row(tag)
        lines.append("{I%d|%s}" % (r if r is not None else -1, text))
        rows.append(r)
    return lines, rows


def owned_phrase(rows):
    return "{O%s}" % rows_token(rows) if rows else ""


def count_phrase(n, chance):
    parts = []
    if n > 1:
        parts.append("one of %d" % n)
    if chance < 0.999:
        parts.append("%d %% chance" % round(chance * 100))
    return (" · " + " · ".join(parts)) if parts else ""


def ranks_phrase(ranks):
    ranks = [r for r in ranks if r]
    if not ranks:
        return ""
    return ranks[0] if len(ranks) == 1 else ", ".join(ranks[:-1]) + " and " + ranks[-1]


# --- what tells variants apart ---------------------------------------------------
# Pyro's contract regions, in the planets' names: the location tags a region's
# contracts search for are carried by these starmap objects (4.10) -
# the same places CraftingData's system column names
REGION_PLACES = {"A": ["Pyro I", "Monox"], "B": ["Bloom"], "C": ["Pyro IV", "Pyro V"], "D": ["Terminus"]}
SYSTEMS = ("Stanton", "Pyro", "Nyx")


def disjoint(sets):
    return all(s for s in sets) and all(not (a & b) for i, a in enumerate(sets) for b in sets[i + 1:])


def split_camel(token):
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", token)


def title_labels(groups):
    """The titles, where each group of contracts goes by one title of its own
    - what the player sees - less the words all of them start with"""
    titles = [{r["title"] for r in g} for g in groups]
    if not all(len(t) == 1 for t in titles) or not disjoint(titles):
        return None
    titles = [next(iter(t)) for t in titles]
    if any("~mission(" in t or not t.strip() for t in titles):
        return None
    head = titles[0]
    for t in titles[1:]:
        while not t.startswith(head):
            head = head[:-1]
    cut = head.rfind(": ") + 2 if ": " in head else 0
    return [t[cut:].strip() for t in titles]


def systems_of(r):
    return {s for s in SYSTEMS if re.search(r"(^|_|/)%s(_|/|$)" % s, r["debug"])}


def contract_words(group, everyone):
    """Who offers these contracts and where, in names the player knows, as
    far as it sets them apart from the description's other contracts:
    'BitZeros · Dead Saints in Nyx'"""
    parts = []
    facs = sorted({r["faction"] for r in group if r["faction"]})
    if facs and len({r["faction"] for r in everyone}) > 1:
        parts.append(" · ".join(facs))
    regs = sorted({m for r in group for m in re.findall(r"Pyro_Region([A-D])", r["debug"])})
    if regs and len({m for r in everyone for m in re.findall(r"Pyro_Region([A-D])", r["debug"])}) > 1:
        place = " · ".join(p for x in regs for p in REGION_PLACES[x])
    else:
        found = set().union(*[systems_of(r) for r in group])
        place = " · ".join(s for s in SYSTEMS if s in found) \
            if found and len(set().union(*[systems_of(r) for r in everyone])) > 1 else ""
    if place:
        parts.append(("in " + place) if parts else place)
    return " ".join(parts)


LABEL_MAX = 36     # a group's label, so that its line stays whole in the mobiGlas panel


def token_labels(groups):
    """The words of the contracts' own names that only one group has - the
    ore of a mining order, the item a recovery is after - with the system
    names left out (they are not what sets such groups apart)"""
    def tokens(r):
        return {t for t in re.split(r"[_/]", r["debug"]) if t and t not in SYSTEMS and not t.isdigit()}
    unions = [set().union(*[tokens(r) for r in g]) for g in groups]
    labels = []
    for i, u in enumerate(unions):
        own = u - set().union(*[v for j, v in enumerate(unions) if j != i])
        if not own or len(own) > 4:
            return None
        labels.append(" · ".join(split_camel(x) for x in sorted(own)))
    if any(len(x) > LABEL_MAX for x in labels):
        return None
    return labels if len(set(labels)) == len(labels) else None


def variant_labels(groups):
    """(header phrase, one label per group) for lists that depend on the
    variant: the Pyro region, the system, the title, the rank - whatever the
    game data shows to set the groups of contracts apart - or the words of
    the contract names"""
    regions = [{m for r in g for m in re.findall(r"Pyro_Region([A-D])", r["debug"])} for g in groups]
    if disjoint(regions):
        return "depends on the region", [" · ".join(p for x in sorted(s) for p in REGION_PLACES[x]) for s in regions]
    systems = [set().union(*[systems_of(r) for r in g]) for g in groups]
    if disjoint(systems):
        return "depends on the system", [" · ".join(s for s in SYSTEMS if s in x) for x in systems]
    titles = title_labels(groups)
    if titles:
        return "depends on the contract", titles
    ranks = [{r["standing"] for r in g if r["standing"]} for g in groups]
    if disjoint(ranks):
        order = [r["standing"] for g in groups for r in g]
        return "depends on your rank", [ranks_phrase(sorted(s, key=order.index)) for s in ranks]
    tokens = token_labels(groups)
    if tokens:
        return "depends on the contract", tokens
    everyone = [r for g in groups for r in g]
    words = [contract_words(g, everyone) for g in groups]
    if all(words) and len(set(words)) == len(words):
        return "depends on the contract", words
    return "depends on the variant", None


KIND_WORD = [("WeaponPersonal", "Weapons"), ("WeaponAttachment", "Weapons"), ("Char_Armor_Undersuit", "Suits"),
             ("Char_Armor_Backpack", "Backpacks"), ("Char_Armor_", "Armor"), ("Char_Clothing", "Clothing"),
             ("WeaponGun", "Ship Weapons"), ("PowerPlant", "Power Plants"), ("Cooler", "Coolers"),
             ("Shield", "Shields"), ("QuantumDrive", "Quantum Drives"), ("Radar", "Radars"),
             ("WeaponMining", "Mining Lasers"), ("MiningModifier", "Mining Modules"),
             ("SalvageModifier", "Scraper Modules"), ("DockingCollar", "Fuel Nozzles"), ("Container", "Cargo"),
             ("Cargo", "Cargo")]
KIND_ORDER = ["Weapons", "Armor", "Suits", "Backpacks", "Clothing", "Ship Weapons", "Power Plants", "Coolers",
              "Shields", "Quantum Drives", "Radars", "Mining Lasers", "Mining Modules", "Scraper Modules",
              "Fuel Nozzles", "Cargo", "Other"]


def kinds_label(tags):
    """What a list is made of, in the player's words and in the order its
    lines come: 'Weapons · Armor', 'Radars · Mining Lasers'"""
    words = set()
    for tag in tags:
        t = (blueprints.get(tag) or {}).get("type") or ""
        words.add(next((w for p, w in KIND_WORD if t.startswith(p)), "Other"))
    words = sorted(words, key=KIND_ORDER.index)
    label = " · ".join(words)
    return label if len(words) <= 3 and len(label) <= 30 else "Mixed"


GENERIC = {"BP", "REWARDS", "REWARD", "MISSIONREWARD", "MISSIONREWARDS"}


def pool_words(name, others):
    """The words of a pool's name the other pools' names do not have:
    'BP_REWARDS_EckhartSecurityEscortShipsMed' among its siblings is 'Medium'"""
    def words(s):
        return re.findall(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+", s)
    everyone = set.intersection(*[set(words(o)) for o in others])
    own = [w for w in words(name) if w not in everyone and w not in GENERIC]
    text = " ".join({"Med": "Medium", "Plus": "+"}.get(w, w) for w in own)
    text = re.sub(r"\b(%s) (?=%s\b)" % ("|".join(SYSTEMS), "|".join(SYSTEMS)), r"\1 · ", text)   # 'Pyro · Nyx'
    return re.sub(r"(\d+) to (\d+)", r"\1–\2", text).replace(" +", "+")


def distinct_labels(labels, names, context):
    """Labels made unique: lists made of the same kinds go by the words of
    their pool names ('Easy', 'Medium', 'Hard+', 'Rank 0–1'), and failing
    that, a number. context: the pool names to tell them apart from"""
    if len(set(labels)) == len(labels):
        return labels
    words = [pool_words(name, context) for name in names]
    out = [w if w and labels.count(label) > 1 else label for label, w in zip(labels, words)]
    if len(set(out)) == len(out):
        return out
    return ["%s %d" % (label, i) for i, label in enumerate(labels, 1)]


def block_from_data(key, records):
    """The description's list, from the contracts that use the description"""
    pools = OrderedDict()        # pool name -> {tags, chance}
    combos = OrderedDict()       # the pools one contract pays out of, together
    for r in records:
        combo = []
        for p in r["pools"]:
            pools.setdefault(p["poolName"], {"tags": p["items"], "chance": p["chance"], "ranks": []})
            if r["standing"] and r["standing"] not in pools[p["poolName"]]["ranks"]:
                pools[p["poolName"]]["ranks"].append(r["standing"])
            combo.append(p["poolName"])
        combos.setdefault(tuple(combo), []).append(r)
    only_some = usage[key]["none"] > 0
    lines = []
    each = len(combos) == 1          # every contract pays out of all of them
    singles = each and len(pools) > 1 and all(len(p["tags"]) == 1 and p["chance"] >= 0.999 for p in pools.values())
    if len(pools) == 1 or singles:
        tags = [t for p in pools.values() for t in p["tags"]]
        body, rows = item_lines(tags)
        chance = min(p["chance"] for p in pools.values())
        head = " · all of these" if singles else count_phrase(len(rows), chance)
        lines.append("<EM4>BLUEPRINTS</EM4>" + head + owned_phrase(rows))
        if only_some:
            ranks = [x for p in pools.values() for x in p["ranks"] if x]
            lines.append(("Only the %s variants pay out" if len(ranks) > 1 else "Only the %s variant pays out")
                         % ranks_phrase(ranks) if ranks else "Only some variants pay out")
        lines += body
        return NL + NL + NL.join(lines)
    names = list(pools)
    if each:
        # one contract, several lists: a blueprint out of each
        header = "one from each group"
        labels = distinct_labels([kinds_label(pools[p]["tags"]) for p in names], names, names)
        groups = [(label, pools[p]["tags"], pools[p]["chance"]) for label, p in zip(labels, names)]
    elif all(len(c) == 1 for c in combos):
        # one list per contract, which one depends on the variant
        header, labels = variant_labels(list(combos.values()))
        if not labels:
            labels = distinct_labels(["Variant"] * len(combos), [c[0] for c in combos], [c[0] for c in combos])
        groups = [(label, pools[c[0]]["tags"], pools[c[0]]["chance"]) for label, c in zip(labels, combos)]
    else:
        # several lists per contract, some of them only in some variants:
        # those named for the contracts that pay out of them, the ones every
        # variant pays out of for what they are made of
        header = "one per group that fits"
        shared = [p for p in names if all(p in c for c in combos)]
        varying = [p for p in names if p not in shared]
        sets = OrderedDict()             # which combos a varying list belongs to -> its lists
        for p in varying:
            sets.setdefault(tuple(c for c in combos if p in c), []).append(p)
        record_groups = [[r for c in s for r in combos[c]] for s in sets]
        if all(not (set(a) & set(b)) for i, a in enumerate(sets) for b in list(sets)[i + 1:]):
            vlabels = variant_labels(record_groups)[1]
        else:
            # a contract pays out of more than one of them: say who pays what
            vlabels = [contract_words(g, records) for g in record_groups]
            if not all(vlabels) or len(set(vlabels)) < len(vlabels):
                vlabels = variant_labels(record_groups)[1]
        if not vlabels:
            # the lists' own names ('Stanton', 'Pyro · Nyx'), where one list stands for each
            firsts = [ps[0] for ps in sets.values()]
            vlabels = distinct_labels(["Variant"] * len(sets), firsts, firsts)
        groups = []
        for vlabel, (s, ps) in zip(vlabels, sets.items()):
            # several lists in the same variants: by their own names ('Easy', 'Hard'), else by kinds
            inner = [""]
            if len(ps) > 1:
                inner = [pool_words(p, ps) for p in ps]
                if not all(inner) or len(set(inner)) < len(inner):
                    inner = distinct_labels([kinds_label(pools[p]["tags"]) for p in ps], ps, ps)
            for p, extra in zip(ps, inner):
                groups.append((vlabel + (" · " + extra if extra else ""), pools[p]["tags"], pools[p]["chance"]))
        for label, p in zip(distinct_labels([kinds_label(pools[p]["tags"]) for p in shared], shared, names), shared):
            groups.append((label, pools[p]["tags"], pools[p]["chance"]))
    rendered = [(label, item_lines(tags), chance) for label, tags, chance in groups]
    every_row = [r for _l, (_b, rows), _c in rendered for r in rows]
    lines.append("<EM4>BLUEPRINTS</EM4> · " + header + ("{A%s}" % rows_token(every_row) if every_row else ""))
    if only_some:
        lines.append("Only some variants pay out")
    for label, (body, rows), chance in rendered:
        lines.append("")                 # an empty line before each group
        lines.append("<EM4>%s</EM4>%s%s" % (label, count_phrase(len(rows), chance), owned_phrase(rows)))
        lines += body
    return NL + NL + NL.join(lines)


# --- the marks ----------------------------------------------------------------
marks = OrderedDict()                     # the game's key -> what is added to its value
report = defaultdict(int)
bp_by_title, bp_by_desc = defaultdict(list), defaultdict(list)
for r in bp_records:
    if real_key(r["titleKey"]):
        bp_by_title[real_key(r["titleKey"])].append(r)
    if real_key(r["descKey"]):
        bp_by_desc[real_key(r["descKey"])].append(r)

for key, records in bp_by_title.items():
    tags = sorted({t for r in records for p in r["pools"] for t in p["items"] if (blueprints.get(t) or {}).get("name")})
    tag = "[BP*]" if usage[key]["none"] > 0 else "[BP]"
    marks[key] = "{S}{T%s|%s}" % (rows_token([row(t) for t in tags]), tag)
    report["titles"] += 1
for key, records in bp_by_desc.items():
    marks[key] = block_from_data(key, records)
    report["descriptions"] += 1

# A [BP*] title over a variant that pays nothing: its own description says so,
# and which variant does ('They Will Be Missed [BP*]': its Neutral contract
# pays nothing, the Veteran one pays). Only where every contract using the
# description goes by such a title.
star_titles = {k for k in bp_by_title if usage[k]["none"] > 0}
desc_users = defaultdict(list)
for r in all_records:
    if real_key(r["descKey"]):
        desc_users[real_key(r["descKey"])].append(r)
for key, users in desc_users.items():
    if key in marks or usage[key]["bp"] > 0:
        continue
    titles = {real_key(r["titleKey"]) for r in users}
    if not titles or not titles <= star_titles:
        continue
    payers = [b for t in titles for b in bp_by_title[t]]
    ranks = list(OrderedDict.fromkeys(b["standing"] for b in payers if b["standing"]))
    mine = {r["standing"] for r in users if r["standing"]}
    if ranks and not mine & set(ranks):
        note = ("Only the %s variants pay out" if len(ranks) > 1 else "Only the %s variant pays out") % ranks_phrase(ranks)
    else:
        note = "Another variant of this contract pays out"
    marks[key] = NL + NL + "<EM4>BLUEPRINTS</EM4> · none in this variant" + NL + note
    report["variants that pay nothing"] += 1

# --- what a contract is sure to pay out of ---------------------------------------
# For BlueprintLibrary: while one of its blueprints is missing, a contract that
# pays out blueprints gives one every time it is completed, and once it has
# none left it gives nothing - so completing one and getting nothing says
# everything it could have given is owned already. Measured on 4.10 logs:
# 42 of 42 such completions paid, and the ones that paid nothing had nothing
# left to give. Before 4.10 it did not hold.
#
# Only where it is sure, by the English title the log shows: every contract
# going by that title pays out blueprints (a Neutral variant that pays
# nothing would make "nothing" mean nothing), every pool of every variant
# rolls at 100 % (Additional Resources For Research rolls 25 %), and the
# variant being unknown - a region's pool, a rank's - what is certain is only
# what every variant pays out of: their overlap.
bp_by_key = defaultdict(list)
for r in bp_records:
    if real_key(r["titleKey"]):
        bp_by_key[real_key(r["titleKey"])].append(r)
users_of_text = defaultdict(list)
for r in all_records:
    k = real_key(r["titleKey"])
    if k and game[k].strip():
        users_of_text[game[k].strip()].append(r)
payouts = OrderedDict()
payout_source = {}                       # pattern -> the title text it stands for
for text in sorted(users_of_text):
    users = users_of_text[text]
    if "%" in text or any(not r["bp"] for r in users):
        continue
    variants = [b for k in sorted({real_key(r["titleKey"]) for r in users}) for b in bp_by_key.get(k, [])]
    sure = None
    for v in variants:
        if not v["pools"] or any(abs(float(pool.get("chance", 0)) - 1.0) > 1e-6 for pool in v["pools"]):
            sure = None
            break
        rows = {row(t) for pool in v["pools"] for t in pool["items"]} - {None}
        sure = rows if sure is None else sure & rows
    if sure:
        # the places and people the game fills in at run time match anything
        pattern = re.sub(r"~\w+\([^)]*\)", "{*}", text)
        payouts[pattern] = sorted(sure)
        payout_source[pattern] = text


def pattern_matches(pattern, title):
    """ContractScan::titleMatches: {*} stands for anything, case aside"""
    pat, tit = pattern.lower(), title.lower()
    parts = pat.split("{*}")
    if len(parts) == 1:
        return pat == tit
    if not tit.startswith(parts[0]):
        return False
    at = len(parts[0])
    for middle in parts[1:-1]:
        k = tit.find(middle, at)
        if k < 0:
            return False
        at = k + len(middle)
    return len(tit) >= at + len(parts[-1]) and tit.endswith(parts[-1])


# A title with a place or a name in it has to be one the log cannot mistake
# for another contract's: eight letters of its own at least ("{*}" alone
# stood for every title there is), and no other title, its own places filled
# in, that it would match ("{*} needs help" and a contract of that name that
# pays nothing would make the guess wrong)
rendered = {t: re.sub(r"~\w+\([^)]*\)", "X", t) for t in users_of_text}
for pattern in list(payouts):
    if "{*}" in pattern and len(re.sub(r"[^0-9A-Za-z]", "", pattern.replace("{*}", ""))) < 8:
        del payouts[pattern]
        report["payouts too vague"] += 1
        continue
    if "{*}" in pattern and any(other != payout_source[pattern] and pattern_matches(pattern, shown)
                                for other, shown in rendered.items()):
        del payouts[pattern]
        report["payouts matching another title"] += 1
report["sure payouts"] = len(payouts)


# --- only additions of these kinds, and tokens GameTexts.cpp can read -------------
TOKEN = re.compile(r"\{S\}|\{I-?\d+\|[^{}]*\}|\{[OA]\d+:[\d,]*\}|\{T\d+:[\d,]*\|[^{}]*\}")
for key, text in marks.items():
    literal = TOKEN.sub("", text)
    assert "{" not in literal and "}" not in literal, key
    assert "[BP" in text or "<EM4>BLUEPRINTS</EM4>" in text, key
    assert "Rep]" not in text and "Reputation" not in text and "~mission(" not in text, key


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


with io.open(out_path, "w", encoding="utf-8", newline="\n") as f:
    w = f.write
    w("#pragma once\n")
    w("// The marks Bedrock adds to the game's contract texts - GENERATED by\n")
    w("// gen_game_texts.py, do not edit by hand. See GameTexts.h for what the\n")
    w("// tokens in them mean and how they come together with the game's global.ini.\n\n")
    w("namespace SCMiner {\n\n")
    w("struct GameTextMark {\n")
    w("    const char* key;      // the game's key, as its global.ini spells it\n")
    w("    const char* text;     // what is added to its value, with tokens\n")
    w("};\n\n")
    w("inline const GameTextMark GAME_TEXT_MARKS[] = {\n")
    for key, text in marks.items():
        w('    {"%s", "%s"},\n' % (esc(key), esc(text)))
    w("};\n")
    w("inline constexpr int GAME_TEXT_MARK_COUNT = %d;\n\n" % len(marks))
    w("// What a contract is sure to pay out of, by the English title the log\n")
    w("// gives it ({*} where the game fills in a place or a name): the rows of\n")
    w("// CRAFT_BLUEPRINTS every variant of it pays out of at 100 %. Completing it\n")
    w("// and getting no blueprint means they are all owned - see BlueprintLibrary.\n")
    w("struct ContractPayout {\n")
    w("    const char* title;\n")
    w("    const char* rows;     // \"12,57,300\"\n")
    w("};\n\n")
    w("inline const ContractPayout CONTRACT_PAYOUTS[] = {\n")
    for title, rows in payouts.items():
        w('    {"%s", "%s"},\n' % (esc(title), ",".join(str(r) for r in rows)))
    w("};\n")
    w("inline constexpr int CONTRACT_PAYOUT_COUNT = %d;\n\n" % len(payouts))
    w("}  // namespace SCMiner\n")
print("%d marks: %s" % (len(marks), ", ".join("%d %s" % (v, k) for k, v in report.items())))
