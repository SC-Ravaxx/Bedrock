"""Wikelo's trades, straight from the game files, for the Wikelo plugin.

    gen_wikelo_from_p4k.py <extract-dir> <global.ini> <wikelo.json> [game label] [resource_guids.json]

resource_guids.json: default the repository's root.

Wikelo (the Banu collector, "TheCollector" in the files) trades things for
things: bring him so many of this and that, and he hands over a weapon, a set
of armour or a ship. Every trade is a contract of his generator:

    records/contracts/contractgenerator/thecollector.xml
        <Contract debugName="TheCollector_apar_hmg" notForRelease="0" template="T">
          <ContractStringParam param="Title" value="@TheCollector_..._Title" />
          <HaulingOrderContent_EntityClass entityClass="E" minAmount="1" />   what he wants
          <HaulingOrderContent_Resource resource="R" minSCU="24" />           ... or a commodity
          <ContractResult_Item entityClass="E" amount="1" />                  what he gives
          <ContractResult_ItemsWeighting> ... <ItemAwardEntityClass ... />    ... a set of things
          <ContractPrerequisite_CompletedContractTags> ... tags              after another trade
    records/contracts/contracttemplates/<T>.xml
          <HaulingOrder_EntityClass entityClass="E" minAmount="16" />         what he wants, where
                                                                              the contract names none
    records/entities/**, records/actor/actors/*.xml
          <... __ref="E"> <Localization Name="@item_Name..." />              the thing's name

The debug name is what Game.log knows a trade by when the player takes it:

    <CLocalMissionPhaseMarker::CreateMarker> ... generator name [TheCollector],
        contract [TheCollector_apar_hmg] ...
    <SHUDEvent_OnNotification> Added notification "New Objective: Need 0/1 of
        Vendetta HMG: " ... MissionId: [...]

- and the names here are the game's own, as the log writes them. 4.10.2 LIVE
has 71 trades released, every one asking for things - 101 different ones, by
name. One asks for a commodity by the SCU (24 SCU of Quantainium for a
Polaris bit): resources live server-side, resource_guids.json names the mined
ones. Trades marked notForRelease are left out. A trade that only comes after
another (the favours after the intro) says so in "after".
"""
import io, json, os, re, sys

extract, ini_path, out = sys.argv[1], sys.argv[2], sys.argv[3]
label = sys.argv[4] if len(sys.argv) > 4 else ""
guids_path = sys.argv[5] if len(sys.argv) > 5 else ""
records = os.path.join(extract, "Data", "libs", "foundry", "records")


def read(path):
    return io.open(path, encoding="utf-8", errors="replace").read()


# ---- names ---------------------------------------------------------------
loc = {}
for line in io.open(ini_path, encoding="utf-8", errors="replace"):
    k, sep, v = line.partition("=")
    if sep and "," in k:
        loc.setdefault(k.split(",")[0].strip().lower(), v.strip())
    if sep:
        loc[k.strip().lower()] = v.strip()


def text_of(key):
    """The game's words for a key, its markup and placeholders cleaned up:
    <EM4>...</EM4> goes, "\\n" becomes a line break, ~mission(...) a gap."""
    raw = loc.get((key or "").lstrip("@").lower(), "")
    raw = raw.replace("\\n", "\n").replace("\u00a0", " ")
    raw = re.sub(r"<[^>]+>", "", raw)
    raw = re.sub(r"~\w+\([^)]*\)", "", raw)
    lines = [" ".join(part.split()) for part in raw.split("\n")]
    return "\n".join(lines).strip()


_mined_path = guids_path or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "resource_guids.json")
mined_names = json.load(io.open(_mined_path, encoding="utf-8")) if os.path.exists(_mined_path) else {}
if not mined_names:
    print(f"no resource_guids.json at {_mined_path}: commodities go without a name")


# ---- the trades ----------------------------------------------------------
generator = read(os.path.join(records, "contracts", "contractgenerator", "thecollector.xml"))
contracts = re.findall(r'(<Contract id="[^"]+"[^>]*>)(.*?)</Contract>', generator, re.S)

templates = {}
for base, _dirs, files in os.walk(os.path.join(records, "contracts", "contracttemplates")):
    for f in files:
        if f.lower().endswith(".xml"):
            body = read(os.path.join(base, f))
            m = re.search(r'__ref="([0-9a-f-]{36})"', body[:4000])
            if m:
                templates[m.group(1)] = body


def attribute(head, name):
    m = re.search(r'\s' + name + r'="([^"]*)"', head)
    return m.group(1) if m else ""


def string_param(block, template, param):
    for source in (block, template):
        m = re.search(r'<ContractStringParam param="' + param + r'" value="([^"]*)"', source)
        if m:
            return m.group(1)
    return ""


