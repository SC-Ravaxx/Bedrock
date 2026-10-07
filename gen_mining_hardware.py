"""Builds the mining hardware table out of the game files - lasers, modules,
gadgets, the ores' own physics, and the constants breaking runs on.

    gen_mining_hardware.py <extract-dir> <global.ini> <out.h>

Everything comes from the unpacked game database, the same way the crafting
tables do. Five places carry it:

  mining/miningglobalparams*.xml
      the constants: window size, fill rates, the instability wave

  mining/mineableelements/*.xml
      each ore's own physics - resistance, instability, where its optimal
      window sits, and how much of it survives shattering

  entities/scitem/ships/weapons/mining_laser_*.xml
      SWeaponActionFireBeamParams, one block per fire mode
        hitType="ElectricArc"   breaking     DamageEnergy = the fracture power
        hitType="Extraction"    drawing in
        fullDamageRange / zeroDamageRange    the distance falloff
      SEntityComponentMiningLaserParams/miningLaserModifiers
        the laser's own percentages on window, rate and resistance

  entities/scitem/ships/utility/mining/miningarm/*.xml        the modules
      ItemWeaponModifiersParams/weaponStats @damageMultiplier   laser power
        ...and in the block for fireActionIndex 1, the drawing-in beam:
        what the HUD calls Extraction Rate
      ItemMiningModifierParams/MiningLaserModifier              the rest

  entities/scitem/weapons/devices/mining_gadget_*.xml         the gadgets
      ItemMineableRockModifierParams/MiningLaserModifier
        these attach to the ROCK, so they change what is in front of you
        rather than what you are holding

  mining/rockcompositionpresets/{asteroid,surface}shipmining/*.xml
      what a rock of each ore is made of, part by part:
        <MineableCompositionPart mineableElement=".." minPercentage="5.7"
            maxPercentage="11.7" probability="1" qualityScale="1" />
      and crafting/qualityquantization/quantization_<material>.xml, the eight
      values a material's quality comes out at (see rocks() below)

Power and the percentages live in different blocks because they are different
kinds of number: damageMultiplier is a plain factor (1.25), the modifiers are
percentages (-1). Checked against a screenshot: a Helix I with Focus III and
Rieger-C3 prints 119 fracture power and 83 window size, which is
100 x 0.95 x 1.25 and 100 x 0.60 x 1.40 x 0.99.

A note on the cluster factor, because the name misleads: it is not how the
rock splits. The gadgets' own descriptions say what it is - OptiMax "reduces
the loss of valuable resource ... keeps them clustered together", BoreMax
"keeping clusters of valuable deposits together upon shattering", and Okunis
at -20% warns that "some valuable resource will be lost when it shatters". It
is how much ore survives the break, and Riccite and Sileron sit at zero.

The descriptions come through as well, and they do not always agree with the
records: OptiMax's text says resistance -30% where its record says -25. The
record is what the game runs on, so that is what is written here.
"""
import io, os, re, sys, glob

extract, ini_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
records = os.path.join(extract, "Data", "libs", "foundry", "records")


def read(path):
    return io.open(path, encoding="utf-8", errors="replace").read()


# --- localisation, for the names and blurbs players see --------------------
NAMES = {}
for line in io.open(ini_path, encoding="utf-8", errors="replace"):
    if "=" in line:
        key, _, value = line.partition("=")
        NAMES[key.strip().lower()] = value.strip()


def attr(text, name, default=""):
    m = re.search(r'\b%s="([^"]*)"' % name, text)
    return m.group(1) if m else default


def localised(key, default=""):
    if not key:
        return default
    return NAMES.get(key.lstrip("@").lower(), default)


def localisation(text):
    """Name and description out of the entity's own <Localization> block.
    Reading any Name= attribute in the file instead picks up whatever
    component happens to carry one - which is how the MPUV's arm first came
    out called an Arbor."""
    for m in re.finditer(r"<Localization\b([^>]*)>", text):
        block = m.group(1)
        name = localised(attr(block, "Name"))
        if name and not name.startswith("@"):
            desc = localised(attr(block, "Description"))
            if desc.startswith("@"):
                desc = ""
            return name, desc
    return "", ""


