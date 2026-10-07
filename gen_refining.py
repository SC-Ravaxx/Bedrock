"""Generates include/RefiningData.h - what refining actually costs and yields.

    python gen_refining.py <regolith lookupMocks.json> <mining_data.json> <out.h>

THE FORMULA, all three of the same shape:

    yield = amount * ore.yield * station.yield[ore] * method.yield
    cost  = amount * ore.cost  * station.cost[ore]  * method.cost    -> aUEC
    time  = amount * ore.time  * station.time[ore]  * method.time    -> seconds

with amount in cSCU (1 SCU = 100 cSCU). Turned around, the amount of raw ore
needed for a wanted yield is that first line divided instead of multiplied,
which is what the shopping list works back through.

WHERE IT COMES FROM. Regolith Co.'s common library (github.com/RegolithCo/
RegolithCo-Common), MIT licensed, ships the equations and the lookup tables.
This takes the tables and the formula; it deliberately does NOT copy their
implementation, because yieldCalc() there reads the station entry as a plain
number while the table holds [yield, time, cost] - so the station's yield
bonus falls back to 1 and is silently lost. getRefiningCost and
getRefiningTime index [2] and [1], which is the consistent reading, and index
[0] for yield is confirmed independently: it matches the scmdb extract's yield
percentages on 82 of the 84 values the two sources share.

STATIONS. Regolith covers twelve, with all three factors. The scmdb extract
covers twenty, with the yield percentage only - it has the Pyro and Nyx
refineries Regolith does not. Both are used: Regolith where it reaches, the
extract elsewhere with time and cost left at 1.0 and marked as such, so the
window can say which figures are complete and which are yield-only.
"""
import io, json, os, re, sys, datetime

REG, MINING, OUT = sys.argv[1], sys.argv[2], sys.argv[3]

# Regolith's station keys against the extract's names. The gateways are matched
# only where the name says which side of the jump it stands on; MAGNG has no
# counterpart in the extract at all and is left out rather than guessed at.
STATION_KEY = {
    "ARCL1": "ARC-L1 Wide Forest Station",
    "ARCL2": "ARC-L2 Lively Pathway Station",
    "ARCL4": "ARC-L4 Faint Glen Station",
    "CRUL1": "CRU-L1 Ambitious Dream Station",
    "HURL1": "HUR-L1 Green Glade Station",
    "HURL2": "HUR-L2 Faithful Dream Station",
    "MICL1": "MIC-L1 Shallow Frontier Station",
    "MICL2": "MIC-L2 Long Forest Station",
    "MICL5": "MIC-L5 Modern Icarus Station",
    "PYROG": "Pyro Gateway (Stanton)",
    "TERRG": "Terra Gateway (Stanton)",
}

ORE_KEY = {
    "AGRICIUM": "Agricium (Ore)", "ALUMINUM": "Aluminum (Ore)", "BERYL": "Beryl (Raw)",
    "BEXALITE": "Bexalite (Raw)", "BORASE": "Borase (Ore)", "COPPER": "Copper (Ore)",
    "CORUNDUM": "Corundum (Raw)", "GOLD": "Gold (Ore)", "HEPHAESTANITE": "Hephaestanite",
    "IRON": "Iron (Ore)", "LARANITE": "Laranite (Raw)", "QUANTANIUM": "Quantainium (Raw)",
    "QUARTZ": "Quartz (Raw)", "TARANITE": "Taranite (Raw)", "TITANIUM": "Titanium (Ore)",
    "TUNGSTEN": "Tungsten (Ore)",
}

