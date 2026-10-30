"""The game's names for the Blackbox plugin, out of SC Blackbox's names.txt.

    gen_blackbox_names.py <names.txt> <names.json>

names.txt (src/names.txt in SC Blackbox, made from the game's
defaultProfile.xml and global.ini) has one line per name, tab-separated:

    M  map  label
    A  map  action  label  keyboard  mouse  gamepad  joystick   (defaults)
    O  device-type  option  label

names.json keeps them keyed in lower case, the first of a name winning, as
SC Blackbox looks them up (without regard to case):

    {"game": "<build from the header>",
     "maps": {map: label},
     "actions": {map: {action: [label, keyboard, mouse, gamepad, joystick]}},
     "options": {device-type: {option: label}}}
"""
import io, json, re, sys

source, out = sys.argv[1], sys.argv[2]
text = io.open(source, encoding="utf-8-sig").read()

game = ""
maps, actions, options = {}, {}, {}
for line in re.split(r"\r\n|\r|\n", text):
    if line.startswith("#"):
        m = re.search(r"game files \(([^)]*)\)", line)
        if m:
            game = m.group(1)
        continue
    f = line.split("\t")
    if f[0] == "M" and len(f) >= 3:
        maps.setdefault(f[1].lower(), f[2])
    elif f[0] == "A" and len(f) >= 8:
        actions.setdefault(f[1].lower(), {}).setdefault(f[2].lower(), f[3:8])
    elif f[0] == "O" and len(f) >= 4:
        options.setdefault(f[1].lower(), {}).setdefault(f[2].lower(), f[3])


def q(s):
    return json.dumps(s, ensure_ascii=False)


# one line per name, so a new game build shows as a short diff
lines = ["{", ' "game": ' + q(game) + ",", ' "maps": {']
lines += ["  " + q(k) + ": " + q(v) + "," for k, v in maps.items()]
lines[-1] = lines[-1].rstrip(",")
lines += [" },", ' "actions": {']
for m, acts in actions.items():
    lines.append("  " + q(m) + ": {")
    lines += ["   " + q(k) + ": [" + ", ".join(q(x) for x in v) + "]," for k, v in acts.items()]
    lines[-1] = lines[-1].rstrip(",")
    lines.append("  },")
lines[-1] = lines[-1].rstrip(",")
lines += [" },", ' "options": {']
for d, opts in options.items():
    lines.append("  " + q(d) + ": {")
    lines += ["   " + q(k) + ": " + q(v) + "," for k, v in opts.items()]
    lines[-1] = lines[-1].rstrip(",")
    lines.append("  },")
lines[-1] = lines[-1].rstrip(",")
lines += [" }", "}", ""]
io.open(out, "w", encoding="utf-8", newline="\n").write("\n".join(lines))

json.loads(io.open(out, encoding="utf-8").read())
n = sum(len(v) for v in actions.values())
print("%s: %d maps, %d actions, %d options (%s)" % (out, len(maps), n, sum(len(v) for v in options.values()), game))
