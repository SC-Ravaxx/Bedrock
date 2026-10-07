"""Builds the "which contract drops this blueprint" table straight from the
game files, so nothing here depends on someone else's export.

    gen_contracts_from_p4k.py <extract-dir> <global.ini> <contracts.json> [reference.json]

The chain, all of it inside the unpacked game database:

    records/contracts/contractgenerator/<guild>/<faction>/*.xml
        <CareerContract debugName minStanding titleKey ...>
          <BlueprintRewards chance blueprintPool="POOL-GUID" />
    records/crafting/blueprintrewards/**/*.xml
        <BlueprintPoolRecord __ref="POOL-GUID">
          <BlueprintReward weight blueprintRecord="BP-GUID" />
    records/crafting/blueprints/**/*.xml
        <...Record.BP_CRAFT_xyz __ref="BP-GUID">          -> the tag we key on

Titles come from Data/Localization/english/global.ini - the game's own, out
of the same build's Data.p4k - where a key like
headhunters_eliminateall_cfp_M_title_001 reads

    ~mission(Location) has been greenlit

- a template with placeholders, cleaned up below. Not a community language
pack's in its place: a pack writes into the titles and briefings what the
game does not ("[150 Rep] [BP]*", "Reputation Awarded: 150", lists of
blueprint pools), names some items its own way, and does not know the
contracts of a build newer than itself. What a contract awards in
reputation is read out of the contracts themselves (see reputation_of).

Two more outputs, each written only when its environment variable names a
file - for gen_game_texts.py, which marks the game's own texts:
    BP_CONTRACT_KEYS   one record per contract with blueprints: its title and
                       description keys, debug name, rank, faction, pools
    ALL_CONTRACT_KEYS  one record per contract of every contract file, with
                       or without blueprints - a title shared with contracts
                       that pay nothing is told apart from one that always pays
and one for gen_crafting.py, which turns it into the planner's ranks:
    REPUTATION_JSON    the reputation ladders, the outfits, and per contract -
                       by the names Game.log knows it by - its title, the
                       rank it asks for, what completing it awards, and for
                       a gathering order what it wants brought (see the end)
"""
import io, json, os, re, sys, collections

extract, ini_path, out = sys.argv[1], sys.argv[2], sys.argv[3]
reference = sys.argv[4] if len(sys.argv) > 4 else None
records = os.path.join(extract, "Data", "libs", "foundry", "records")


def read(path):
    return io.open(path, encoding="latin-1", errors="replace").read()


def walk(sub):
    root = os.path.join(records, sub)
    for base, _dirs, files in os.walk(root):
        for f in files:
            if f.lower().endswith(".xml"):
                yield os.path.join(base, f)


# ---- difficulty profiles ---------------------------------------------------
# The game does not leave "how hard is this" to a guess: every contract names
# a profile that weights the four axes. Five of them exist; General is by far
# the most used. The weights of the others do not add up to one, so the score
# is the weighted average - which keeps it on the same 1 to 7 scale as the
# axes themselves.
difficulty_profiles = {}
for _dir, _sub, _files in os.walk(os.path.join(extract, "Data", "libs", "foundry", "records",
                                               "contracts", "contractdifficultyprofiles")):
    for _fn in _files:
        if not _fn.endswith(".xml"):
            continue
        _s = io.open(os.path.join(_dir, _fn), encoding="utf-8", errors="replace").read()
        _ref = re.search(r'__ref="([0-9a-f-]+)"', _s)
        if not _ref:
            continue
        _w = {}
        for _axis in ("mechanicalSkill", "mentalLoad", "riskOfLoss", "gameKnowledge"):
            _m = re.search(_axis + r'Weight="([\d.]+)"', _s)
            if _m:
                _w[_axis] = float(_m.group(1))
        if _w:
            difficulty_profiles[_ref.group(1)] = _w
print(f"{len(difficulty_profiles)} difficulty profiles")


def axis_words(raw):
    """"Ship_could_get_damaged_Could_lose_cargo_4" -> "ship could get damaged,
    could lose cargo". The game writes the meaning into the name; only the
    step number and the underscores have to go."""
    s = re.sub(r"_\d+$", "", raw or "").replace("_", " ").strip()
    # The name runs two clauses together, the second starting on a capital:
    # "Ship_could_get_damaged_Could_lose_cargo". Only the FIRST such boundary
    # is a clause break - "and Ship die" is one clause, not two - and an
    # all-caps word (PvE, FPS) is an abbreviation, not a new sentence.
    m = re.search(r"(?<=[a-z]) (?=[A-Z][a-z])", s)
    if m:
        s = s[:m.start()] + ", " + s[m.end():m.end() + 1].lower() + s[m.end() + 1:]
    # A plain capitalised word inside the sentence is just the game's file
    # naming ("and Ship die"); an all-caps one is an abbreviation and stays.
    s = " ".join(w.lower() if re.match(r"^[A-Z][a-z]+$", w) else w for w in s.split())
    return s[:1].lower() + s[1:] if s else ""


# ---- blueprint GUID -> readable name --------------------------------------
# (filled from the blueprint dump when one is passed in, so the pool siblings
# can be named rather than shown as BP_CRAFT_ tags)
contract_records = []   # one per contract with blueprints
all_records = []        # one per contract, with or without
tag_name = {}
names_file = os.environ.get("SIGCOMP_BLUEPRINT_NAMES", "")
if names_file and os.path.exists(names_file):
    for b in json.load(io.open(names_file, encoding="utf-8")):
        if b.get("tag") and b.get("name"):
            tag_name[b["tag"]] = b["name"]

# ---- blueprint GUID -> tag ------------------------------------------------
guid_to_tag = {}
for path in walk(os.path.join("crafting", "blueprints")):
    text = read(path)
    m = re.search(r'<(\w+)\.(BP_CRAFT_[\w]+)[^>]*__ref="([0-9a-f-]{36})"', text)
    if m:
        guid_to_tag[m.group(3)] = m.group(2)
print(f"{len(guid_to_tag)} blueprints with a tag")

# ---- pool GUID -> [blueprint GUIDs with weight] --------------------------
pools = {}
for path in walk(os.path.join("crafting", "blueprintrewards")):
    text = read(path)
    m = re.search(r'__ref="([0-9a-f-]{36})"', text)
    if not m:
        continue
    entries = [(g, float(w or 1)) for w, g in
               re.findall(r'<BlueprintReward weight="([\d.]*)"\s+blueprintRecord="([0-9a-f-]{36})"', text)]
    if entries:
        name = re.search(r"<BlueprintPoolRecord\.(\w+)", text)
        pools[m.group(1)] = (entries, name.group(1) if name else "")
print(f"{len(pools)} blueprint pools")