# The names and the ratings as the refinery terminal shows them. They do
# not agree with the numbers, and that is the game's doing, not a mistake
# here: the terminal calls Dinyx Solventation "High Cost" while it is the
# cheapest of the nine, and Gaskin "Moderate Cost" while it is the dearest.
# global.ini is no guide here: it has paired the same nine names with
# ratings that DO match the numbers. The app shows what the terminal shows;
# the figures beside them are measured rather than described.
METHOD_NAMES = {
    "PYROMETRIC_CHROMALYSIS": ("Pyrometric Chromalysis", "Very Low Speed // Low Cost // High Yield"),
    "GASKIN_PROCESS":         ("Gaskin Process",         "Low Speed // Moderate Cost // High Yield"),
    "XCR_REACTION":           ("XCR Reaction",           "High Speed // High Cost // Low Yield"),
    "FERRON_EXCHANGE":        ("Ferron Exchange",        "Low Speed // Low Cost // Moderate Yield"),
    "ELECTROSTAROLYSIS":      ("Electrostarolysis",      "Moderate Speed // Low Cost // Low Yield"),
    "CORMACK":                ("Cormack Method",         "High Speed // Moderate Cost // Low Yield"),
    "DINYX_SOLVENTATION":     ("Dinyx Solventation",     "Low Speed // High Cost // High Yield"),
    "THERMONATIC_DEPOSITION": ("Thermonatic Deposition", "High Speed // High Cost // Moderate Yield"),
    "KAZEN_WINNOWING":        ("Kazen Winnowing",        "Moderate Speed // Moderate Cost // Moderate Yield"),
}

# --- calibrated against a real work order -------------------------------
#
# Nine work orders of the same load at Levski: aluminium in three qualities,
# 2263 + 996 + 426 units, run through every method in turn. From them:
#
#   yield = qty * 0.45 * station.yield * method.yield
#
# reproduces all 27 observed figures exactly, Levski's +8 % on aluminium
# included - which is also what proves the station factor applies at all.
# Regolith's tables carry 0.85 as the ore base; the game is at 0.45. Their
# method factors, all three sets, still hold: 1 / 0.85 / 0.70 for yield,
# 1 / 2 / 6 for cost, and nine time steps from 0.125 to 12.
ORE_YIELD_BASE = 0.45

# Cost and time are measured for ONE ore. Aluminium comes out at 0.052917
# aUEC and 0.74654 s per unit at method factor 1, against 0.08 and 2.0 in
# Regolith's tables. The other ores keep their proportions from those tables
# and are scaled by the same amount - a guess, but a bounded one, and the
# only alternative is eighteen more work orders. ore.known is true only
# where the tables have the ore.
COST_SCALE = 0.052917 / 0.08
TIME_SCALE = 0.74654 / 2.0
# The order the game lists them in: slowest and kindest first
METHOD_ORDER = ["DINYX_SOLVENTATION", "FERRON_EXCHANGE", "PYROMETRIC_CHROMALYSIS",
                "THERMONATIC_DEPOSITION", "ELECTROSTAROLYSIS", "GASKIN_PROCESS",
                "KAZEN_WINNOWING", "CORMACK", "XCR_REACTION"]

def flt(x):
    """A C++ float literal. %g alone yields "1" for 1.0, and "1f" is not a
    valid literal - the compiler stops on it with C3688."""
    s = "%g" % float(x)
    if "." not in s and "e" not in s and "E" not in s:
        s += ".0"
    return s + "f"


lut = json.load(io.open(REG, encoding="utf-8"))["data"]["lookups"]["CIG"]
methods, oreproc, stations = lut["methodsBonusLookup"], lut["oreProcessingLookup"], lut["refineryBonusLookup"]

scm = json.load(io.open(MINING, encoding="utf-8"))
prof = scm["refineryProfiles"]

# The stations and the materials are READ OUT of RefineryData.h rather than
# sorted the same way again here. These rows are indexed by a REFINERIES
# index and a REFINERY_MATERIALS index; deriving them separately lets the two
# generators drift apart (a station pointing at another station's numbers,
# an ore one knows and the other does not). One list, read.
REFINERY_H = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "include", "RefineryData.h")
header = io.open(REFINERY_H, encoding="utf-8").read()


def between(start, end):
    a = header.index(start) + len(start)
    return header[a:header.index(end, a)]


mats = re.findall(r'"([^"]+)"', between("REFINERY_MATERIALS[] = {", "\n};"))
names = re.findall(r'\{"([^"]+)",\s*"([^"]+)",\s*(\d+)\}',
                   between("REFINERIES[] = {", "\n};"))
byname = {r["name"]: r for r in scm["refineries"]}
refs = [byname[n] for n, _system, _p in names]
if len(mats) < 20 or len(refs) < 10:
    raise SystemExit("could not read the lists out of %s" % REFINERY_H)

