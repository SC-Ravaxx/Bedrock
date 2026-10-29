#!/usr/bin/env python3
"""What the text check found, picture by picture.

    python scripts/text_check_report.py <shots.log> [<out.md>]

The log is a shot run's output with BEDROCK_TEXT_CHECK=1, stdout and stderr
in one file (Linux build, WSL):

    QT_QPA_PLATFORM=offscreen BEDROCK_TEXT_CHECK=1 ./bedrock --screenshot <dir> > shots.log 2>&1

and once more with BEDROCK_SHOT_PLUGINS=<repo>/plugins for the plugin
pictures. Texts that fit at 100 % can be cut at 125 %: BEDROCK_SHOT_SCALE=125
with a screen as tall as 1440 px, so that the windows keep their size
(QT_QPA_PLATFORM=offscreen:configfile=<json> naming one screen 5120x1440).

The shim prints, at the end of each painting of a picture, a line
"TEXTCHECK PASS WxH texts=N" and then what it found in that painting
(TEXTCHECK CUT / CLIPPED / OVERLAP / SPLIT, see include/GdiplusShim.h). The shot run
prints "<name>.png: WxH ..." for each picture it saves. A picture is given
the findings of the last painting of its size since the picture before it:
that painting is the one in the file. Paintings of other sizes (an
owner-drawn button, another window repainting meanwhile) go under "Not in a
picture" where what they found is not in any picture already.

Prints the counts; writes the whole report as markdown to <out.md>, or to
stdout without one.
"""
import os
import re
import sys
from collections import OrderedDict, defaultdict

PNG = re.compile(r"^([\w.\-]+\.png): (\d+)x(\d+)")
PASS = re.compile(r"^TEXTCHECK PASS (\d+)x(\d+) texts=(\d+)")
FOUND = re.compile(r"^TEXTCHECK (CUT|CLIPPED|OVERLAP|SPLIT) (\d+)x(\d+) (.*)$")
KINDS = ("CUT", "CLIPPED", "OVERLAP", "SPLIT")


def quoted(s, i):
    """The string quoted at s[i] (backslash escapes), and where it ends"""
    assert s[i] == '"', s[i:i + 20]
    out = []
    i += 1
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append("\n" if n == "n" else n)
            i += 2
            continue
        if c == '"':
            return "".join(out), i + 1
        out.append(c)
        i += 1
    return "".join(out), i


def parse(kind, rest):
    """A finding's parts: boxes, texts, and the words after them"""
    f = {"kind": kind, "line": rest}
    box, i = rest.split(" ", 1)[0], len(rest.split(" ", 1)[0]) + 1
    f["box"] = box
    f["text"], i = quoted(rest, i)
    tail = rest[i:].strip()
    if kind in ("CUT", "SPLIT"):
        assert tail.startswith("->"), tail
        f["shown"], j = quoted(tail, tail.index('"'))
        f["note"] = tail[j:].strip()
    elif kind == "CLIPPED":
        f["why"] = tail
    else:
        m = re.match(r'vs (\S+) ', tail)
        if not m:
            raise ValueError(f"not an OVERLAP line: {rest}")
        f["box2"] = m.group(1)
        f["text2"], j = quoted(tail, m.end())
        f["by"] = tail[j:].strip().replace("by ", "")
    return f


def key(f):
    """What makes two findings the same, wherever and in whichever picture"""
    if f["kind"] in ("CUT", "SPLIT"):
        return (f["kind"], f["text"], f["shown"])
    if f["kind"] == "CLIPPED":
        reasons = tuple(sorted(r.split(":")[0] for r in f["why"].split("; ")))
        return ("CLIPPED", f["text"], reasons)
    a, b = sorted((f["text"], f["text2"]))
    return ("OVERLAP", a, b)


def read(path):
    pictures = OrderedDict()   # name -> {"size", "findings": [...], "passed": bool}
    apart = defaultdict(list)  # size -> findings
    blocks = []                # paintings since the last picture: [size, findings]
    lines = 0
    with open(path, encoding="utf-8", errors="replace") as log:
        for raw in log:
            line = raw.rstrip("\r\n")
            lines += 1
            m = PASS.match(line)
            if m:
                blocks.append([(int(m.group(1)), int(m.group(2))), []])
                continue
            m = FOUND.match(line)
            if m:
                size = (int(m.group(2)), int(m.group(3)))
                if not blocks or blocks[-1][0] != size:
                    blocks.append([size, []])   # (a line without its PASS: kept anyway)
                blocks[-1][1].append(parse(m.group(1), m.group(4)))
                continue
            m = PNG.match(line)
            if m:
                name, size = m.group(1), (int(m.group(2)), int(m.group(3)))
                same = [b for b in blocks if b[0] == size]
                for b in blocks:
                    if b[0] != size:
                        apart[b[0]].extend(b[1])
                pictures[name] = {"size": size, "findings": same[-1][1] if same else [], "passed": bool(same)}
                blocks = []
    for b in blocks:
        apart[b[0]].extend(b[1])
    return pictures, apart, lines


def short(s, n=110):
    s = s.replace("\n", "\u23ce")
    return s if len(s) <= n else s[:n - 1] + "\u2026"