def wanted_of(block, template):
    """[[guid, amount, "item" or "scu"]] - the contract's own list, else its
    template's: a trade names its things in one of the two"""
    rows = [[g, int(float(n)), "item"] for g, n in
            re.findall(r'<HaulingOrderContent_EntityClass entityClass="([0-9a-f-]{36})" minAmount="([\d.]+)"', block)]
    rows += [[g, int(float(n)), "scu"] for g, n in
             re.findall(r'<HaulingOrderContent_Resource resource="([0-9a-f-]{36})"[^>]*?minSCU="([\d.]+)"', block)]
    if not rows:
        rows = [[g, int(float(n)), "item"] for g, n in
                re.findall(r'<HaulingOrder_EntityClass entityClass="([0-9a-f-]{36})" minAmount="([\d.]+)"', template)]
    return rows


def given_of(block):
    """[[guid, amount]] - every thing the trade hands over. A weighting with
    one group gives the whole group (a set of armour comes as five pieces)."""
    rows = [[g, int(n)] for g, n in re.findall(r'<ContractResult_Item entityClass="([0-9a-f-]{36})" amount="(\d+)"', block)]
    rows += [[g, int(n)] for n, g in
             re.findall(r'<ItemAwardEntityClass amountToAward="(\d+)" entityClass="([0-9a-f-]{36})"', block)]
    return rows


trades = []
tag_givers = {}   # a completion tag -> the trades that award it
for head, block in contracts:
    debug = attribute(head, "debugName")
    for tag in re.findall(r'<ContractResult_CompletionTag\b[^>]*?\stag="([0-9a-f-]{36})"', block):
        tag_givers.setdefault(tag, []).append(debug.strip())
    if attribute(head, "notForRelease") != "0":
        continue
    template = templates.get(attribute(head, "template"), "")
    needs_tags = []
    for pre in re.findall(r"<ContractPrerequisite_CompletedContractTags\b.*?</ContractPrerequisite_CompletedContractTags>",
                          block, re.S):
        required = re.search(r"<requiredCompletedContractTags>(.*?)</requiredCompletedContractTags>", pre, re.S)
        if required:
            needs_tags += re.findall(r'value="([0-9a-f-]{36})"', required.group(1))
    trades.append({
        "id": debug.strip(),
        "title": text_of(string_param(block, template, "Title")),
        "description": text_of(string_param(block, template, "Description")),
        "wants": wanted_of(block, template),
        "gives": given_of(block),
        "tags": needs_tags,
    })
print(f"{len(contracts)} contracts of Wikelo's, {len(trades)} released")

# ---- what the things are called --------------------------------------------
# Only the files of the things asked for or given are read whole: the
# entities are over a hundred thousand files, their reference in the first
# line of each.
guids = {row[0] for t in trades for row in t["wants"] if row[2] == "item"} | {row[0] for t in trades for row in t["gives"]}
names = {}
for sub in (os.path.join(records, "entities"), os.path.join(records, "actor", "actors")):
    for base, _dirs, files in os.walk(sub):
        for f in files:
            if not f.lower().endswith(".xml"):
                continue
            path = os.path.join(base, f)
            with io.open(path, encoding="utf-8", errors="replace") as fh:
                head = fh.read(1500)
            m = re.search(r'__ref="([0-9a-f-]{36})"', head)
            if not m or m.group(1) not in guids or m.group(1) in names:
                continue
            body = read(path)
            n = re.search(r'<Localization\b[^>]*?\sName="([^"]*)"', body)
            names[m.group(1)] = text_of(n.group(1)) if n else ""
print(f"{len(names)} of {len(guids)} things named")

unnamed = set()


def thing(guid, amount, unit="item"):
    name = names.get(guid, "") if unit == "item" else mined_names.get(guid, "")
    if not name:
        unnamed.add(guid)
    return {"name": name, "amount": amount, **({"unit": "SCU"} if unit == "scu" else {})}


out_trades = []
for t in trades:
    after = sorted({giver for tag in t["tags"] for giver in tag_givers.get(tag, []) if giver != t["id"]})
    out_trades.append({
        "id": t["id"],
        "title": t["title"],
        "description": t["description"],
        "wants": [thing(g, n, u) for g, n, u in t["wants"]],
        "gives": [thing(g, n) for g, n in t["gives"]],
        **({"after": after} if after else {}),
    })

result = {
    "game": label,
    "trades": out_trades,
}
with io.open(out, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(result, fh, ensure_ascii=False, indent=1)
    fh.write("\n")
asking = [t for t in out_trades if t["wants"]]
things = {w["name"] for t in out_trades for w in t["wants"]}
print(f"{len(out_trades)} trades, {len(asking)} asking for things ({len(things)} different), "
      f"{len(unnamed)} things without a name -> {out}")