lines = [
    "#pragma once",
    "// What refining costs, yields and takes - GENERATED by gen_refining.py.",
    "// Pulled %s. Do not edit by hand." % datetime.date.today().isoformat(),
    "//",
    "//   yield = amount * ore.yield * station.yield * method.yield",
    "//   cost  = amount * ore.cost  * station.cost  * method.cost    -> aUEC",
    "//   time  = amount * ore.time  * station.time  * method.time    -> seconds",
    "//",
    "// The amount is in the units the refinery terminal counts in - the same",
    "// figure it prints under TO REFINE. The yield comes out in those units",
    "// too; the cost is aUEC and the time seconds.",
    "//",
    "// Calibrated against nine work orders at Levski, one per",
    "// method, on the same load of aluminium. All 27 observed yields come",
    "// back exactly. The cost and the time are measured for aluminium only;",
    "// the other ores keep their old proportions, scaled to match it.",
    "//",
    "// Where Regolith holds [1, 2, 2] for a station and an ore it has",
    "// nothing on, only the yield of 1.0 is taken; the 2x time and cost",
    "// are its filler for the gap, not the game's doing - the 15 cells",
    "// that share the same neutral yield and DO carry numbers sit",
    "// between 0.89 and 1.18.",
    "//",
    "// Sources: the equations and factor tables from Regolith Co.'s common",
    "// library (MIT), cross-checked against the scmdb game-data extract - 82 of",
    "// the 84 station values the two share are identical. The extract reaches",
    "// eight refineries Regolith does not, and for those only the yield is",
    "// known; `full` says which is which.",
    "#include <cstdint>",
    "",
    'namespace SCMiner {',
    "",
    "inline constexpr float CSCU_PER_SCU = 100.0f;",
    "",
    "struct RefiningMethod {",
    "    const char* key;      // as the game files spell it",
    "    const char* name;     // as the refinery terminal shows it",
    "    const char* rating;   // the game's own words for it",
    "    float yield, time, cost;",
    "};",
    "",
    "inline const RefiningMethod REFINING_METHODS[] = {",
]
for k in METHOD_ORDER:
    y, t, c = methods[k]
    name, rating = METHOD_NAMES[k]
    lines.append('    {"%s", "%s", "%s", %s, %s, %s},' % (k, name, rating, flt(y), flt(t), flt(c)))
lines += ["};",
          "inline constexpr int REFINING_METHOD_COUNT = %d;" % len(METHOD_ORDER),
          "",
          "// Per ore, in the order of REFINERY_MATERIALS. The yield base is the",
          "// same 0.45 for every ore - measured, and the tables this came from",
          "// carried one constant too. known=false means the COST and the TIME",
          "// are not published for it; the yield still holds.",
          "struct OreRefining {",
          "    float yield, time, cost;",
          "    bool known;",
          "};",
          "",
          "inline const OreRefining ORE_REFINING[] = {"]
for m in mats:
    key = next((k for k, v in ORE_KEY.items() if v == m), None)
    if key and key in oreproc:
        y, t, c = oreproc[key]
        lines.append("    {%s, %s, %s, true},   // %s" %
                     (flt(ORE_YIELD_BASE), flt(t * TIME_SCALE), flt(c * COST_SCALE), m))
    else:
        # The yield base is not per-ore: 0.45 was measured on aluminium and
        # Regolith's tables carry one constant for all eighteen ores they
        # cover, so it applies here too. What is missing for these is the
        # cost and the time, and `known` says exactly that.
        lines.append("    {%s, 1.0f, 1.0f, false},   // %s - yield only" % (flt(ORE_YIELD_BASE), m))
lines += ["};", "",
          "// Per station, per ore. Row order follows REFINERIES.",
          "struct StationRefining {",
          "    float yield, time, cost;",
          "};",
          "",
          "// true where all three factors are known, false where only the yield is",
          "inline const bool STATION_FULL[] = {"]
full_flags = []
for r in refs:
    key = next((k for k, v in STATION_KEY.items() if v == r["name"]), None)
    full_flags.append(bool(key and key in stations))
    lines.append("    %s,   // %s" % ("true " if full_flags[-1] else "false", r["name"]))