# ---- contract templates: where a title hides when the contract has none --
# A contract can carry its own <ContractStringParam param="Title">, and most
# do. The ones that do not inherit it from the TEMPLATE they are built on,
# where it sits in the display block instead:
#   <contractDisplayInfo><ContractDisplayInfo ...>
#     <displayString><LocID value="@Hockrow_FacilityDelve_P3M1_title" />
# Without this, ten contracts fall back to their debug name
# ("Hockrow_FacilityDelve_P3MM_Repeat" instead of "Jorrit Dossier: Project
# Hyperion").
template_titles = {}
template_names = {}      # the record's own name: "ItemResourceGathering_Generic", "HaulCargo_AtoB_Override"
for path in walk(os.path.join("contracts", "contracttemplates")):
    text = read(path)
    ref = re.search(r'__ref="([0-9a-f-]{36})"', text)
    if not ref:
        continue
    root = re.match(r"\s*<\w+\.(\w+)", text)
    if root:
        template_names[ref.group(1)] = root.group(1)
    info = re.search(r"<contractDisplayInfo>.*?</contractDisplayInfo>", text, re.S)
    if not info:
        continue
    ids = [k for k in re.findall(r'<LocID value="@?([\w]+)"', info.group(0))
           if not k.startswith("LOC_")]
    if ids:
        template_titles[ref.group(1)] = ids[0]
print(f"{len(template_titles)} contract templates carry a title of their own")

# ---- titles --------------------------------------------------------------
titles = {}
# Keys sometimes carry a suffix after a comma (name,P=...), so both forms go in
for line in io.open(ini_path, encoding="utf-8", errors="replace"):
    key, _, value = line.partition("=")
    if _ and "," in key:
        titles.setdefault(key.split(",")[0].strip().lower(), value.rstrip("\n"))
    if _ and key:
        titles[key.strip().lower()] = value.rstrip("\n")


# ---- the outfit's own strings --------------------------------------------
# A title or a briefing can be nothing but a slot into the OUTFIT's strings:
#
#     blackbox_recover_title_E_001 = ~mission(Contractor|BlackBoxRecoverTitleEasy) [100 Rep] [BP]*
#
# The game fills that from the organization record -
# records/missiondata/pu_organizations/<outfit>.xml - which lists the outfit's
# strings, each under a tag:
#
#     <MissionStringVariant tag="4bf37fcf-..." string="@BitZeros_blackbox_E_Title_001" />
#
# The tag IS the slot's name, BlackBoxRecoverTitleEasy, by way of the tag
# database - and the tag database is the one record the extractor writes out
# empty. So the two are matched on what both say in their own words: the kind
# of contract (BlackBoxRecover / blackbox), whether it is a title or a
# briefing, and the difficulty tier (Easy / E). The tags then check the match:
# every key taken for one slot has to sit under one tag, and one slot has to
# sit under the same tag in every outfit that has it - which it does, for
# Bit Zeros as for Hockrow.
#
# An outfit writes some slots several times over - three titles under one tag
# - and the game draws one when it offers the contract. All of them are
# given, "Bleeding Edge Tech / Turning the Tables / Upgrade Grab", so that
# whichever the game shows can be found in this list.
#
# Without this, 193 of 3,410 contracts have no name and 131 no briefing.
TIERS = {"Intro": ("intro",), "VeryEasy": ("ve", "veryeasy"), "Easy": ("e", "easy"),
         "Medium": ("m", "medium"), "Hard": ("h", "hard"), "VeryHard": ("vh", "veryhard"),
         "Super": ("s", "super")}
TIER_TOKENS = {t for ts in TIERS.values() for t in ts}

organizations = []
for path in walk(os.path.join("missiondata", "pu_organizations")):
    text = read(path)
    variants = re.findall(r'<MissionStringVariant tag="([0-9a-f-]{36})" string="@?([^"]*)"', text)
    if not variants:
        continue
    fm = re.search(r'<MissionOrganization[^>]*?factionReputation="([0-9a-f-]{36})"', text)
    organizations.append({
        "file": os.path.basename(path),
        "faction": fm.group(1) if fm else "",
        "keys": {k.lower() for _t, k in variants},
        "variants": variants,
    })
print(f"{len(organizations)} organizations with strings of their own")


def organization_for(faction, contractor_key):
    """The outfit a handler's contracts belong to: the one of its faction that
    also carries the handler's own Contractor string, else any of its
    faction, else whichever carries that string."""
    key = (contractor_key or "").lstrip("@").lower()
    of_faction = [o for o in organizations if faction and o["faction"] == faction]
    for o in of_faction:
        if key and key in o["keys"]:
            return o
    if of_faction:
        return of_faction[0]
    for o in organizations:
        if key and key in o["keys"]:
            return o
    return None


slot_tags = {}                              # slot -> tag, the cross-outfit check
slot_log = collections.OrderedDict()        # (outfit, slot) -> keys, for the run's report
slot_warnings = []


def contractor_strings(org, slot):
    """The outfit's keys for one slot, "BlackBoxRecoverTitleEasy": matched on
    kind, role and tier, checked against the tags."""
    words = re.findall(r"[A-Z][a-z]*", slot)
    tier = None
    if len(words) >= 2 and words[-2] + words[-1] in TIERS:
        tier = words[-2] + words[-1]
        words = words[:-2]
    elif words and words[-1] in TIERS:
        tier = words.pop()
    role = None
    if words and words[-1] in ("Title", "Name"):
        role = "title"
        words = words[:-1]
    elif words and words[-1] in ("Description", "Desc"):
        role = "desc"
        words = words[:-1]
    if not words:
        return []
    # the kind, in the spellings an outfit's key may use: all of it, its
    # first two words, its first word - the longest that matches wins. A
    # slot that is neither a title nor a briefing - SignOff, the courier's
    # closing line; DestroyProbeAmount, a count - is matched on its whole
    # name and nothing shorter.
    kinds = ["".join(words).lower()]
    if role is not None:
        if len(words) > 2:
            kinds.append("".join(words[:2]).lower())
        if len(words) > 1 and len(words[0]) >= 6:
            kinds.append(words[0].lower())
    want_tier = TIERS[tier] if tier else ()

    def fits(key, kind, strict_tier):
        tokens = key.lower().split("_")
        flat = "".join(tokens)
        if kind not in flat:
            return False
        if role == "title" and not any(t in ("title", "name") for t in tokens):
            return False
        if role == "desc" and not any(t.startswith("desc") for t in tokens):
            return False
        has_tier = [t for t in tokens if t in TIER_TOKENS]
        if tier:
            return any(t in want_tier for t in has_tier)
        # a slot with no tier of its own: the outfit's untiered key, or
        # failing that whatever it has (Foxwell writes one probe title, and
        # names it for the middle tier)
        return not has_tier or not strict_tier

    chosen = []
    for kind in kinds:
        for strict in (True, False):
            chosen = [(t, k) for t, k in org["variants"] if fits(k, kind, strict)]
            if chosen:
                break
        if chosen:
            break
    if not chosen:
        return []
    # one tag per slot - keys under another tag are another slot's
    by_tag = collections.OrderedDict()
    for t, k in chosen:
        by_tag.setdefault(t, []).append(k)
    tag, keys = max(by_tag.items(), key=lambda item: len(item[1]))
    if len(by_tag) > 1:
        slot_warnings.append(f"{org['file']}: {slot} matched keys under {len(by_tag)} tags, took {keys}")
    if slot_tags.setdefault(slot, tag) != tag:
        slot_warnings.append(f"{org['file']}: {slot} sits under another tag than in the first outfit that had it")
    slot_log[(org["file"], slot)] = keys
    return keys


