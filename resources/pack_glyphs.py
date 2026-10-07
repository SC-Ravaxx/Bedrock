import glob, os, struct, sys
# Packs glyphs/*.png into resources/glyphs.bin, the RCDATA blob the reader
# loads when no glyphs folder sits beside the exe. Run after every harvest,
# before a package build:  python resources/pack_glyphs.py
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def pack(folder, blob):
    # A folder that is not there packs as an empty blob: the resource has
    # to exist for the build, and the reader treats an empty set as "no
    # such font", which falls back to the canopy set.
    names = sorted(glob.glob(os.path.join(root, folder, "*.png")))
    out = bytearray(b"SGLY" + struct.pack("<I", len(names)))
    for path in names:
        name = os.path.basename(path).encode("ascii")
        data = open(path, "rb").read()
        out += struct.pack("<H", len(name)) + name + struct.pack("<I", len(data)) + data
    open(os.path.join(root, "resources", blob), "wb").write(out)
    print(f"{blob}: packed {len(names)} templates from {folder}/, {len(out)} bytes")


# Two fonts, two blobs. glyphs/ is the canopy marker every ship draws in
# the world; glyphs_mfd/ is the Drake instrument panel, where the same
# number is set in a wider face on a dark screen. Never one pool.
pack("glyphs", "glyphs.bin")
pack("glyphs_mfd", "glyphs_mfd.bin")