def presentable(stem):
    parts = [p for p in stem.split("_") if p not in ("passive", "active")]
    return " ".join(p.upper() if len(p) <= 3 else p.capitalize() for p in parts)


def blurb(description):
    """The prose at the end of a gadget's description - what it is for, without
    the stat list the game prints above it."""
    text = description.replace("\\n", "\n")
    lines = [p.strip() for p in text.split("\n") if p.strip()]
    prose = [p for p in lines if len(p) > 80 and ":" not in p[:28]]
    if not prose:
        return ""
    return " ".join(re.split(r"Manufacturer Warning", prose[0])[0].split())


def modifiers(text, start_tag, wanted):
    """The <xModifier><FloatModifierMultiplicative value="n"/></xModifier> pairs
    inside one block. Some are spelled without the Modifier suffix
    (laserInstability), so both spellings are accepted."""
    out = {k: 0.0 for k in wanted}
    i = text.find(start_tag)
    if i < 0:
        return out
    block = text[i:i + 1400]
    for m in re.finditer(r"<(\w+?)(?:Modifier)?>\s*<Float\w+\s+[^>]*value=\"([-\d.]+)\"", block):
        key = m.group(1)
        if key in out:
            out[key] = float(m.group(2))
    return out


# --- the constants ---------------------------------------------------------
def constants():
    s = read(os.path.join(records, "mining", "miningglobalparamsship.xml"))
    wanted = ("powerCapacityPerMass", "decayPerMass", "optimalWindowSize",
              "optimalWindowFactor", "resistanceCurveFactor",
              "optimalWindowThinnessCurveFactor", "optimalWindowMaxSize",
              "controlledBreakingFillRate", "controlledBreakingFillRateDanger",
              "controlledBreakingDecayRate", "dangerBreakingFillRate",
              "dangerBreakingFillRateExponent", "dangerBreakingDecayRate")
    out = {k: attr(s, k, "0") for k in wanted}
    inst = re.search(r"<mineableInstabilityParams([^/>]*)", s)
    if inst:
        for k in ("instabilityWavePeriod", "instabilityWaveVariance",
                  "instabilityCurveFactor"):
            out[k] = attr(inst.group(1), k, "0")
    return out


# --- each ore's own physics ------------------------------------------------
def elements():
    found = []
    folder = os.path.join(records, "mining", "mineableelements")
    for path in sorted(glob.glob(os.path.join(folder, "*.xml"))):
        stem = os.path.basename(path)[:-4]
        if "template" in stem or "testelement" in stem:
            continue
        s = read(path)

        def number(key):
            value = attr(s, key, "")
            return float(value) if value else 0.0

        found.append({
            "id": stem,
            "instability": number("elementInstability"),
            "resistance": number("elementResistance"),
            "windowMid": number("elementOptimalWindowMidpoint"),
            "windowRandom": number("elementOptimalWindowMidpointRandomness"),
            "windowThin": number("elementOptimalWindowThinness"),
            "explosion": number("elementExplosionMultiplier"),
            "cluster": number("elementClusterFactor"),
        })
    return found


# --- what a rock is made of, and at what quality ------------------------------
# Since the 4.8 rework a ship-mining rock is a rock OF an ore, built from
# parts. A Quantainium rock:
#     Quantainium   5.7 - 11.7 %   qualityScale 1
#     Quantainium  28.3 - 78.3 %   qualityScale 0.49
#     Beryl           5 - 10 %     qualityScale 0.789
# and the rest inert. The scan panel lists exactly these lines, each with its
# share and its quality ("10.99% TIN (ORE) 537 / 63.84% TIN (ORE) 340 / 8.38%
# COPPER (ORE) 652" on a Tin rock). A part's quality is a roll
# times its scale, handed out as the value of the band it falls in - the
# bands' edges are the same for all 63 materials (0, 400, 600, 700, 800, 900,
# 950, 999), their values each material's own (Iron: 325, 521, 664, 710, 874,
# 907, 970, 1000). The roll is no flat 0..1000: a normal of mean 500 cut to
# 501..1000, wider the rarer the rock (crafting/qualitydistribution/
# shipmineables: 143 for a common rock, 203 for a legendary one). So the small
# part is 501 and up, mostly just over it, and the bulk of the ore 501 times
# 0.49 and up - almost always the lowest band; observed scan lists show
# none outside it. The asteroid and the surface presets say the same for
# every ore; they are written once.
ORE_NAME = {"sileron": "Stileron", "ice": "Ice"}     # the files' own slip, and a name the tables shorten