def fill_contractor_slots(raw, org, depth=0):
    """Every ~mission(Contractor|X) in a string, replaced by what the outfit
    writes there. Several strings under one tag are all given: titles side
    by side, briefings one after the other."""
    if not org or depth > 2 or "~mission(Contractor|" not in raw:
        return raw

    def one(m):
        slot = m.group(1)
        texts = []
        for k in contractor_strings(org, slot):
            t = titles.get(k.lower(), "")
            if t:
                texts.append(fill_contractor_slots(t, org, depth + 1).strip())
        if not texts:
            return m.group(0)
        if "Desc" in slot:
            return "\\n\\n- or -\\n\\n".join(texts)       # the game's own line breaks
        if "Title" in slot or "Name" in slot:
            return " / ".join(texts)
        # a closing line, a count - one of them reads as the game reads;
        # eight couriers' sign-offs in a row do not
        return texts[0]

    return re.sub(r"~mission\(Contractor\|([A-Za-z0-9_]+)\)", one, raw)


def clean_title(key, org=None):
    """The stored string is a template: placeholders like ~mission(Location)
    and a markup tail with the reward hints. Both go. Given the outfit, its
    own slots are filled first - see fill_contractor_slots."""
    raw = titles.get((key or "").lstrip("@").lower())
    if not raw:
        return ""
    raw = fill_contractor_slots(raw, org)
    raw = re.sub(r"<[^>]+>", "", raw)                       # <EM4>...</EM4>
    raw = re.sub(r"~mission\(([^)]*)\)", lambda m: "[" + m.group(1).upper() + "]", raw)
    raw = re.sub(r"\[[\d]+ Rep\]|\[BP\]\*?|\[BP\]", "", raw)
    return " ".join(raw.split()).strip()


# ---- who offers it, and at what standing ---------------------------------
# factions/factionreputation/*.xml : displayName is a localisation key
faction_names = {}
for path in walk(os.path.join("factions", "factionreputation")):
    text = read(path)
    ref = re.search(r'__ref="([0-9a-f-]{36})"', text)
    disp = re.search(r'displayName="@?([\w]+)"', text)
    if ref:
        name = clean_title(disp.group(1)) if disp else ""
        if not name:
            # fall back to the record name: FactionReputation_Unlawful_HeadHunters
            tag = re.search(r"<FactionReputation\.FactionReputation_\w+?_(\w+)", text)
            name = tag.group(1) if tag else ""
        faction_names[ref.group(1)] = name
print(f"{len(faction_names)} factions")

# reputation/standings/**/*.xml : the rank a contract asks for
standing_names = {}
for path in walk(os.path.join("reputation", "standings")):
    text = read(path)
    for m in re.finditer(r'displayName="@?([\w]+)"[^>]*?__ref="([0-9a-f-]{36})"', text):
        standing_names[m.group(2)] = clean_title(m.group(1))
    for m in re.finditer(r'__ref="([0-9a-f-]{36})"', text):
        standing_names.setdefault(m.group(1), "")
print(f"{len(standing_names)} reputation ranks")

# ---- reputation, as the contract awards it ---------------------------------
# A contract lists what its ends bring under <contractResults>; each
#   <ContractResult_LegacyReputation>
#     <missionResults> five flags, the ends it is for - the first a mission
#                      completed, the third one abandoned (with a negative
#                      amount, on 11 of the 14 that have it)
#     <contractResultReputationAmounts factionReputation=".." reputationScope=".."
#                                      reward="GUID" />
# and the reward GUID names a record under reputation/rewards whose
# reputationAmount is the figure. A completed contract awards in one scope or
# two: the outfit's Standing - which decides the contracts it offers - and on
# some Affinity with whoever gave it, or a career's own (Hauling, Security).
# Of the 786 contracts with blueprints in 4.10.0, 601 award once, 149 twice.
rep_amount = {}
for path in walk(os.path.join("reputation", "rewards")):
    for m in re.finditer(r'<SReputationRewardAmount\.\w+[^>]*?reputationAmount="(-?\d+)"[^>]*?__ref="([0-9a-f-]{36})"',
                         read(path)):
        rep_amount[m.group(2)] = int(m.group(1))
rep_scope = {}
for path in walk(os.path.join("reputation", "scopes")):
    text = read(path)
    ref = re.search(r'__ref="([0-9a-f-]{36})"', text)
    if not ref:
        continue
    # the name the mobiGlas gives it - Standing, Affinity, Hauling - else the
    # files' own, where the game has none in words
    disp = re.search(r'displayName="@?([\w]+)"', text)
    label = clean_title(disp.group(1)) if disp else ""
    if not label or "PLACEHOLDER" in label:
        own = re.search(r'scopeName="([^"]*)"', text)
        label = own.group(1) if own else ""
    rep_scope[ref.group(1)] = label
print(f"{len(rep_amount)} reputation amounts, {len(rep_scope)} reputation scopes")

# ---- the ladders ------------------------------------------------------------
# A scope lists its standings under <standingMap>, and each standing says from
# how much reputation on it counts: the outfits' Standing runs from Neutral (0)
# to Elite Contractor (95,250), Hauling from Trainee (0) to Master (237,750).
# Ranks below zero - "Not Eligible", -1000 - are where abandoning contracts
# drops a player for an hour or a day, and drift back by themselves; nobody
# plans with them, and they are left off. gated="1" marks a rank the game
# holds a player at until a contract of its own lets them past (the outfits'
# Neutral, Hauling's Trainee).
standing_min = {}
standing_gated = {}
for path in walk(os.path.join("reputation", "standings")):
    for m in re.finditer(r"<SReputationStandingParams\.\w+([^>]*)>", read(path)):
        ref = re.search(r'__ref="([0-9a-f-]{36})"', m.group(1))
        low = re.search(r'minReputation="(-?\d+)"', m.group(1))
        if ref and low:
            standing_min[ref.group(1)] = int(low.group(1))
            standing_gated[ref.group(1)] = 'gated="1"' in m.group(1)
