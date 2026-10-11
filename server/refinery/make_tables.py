#!/usr/bin/env python3
"""Bedrock's refining model, out of its headers, for the service to check
quotes against: python make_tables.py (from this folder) writes tables.json.
Run again whenever gen_refining.py has made the headers anew.

    yield = amount * ore.yield * station.yield * method.yield
    cost  = amount * ore.cost  * station.cost  * method.cost     (aUEC)
    time  = amount * ore.time  * station.time  * method.time     (seconds)

(include/RefiningData.h says where the factors come from and how sure they
are: the yields came back exactly on every work order measured, cost and
time only for some ores and stations - `known` and `full`.)
"""

import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def block(text, name):
    m = re.search(r"\b%s\b[^=]*=\s*\{(.*?)\n\};" % re.escape(name), text, re.S)
    if not m:
        sys.exit("no %s in the headers" % name)
    return m.group(1)


def main():
    refining = open(os.path.join(ROOT, "include", "RefiningData.h"), encoding="utf-8").read()
    refinery = open(os.path.join(ROOT, "include", "RefineryData.h"), encoding="utf-8").read()

    materials = re.findall(r'"([^"]+)"', block(refinery, "REFINERY_MATERIALS"))
    methods = {}
    for key, y, t, c in re.findall(r'\{"([A-Z_]+)",\s*"[^"]*",\s*"[^"]*",\s*([\d.]+)f,\s*([\d.]+)f,\s*([\d.]+)f\}',
                                   block(refining, "REFINING_METHODS")):
        methods[key] = {"yield": float(y), "time": float(t), "cost": float(c)}
    ores = {}
    rows = re.findall(r"\{([\d.]+)f,\s*([\d.]+)f,\s*([\d.]+)f,\s*(true|false)\}", block(refining, "ORE_REFINING"))
    if len(rows) != len(materials):
        sys.exit("ORE_REFINING has %d rows for %d materials" % (len(rows), len(materials)))
    for name, (y, t, c, known) in zip(materials, rows):
        ores[name] = {"yield": float(y), "time": float(t), "cost": float(c), "known": known == "true"}
    names = re.findall(r'"([^"]+)"', block(refining, "STATION_REFINING_NAME"))
    full = re.findall(r"\b(true|false)\b", block(refining, "STATION_FULL"))
    table = [line for line in block(refining, "STATION_REFINING").split("\n") if line.strip().startswith("{{")]
    if not (len(names) == len(full) == len(table)):
        sys.exit("the station tables do not line up: %d names, %d flags, %d rows" % (len(names), len(full), len(table)))
    stations = {}
    for name, is_full, line in zip(names, full, table):
        cells = re.findall(r"\{([\d.]+)f,\s*([\d.]+)f,\s*([\d.]+)f\}", line)
        if len(cells) != len(materials):
            sys.exit("%s has %d cells for %d materials" % (name, len(cells), len(materials)))
        stations[name] = {
            "full": is_full == "true",
            "materials": {m: {"yield": float(y), "time": float(t), "cost": float(c)}
                          for m, (y, t, c) in zip(materials, cells)},
        }
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tables.json")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"source": "include/RefiningData.h, include/RefineryData.h", "methods": methods, "ores": ores,
                   "stations": stations}, f, indent=1, sort_keys=True)
        f.write("\n")
    print("%s: %d methods, %d ores, %d stations" % (out, len(methods), len(ores), len(stations)))


if __name__ == "__main__":
    main()