def ore_name(element_stem):
    """aluminium_ore -> Aluminium, quantainium_raw -> Quantainium, the way the
    signature table spells them"""
    bare = re.sub(r"_(ore|raw)$", "", element_stem.lower())
    return ORE_NAME.get(bare, bare.capitalize())


def rolls():
    """the default quality roll of each kind of rock: (least, most, mean, spread)"""
    found = {}
    folder = os.path.join(records, "crafting", "qualitydistribution", "shipmineables")
    for path in glob.glob(os.path.join(folder, "*shipmineable_qualitydistribution_default.xml")):
        tier = os.path.basename(path).split("shipmineable_")[0]
        m = re.search(r'<CraftingQualityDistributionNormal min="([\d.]+)" max="([\d.]+)" mean="([\d.]+)" '
                      r'stddev="([\d.]+)"', read(path))
        if m:
            found[tier] = tuple(float(v) for v in m.groups())
    return found


def rocks():
    by_ref = {}
    for path in glob.glob(os.path.join(records, "mining", "mineableelements", "*.xml")):
        ref = re.search(r'__ref="([0-9a-f-]{36})"', read(path))
        if ref:
            by_ref[ref.group(1)] = os.path.basename(path)[:-4]
    found = {}
    for family in ("asteroidshipmining", "surfaceshipmining"):
        folder = os.path.join(records, "mining", "rockcompositionpresets", family)
        for path in sorted(glob.glob(os.path.join(folder, "*.xml"))):
            if "template" in os.path.basename(path):
                continue
            parts = []
            for g, lo, hi, scale in re.findall(
                    r'<MineableCompositionPart mineableElement="([0-9a-f-]{36})" minPercentage="([\d.]+)" '
                    r'maxPercentage="([\d.]+)"[^>]*?qualityScale="([\d.]+)"', read(path)):
                if g in by_ref:
                    parts.append((ore_name(by_ref[g]), float(lo), float(hi), float(scale)))
            if not parts:
                continue
            # The rock is its first part's ore. One file per ore and family;
            # the RCD variant of Savrilium (a contested-zone rock) is left to
            # the plain one.
            if "_rcd" in os.path.basename(path).lower():
                continue
            # ...and its kind, common to legendary, says how its quality rolls
            tier = os.path.basename(path).lower().split("shipmineables")[0]
            roll = ROLLS.get(tier)
            if not roll:
                sys.exit("no quality roll for a %s rock (%s)" % (tier, path))
            parts = [p + roll for p in parts]
            rock = parts[0][0]
            if rock in found and found[rock] != parts:
                print("  WARNING %s: the %s preset differs from the other family's" % (rock, family))
            found.setdefault(rock, parts)
    return found


def quality_bands():
    found = {}
    folder = os.path.join(records, "crafting", "qualityquantization")
    for path in sorted(glob.glob(os.path.join(folder, "quantization_*.xml"))):
        values = [int(float(v)) for v in re.findall(
            r'<CraftingQualityQuantizationBand[^>]*?mappedValue="([\d.]+)"', read(path))]
        edges = [int(v) for v in re.findall(r'<CraftingQualityQuantizationBand start="(\d+)"', read(path))]
        if len(values) == 8:
            found[os.path.basename(path)[len("quantization_"):-4]] = (values, edges)
    return found