ladders = {}              # scope GUID -> its standings' GUIDs, lowest first
scope_of_standing = {}    # standing GUID -> the first scope that lists it
for path in walk(os.path.join("reputation", "scopes")):
    text = read(path)
    ref = re.search(r'__ref="([0-9a-f-]{36})"', text)
    if not ref:
        continue
    steps = [s for s in re.findall(r'<Reference value="([0-9a-f-]{36})"', text)
             if standing_min.get(s, -1) >= 0]
    steps.sort(key=lambda s: standing_min[s])
    if steps:
        ladders[ref.group(1)] = steps
        for s in steps:
            scope_of_standing.setdefault(s, ref.group(1))
print(f"{len(ladders)} reputation ladders")


def requirement_of(block, handler_faction, handler_scope):
    """[faction, scope, lowest standing, highest standing] - GUIDs - of the
    rank a contract is offered at, or None when it asks for none. A career
    contract carries its range on itself (minStanding, maxStanding) and the
    ladder it is on on the handler above it (reputationScope); a list
    contract asks through a <ContractPrerequisite_Reputation> with a faction
    and a scope of its own. exclude="1" turns a prerequisite round - the
    contract is NOT offered in that range - and says nothing of a rank to
    reach."""
    head = block[:block.find(">") + 1]
    low = re.search(r'minStanding="([0-9a-f-]{36})"', head)
    if low:
        high = re.search(r'maxStanding="([0-9a-f-]{36})"', head)
        scope = handler_scope if low.group(1) in ladders.get(handler_scope, ()) \
            else scope_of_standing.get(low.group(1), "")
        return [handler_faction, scope, low.group(1), high.group(1) if high else ""]
    for pre in re.findall(r"<ContractPrerequisite_Reputation[^>]*>", block):
        if 'exclude="1"' in pre:
            continue
        low = re.search(r'minStanding="([0-9a-f-]{36})"', pre)
        if not low:
            continue
        high = re.search(r'maxStanding="([0-9a-f-]{36})"', pre)
        own = re.search(r'\sscope="([0-9a-f-]{36})"', pre)
        outfit = re.search(r'factionReputation="([0-9a-f-]{36})"', pre)
        scope = own.group(1) if own and low.group(1) in ladders.get(own.group(1), ()) \
            else scope_of_standing.get(low.group(1), "")
        return [outfit.group(1) if outfit else handler_faction, scope, low.group(1), high.group(1) if high else ""]
    return None


# ---- gathering orders ------------------------------------------------------
# A contract built on one of the ItemResourceGathering templates asks for
# things to be brought, and so does every trade of Wikelo's (TheCollector_...).
# Shubin's "Purchase Order: Ship Mined Ore" for 16 SCU of Torite and 11 of
# Bexalite:
#   <HaulingOrderContent_Resource resource="GUID" minSCU="16" maxSCU="16" />
# hand and ground mining orders for gems by the piece:
#   <HaulingOrderContent_EntityClass entityClass="GUID" minAmount="15" maxAmount="15" />
# and some for any one of several (<HaulingOrderContent_Or>). The hauling
# contracts list their freight with the same rows, on HaulCargo templates -
# that is cargo handed over, not something to go and mine, and Covalex's
# Tungsten is not wanted by anybody's laser. Only what resource_guids.json
# names counts: the mined materials, by the records the blueprints use too.
_mined_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "resource_guids.json")
mined_names = json.load(io.open(_mined_path, encoding="utf-8")) if os.path.exists(_mined_path) else {}


def order_of(block, head):
    """{"rows": [[material, least, most, "SCU" or ""]], "choice": any one of
    them will do} for a gathering order, None for every other contract."""
    tm = re.search(r'template="([0-9a-f-]{36})"', head)
    if not tm or not template_names.get(tm.group(1), "").startswith(("ItemResourceGathering", "TheCollector_")):
        return None
    start = block.find("<haulingOrderContent>")
    end = block.find("</haulingOrderContent>", start)
    if start < 0 or end < 0:
        return None
    body = block[start:end]
    rows = collections.OrderedDict()
    for m in re.finditer(r'<HaulingOrderContent_(Resource|EntityClass) (?:resource|entityClass)="([0-9a-f-]{36})"([^>]*)>',
                         body):
        name = mined_names.get(m.group(2))
        if not name:
            continue
        low, high = ("minSCU", "maxSCU") if m.group(1) == "Resource" else ("minAmount", "maxAmount")
        least = re.search(low + r'="([\d.]+)"', m.group(3))
        most = re.search(high + r'="([\d.]+)"', m.group(3))
        least = int(float(least.group(1))) if least else 0
        most = max(least, int(float(most.group(1))) if most else 0)   # a maximum of 0: no more than the least
        if name in rows:
            least, most = min(least, rows[name][1]), max(most, rows[name][2])
        rows[name] = [name, least, most, "SCU" if m.group(1) == "Resource" else ""]
    if not rows:
        return None
    return {"rows": list(rows.values()), "choice": "<HaulingOrderContent_Or>" in body}


def offering_outfit(block, need, handler_outfit):
    """Who offers the contract: the outfit whose rank it asks for, else the
    handler's, else the first outfit the contract names that is not kept
    away from it. The first factionReputation of the block was taken here
    before, and on 229 rows that is a prerequisite with exclude="1" - one
    that keeps the contract FROM a rival's friends: Headhunters' "A little
    retaliation" was listed under Citizens For Prosperity, whose outposts it
    attacks, and CFP's "Reclaim Headhunter Outpost" under Headhunters."""
    if need and need[0]:
        return need[0]
    if handler_outfit:
        return handler_outfit
    for m in re.finditer(r'<(\w+)[^>]*?factionReputation="([0-9a-f-]{36})"[^>]*>', block):
        if m.group(1) == "ContractPrerequisite_Reputation" and 'exclude="1"' in m.group(0):
            continue
        return m.group(2)
    return ""


def awards_of(block):
    """[[faction, scope, amount]] that completing the contract awards, summed
    per outfit and scope - the same results reputation_of reads, kept apart
    by outfit, which a rank belongs to."""
    out = collections.OrderedDict()
    for res in re.findall(r"<ContractResult_LegacyReputation>(.*?)</ContractResult_LegacyReputation>", block, re.S):
        flags = re.findall(r'<Bool value="(\d)"', res)
        if not flags or flags[0] != "1":
            continue
        for tag in re.findall(r"<contractResultReputationAmounts[^>]*>", res):
            outfit = re.search(r'factionReputation="([0-9a-f-]{36})"', tag)
            scope = re.search(r'reputationScope="([0-9a-f-]{36})"', tag)
            reward = re.search(r'reward="([0-9a-f-]{36})"', tag)
            amount = rep_amount.get(reward.group(1), 0) if reward else 0
            if outfit and scope and amount > 0:
                key = (outfit.group(1), scope.group(1))
                out[key] = out.get(key, 0) + amount
    return [[f, s, a] for (f, s), a in out.items()]