def code(s):
    s = s.replace("\n", "\u23ce")
    fence = "``" if "`" in s else "`"
    pad = " " if s.startswith("`") or s.endswith("`") else ""
    return f"{fence}{pad}{s}{pad}{fence}"


def cut_pair(text, shown):
    """The text and what is shown of it, from a little before they part"""
    c = 0
    while c < min(len(text), len(shown)) and text[c] == shown[c]:
        c += 1
    if c > 50:
        return "\u2026" + text[c - 35:], "\u2026" + shown[c - 35:]
    return text, shown


def describe(f):
    if f["kind"] == "CUT":
        t, s = cut_pair(f["text"], f["shown"])
        note = f" {f['note']}" if f.get("note") else ""
        return f"{code(short(t, 140))} \u2192 {code(short(s, 140))}{note}"
    if f["kind"] == "CLIPPED":
        return f"{code(short(f['text']))} \u2014 {f['why']}"
    if f["kind"] == "SPLIT":
        return f"{code(short(f['shown'], 160))}"
    return f"{code(short(f['text'], 80))} at {f['box']} \u00d7 {code(short(f['text2'], 80))} at {f['box2']}, by {f['by']}"


def names(pics, most=8):
    pics = list(pics)
    if len(pics) <= most:
        return ", ".join(pics)
    return ", ".join(pics[:most]) + f", \u2026 (+{len(pics) - most})"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    pictures, apart, lines = read(path)

    # one of each finding per picture (the same words in two places are two)
    for p in pictures.values():
        seen, unique = set(), []
        for f in p["findings"]:
            k = key(f) + (f["box"], f.get("box2"))
            if k not in seen:
                seen.add(k)
                unique.append(f)
        p["findings"] = unique

    distinct = OrderedDict()   # key -> (first finding, [pictures])
    for name, p in pictures.items():
        for f in p["findings"]:
            k = key(f)
            if k not in distinct:
                distinct[k] = (f, [])
            if name not in distinct[k][1]:
                distinct[k][1].append(name)

    counts = {k: sum(1 for p in pictures.values() for f in p["findings"] if f["kind"] == k) for k in KINDS}
    kinds_distinct = {k: sum(1 for d in distinct if d[0] == k) for k in KINDS}
    with_findings = sum(1 for p in pictures.values() if p["findings"])
    unpassed = [n for n, p in pictures.items() if not p["passed"]]

    apart_left = OrderedDict()
    for size in sorted(apart):
        seen = set()
        for f in apart[size]:
            k = key(f)
            if k in distinct or k in seen:
                continue
            seen.add(k)
            apart_left.setdefault(size, []).append(f)

    summary = [
        f"{len(pictures)} pictures, {with_findings} with findings; "
        f"{len(unpassed)} without a painting of their size in the log",
    ]
    for k in KINDS:
        summary.append(f"{k}: {counts[k]} in pictures, {kinds_distinct[k]} distinct")
    summary.append(f"not in a picture: {sum(len(v) for v in apart_left.values())} distinct, "
                   f"from {len(apart_left)} sizes")
    print("\n".join(summary))

    md = [f"# Text check: {os.path.basename(path)}", ""]
    md += [f"- {s}" for s in summary]
    if unpassed:
        md.append(f"- without a painting of their size: {names(unpassed, 20)}")
    md.append("")
    md.append("A finding is listed once per picture; \"distinct\" counts it once for the whole run.")
    md.append("")
    md.append("## Distinct findings")
    for kind in KINDS:
        items = [(f, pics) for k, (f, pics) in distinct.items() if k[0] == kind]
        md.append("")
        md.append(f"### {kind} ({len(items)})")
        md.append("")
        items.sort(key=lambda fp: (-len(fp[1]), fp[1][0]))
        for f, pics in items:
            md.append(f"- {describe(f)} \u2014 {len(pics)} picture{'s' if len(pics) != 1 else ''}: {names(pics)}")
    md.append("")
    md.append("## Per picture")
    for name, p in pictures.items():
        if not p["findings"]:
            continue
        by = defaultdict(list)
        for f in p["findings"]:
            by[f["kind"]].append(f)
        head = ", ".join(f"{len(by[k])} {k}" for k in KINDS if by[k])
        md.append("")
        md.append(f"### {name} ({p['size'][0]}x{p['size'][1]}): {head}")
        md.append("")
        for k in KINDS:
            for f in by[k]:
                md.append(f"- {k} {f['box']} {describe(f)}")
    if apart_left:
        md.append("")
        md.append("## Not in a picture")
        md.append("")
        md.append("Paintings whose size no picture after them has - owner-drawn buttons, a page under")
        md.append("frosted glass, windows that repainted meanwhile - with what they found that is not")
        md.append("in a picture already.")
        for size, found in apart_left.items():
            md.append("")
            md.append(f"### {size[0]}x{size[1]}")
            md.append("")
            for f in found:
                md.append(f"- {f['kind']} {f['box']} {describe(f)}")
    text = "\n".join(md) + "\n"
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as out:
            out.write(text)
        print(f"written: {sys.argv[2]}")
    else:
        getattr(sys.stdout, "reconfigure", lambda **_: None)(encoding="utf-8")
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
