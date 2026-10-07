"""Reads the quality bands straight out of the game files, so the app does
not depend on anyone else's export.

    gen_bands_from_p4k.py <extract-dir> <bands.json> [reference.json]

<extract-dir> is where the p4k was unpacked to, e.g. C:\\SCDataMine\\extract410.
Getting there takes two commands and about a minute:

    unp4k.exe  "E:\\RSI\\StarCitizen\\LIVE\\Data.p4k"  *.dcb      (4 s, 316 MB)
    unforge.cli.exe Data\\Game2.dcb                                (~7 min)

The bands live in libs/foundry/records/crafting/qualityquantization/*.xml, one
file per material, each with eight <CraftingQualityQuantizationBand> rows:

    <CraftingQualityQuantizationBand start="0" end="399" mappedValue="346" />

A rock rolled anywhere in 0..399 comes out of the ground at quality 346 - the
band's mappedValue is the number the bench sees, and the only one worth
putting on a slider.

Pass a reference export (scmdb's quality_bands.json) as the third argument to
have the two compared; the run then says whether they agree.
"""
import json, io, os, re, sys, xml.etree.ElementTree as ET

extract, out = sys.argv[1], sys.argv[2]
reference = sys.argv[3] if len(sys.argv) > 3 else None

root = os.path.join(extract, "Data", "libs", "foundry", "records", "crafting", "qualityquantization")
if not os.path.isdir(root):
    # unforge writes the tree in whatever case the archive had
    for base, dirs, _ in os.walk(extract):
        for d in dirs:
            if d.lower() == "qualityquantization":
                root = os.path.join(base, d)
                break
if not os.path.isdir(root):
    raise SystemExit(f"no qualityquantization records under {extract}")

# The rarity a material is mined at, for display - one file per rarity class
bands = {}
for name in sorted(os.listdir(root)):
    if not name.lower().endswith(".xml"):
        continue
    path = os.path.join(root, name)
    try:
        tree = ET.parse(path)
    except ET.ParseError as e:
        print(f"  skipped {name}: {e}")
        continue
    # quantization_agricium.xml -> Agricium
    material = re.sub(r"^quantization_", "", os.path.splitext(name)[0])
    material = material.replace("_", " ").strip().title()
    values = []
    for band in tree.iter("CraftingQualityQuantizationBand"):
        v = band.get("mappedValue")
        if v is not None:
            values.append(int(float(v)))
    if values:
        bands[material] = {"bands": values, "rarity": ""}

print(f"{len(bands)} materials with bands from the game files")

if reference and os.path.exists(reference):
    ref = json.load(io.open(reference, encoding="utf-8"))

    def norm(s):
        return re.sub(r"\s*\(.*\)$", "", s).strip().lower().replace("aluminum", "aluminium")

    ref_norm = {norm(k): v for k, v in ref.items()}
    same = diff = only_mine = 0
    for m, v in bands.items():
        r = ref_norm.get(norm(m))
        if not r:
            only_mine += 1
            continue
        if list(r.get("bands") or []) == v["bands"]:
            same += 1
            v["rarity"] = r.get("rarity") or ""
        else:
            diff += 1
            print(f"  DIFFERS {m}: game {v['bands']} vs reference {r.get('bands')}")
    print(f"compared with {reference}: {same} identical, {diff} different, {only_mine} only in the game files")

json.dump(bands, io.open(out, "w", encoding="utf-8"), indent=1)
print(f"wrote {out}")