def reputation_of(block):
    """What completing the contract awards, {scope's name: amount}, summed per
    scope; only what it gains - a rival's loss on the same end is not the
    player's to plan with."""
    out = collections.OrderedDict()
    for res in re.findall(r"<ContractResult_LegacyReputation>(.*?)</ContractResult_LegacyReputation>", block, re.S):
        flags = re.findall(r'<Bool value="(\d)"', res)
        if not flags or flags[0] != "1":
            continue
        for scope, reward in re.findall(r'<contractResultReputationAmounts[^>]*?reputationScope="([0-9a-f-]{36})"'
                                        r'[^>]*?reward="([0-9a-f-]{36})"', res):
            amount = rep_amount.get(reward, 0)
            if amount > 0:
                label = rep_scope.get(scope) or "Reputation"
                out[label] = out.get(label, 0) + amount
    return dict(out)


def reputation_text(seen):
    """'100–200 Standing, 50 Affinity': per scope, the least and the most
    the contracts behind one row award on completion - Standing first."""
    by = collections.OrderedDict()
    for awards in seen:
        for label, amount in awards.items():
            by.setdefault(label, []).append(amount)
    parts = []
    for label in sorted(by, key=lambda l: (l != "Standing", l)):
        low, high = min(by[label]), max(by[label])
        parts.append(("%d" % low if low == high else "%d–%d" % (low, high)) + " " + label)
    return ", ".join(parts)


def region_of(debug_name):
    """Pyro is split into four regions and the generator names its contracts
    after them - HH_Pyro_RegionA_E_... - each with its own reward pool. Which
    region you complete a mission in therefore decides what can drop."""
    # No word boundary here: the name reads "RegionA_E_..." and an
    # underscore counts as a word character, so \b never matches
    m = re.search(r"Region([A-D])(?![A-Za-z])", debug_name or "")
    return m.group(1) if m else ""


def systems_of_pool(pool_name):
    """The pool name is the honest answer: BP_REWARDS_StantonFoxwellHard is
    the Stanton version of a contract that also exists for Pyro and Nyx, and
    a name ending in PyroNyx means those two - not "everywhere"."""
    return [w for w in ("Stanton", "Pyro", "Nyx") if w.lower() in (pool_name or "").lower()]


def system_of(debug_name):
    """The generator names its contracts after where they run:
    HH_Pyro_RegionA_E_... , CFP_Stanton_... - so the system is in the name."""
    low = (debug_name or "").lower()
    hits = [s for s in ("pyro", "stanton", "nyx") if s in low]
    if len(hits) == 1:
        return hits[0].capitalize()
    return "Multi" if len(hits) > 1 else ""


# ---- contracts -----------------------------------------------------------