# --- the lasers ------------------------------------------------------------
def lasers():
    found = []
    folder = os.path.join(records, "entities", "scitem", "ships", "weapons")
    for path in sorted(glob.glob(os.path.join(folder, "mining_laser_*.xml"))):
        stem = os.path.basename(path)[:-4]
        # The MPUV's arm is not a head anyone fits to a mining ship - it is
        # part of a different vehicle entirely, and listing it would only
        # offer a laser nobody can choose.
        if "test" in stem or "template" in stem or "mpuv" in stem:
            continue
        s = read(path)

        fracture = extraction = full = zero = 0.0
        for m in re.finditer(r"<SWeaponActionFireBeamParams(.*?)</SWeaponActionFireBeamParams>",
                             s, re.S):
            block = m.group(1)
            energy = re.search(r'DamageEnergy="([\d.]+)"', block)
            value = float(energy.group(1)) if energy else 0.0
            hit = attr(block, "hitType")
            if hit == "ElectricArc":
                fracture = value
                full = float(attr(block, "fullDamageRange", "0"))
                zero = float(attr(block, "zeroDamageRange", "0"))
            elif hit == "Extraction":
                extraction = value
        if fracture <= 1:
            continue    # the S0 heads are hand lasers, on another scale

        # How many modules it takes. The ports are named
        # BONE_ItemPort_Consumable_1..n, and the count matches what the game
        # says in prose - "Helix II features three module slots".
        # The word boundary is not decoration: without it [^>]* runs on to
        # the DisplayName= that follows, Name= never matches, and every
        # laser silently comes out with nought slots.
        slots = len([x for x in re.findall(r'<SItemPortDef[^>]*\bName="([^"]*)"', s)
                     if "Consumable" in x])

        name, _ = localisation(s)
        mods = modifiers(s, "<miningLaserModifiers",
                         ("optimalChargeWindowSize", "optimalChargeWindowRate",
                          "resistance", "laserInstability", "filter"))
        short = stem.replace("mining_laser_", "")
        found.append({
            "id": short,
            "name": name or presentable(short),
            "size": int(attr(s, "Size", "0")),
            "fracture": fracture, "extraction": extraction,
            "full": full, "zero": zero,
            "window": mods["optimalChargeWindowSize"],
            "rate": mods["optimalChargeWindowRate"],
            "resistance": mods["resistance"],
            "instability": mods["laserInstability"],
            "filter": mods["filter"],
            "slots": slots,
            "throttleMin": float(attr(s, "throttleMinimum", "0")),
        })
    return found


# --- the modules, which change the laser -----------------------------------
def modules():
    found = []
    folder = os.path.join(records, "entities", "scitem", "ships", "utility",
                          "mining", "miningarm")
    for path in sorted(glob.glob(os.path.join(folder, "*.xml"))):
        stem = os.path.basename(path)[:-4]
        # vehiclemod_rocds and vehiclemod_argogeo sit beside the modules in the
        # files, but they are what a ROC and an ATLS GEO have built into their
        # own arm, not modules a ship can fit.
        if stem.startswith("mining_modules_vehiclemod"):
            continue
        s = read(path)

        power = 1.0
        i = s.find("ItemWeaponModifiersParams")
        if i >= 0:
            m = re.search(r'damageMultiplier="([\d.]+)"', s[i:i + 1200])
            if m:
                power = float(m.group(1))

        # The drawing-in beam's power, which the module's lines on the HUD
        # print as "Extraction Rate" - every FLTR, XTR and Vaux has one. It is
        # not the optimal charge rate.
        extraction = 0.0
        x = s.find('ItemWeaponModifiersParams fireActionIndex="1"')
        if x >= 0:
            m = re.search(r'damageMultiplier="([\d.]+)"', s[x:x + 1200])
            if m:
                extraction = (float(m.group(1)) - 1.0) * 100.0

        mods = modifiers(s, "ItemMiningModifierParams",
                         ("optimalChargeWindowSize", "optimalChargeWindowRate",
                          "resistance", "laserInstability", "filter",
                          "shatterdamage", "catastrophicChargeWindowRate"))
        if power == 1.0 and extraction == 0.0 and not any(mods.values()):
            continue    # moves none of them - a decoration

        # How an active module gets on with the others, off the component that
        # fires it. One that canInterrupt cuts the running ones off when it
        # fires, and one that isInterruptible is cut off by them - so of those
        # only one runs at a time, and a second copy of the same one only
        # replaces the first. Surge is 0 and 0, like every passive module: it
        # neither cuts anything off nor is cut off, so it runs alongside the
        # rest and alongside itself. That is what makes two Surge the strongest
        # fit on a Helix I and two Stampede no better than one (a community 4.10
        # Prospector guide says the same).
        fire = re.search(r"<EntityComponentAttachableModifierParams\b([^>]*)>", s)
        firing = fire.group(1) if fire else ""
        name, _ = localisation(s)
        short = stem.replace("mining_modules_", "")
        found.append({
            "id": short,
            "name": name or presentable(short),
            "active": stem.startswith("mining_modules_active"),
            "interrupts": attr(firing, "canInterrupt", "0") == "1",
            "interruptible": attr(firing, "isInterruptible", "0") == "1",
            "power": power,
            "window": mods["optimalChargeWindowSize"],
            "rate": mods["optimalChargeWindowRate"],
            "resistance": mods["resistance"],
            "instability": mods["laserInstability"],
            "filter": mods["filter"],
            "shatter": mods["shatterdamage"],
            "danger": mods["catastrophicChargeWindowRate"],
            "extraction": extraction,
        })
    return found