lines += ["};", "",
          "// Which station each row belongs to, so the self-test can prove the",
          "// rows still line up with REFINERIES rather than assuming it",
          "inline const char* const STATION_REFINING_NAME[] = {"]
for r in refs:
    lines.append('    "%s",' % r["name"])
lines += ["};", "",
          "inline const StationRefining STATION_REFINING[][%d] = {" % len(mats)]
for r, isfull in zip(refs, full_flags):
    key = next((k for k, v in STATION_KEY.items() if v == r["name"]), None)
    cells = []
    for m in mats:
        okey = next((k for k, v in ORE_KEY.items() if v == m), None)
        if isfull and okey and okey in stations[key]:
            y, t, c = stations[key][okey]
            # [1, 2, 2] is Regolith's filler for a cell it has no numbers
            # for, not a rule of the game. It sits on exactly the 47
            # station/ore pairs the game extract also leaves out - but 15
            # OTHER cells share the same 'no yield preference' (yield
            # exactly 1.0) and carry real time and cost, between 0.89 and
            # 1.18. A doubling would have to show up there too and does
            # not, and no refinery has been seen in game charging twice for
            # an ore it does not specialise in. So the yield stands and the
            # other two go back to neutral: the comparable cells put the
            # true value within about a tenth of 1.0, which beats being
            # wrong by a factor of two.
            if [y, t, c] == [1, 2, 2]:
                t = c = 1.0
            cells.append("{%s,%s,%s}" % (flt(y), flt(t), flt(c)))
        else:
            # the extract's percentage where there is one, and 1.0 for the two
            # factors it does not carry. A material no profile mentions at all
            # lands here too, at the standard rate - which is what an absent
            # entry means, confirmed against Regolith on all 60 blanks.
            pct = prof[r["profileId"]].get(m, 0)
            cells.append("{%s,1.0f,1.0f}" % flt(1.0 + pct / 100.0))
    lines.append("    {%s},  // %s" % (", ".join(cells), r["name"]))
lines += ["};", "",
          "// One refining job. Amount in cSCU; the yield comes back in cSCU too.",
          "struct RefiningResult {",
          "    float yield;      // cSCU out",
          "    float cost;       // aUEC",
          "    float seconds;    // how long the work order runs",
          "    bool complete;    // false when a factor had to be assumed",
          "};",
          "",
          "inline RefiningResult refine(float cscu, int station, int ore, int method) {",
          "    RefiningResult r{0.0f, 0.0f, 0.0f, false};",
          "    if (station < 0 || station >= REFINERY_COUNT) return r;",
          "    if (ore < 0 || ore >= REFINERY_MATERIAL_COUNT) return r;",
          "    if (method < 0 || method >= REFINING_METHOD_COUNT) return r;",
          "    const OreRefining& o = ORE_REFINING[ore];",
          "    const StationRefining& s = STATION_REFINING[station][ore];",
          "    const RefiningMethod& m = REFINING_METHODS[method];",
          "    r.yield = cscu * o.yield * s.yield * m.yield;",
          "    r.cost = cscu * o.cost * s.cost * m.cost;",
          "    r.seconds = cscu * o.time * s.time * m.time;",
          "    r.complete = o.known && STATION_FULL[station];",
          "    return r;",
          "}",
          "",
          "// Turned round: how much raw ore has to go in for a wanted yield. This",
          "// is what a shopping list needs - it knows what the recipes want out.",
          "inline float oreNeeded(float wantedCscu, int station, int ore, int method) {",
          "    if (station < 0 || station >= REFINERY_COUNT) return 0.0f;",
          "    if (ore < 0 || ore >= REFINERY_MATERIAL_COUNT) return 0.0f;",
          "    if (method < 0 || method >= REFINING_METHOD_COUNT) return 0.0f;",
          "    const float f = ORE_REFINING[ore].yield * STATION_REFINING[station][ore].yield *",
          "                    REFINING_METHODS[method].yield;",
          "    if (f <= 0.0f) return 0.0f;",
          "    return wantedCscu / f;",
          "}",
          "",
          "} // namespace SCMiner"]

io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
print("%s: %d methods, %d ores, %d stations (%d with all three factors)" %
      (OUT, len(METHOD_ORDER), len(mats), len(refs), sum(full_flags)))