per_tag = collections.defaultdict(list)
contract_pools = {}      # a contract's own id -> the pools it pays out of, in the files' order
contract_files = 0
for path in walk(os.path.join("contracts", "contractgenerator")):
    text = read(path)
    if "BlueprintRewards" not in text:
        for _blk in re.split(r"(?=<(?:Career)?Contract\s)", text):
            if not re.match(r"<(?:Career)?Contract\s", _blk):
                continue
            _head = _blk[:_blk.find(">") + 1]
            _tk = ""
            _mt = re.search(r'<ContractStringParam param="Title" value="@?([\w]+)"', _blk)
            if _mt:
                _tk = _mt.group(1)
            else:
                _tm = re.search(r'template="([0-9a-f-]{36})"', _head)
                if _tm:
                    _tk = template_titles.get(_tm.group(1), "")
            _md = re.search(r'<ContractStringParam param="Description" value="@?([\w]+)"', _blk)
            all_records.append({"titleKey": _tk, "descKey": _md.group(1) if _md else "",
                                "debug": (re.search(r'debugName="([^"]*)"', _blk) or [None, ""])[1],
                                "standing": standing_names.get((re.search(r'minStanding="([0-9a-f-]{36})"', _blk) or [None, ""])[1], ""),
                                "bp": "blueprintPool" in _blk, "unreleased": 'notForRelease="1"' in _head})
    if "BlueprintRewards" not in text:
        continue
    contract_files += 1
    # A file holds one or more handlers, and the FACTION is declared on the
    # handler - the contracts under it carry none of their own. The text is
    # cut at handler boundaries first, so no contract takes the next
    # handler's faction.
    handler_re = re.compile(r"<ContractGeneratorHandler_\w+[^>]*>")
    parts, marks = [], [m.start() for m in handler_re.finditer(text)]
    if marks:
        for i, start in enumerate(marks):
            end = marks[i + 1] if i + 1 < len(marks) else len(text)
            parts.append(text[start:end])
    else:
        parts = [text]

    for part in parts:
        handler_faction = ""
        hm = re.search(r'factionReputation="([0-9a-f-]{36})"', part)
        if hm:
            handler_faction = hm.group(1)
        # ...and the handler's own word on it, for the ranks: the outfit and
        # the ladder its career contracts' minStanding is on
        handler_tag = re.match(r"<ContractGeneratorHandler_\w+[^>]*>", part)
        handler_tag = handler_tag.group(0) if handler_tag else ""
        handler_scope = (re.search(r'reputationScope="([0-9a-f-]{36})"', handler_tag) or [None, ""])[1]
        handler_outfit = (re.search(r'factionReputation="([0-9a-f-]{36})"', handler_tag) or [None, ""])[1] \
            or handler_faction
        # Some handlers keep no reputation with the player and so declare no
        # faction - the Jorrit dossiers, the Orison relief orders. They do
        # name who is asking, as the string the title's [CONTRACTOR] slot is
        # filled from, and an outfit's name is worth more in the list than an
        # empty column.
        handler_contractor = ""
        cm = re.search(r'<ContractStringParam param="Contractor" value="@?([\w]+)"', part)
        if cm:
            handler_contractor = clean_title(cm.group(1))
        # ...and the outfit's own strings, for a title or briefing that is
        # only a slot into them (see fill_contractor_slots)
        org = organization_for(handler_faction, cm.group(1) if cm else "")

        # <defaultAvailability ... abandonedCooldownTime="45" ...> - how long
        # the contract is blocked, and whether it can be taken again at all
        avail = re.search(r"<defaultAvailability[^>]*>", part)
        cool_done = cool_abandon = 0
        once_only = False
        if avail:
            a = avail.group(0)

            def num(name):
                m = re.search(name + r'="(\d+)"', a)
                return int(m.group(1)) if m else 0

            cool_done = num("personalCooldownTime")
            cool_abandon = num("abandonedCooldownTime")
            once_only = 'onceOnly="1"' in a
            # canReacceptAfterAbandoning="0" does not mean "gone for good": all
            # 141 templates that carry it also carry an abandon cooldown of 1 or 15
            # minutes. It means "not right away", which the cooldown beside it
            # already says - so nothing is made of the flag, and only the cooldown
            # is carried.
            if 'hasPersonalCooldown="0"' in a:
                cool_done = 0
        # A handler holds <CareerContract> entries, <Contract> entries, or
        # both, so the split is at either. Splitting at CareerContract alone
        # glues every plain Contract onto the one before it, and the merged
        # block takes the FIRST title in it - a pool then lands under the
        # wrong contract.
        blocks = re.split(r"(?=<(?:Career)?Contract\s)", part)
        for block in blocks:
            _head = block[:block.find(">") + 1]
            _tk = ""
            _mt = re.search(r'<ContractStringParam param="Title" value="@?([\w]+)"', block)
            if _mt:
                _tk = _mt.group(1)
            else:
                _tm = re.search(r'template="([0-9a-f-]{36})"', _head)
                if _tm:
                    _tk = template_titles.get(_tm.group(1), "")
            _md = re.search(r'<ContractStringParam param="Description" value="@?([\w]+)"', block)
            all_records.append({"titleKey": _tk, "descKey": _md.group(1) if _md else "",
                                "debug": (re.search(r'debugName="([^"]*)"', block) or [None, ""])[1],
                                "standing": standing_names.get((re.search(r'minStanding="([0-9a-f-]{36})"', block) or [None, ""])[1], ""),
                                "bp": "blueprintPool" in block, "unreleased": 'notForRelease="1"' in _head})
            if "blueprintPool" not in block:
                continue
            # ...and what the game has not released is not a way to earn
            # anything. notForRelease="1" is the files' own word for it.
            head = block[:block.find(">") + 1]
            if 'notForRelease="1"' in head:
                continue
            # The title is a string parameter, not an attribute:
            #   <ContractStringParam param="Title" value="@..._title_001" />
            title_key = ""
            mt = re.search(r'<ContractStringParam param="Title" value="@?([\w]+)"', block)
            if mt:
                title_key = mt.group(1)
            else:
                # ...and when it names none, the template it is built on does
                tm = re.search(r'template="([0-9a-f-]{36})"', head)
                if tm:
                    title_key = template_titles.get(tm.group(1), "")
            # The briefing, and how long the contract gives you
            desc_key = ""
            md = re.search(r'<ContractStringParam param="Description" value="@?([\w]+)"', block)
            if md:
                desc_key = md.group(1)
            minutes = 0
            mt2 = re.search(r'timeToComplete="(\d+)"', block)
            if mt2:
                minutes = int(mt2.group(1))

            # <ContractDifficulty mechanicalSkill="Normal_PvE_only_action_4" ...>
            # Four axes, each ending in its step; the hardest of them is what
            # the contract is worth warning about.
            difficulty = 0
            axes = {}
            # Everything else worth knowing before accepting
            group_max = 0
            gm = re.search(r'maxGroupSize="(\d+)"', block)
            if gm:
                group_max = int(gm.group(1))
            dm = re.search(r"<ContractDifficulty[^>]*>", block)
            if dm:
                for axis in ("mechanicalSkill", "mentalLoad", "riskOfLoss", "gameKnowledge"):
                    am = re.search(axis + r'="([^"]*)"', dm.group(0))
                    if not am:
                        continue
                    step = re.search(r"_(\d+)$", am.group(1))
                    if step:
                        axes[axis] = int(step.group(1))
                if axes:
                    difficulty = max(axes.values())
            # One figure, the way the game weights it
            score = 0.0
            risk_words = ""
            if dm and axes:
                pm = re.search(r'difficultyProfile="([0-9a-f-]+)"', dm.group(0))
                weights = difficulty_profiles.get(pm.group(1) if pm else "", {})
                total = sum(weights.get(a, 0.0) for a in axes)
                if total > 0:
                    score = sum(axes[a] * weights.get(a, 0.0) for a in axes) / total
                else:
                    score = sum(axes.values()) / float(len(axes))
                rm = re.search(r'riskOfLoss="([^"]*)"', dm.group(0))
                risk_words = axis_words(rm.group(1)) if rm else ""

            # How many enemy ships the contract puts up at once. Which ships
            # they are is behind spawn tags the dump does not define, so the
            # count is all that can honestly be said.
            ships = [int(x) for x in re.findall(r'<SpawnDescription_Ship concurrentAmount="(\d+)"', block)]

            # <ContractGenerationParams_Legacy maxInstancesPerPlayer="1"
            #  respawnTime="5" respawnTimeVariation="2" /> - how long after a
            # run ends the contract is generated again, in minutes, give or
            # take the variation. This is the wait a player actually sees
            # after finishing one: hasPersonalCooldown is "0" on most of the
            # handlers, so the personal cooldown above says nothing for them.
            respawn = respawn_var = 0
            gp = re.search(r"<ContractGenerationParams_Legacy[^>]*>", block)
            if gp:
                rt = re.search(r'respawnTime="(\d+)"', gp.group(0))
                rv = re.search(r'respawnTimeVariation="(\d+)"', gp.group(0))
                respawn = int(rt.group(1)) if rt else 0
                respawn_var = int(rv.group(1)) if rv else 0

            # (A ladder for partly finished runs exists in the game files, but
            # only on the hauling templates - none of the contracts that hand
            # out blueprints carries one, so nothing is read for it here.)
            debug = (re.search(r'debugName="([^"]*)"', block) or [None, ""])[1]
            standing = (re.search(r'minStanding="([0-9a-f-]{36})"', block) or [None, ""])[1]
            need = requirement_of(block, handler_outfit, handler_scope)
            award_list = awards_of(block)
            faction = offering_outfit(block, need, handler_outfit) or handler_faction
            title = clean_title(title_key, org) or debug
            awards = reputation_of(block)
            illegal = "illegal" in (debug or "").lower() or "criminal" in (debug or "").lower()
            # A contract can pay out of several pools - Foxwell's Orange
            # "Protect ... and Escort Employees" in Nyx out of one of coolers
            # and one of repeaters, Eckhart's escorts out of three - and the
            # game's own briefing lists them as "Pool 1", "Pool 2". Each pool
            # makes rows of its own here, one per blueprint, so which pools
            # belong to one contract is kept by the contract's own id and
            # handed on (see otherPools below), or a card would show only one
            # pool of the contract.
            contract_id = (re.search(r'\sid="([0-9a-f-]{36})"', head) or [None, ""])[1] or (debug + "|" + path)
            _pools_here = []
            for _ch, _pl in re.findall(r'<BlueprintRewards chance="([\d.]*)"\s+blueprintPool="([0-9a-f-]{36})"', block):
                _entries, _pname = pools.get(_pl) or ([], "")
                _pools_here.append({"chance": float(_ch or 1), "poolName": _pname,
                                    "items": [guid_to_tag.get(g, "") for g, _w in _entries if guid_to_tag.get(g)]})
            contract_records.append({"debug": debug, "titleKey": title_key, "descKey": desc_key, "title": title,
                                     "faction": faction_names.get(faction, "") or handler_contractor,
                                     "standing": standing_names.get(standing, ""), "pools": _pools_here,
                                     "intro": "intro" in (debug or "").lower() or "intro" in (title_key or "").lower(),
                                     "onceOnly": once_only, "file": path})
            for _c, pool in re.findall(
                    r'<BlueprintRewards chance="([\d.]*)"\s+blueprintPool="([0-9a-f-]{36})"', block):
                contract_pools.setdefault(contract_id, [])
                if pool not in contract_pools[contract_id]:
                    contract_pools[contract_id].append(pool)
            for chance, pool in re.findall(
                    r'<BlueprintRewards chance="([\d.]*)"\s+blueprintPool="([0-9a-f-]{36})"', block):
                entries, pool_name = pools.get(pool) or ([], "")
                total = sum(w for _g, w in entries) or 1.0
                for guid, weight in entries:
                    tag = guid_to_tag.get(guid)
                    if not tag:
                        continue
                    # The contract's own name wins when it names a system:
                    # a PyroNyx pool serves both, but the contract using it
                    # may run in only one of them.
                    one = system_of(debug)
                    if one and one != "Multi":
                        where = [one]
                    else:
                        where = systems_of_pool(pool_name)
                    per_tag[tag].append({
                        "title": title,
                        "faction": faction_names.get(faction, "") or handler_contractor,
                        "systems": where,
                        "region": region_of(debug) or region_of(pool_name),
                        "standing": {"name": standing_names.get(standing, "")},
                        "type": "",
                        "description": clean_title(desc_key, org),
                        "minutes": minutes,
                        "chance": float(chance or 1),      # the contract's own roll
                        "poolSize": len(entries),          # how many blueprints share the pool
                        "difficulty": difficulty,
                        "score": round(score, 1),
                        "risk": risk_words,
                        "ships": max(ships) if ships else 0,
                        "axes": axes,
                        "illegal": illegal,
                        # The first rung of an outfit's ladder: the same
                        # standing as the one above it, but the game offers it
                        # to newcomers rather than everyone (Bit Zeros' "Data
                        # Transfer" beside "Pick Up, Put Down", both Neutral).
                        # Named for it in the files.
                        "intro": "intro" in (debug or "").lower() or "intro" in (title_key or "").lower(),
                        "repAwards": awards,
                        # the same, by GUID, for the planner's ranks
                        "repNeed": need,
                        "repAwardList": award_list,
                        "groupMax": group_max,
                        "poolName": pool_name,
                        "poolItems": [tag_name.get(guid_to_tag.get(g, ""), guid_to_tag.get(g, ""))
                                      for g, _w in entries if guid_to_tag.get(g)],
                        "contract": contract_id,
                        "pool": pool,
                        "cooldown": cool_done,
                        "abandonCooldown": cool_abandon,
                        "onceOnly": once_only,
                        "respawn": respawn,
                        "respawnVar": respawn_var,
                        "share": (float(chance or 1)) * weight / total,   # ordering only
                    })