# --- the gadgets, which change the rock ------------------------------------
def gadgets():
    found = []
    folder = os.path.join(records, "entities", "scitem", "weapons", "devices")
    for path in sorted(glob.glob(os.path.join(folder, "mining_gadget_*.xml"))):
        stem = os.path.basename(path)[:-4]
        s = read(path)
        if "ItemMineableRockModifierParams" not in s:
            continue
        mods = modifiers(s, "ItemMineableRockModifierParams",
                         ("laserInstability", "optimalChargeWindowSize",
                          "optimalChargeWindowRate", "resistance", "clusterFactor"))
        name, desc = localisation(s)
        short = stem.replace("mining_gadget_", "")
        found.append({
            "id": short,
            "name": name or presentable(short),
            "blurb": blurb(desc),
            "instability": mods["laserInstability"],
            "window": mods["optimalChargeWindowSize"],
            "rate": mods["optimalChargeWindowRate"],
            "resistance": mods["resistance"],
            "cluster": mods["clusterFactor"],
        })
    return found


# --- write it out ----------------------------------------------------------
LASERS = lasers()

# Two lasers share one localisation key: the MPUV's arm answers to the Arbor
# MH1's name, which makes a suggestion list read as if the same laser were
# offered twice. Whoever comes second keeps its file name instead, so the two
# can be told apart.
_seen = {}
for _l in LASERS:
    if _l["name"] in _seen:
        _l["name"] = presentable(_l["id"])
    else:
        _seen[_l["name"]] = _l["id"]

MODULES = modules()
GADGETS = gadgets()
ELEMENTS = elements()
SHIP = constants()
ROLLS = rolls()
ROCKS = rocks()
# ...and the bands of every ore a rock is made of, by the name the rocks use
_quantized = quality_bands()
BAND_STEM = {"aluminium": "aluminum", "ice": "rawice"}
ORE_BANDS = {}
BAND_EDGES = []
for _parts in ROCKS.values():
    for _ore, _lo, _hi, _scale, *_roll in _parts:
        _stem = BAND_STEM.get(_ore.lower(), _ore.lower())
        if _stem in _quantized:
            ORE_BANDS[_ore] = _quantized[_stem][0]
            BAND_EDGES = BAND_EDGES or _quantized[_stem][1]
        else:
            print("  WARNING no quality bands for " + _ore)
# what the signature table calls the ores, to say so when a rock's name is not among them
_known = set(re.findall(r'\{"([A-Za-z]+)",\s*\d+, Rarity::',
                        read(os.path.join(os.path.dirname(os.path.abspath(__file__)), "include", "MiningData.h"))))