print(f"{contract_files} contract files carry blueprint rewards")

out_rows = {}
for tag, rows in per_tag.items():
    folded = collections.OrderedDict()
    for r in rows:
        key = (r["title"], r["faction"])
        cur = folded.get(key)
        if cur:
            cur["count"] += 1
            cur["share"] = max(cur["share"], r["share"])
            # The same contract can run in several regions; collect them all
            for part in (r.get("region"), *r.get("systems", [])):
                if part and part not in cur["regions"]:
                    cur["regions"].append(part)
            if r.get("contract") and r["contract"] not in cur["contracts"]:
                cur["contracts"].append(r["contract"])
            cur["repVariants"].append({"standing": r["standing"]["name"], "awards": r.get("repAwards") or {}})
        else:
            folded[key] = dict(r, count=1,
                               regions=[x for x in (r.get("region"), *r.get("systems", [])) if x],
                               contracts=[r["contract"]] if r.get("contract") else [],
                               repVariants=[{"standing": r["standing"]["name"], "awards": r.get("repAwards") or {}}])
    # The other pools the contracts behind a row pay out of, each as the
    # blueprints it names - the row's own pool is poolItems
    for row in folded.values():
        others = []
        for cid in row.get("contracts", []):
            for pg in contract_pools.get(cid, []):
                if pg != row.get("pool") and pg not in others:
                    others.append(pg)
        row["otherPools"] = []
        for pg in others:
            entries, _name = pools.get(pg) or ([], "")
            names = [tag_name.get(guid_to_tag.get(g, ""), guid_to_tag.get(g, ""))
                     for g, _w in entries if guid_to_tag.get(g)]
            if names:
                row["otherPools"].append(names)
        row.pop("contracts", None)
        row.pop("contract", None)
        row.pop("pool", None)
        # what completing it awards, over every variant behind the row; the
        # variants themselves stay in the json, rank by rank, for whatever
        # comes to plan with them
        row.pop("repAwards", None)
        row["reputation"] = reputation_text(v["awards"] for v in row["repVariants"])
    # Every contract, so the choice is the player's. Sorted by employer, then
    # by the smallest pool (fewest blueprints to share with), then by name.
    ranked = sorted(folded.values(),
                    key=lambda x: (x["faction"], x.get("poolSize") or 99, x["title"]))
    out_rows[tag] = {
        "contracts": len(rows),
        "factions": len({r["faction"] for r in folded.values() if r["faction"]}),
        "sources": ranked,
    }

# What the outfits' own strings filled in, for the eye: every slot with the
# keys it took, and what is still a bare slot after all
if slot_log:
    print(f"{len(slot_log)} outfit slots filled:")
    for (outfit, slot), keys in slot_log.items():
        print(f"    {outfit:28s} {slot:36s} <- {', '.join(keys)}")
for w in slot_warnings:
    print("  WARNING " + w)
bare = collections.Counter()
for tag_rows in per_tag.values():
    for r in tag_rows:
        for field in ("title", "description"):
            for m in re.findall(r"\[CONTRACTOR\|(\w+)\]", r.get(field) or ""):
                bare[(r.get("faction") or "?", m)] += 1
if bare:
    print(f"{sum(bare.values())} rows still carry a bare outfit slot:")
    for (outfit, slot), n in bare.most_common():
        print(f"    {n:4d}  {outfit:28s} [CONTRACTOR|{slot}]")
else:
    print("no row is left with a bare outfit slot")

json.dump(out_rows, io.open(out, "w", encoding="utf-8"), indent=1)
print(f"{len(out_rows)} blueprints have contracts; wrote {out}")

if reference and os.path.exists(reference):
    ref = json.load(io.open(reference, encoding="utf-8"))
    both = set(ref) & set(out_rows)
    only_ref = set(ref) - set(out_rows)
    only_mine = set(out_rows) - set(ref)
    same_count = sum(1 for t in both if ref[t]["contracts"] == out_rows[t]["contracts"])
    print(f"against {os.path.basename(reference)}: {len(both)} blueprints in both "
          f"({same_count} with the same contract count), {len(only_ref)} only there, "
          f"{len(only_mine)} only here")
    for t in sorted(both)[:3]:
        print(f"  {t}: mine {out_rows[t]['contracts']} / ref {ref[t]['contracts']}")
        print(f"    mine: {out_rows[t]['sources'][0]['title'][:60]!r}")
        print(f"    ref : {ref[t]['sources'][0]['title'][:60]!r}")

for _var, _records in (("BP_CONTRACT_KEYS", contract_records), ("ALL_CONTRACT_KEYS", all_records)):
    _out = os.environ.get(_var, "")
    if _out:
        with open(_out, "w", encoding="utf-8") as _f:
            json.dump(_records, _f, ensure_ascii=False, indent=1)
        print("%d records written to %s" % (len(_records), _out))

# ---- every contract's rank and reputation ------------------------------------
# For the planner's ranks: the player's own logs say which contracts they
# took and finished, by the names the game gives them in
#   Creating objective marker: missionId [..], generator name [Covalex_Hauling],
#   contract [HaulCargo_AToB_Interstellar_Bulk_Plas_Scr_Cob]
# - the generator is the contract file's own record, the contract its
# debugName. Neither alone is enough: that debug name is a Master contract
# of Covalex's and a Rookie one of Red Wind's. Every contract of every file,
# with blueprints or without - a rank is earned on all of them.
rep_out = os.environ.get("REPUTATION_JSON", "")
if rep_out:
    rep_contracts = {}
    twice = 0
    handler_mark = re.compile(r"<ContractGeneratorHandler_\w+[^>]*>")
    for path in walk(os.path.join("contracts", "contractgenerator")):
        text = read(path)
        gen = re.match(r"\s*<ContractGenerator\.(\w+)", text)
        if not gen:
            continue
        marks = [m.start() for m in handler_mark.finditer(text)] or [0]
        for i, start in enumerate(marks):
            part = text[start:marks[i + 1] if i + 1 < len(marks) else len(text)]
            tag = handler_mark.match(part)
            tag = tag.group(0) if tag else ""
            h_outfit = (re.search(r'factionReputation="([0-9a-f-]{36})"', tag) or [None, ""])[1]
            h_scope = (re.search(r'reputationScope="([0-9a-f-]{36})"', tag) or [None, ""])[1]
            # the outfit's own strings, as the blueprint rows above get them:
            # the same contract has to come out under the same title here
            h_any = (re.search(r'factionReputation="([0-9a-f-]{36})"', part) or [None, ""])[1]
            h_contractor = re.search(r'<ContractStringParam param="Contractor" value="@?([\w]+)"', part)
            h_org = organization_for(h_any, h_contractor.group(1) if h_contractor else "")
            for block in re.split(r"(?=<(?:Career)?Contract\s)", part):
                if not re.match(r"<(?:Career)?Contract\s", block):
                    continue
                head = block[:block.find(">") + 1]
                if 'notForRelease="1"' in head:
                    continue
                debug = (re.search(r'debugName="([^"]*)"', head) or re.search(r'debugName="([^"]*)"', block)
                         or [None, ""])[1]
                if not debug:
                    continue
                outfit = h_outfit or (re.search(r'factionReputation="([0-9a-f-]{36})"', block) or [None, ""])[1]
                title_key = re.search(r'<ContractStringParam param="Title" value="@?([\w]+)"', block)
                title_key = title_key.group(1) if title_key else template_titles.get(
                    (re.search(r'template="([0-9a-f-]{36})"', head) or [None, ""])[1], "")
                rec = {"need": requirement_of(block, outfit, h_scope), "awards": awards_of(block),
                       "title": clean_title(title_key, h_org) or debug, "order": order_of(block, head)}
                key = gen.group(1) + "/" + debug
                old = rep_contracts.get(key)
                if old:
                    # the same names twice in one file: what the logs cannot
                    # tell apart counts at the lower rank, and the smaller award
                    twice += 1
                    lows = [standing_min.get(r["need"][2], 0) if r["need"] else -1 for r in (old, rec)]
                    keep = old if lows[0] <= lows[1] else rec
                    mine = {(f, s): a for f, s, a in rec["awards"]}
                    keep["awards"] = [[f, s, min(a, mine[(f, s)])] for f, s, a in old["awards"] if (f, s) in mine]
                    rep_contracts[key] = keep
                else:
                    rep_contracts[key] = rec
    scopes, outfits = set(), set()
    for rec in rep_contracts.values():
        if rec["need"] and rec["need"][1] in ladders:
            outfits.add(rec["need"][0])
            scopes.add(rec["need"][1])
        for f, s, _a in rec["awards"]:
            if s in ladders:
                outfits.add(f)
                scopes.add(s)
    with open(rep_out, "w", encoding="utf-8") as _f:
        json.dump({
            "ladders": {s: {"name": rep_scope.get(s, ""),
                            "ranks": [{"guid": r, "name": standing_names.get(r, ""), "min": standing_min[r],
                                       "gated": standing_gated.get(r, False)} for r in ladders[s]]}
                        for s in sorted(scopes)},
            "factions": {f: faction_names.get(f, "") for f in sorted(outfits)},
            "standings": {r: standing_min[r] for r in sorted(standing_min)},
            "contracts": [dict(key=k, **v) for k, v in sorted(rep_contracts.items())],
        }, _f, ensure_ascii=False, indent=1)
    asking = sum(1 for v in rep_contracts.values() if v["need"])
    orders = sum(1 for v in rep_contracts.values() if v["order"])
    print("%d contracts by generator and debug name (%d asked for twice), %d ask for a rank, %d are gathering "
          "orders; %d ladders, %d outfits written to %s" % (len(rep_contracts), twice, asking, orders, len(scopes),
                                                            len(outfits), rep_out))