for _rock in sorted(ROCKS):
    for _ore in sorted({p[0] for p in ROCKS[_rock]} | {_rock}):
        if _known and _ore not in _known:
            print("  WARNING %s (in a %s rock) is no ore of the signature table" % (_ore, _rock))


def floating(v):
    """5.7 -> 5.7f, 5 -> 5.0f"""
    text = "%g" % v
    return text + ("" if "." in text or "e" in text else ".0") + "f"


def quoted(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


with io.open(out_path, "w", encoding="utf-8", newline="\n") as f:
    w = f.write
    w("#pragma once\n")
    w("// Mining hardware, and the constants breaking runs on.\n")
    w("// Generated by gen_mining_hardware.py - do not edit by hand.\n")
    w("//\n")
    w("// Fracture power is DamageEnergy on the laser's ElectricArc fire mode.\n")
    w("// A module's `power` is a plain factor; every other figure here is a\n")
    w("// percentage. They multiply, they do not add - a Helix I at 3900 with\n")
    w("// Focus III (0.95) and Rieger-C3 (1.25) puts out 4631, and the HUD shows\n")
    w("// the same chain as 119 against a base of 100.\n")
    w("//\n")
    w("// Modules hang on the ship and change the laser. Gadgets attach to the\n")
    w("// rock and change what is in front of you.\n")
    w("//\n")
    w("// `cluster` is not how the rock splits - it is how much ore survives the\n")
    w("// break. The gadgets say so themselves: OptiMax at +60 'reduces the loss\n")
    w("// of valuable resource', Okunis at -20 warns that 'some valuable resource\n")
    w("// will be lost when it shatters'.\n")
    w("//\n")
    w("// Distance: full power out to `fullRange`, nothing from `zeroRange`. What\n")
    w("// happens between them is NOT in the game files - see tests/scanpanel.\n")
    w("#include <cstdint>\n\n")
    w("namespace SCMiner {\n\n")

    w("struct MiningLaser {\n")
    w("    const char* id;\n")
    w("    const char* name;\n")
    w("    int8_t size;\n")
    w("    float fracture;      // energy per second, breaking\n")
    w("    float extraction;    // energy per second, drawing in\n")
    w("    float fullRange;     // metres: full power out to here\n")
    w("    float zeroRange;     // metres: nothing from here\n")
    w("    float window;        // per cent on the optimal charge window\n")
    w("    float rate;          // per cent on its fill rate\n")
    w("    float resistance;    // per cent on resistance\n")
    w("    float instability;   // per cent it ADDS to the rock's instability\n")
    w("    float filter;        // per cent less inert material drawn in\n")
    w("    int8_t slots;        // module slots it takes\n")
    w("    float throttleMin;\n")
    w("};\n\n")

    w("struct MiningModule {\n")
    w("    const char* id;\n")
    w("    const char* name;\n")
    w("    bool active;         // false for the passive ones\n")
    w("    // How an active one gets on with the others when it fires: one that\n")
    w("    // interrupts cuts the running ones off, one that is interruptible is\n")
    w("    // cut off by them. Only one of those runs at a time, and a second\n")
    w("    // copy of the same replaces the first. Surge is neither, like every\n")
    w("    // passive module, and runs alongside the rest and alongside itself.\n")
    w("    bool interrupts;\n")
    w("    bool interruptible;\n")
    w("    float power;         // a factor: 1.25 is +25 per cent\n")
    w("    float window;\n")
    w("    float rate;\n")
    w("    float resistance;\n")
    w("    float instability;\n")
    w("    float filter;        // per cent less inert material drawn in\n")
    w("    float shatter;       // per cent on shatter damage\n")
    w("    float danger;        // per cent on how fast the danger band fills\n")
    w("    float extraction;    // per cent on the drawing-in beam's power - the HUD's Extraction Rate\n")
    w("};\n\n")

    w("struct MiningGadget {\n")
    w("    const char* id;\n")
    w("    const char* name;\n")
    w("    const char* blurb;   // what the game says it is for\n")
    w("    float instability;   // per cent on the rock's instability\n")
    w("    float window;\n")
    w("    float rate;\n")
    w("    float resistance;    // per cent on the rock's resistance\n")
    w("    float cluster;       // per cent on how much ore survives\n")
    w("};\n\n")

    w("// Each ore brings its own physics to a rock it is part of.\n")
    w("struct MineableElement {\n")
    w("    const char* id;\n")
    w("    float instability;\n")
    w("    float resistance;\n")
    w("    float windowMid;     // where its optimal window sits, 0..1\n")
    w("    float windowRandom;\n")
    w("    float windowThin;\n")
    w("    float explosion;\n")
    w("    float cluster;       // how much of it survives the break\n")
    w("};\n\n")

    w("// What a ship-mining rock of an ore is made of, part by part, in the\n")
    w("// files' order: a little of the ore at any quality, the bulk of it at a\n")
    w("// quality scaled down, and a side ore or two. The rest of the rock is inert.\n")
    w("struct RockPart {\n")
    w("    const char* rock;      // the ore the rock is of, as the signature table spells it\n")
    w("    const char* ore;       // what this part is: that ore, or a side ore\n")
    w("    float least, most;     // per cent of the rock\n")
    w("    float qualityScale;    // its quality roll times this: 1, 0.789 or 0.49\n")
    w("    // The roll: a normal of this mean and spread, cut to least..most - the\n")
    w("    // same for every part of a kind of rock, wider the rarer the rock\n")
    w("    float rollLeast, rollMost, rollMean, rollSpread;\n")
    w("};\n\n")
    w("// The eight values an ore's quality comes out at - what the scan panel\n")
    w("// prints beside a part and the bench reads off the material\n")
    w("struct OreBands {\n")
    w("    const char* ore;\n")
    w("    uint16_t bands[8];\n")
    w("};\n\n")

    w("inline const MiningLaser MINING_LASERS[] = {\n")
    for l in sorted(LASERS, key=lambda x: (x["size"], -x["fracture"])):
        w('    {"%s", "%s", %d, %.1ff, %.1ff, %.1ff, %.1ff, %.1ff, %.1ff, %.1ff, '
          '%.1ff, %.1ff, %d, %.3ff},\n'
          % (l["id"], quoted(l["name"]), l["size"], l["fracture"], l["extraction"],
             l["full"], l["zero"], l["window"], l["rate"], l["resistance"],
             l["instability"], l["filter"], l["slots"], l["throttleMin"]))
    w("};\n\n")

    w("// On the ship: these change the laser.\n")
    w("//\n")
    w("// Only the ones that can be fitted to one. The game files carry two more\n")
    w("// under the same kind - vehiclemod_rocds and vehiclemod_argogeo - and those\n")
    w("// are what a ROC and an ATLS GEO have built into their own arm, not\n")
    w("// modules a ship can fit.\n")
    w("inline const MiningModule MINING_MODULES[] = {\n")
    for m in sorted(MODULES, key=lambda x: (x["active"], -x["power"])):
        w('    {"%s", "%s", %s, %s, %s, %.3ff, %.1ff, %.1ff, %.1ff, %.1ff, %.1ff, %.1ff, %.1ff, %.1ff},\n'
          % (m["id"], quoted(m["name"]), "true" if m["active"] else "false",
             "true" if m["interrupts"] else "false", "true" if m["interruptible"] else "false",
             m["power"], m["window"], m["rate"], m["resistance"],
             m["instability"], m["filter"], m["shatter"], m["danger"], m["extraction"]))
    w("};\n\n")

    w("// On the rock: these change what is in front of you\n")
    w("inline const MiningGadget MINING_GADGETS[] = {\n")
    for g in sorted(GADGETS, key=lambda x: x["name"]):
        w('    {"%s", "%s",\n     "%s",\n     %.1ff, %.1ff, %.1ff, %.1ff, %.1ff},\n'
          % (g["id"], quoted(g["name"]), quoted(g["blurb"]), g["instability"],
             g["window"], g["rate"], g["resistance"], g["cluster"]))
    w("};\n\n")

    w("inline const MineableElement MINEABLE_ELEMENTS[] = {\n")
    for e in sorted(ELEMENTS, key=lambda x: x["id"]):
        w('    {"%s", %.2ff, %.2ff, %.2ff, %.2ff, %.2ff, %.1ff, %.2ff},\n'
          % (e["id"], e["instability"], e["resistance"], e["windowMid"],
             e["windowRandom"], e["windowThin"], e["explosion"], e["cluster"]))
    w("};\n\n")

    w("inline const RockPart ROCK_PARTS[] = {\n")
    for rock in sorted(ROCKS):
        for ore, lo, hi, scale, least, most, mean, spread in ROCKS[rock]:
            w('    {"%s", "%s", %s, %s, %s, %s, %s, %s, %s},\n'
              % (rock, ore, floating(lo), floating(hi), floating(scale), floating(least), floating(most),
                 floating(mean), floating(spread)))
    w("};\n\n")
    w("inline const OreBands ORE_BANDS[] = {\n")
    for ore in sorted(ORE_BANDS):
        w('    {"%s", {%s}},\n' % (ore, ", ".join(str(v) for v in ORE_BANDS[ore])))
    w("};\n\n")
    w("// Each band's first roll, the same for every material\n")
    w("inline constexpr int QUALITY_BAND_EDGES[8] = {%s};\n\n" % ", ".join(str(e) for e in BAND_EDGES))

    w("// From mining/miningglobalparamsship.xml\n")
    w("struct MiningConstants {\n")
    for k in sorted(SHIP):
        w("    float %s;\n" % k)
    w("};\n\n")
    w("inline constexpr MiningConstants MINING_SHIP = {\n")
    for k in sorted(SHIP):
        # through float(), because the records hold "2" and "2f" is not a
        # literal any compiler accepts
        text = "%gf" % float(SHIP[k] or 0)
        if "." not in text and "e" not in text:
            text = text[:-1] + ".0f"
        w("    %s,%s// %s\n" % (text, " " * max(1, 12 - len(text)), k))
    w("};\n\n")

    w("inline constexpr int MINING_LASER_COUNT = %d;\n" % len(LASERS))
    w("inline constexpr int MINING_MODULE_COUNT = %d;\n" % len(MODULES))
    w("inline constexpr int MINING_GADGET_COUNT = %d;\n" % len(GADGETS))
    w("inline constexpr int MINEABLE_ELEMENT_COUNT = %d;\n" % len(ELEMENTS))
    w("inline constexpr int ROCK_PART_COUNT = %d;\n" % sum(len(p) for p in ROCKS.values()))
    w("inline constexpr int ORE_BANDS_COUNT = %d;\n\n" % len(ORE_BANDS))
    w("// The highest band a part of that quality scale can come out of, 0..7:\n")
    w("// its roll reaches 1000 times the scale at the most\n")
    w("inline int topQualityBand(float qualityScale) {\n")
    w("    const int reach = (int)(1000.0f * qualityScale + 0.5f);\n")
    w("    for (int band = 7; band > 0; band--) {\n")
    w("        if (reach >= QUALITY_BAND_EDGES[band]) return band;\n")
    w("    }\n")
    w("    return 0;\n")
    w("}\n\n")
    w("// ...and the lowest: its roll is never under rollLeast\n")
    w("inline int lowestQualityBand(const RockPart& part) {\n")
    w("    const int least = (int)(part.rollLeast * part.qualityScale);\n")
    w("    for (int band = 7; band > 0; band--) {\n")
    w("        if (least >= QUALITY_BAND_EDGES[band]) return band;\n")
    w("    }\n")
    w("    return 0;\n")
    w("}\n\n")
    w("}  // namespace SCMiner\n")

print("%s: %d lasers, %d modules, %d gadgets, %d elements, %d constants, %d rocks of %d parts, %d ores with bands"
      % (out_path, len(LASERS), len(MODULES), len(GADGETS), len(ELEMENTS), len(SHIP), len(ROCKS),
         sum(len(p) for p in ROCKS.values()), len(ORE_BANDS)))
