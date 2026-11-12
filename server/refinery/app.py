#!/usr/bin/env python3
"""Bedrock's refinery observations.

Bedrock reads a refinery terminal while the player stands at it: the
station's workload ("Current capacity 106 %" and the line under it) and,
when a work order is set up, the quote - method, materials, cost and
processing time. The game sets cost and time by that workload, and only
the terminal shows it. Players who agreed send what their Bedrock read
here, and every Bedrock can ask how busy each refinery is and what it
quoted.

Nothing of the player is kept: no name, no account, no address. Senders
are told apart by a token made of the address with a salt that lives in
memory for a day; readings lose it after two days. The log has the
request line and the answer only. Requests are counted by the hour and
the day - what was asked for, the Bedrock version and platform the
User-Agent names, and how many installs asked that day, told apart by the
number Bedrock sends since 2.6.7 (X-Bedrock-Install) or else the address,
salted like the token and in memory only - never by who.

Since anyone can send anything, nothing sent is taken on its word:
  - a reading has to look like what a terminal shows (validate)
  - and fit what the game does: a workload level only at the capacities
    other senders saw it at, a quote's yield, cost and time against
    Bedrock's refining model (judge)
  - a station's state is its newest reading, confirmed once another sender
    saw the same in the last hours, and outvoted where two or more senders
    agree on something else (consensus)
  - one sender reports a station a few times an hour at most
  - an install that sends too many readings that do not fit is held back
    for ten minutes - by its number and by its address, as a number is
    easily made up
  - what slipped through can be found and taken out (--purge, --stats)

Bedrock also sends the rocks a player scanned - what the mining scan panel
printed and the rock's composition - unless sharing is turned off in its
Settings. They are kept to work out rock sizes, densities and the game's
resistance and instability, grouped by a random number Bedrock makes at
each start and with nothing of the player. A rock whose figures do not fit
together is kept but marked (judge_rock), and each says which of its
sender's rocks of the day it was, so that no one sender outweighs the rest;
only the operator gets them out.

Standard library only, SQLite beside it. See README.md.
"""

import argparse
import collections
import csv
import datetime
import hashlib
import hmac
import io
import json
import os
import re
import secrets
import sqlite3
import statistics
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

API_VERSION = 1
HERE = os.path.dirname(os.path.abspath(__file__))
PREFIX = os.environ.get("BEDROCK_API_PREFIX", "/bedrock/api").rstrip("/")
DB_PATH = os.environ.get("BEDROCK_API_DB", "/data/refinery.db")
PORT = int(os.environ.get("BEDROCK_API_PORT", "8080"))
POSTS_PER_HOUR = int(os.environ.get("BEDROCK_API_POSTS_PER_HOUR", "120"))          # from one address
POSTS_PER_HOUR_ALL = int(os.environ.get("BEDROCK_API_POSTS_PER_HOUR_ALL", "20000"))  # from everyone
ROCK_POSTS_PER_HOUR = int(os.environ.get("BEDROCK_API_ROCK_POSTS_PER_HOUR", "60"))  # batches of rocks, from one address
ROCK_POSTS_PER_HOUR_ALL = int(os.environ.get("BEDROCK_API_ROCK_POSTS_PER_HOUR_ALL", "20000"))  # ...from everyone
# ...and the rocks in them. A player mining hard scans about 100 rocks in an
# hour (99 at most in an hour of Bedrock's own rock logs, September to
# November 2026); three times that leaves room for a few players behind one
# address, and holds one sender to what a few players send.
ROCKS_PER_HOUR = int(os.environ.get("BEDROCK_API_ROCKS_PER_HOUR", "300"))
ROCKS_PER_HOUR_ALL = int(os.environ.get("BEDROCK_API_ROCKS_PER_HOUR_ALL", "100000"))
MAX_BODY = 16 * 1024
MAX_ROCK_BODY = 64 * 1024     # a batch of rocks: up to 50, each with its composition
STATION_DAYS = 7              # /stations: how old a station's last reading may be
QUOTE_DAYS_MAX = 60           # /quotes: the most days asked for at once
QUOTES_MAX = 5000
OBSERVED_PAST = 3 * 86400     # a reading sent this late is still taken (Bedrock was offline)
OBSERVED_FUTURE = 600         # ...and one this far ahead (a clock a little fast)

# The readings that make a station's state now, each sender's newest: hours
# rather than minutes while few players send - a workload moves slowly
# (Checkmate went from 103 to 124 % in four hours of 11 October 2026, and
# stayed moderate)
CONSENSUS_SECONDS = 3 * 3600
OUTLIER_POINTS = 15           # two capacities this far apart - or 25 % of the larger - tell different things
STATION_READINGS_PER_HOUR = 12  # from one sender, for one station
SOURCE_HOURS = 48             # how long a reading keeps the token that tells senders apart
RANGE_DAYS = 30               # a workload level's capacity span is learned over this long
RANGE_MIN_READINGS = 10       # ...from this many readings
RANGE_MIN_SOURCES = 3         # ...by this many senders
RANGE_MARGIN = 10             # percentage points beyond a level's capacities that are still taken
YIELD_BAND = (0.85, 1.15)     # a quote's yield against the model, where its factors are known
COST_BAND = (0.1, 20.0)       # cost and time far looser: what moves them is still to be learned - over
TIME_BAND = (0.1, 20.0)       # the quotes of October 2026 the model's cost came out 0.26 to 9.6 times what
                              # the terminal asked, while every yield matched it within 3 %
COUNTER_DAYS = 180
HOURLY_DAYS = 14              # requests are kept by the hour this long, by the day for COUNTER_DAYS

# What a request asked for, as the counters name it (req:<name>)
ROUTE_NAMES = {
    ("GET", "/v1/refinery/stations"): "stations",
    ("GET", "/v1/refinery/quotes"): "quotes",
    ("GET", "/v1/refinery/export.csv"): "export",
    ("GET", "/v1/health"): "health",
    ("POST", "/v1/refinery/observations"): "observations",
    ("POST", "/v1/rocks/observations"): "rocks",
    ("GET", "/v1/rocks/export.csv"): "rocks-export",
}
# Bedrock's own User-Agent: "Bedrock/2.6.6 (Windows)", "(Linux)" - the
# version and the platform, which is all it says
AGENT = re.compile(r"^Bedrock/(\d{1,3}(?:\.\d{1,4}){1,3}) \((Windows|Linux)\)")
# ...and since 2.6.7 the install's own number, 32 hex characters it makes
# itself the first time (stats=0 leaves it out). Installs are counted by it;
# readings are still told apart by the address, as a number is easily made up.
INSTALL = re.compile(r"^[0-9a-f]{32}$")
# Readings that do not fit - turned down, or kept as implausible - from one
# install or one address in an hour before it is held back, and for how long
MISFITS_PER_HOUR = int(os.environ.get("BEDROCK_API_MISFITS_PER_HOUR", "5"))
MISFIT_HOLD_SECONDS = int(os.environ.get("BEDROCK_API_MISFIT_HOLD_SECONDS", "600"))

# The game's own four (refinery_ui_Capacity_Low/Medium/High/Extreme), in
# order: light - no surcharge, moderate - small, heavy - moderate, extreme - large
WORKLOADS = ("light", "moderate", "heavy", "extreme")
# ...and the capacity each begins at, in per cent. The game files do not
# say; the terminals do: of the 309 plausible readings with both, from 11
# October to 1 November 2026, all but 2 fit, and so did the 28 the spans
# learned before turned away (judge).
LEVEL_FROM = (0, 100, 200, 300)
CHANNELS = ("LIVE", "PTU", "EPTU", "TECH-PREVIEW", "HOTFIX")
NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 .'()\-]{0,63}$")
METHOD = re.compile(r"^[A-Z][A-Z_]{2,39}$")
VERSION = re.compile(r"^\d{1,3}(\.\d{1,4}){1,3}$")

# The rocks a player scanned (POST /v1/rocks/observations): what the mining
# scan panel printed for each, and its composition
ROCKS_MAX = 50                # rocks in one batch
ROCK_PARTS_MAX = 8            # lines of a rock's composition
ROCK_KG_PER_SCU = 142.857     # at density 1: one SCU of rock is 142.857 m3
ROCK_SCU_MAX = 59.3           # the largest rock model holds 59.22 SCU
# The densest element a rock of each ore can hold, from the game's element
# densities (game data 4.10.2, recheck with each patch): no rock of that ore
# weighs more per SCU
ROCK_DENSITY_MAX = {
    "Agricium": 7.14, "Aluminium": 4.03, "Aslarite": 7.14, "Beryl": 2.74, "Bexalite": 21.07, "Borase": 21.07,
    "Copper": 9.03, "Corundum": 4.03, "Gold": 21.07, "Hephaestanite": 3.2, "Ice": 1.0, "Iron": 7.87,
    "Laranite": 19.35, "Lindinium": 19.35, "Ouratite": 7.14, "Quantainium": 20.46, "Quartz": 3.2,
    "Riccite": 11.49, "Savrilium": 21.07, "Silicon": 3.2, "Stileron": 10.2, "Taranite": 10.2, "Tin": 9.03,
    "Titanium": 7.14, "Torite": 2.8, "Tungsten": 19.35,
}
ROCK_ORES = tuple(ROCK_DENSITY_MAX)
# The eight values each ore's quality comes out at - what the scan panel
# prints beside a part - from the game data (4.10.2, as Bedrock's ORE_BANDS;
# recheck with each patch). Bedrock takes a composition only where every
# quality in it is one of these, so another one was not read off a panel.
ORE_QUALITIES = {
    "Agricium": (346, 588, 667, 796, 852, 943, 971, 1000),
    "Aluminium": (318, 511, 614, 783, 896, 919, 953, 1000),
    "Aslarite": (287, 575, 602, 741, 854, 927, 963, 1000),
    "Beryl": (324, 547, 677, 717, 860, 937, 955, 1000),
    "Bexalite": (302, 597, 632, 756, 875, 941, 959, 1000),
    "Borase": (359, 584, 676, 743, 892, 903, 976, 1000),
    "Copper": (359, 593, 652, 742, 855, 917, 958, 1000),
    "Corundum": (309, 504, 665, 793, 886, 904, 971, 1000),
    "Gold": (264, 553, 644, 786, 864, 916, 959, 1000),
    "Hephaestanite": (330, 572, 692, 758, 896, 916, 975, 1000),
    "Ice": (322, 561, 659, 714, 873, 922, 966, 1000),
    "Iron": (325, 521, 664, 710, 874, 907, 970, 1000),
    "Laranite": (298, 510, 698, 707, 858, 910, 975, 1000),
    "Lindinium": (305, 585, 618, 729, 853, 930, 954, 1000),
    "Ouratite": (310, 523, 647, 779, 860, 912, 960, 1000),
    "Quantainium": (344, 514, 669, 762, 852, 901, 974, 1000),
    "Quartz": (330, 522, 641, 710, 899, 914, 969, 1000),
    "Riccite": (325, 525, 671, 743, 870, 942, 965, 1000),
    "Savrilium": (322, 580, 636, 776, 878, 905, 967, 1000),
    "Silicon": (310, 510, 672, 782, 889, 926, 968, 1000),
    "Stileron": (330, 517, 681, 747, 874, 947, 972, 1000),
    "Taranite": (310, 525, 646, 718, 853, 925, 957, 1000),
    "Tin": (340, 537, 664, 704, 880, 940, 964, 1000),
    "Titanium": (295, 516, 622, 784, 866, 916, 959, 1000),
    "Torite": (262, 528, 661, 785, 873, 944, 951, 1000),
    "Tungsten": (363, 530, 662, 787, 858, 902, 964, 1000),
}
ROCK_SHIPS = ("prospector", "mole", "golem")
# how the panel printed the resistance: white, or green or red once a laser
# on the rock had changed it
ROCK_PRINTED = ("white", "colour")
# A random number Bedrock makes at each start: one session's rocks - a rock
# and the pieces it broke into - go together, and no install or player is
# told apart by it
SESSION = re.compile(r"^[0-9a-f]{16}\Z")


def load_stations(path):
    names = set()
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    names.add(line)
    except OSError:
        pass
    return names


def load_tables(path):
    """Bedrock's refining model (make_tables.py); {} where it is not there,
    and quotes are then judged by the rules that need no model"""
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


STATIONS = load_stations(os.environ.get("BEDROCK_API_STATIONS", os.path.join(HERE, "stations.txt")))
TABLES = load_tables(os.environ.get("BEDROCK_API_TABLES", os.path.join(HERE, "tables.json")))


def utc_day(t):
    return time.strftime("%Y-%m-%d", time.gmtime(t))


def utc_hour(t):
    return time.strftime("%Y-%m-%d %H", time.gmtime(t))


def utc_iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


# ---------------------------------------------------------------- a report

class Invalid(ValueError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def _number(value, low, high, what, optional=False):
    if value is None and optional:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value != value:
        raise Invalid(what, what + " is not a number")
    if value < low or value > high:
        raise Invalid(what, "%s is out of range (%s to %s)" % (what, low, high))
    return int(round(value))


def _text(value, pattern, what, optional=False):
    if value is None and optional:
        return None
    if not isinstance(value, str) or not pattern.match(value):
        raise Invalid(what, what + " is not in the expected form")
    return value


def validate(body, now):
    """A report as Bedrock sends it (README.md), checked and put in order;
    Invalid where anything in it is not what a terminal can show"""
    if not isinstance(body, dict):
        raise Invalid("report", "the report is not a JSON object")
    if body.get("v") != API_VERSION:
        raise Invalid("version", "unknown report version")
    channel = body.get("channel")
    if channel is not None and channel not in CHANNELS:
        raise Invalid("channel", "unknown channel")
    workload = body.get("workload")
    if workload is not None and workload not in WORKLOADS:
        raise Invalid("workload", "unknown workload")
    out = {
        "bedrock": _text(body.get("bedrock"), VERSION, "bedrock"),
        "game": _text(body.get("game"), VERSION, "game", optional=True),
        "channel": channel,
        "observed": _number(body.get("observed"), now - OBSERVED_PAST, now + OBSERVED_FUTURE, "observed"),
        "station": _text(body.get("station"), NAME, "station"),
        # before 4.10.2 raised every refinery's capacity, Levski stood at 42804 %
        "capacity": _number(body.get("capacity"), 0, 999999, "capacity", optional=True),
        "workload": workload,
        "quote": None,
    }
    quote = body.get("quote")
    if quote is not None:
        if not isinstance(quote, dict):
            raise Invalid("quote", "quote is not an object")
        items = quote.get("items")
        if not isinstance(items, list) or not 1 <= len(items) <= 30:
            raise Invalid("quote", "a quote has 1 to 30 items")
        q = {
            "method": _text(quote.get("method"), METHOD, "method"),
            "cost": _number(quote.get("cost"), 0, 10 ** 9, "cost"),
            "seconds": _number(quote.get("seconds"), 1, 30 * 86400, "seconds"),
            "items": [],
        }
        for item in items:
            if not isinstance(item, dict):
                raise Invalid("item", "an item is not an object")
            q["items"].append({
                "material": _text(item.get("material"), NAME, "material"),
                "quantity": _number(item.get("quantity"), 1, 10 ** 7, "quantity"),
                "quality": _number(item.get("quality"), 0, 1000, "quality", optional=True),
                "yield": _number(item.get("yield"), 0, 10 ** 7, "yield", optional=True),
            })
        out["quote"] = q
    if out["capacity"] is None and out["workload"] is None and out["quote"] is None:
        raise Invalid("empty", "nothing read: no capacity, workload or quote")
    return out


def level_of(capacity):
    """The workload level a capacity puts a refinery at (LEVEL_FROM)"""
    level = WORKLOADS[0]
    for name, start in zip(WORKLOADS, LEVEL_FROM):
        if capacity >= start:
            level = name
    return level


def judge(r, ranges, tables=None):
    """'' where a reading fits what the game does, else what does not.

    A workload level is taken only at its capacities (LEVEL_FROM), and
    extreme only up to twice what other senders saw it at (ranges,
    Store.ranges). A quote cannot yield more than was put in; where
    Bedrock's model has the factors, its yield has to come out as the
    model says - it did on every work order measured - and its cost and
    time within the bands the workload can move them."""
    tables = TABLES if tables is None else tables
    cap, level = r["capacity"], r["workload"]
    if cap is not None and level is not None:
        # Spans learned from the readings put extreme's floor at 421 % in
        # October 2026: Levski's 700 to 1000 % outnumbered every other
        # station's, and what was turned away never counted - 28 readings of
        # 303 to 404 % extreme, 172 to 175 % moderate and 206 % heavy.
        # The lowest level has no floor - Stanton Gateway (Pyro) stood at 7 % -
        # and the highest no ceiling near what was seen: Levski at 1000 % is
        # busy, not wrong. Only a capacity many times the busiest seen, 10796 %
        # at HUR-L2, is more likely a misreading than a refinery.
        i = WORKLOADS.index(level)
        floor = LEVEL_FROM[i] - RANGE_MARGIN if i > 0 else None
        ceiling = LEVEL_FROM[i + 1] + RANGE_MARGIN if i + 1 < len(WORKLOADS) else None
        if floor is not None and cap < floor or ceiling is not None and cap >= ceiling:
            if ceiling is None:
                return "capacity %d %% where %s is from %d %%" % (cap, level, LEVEL_FROM[i])
            if floor is None:
                return "capacity %d %% where %s is under %d %%" % (cap, level, LEVEL_FROM[i + 1])
            return "capacity %d %% where %s is %d-%d %%" % (cap, level, LEVEL_FROM[i], LEVEL_FROM[i + 1] - 1)
        if ceiling is None and level in ranges and cap > 2 * ranges[level][1] + RANGE_MARGIN:
            return "capacity %d %% where %s was seen up to %d %%" % (cap, level, ranges[level][1])
    q = r["quote"]
    if not q:
        return ""
    for item in q["items"]:
        if item["yield"] is not None and item["yield"] > item["quantity"] * 1.5:
            return "%s yields more than was put in" % item["material"]
    station = tables.get("stations", {}).get(r["station"])
    method = tables.get("methods", {}).get(q["method"])
    if not station or not method:
        return ""
    full = station["full"]
    cost = seconds = 0.0
    for item in q["items"]:
        ore = tables.get("ores", {}).get(item["material"])
        cell = station["materials"].get(item["material"])
        if not ore or not cell:
            return ""    # a material the model has no factors for: nothing more to judge by
        if item["yield"] is not None and ore["known"]:
            want = item["quantity"] * ore["yield"] * cell["yield"] * method["yield"]
            if want > 0 and not YIELD_BAND[0] <= item["yield"] / want <= YIELD_BAND[1]:
                return "%s yields %d where the model has %.0f" % (item["material"], item["yield"], want)
        full = full and ore["known"]
        cost += item["quantity"] * ore["cost"] * cell["cost"] * method["cost"]
        seconds += item["quantity"] * ore["time"] * cell["time"] * method["time"]
    if full and cost > 0 and not COST_BAND[0] <= q["cost"] / cost <= COST_BAND[1]:
        return "cost %d where the model has %.0f" % (q["cost"], cost)
    if full and seconds > 0 and not TIME_BAND[0] <= q["seconds"] / seconds <= TIME_BAND[1]:
        return "time %d s where the model has %.0f s" % (q["seconds"], seconds)
    return ""


def agree(a, b):
    """Two readings of a station tell the same: the same workload where both
    have one, capacities within the outlier margin where both have one"""
    if a["workload"] and b["workload"] and a["workload"] != b["workload"]:
        return False
    if a["capacity"] is not None and b["capacity"] is not None:
        if abs(a["capacity"] - b["capacity"]) > max(OUTLIER_POINTS, 0.25 * max(a["capacity"], b["capacity"])):
            return False
    return True


def consensus(readings):
    """One station's state from each sender's newest reading in
    CONSENSUS_SECONDS: the newest of them - the station as it was seen last -
    unless two or more of the others agree on something else and outnumber
    the ones that back it, when the newest of theirs stands. Confirmed once
    another sender backs it."""
    ordered = sorted(readings, key=lambda r: (r["observed"], r["id"]), reverse=True)
    shown, backing = ordered[0], 0
    for candidate in ordered:
        backers = sum(1 for r in ordered if r is not candidate and agree(r, candidate))
        against = len(ordered) - 1 - backers
        if against < 2 or against <= backers + 1:
            shown, backing = candidate, backers
            break
    return {"capacity": shown["capacity"], "workload": shown["workload"], "observed": shown["observed"],
            "sources": len(readings), "confirmed": backing >= 1}


# ---------------------------------------------------------------- a batch of rocks

def _real(value, low, high, what):
    """As _number, but the value as it was sent: SCU, shares and instability
    have decimals"""
    _number(value, low, high, what)
    return value


def _read(value, high, what):
    """A figure the panel printed, 0 to `high` - or -1 where Bedrock could
    not read it, kept as None"""
    if value == -1 and not isinstance(value, bool):
        return None
    return _real(value, 0, high, what)


def _choice(value, choices, what):
    """One of `choices`, or None where it is left out or empty"""
    if value is None or value == "":
        return None
    if value not in choices:
        raise Invalid(what, "unknown " + what)
    return value


def _part(part):
    """A line of a rock's composition: an ore with its share in per cent and
    its quality, or the inert materials with their share"""
    if not isinstance(part, dict):
        raise Invalid("part", "a part is not an object")
    inert = part.get("inert")
    if inert is not None and not isinstance(inert, bool):
        raise Invalid("inert", "inert is not true or false")
    percent = _real(part.get("percent"), 0, 100, "percent")
    if inert:
        return {"inert": True, "percent": percent}
    if part.get("ore") not in ROCK_ORES:
        raise Invalid("ore", "unknown ore")
    return {"ore": part["ore"], "percent": percent, "quality": _number(part.get("quality"), 0, 1000, "quality")}


def _rock(rock, now):
    if not isinstance(rock, dict):
        raise Invalid("rock", "a rock is not an object")
    if rock.get("ore") not in ROCK_ORES:
        raise Invalid("ore", "unknown ore")
    parts = rock.get("parts")
    if parts is None:
        parts = []
    if not isinstance(parts, list) or len(parts) > ROCK_PARTS_MAX:
        raise Invalid("parts", "a rock has 0 to %d parts" % ROCK_PARTS_MAX)
    place = rock.get("place")
    return {
        "observed": _number(rock.get("observed"), now - OBSERVED_PAST, now + OBSERVED_FUTURE, "observed"),
        "ore": rock["ore"],
        "mass": _real(rock.get("mass"), 1, 1000000, "mass"),             # kg
        "scu": _real(rock.get("scu"), 0.01, 100, "scu"),
        "resistance": _read(rock.get("resistance"), 100, "resistance"),  # per cent, as printed
        "printed": _choice(rock.get("printed"), ROCK_PRINTED, "printed"),
        "instability": _read(rock.get("instability"), 100000, "instability"),
        # metres to the rock; 0 where not known
        "range": _number(rock.get("range"), 0, 2000, "range", optional=True) or None,
        "ship": _choice(rock.get("ship"), ROCK_SHIPS, "ship"),
        "place": None if place in (None, "") else _text(place, NAME, "place"),
        # in the panel's order; none where the list was not read whole
        "parts": [_part(part) for part in parts],
    }


def validate_rocks(body, now):
    """A batch of rocks as Bedrock sends it (README.md), checked and put in
    order; Invalid where anything in it is not what the scan panel can show,
    and the whole batch is turned down"""
    if not isinstance(body, dict):
        raise Invalid("report", "the report is not a JSON object")
    if body.get("v") != API_VERSION:
        raise Invalid("version", "unknown report version")
    channel = body.get("channel")
    if channel is not None and channel not in CHANNELS:
        raise Invalid("channel", "unknown channel")
    rocks = body.get("rocks")
    if not isinstance(rocks, list) or not 1 <= len(rocks) <= ROCKS_MAX:
        raise Invalid("rocks", "a report has 1 to %d rocks" % ROCKS_MAX)
    return {
        "bedrock": _text(body.get("bedrock"), VERSION, "bedrock"),
        "game": _text(body.get("game"), VERSION, "game", optional=True),
        "channel": channel,
        "session": _text(body.get("session"), SESSION, "session"),
        "rocks": [_rock(rock, now) for rock in rocks],
    }


def judge_rock(rock):
    """'' where a rock's figures fit together, else what does not.

    Its mass has to lie between that of its volume at density 1 and at the
    densest element its ore can hold, with room for the panel rounding the
    mass to a kilogram and the volume to 0.01 SCU. The panel's first
    picture, before the game knows what the rock is made of, shows density 1
    and neither resistance nor instability - only ice is like that once
    scanned. Where the composition was read, its shares add up to 100 % and
    each quality is one its ore has (ORE_QUALITIES).
    A rock that does not fit is kept, marked and left out of the analysis;
    it does not hold its sender back, as a panel is easily misread."""
    mass, scu = rock["mass"], rock["scu"]
    if scu > ROCK_SCU_MAX:
        return "bigger than any rock"
    if (mass + 1) / max(0.001, scu - 0.01) < ROCK_KG_PER_SCU * 0.99:
        return "lighter than its volume"
    if (mass - 1) / (scu + 0.01) > ROCK_KG_PER_SCU * ROCK_DENSITY_MAX[rock["ore"]] * 1.01:
        return "heavier than its parts"
    if (rock["ore"] != "Ice" and rock["resistance"] == 0 and rock["instability"] == 0
            and abs(mass - ROCK_KG_PER_SCU * scu) <= max(1.5, 0.006 * mass)):
        return "not scanned yet"
    if rock["parts"] and not 98 <= sum(part["percent"] for part in rock["parts"]) <= 100.5:
        return "parts do not add up"
    if any(not part.get("inert") and part["quality"] not in ORE_QUALITIES[part["ore"]] for part in rock["parts"]):
        return "a quality its ore does not have"
    return ""


def _figure(x):
    """A figure as it was sent: 25005, not 25005.0"""
    if x is None:
        return ""
    return "%d" % x if x == int(x) else repr(x)


def parts_text(parts):
    """A rock's composition as kept (JSON) on one line, "Taranite 9.37 957;
    inert 52.11" - each ore with its share and quality, the inert share"""
    if not parts:
        return ""
    return "; ".join("inert " + _figure(p["percent"]) if p.get("inert")
                     else "%s %s %d" % (p["ore"], _figure(p["percent"]), p["quality"]) for p in json.loads(parts))


# ---------------------------------------------------------------- the store

SCHEMA = """
CREATE TABLE IF NOT EXISTS observations (
    id INTEGER PRIMARY KEY,
    digest TEXT NOT NULL UNIQUE,      -- the same reading sent twice is kept once
    received INTEGER NOT NULL,        -- server time, UTC seconds
    observed INTEGER NOT NULL,        -- when the terminal showed it
    station TEXT NOT NULL,
    known INTEGER NOT NULL,           -- the station is in stations.txt
    source TEXT,                      -- tells senders apart; NULL after SOURCE_HOURS
    plausible INTEGER NOT NULL,       -- 0: kept, but it did not fit what the game does
    reason TEXT,                      -- ...what did not
    capacity INTEGER,                 -- per cent
    workload TEXT,
    game TEXT,
    channel TEXT,
    bedrock TEXT,
    method TEXT,                      -- a quote's; NULL for a reading without one
    cost INTEGER,                     -- aUEC
    seconds INTEGER
);
CREATE INDEX IF NOT EXISTS observations_station ON observations (station, observed);
CREATE INDEX IF NOT EXISTS observations_observed ON observations (observed);
CREATE INDEX IF NOT EXISTS observations_source ON observations (source, station, received);
CREATE TABLE IF NOT EXISTS quote_items (
    observation INTEGER NOT NULL REFERENCES observations (id),
    position INTEGER NOT NULL,
    material TEXT NOT NULL,
    quantity INTEGER NOT NULL,        -- cSCU put in
    quality INTEGER,
    yield INTEGER,                    -- cSCU it comes out at
    PRIMARY KEY (observation, position)
);
CREATE TABLE IF NOT EXISTS counters (
    day TEXT NOT NULL,                -- UTC
    name TEXT NOT NULL,               -- accepted, duplicate, implausible, limited, held, rejected, rejected:<field>;
                                      -- req:<route>, agent:<version>, platform:<os>, clients, senders;
                                      -- rocks:accepted, rocks:duplicate, rocks:implausible (each rock), rocks:limited,
                                      -- rocks:held, rocks:rejected, rocks:rejected:<field> (each batch), rock-senders
    count INTEGER NOT NULL,
    PRIMARY KEY (day, name)
);
CREATE TABLE IF NOT EXISTS hourly (
    hour TEXT NOT NULL,               -- UTC, 'YYYY-MM-DD HH'
    name TEXT NOT NULL,               -- req:<route>
    count INTEGER NOT NULL,
    PRIMARY KEY (hour, name)
);
CREATE TABLE IF NOT EXISTS rocks (
    id INTEGER PRIMARY KEY,
    received INTEGER NOT NULL,        -- server time, UTC seconds
    observed INTEGER NOT NULL,        -- when the scan panel showed it
    source TEXT,                      -- tells senders apart; NULL after SOURCE_HOURS
    sender_nth INTEGER,               -- which of its sender's rocks of the UTC day it was, 1 for the first
    session TEXT NOT NULL,            -- the random number of the Bedrock start that sent it
    bedrock TEXT,
    game TEXT,
    channel TEXT,
    ore TEXT NOT NULL,
    mass REAL NOT NULL,               -- kg
    scu REAL NOT NULL,
    resistance REAL,                  -- per cent, as printed; NULL where it was not read
    printed TEXT,                     -- white, or colour: a laser on the rock had changed it
    instability REAL,                 -- NULL where it was not read
    range_m INTEGER,                  -- metres to the rock; NULL where not known
    ship TEXT,
    place TEXT,
    parts TEXT,                       -- the composition as JSON, in the panel's order; NULL where not read whole
    implausible TEXT,                 -- what does not fit (judge_rock); NULL where it all does
    UNIQUE (session, observed, ore, mass, scu)   -- the same rock sent twice is kept once
);
-- the prune's: only the rocks of the last two days still have a token
CREATE INDEX IF NOT EXISTS rocks_token ON rocks (received) WHERE source IS NOT NULL;
-- ...and add_rocks', counting a sender's rocks of the day
CREATE INDEX IF NOT EXISTS rocks_source ON rocks (source) WHERE source IS NOT NULL;
"""


class Store:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()   # one writer at a time
        self.salts = {}                # UTC day -> salt; in memory only, so the tokens cannot be undone
        self.salt_lock = threading.Lock()
        self.seen = {}                 # (what, UTC day) -> the tokens seen that day; in memory only
        db = self._db()
        try:
            db.execute("PRAGMA journal_mode=WAL")
            db.executescript(SCHEMA)
        finally:
            db.close()

    def _db(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA foreign_keys=ON")
        return db

    # -- telling senders apart

    def source(self, address, now):
        """The sender's token for today: the same address gives the same one
        all day, another day another one, and without the salt - which never
        leaves memory - no one can tell which address it came from"""
        day = utc_day(now)
        with self.salt_lock:
            salt = self.salts.get(day)
            if salt is None:
                salt = self.salts[day] = secrets.token_bytes(32)
        return hmac.new(salt, address.encode("utf-8", "replace"), hashlib.sha256).hexdigest()[:16]

    def readings_by(self, source, station, now):
        db = self._db()
        try:
            return db.execute("SELECT COUNT(*) FROM observations WHERE source = ? AND station = ? AND received >= ?",
                              (source, station, now - 3600)).fetchone()[0]
        finally:
            db.close()

    # -- what the game does, as the senders saw it

    def ranges(self, now):
        """Each workload level's span of capacities over RANGE_DAYS - the
        10th to the 90th percentile of the plausible readings with it, where
        RANGE_MIN_READINGS of them came from RANGE_MIN_SOURCES senders - and
        only while the levels' medians rise in the game's order; else {}.
        judge takes only extreme's top from them; where each level begins
        is LEVEL_FROM."""
        db = self._db()
        try:
            rows = db.execute(
                "SELECT workload, capacity, source FROM observations WHERE plausible = 1 AND workload IS NOT NULL"
                " AND capacity IS NOT NULL AND observed >= ?", (now - RANGE_DAYS * 86400,)).fetchall()
        finally:
            db.close()
        values = collections.defaultdict(list)
        senders = collections.defaultdict(set)
        for i, row in enumerate(rows):
            values[row["workload"]].append(row["capacity"])
            senders[row["workload"]].add(row["source"] or "#%d" % i)   # an old reading counts as a sender of its own
        out, medians = {}, []
        for level in WORKLOADS:
            caps = sorted(values.get(level, []))
            if len(caps) < RANGE_MIN_READINGS or len(senders[level]) < RANGE_MIN_SOURCES:
                continue
            pick = lambda q: caps[int(round(q * (len(caps) - 1)))]
            out[level] = (pick(0.1), pick(0.9))
            medians.append(statistics.median(caps))
        if any(b <= a for a, b in zip(medians, medians[1:])):
            return {}
        return out

    # -- keeping and counting

    def add(self, r, now, source, reason):
        """Keeps a checked report; False where the same reading is kept already"""
        digest = hashlib.sha256(json.dumps(
            [r["station"], r["observed"], r["capacity"], r["workload"], r["quote"]],
            sort_keys=True).encode("utf-8")).hexdigest()
        q = r["quote"] or {}
        with self.lock:
            db = self._db()
            try:
                with db:
                    cur = db.execute(
                        "INSERT OR IGNORE INTO observations (digest, received, observed, station, known, source,"
                        " plausible, reason, capacity, workload, game, channel, bedrock, method, cost, seconds)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        (digest, now, r["observed"], r["station"], int(r["station"] in STATIONS), source,
                         int(not reason), reason or None, r["capacity"], r["workload"], r["game"], r["channel"],
                         r["bedrock"], q.get("method"), q.get("cost"), q.get("seconds")))
                    if cur.rowcount == 0:
                        return False
                    for i, item in enumerate(q.get("items", [])):
                        db.execute("INSERT INTO quote_items VALUES (?, ?, ?, ?, ?, ?)",
                                   (cur.lastrowid, i, item["material"], item["quantity"], item["quality"],
                                    item["yield"]))
                return True
            finally:
                db.close()

    def add_rocks(self, batch, now, source, reasons):
        """Keeps a checked batch of rocks, each with what did not fit ('' where
        it all did) and which of its sender's rocks of the day it is; for each
        rock whether it is new - the same rock sent twice is kept once"""
        new = []
        with self.lock:
            db = self._db()
            try:
                with db:
                    # A token is one sender's for one UTC day, so its rocks are
                    # that day's: the analysis counts no sender for more than so
                    # many, and one who sends thousands weighs no more than a
                    # player who mined all day. (A restart of the service starts
                    # the day's count again: the salt dies with it.)
                    nth = db.execute("SELECT COUNT(*) FROM rocks WHERE source = ?", (source,)).fetchone()[0]
                    for rock, reason in zip(batch["rocks"], reasons):
                        parts = json.dumps(rock["parts"], separators=(",", ":")) if rock["parts"] else None
                        cur = db.execute(
                            "INSERT OR IGNORE INTO rocks (received, observed, source, sender_nth, session, bedrock,"
                            " game, channel, ore, mass, scu, resistance, printed, instability, range_m, ship, place,"
                            " parts, implausible) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                            (now, rock["observed"], source, nth + 1, batch["session"], batch["bedrock"],
                             batch["game"], batch["channel"], rock["ore"], rock["mass"], rock["scu"],
                             rock["resistance"], rock["printed"], rock["instability"], rock["range"], rock["ship"],
                             rock["place"], parts, reason or None))
                        if cur.rowcount == 1:
                            nth += 1
                        new.append(cur.rowcount == 1)
                return new
            finally:
                db.close()

    def tally(self, now, *names):
        """One more of each, today - and this hour, for the requests (req:)"""
        self.tally_many(now, [(name, 1) for name in names])

    def tally_many(self, now, counts):
        """As tally, n more of each (name, n) - a batch of rocks counts every
        rock; nothing where n is 0"""
        with self.lock:
            db = self._db()
            try:
                with db:
                    for name, n in counts:
                        if n < 1:
                            continue
                        db.execute("INSERT INTO counters VALUES (?, ?, ?)"
                                   " ON CONFLICT (day, name) DO UPDATE SET count = count + excluded.count",
                                   (utc_day(now), name, n))
                        if name.startswith("req:"):
                            db.execute("INSERT INTO hourly VALUES (?, ?, ?)"
                                       " ON CONFLICT (hour, name) DO UPDATE SET count = count + excluded.count",
                                       (utc_hour(now), name, n))
            finally:
                db.close()

    def first_today(self, what, token, now):
        """Whether `token` is new today for `what` (clients, senders) - in
        memory only, so how many asked can be counted without keeping who"""
        with self.salt_lock:
            seen = self.seen.setdefault((what, utc_day(now)), set())
            if token in seen:
                return False
            seen.add(token)
            return True

    def counters(self, since_day):
        db = self._db()
        try:
            out = collections.defaultdict(dict)
            for row in db.execute("SELECT day, name, count FROM counters WHERE day >= ? ORDER BY day, name",
                                  (since_day,)):
                out[row["day"]][row["name"]] = row["count"]
            return dict(out)
        finally:
            db.close()

    def count(self):
        db = self._db()
        try:
            return db.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
        finally:
            db.close()

    def rock_count(self):
        db = self._db()
        try:
            return db.execute("SELECT COUNT(*) FROM rocks").fetchone()[0]
        finally:
            db.close()

    def stats(self, now, days):
        """What the admin view draws (GET /v1/stats): the counters by day and
        the requests by hour, the readings and quotes that came in by day, and
        each station's state with how often it was read - counts, never who"""
        since = now - days * 86400
        db = self._db()
        try:
            counters = collections.defaultdict(dict)
            for row in db.execute("SELECT day, name, count FROM counters WHERE day >= ?", (utc_day(since),)):
                counters[row["day"]][row["name"]] = row["count"]
            hours = collections.defaultdict(dict)
            for row in db.execute("SELECT hour, name, count FROM hourly WHERE hour >= ?",
                                  (utc_hour(now - HOURLY_DAYS * 86400),)):
                hours[row["hour"]][row["name"]] = row["count"]
            came_in = {}
            for row in db.execute(
                    "SELECT strftime('%Y-%m-%d', received, 'unixepoch') AS day, COUNT(*) AS readings,"
                    " COUNT(method) AS quotes, SUM(plausible = 0) AS implausible FROM observations"
                    " WHERE received >= ? GROUP BY day", (since,)):
                came_in[row["day"]] = {"readings": row["readings"], "quotes": row["quotes"],
                                       "implausible": row["implausible"]}
            read = {}
            for row in db.execute(
                    "SELECT station, MAX(known) AS known, COUNT(*) AS readings, COUNT(method) AS quotes,"
                    " MAX(observed) AS last FROM observations WHERE received >= ? GROUP BY station", (since,)):
                read[row["station"]] = {"known": bool(row["known"]), "readings": row["readings"],
                                        "quotes": row["quotes"], "last": row["last"]}
            total = db.execute("SELECT COUNT(*), COUNT(method), COUNT(DISTINCT station) FROM observations").fetchone()
        finally:
            db.close()
        return {"generated": now, "days": days, "hourly_days": HOURLY_DAYS, "counters": dict(counters),
                "hours": dict(hours), "came_in": came_in, "read": read, "stations": self.stations(now),
                "totals": {"observations": total[0], "quotes": total[1], "stations": total[2]}}

    def prune(self, now):
        """Readings and rocks lose their sender's token after SOURCE_HOURS,
        counters go after COUNTER_DAYS and the hourly ones after HOURLY_DAYS,
        only today's and yesterday's salt stay, and only today's tokens seen"""
        with self.lock:
            db = self._db()
            try:
                with db:
                    db.execute("UPDATE observations SET source = NULL WHERE source IS NOT NULL AND received < ?",
                               (now - SOURCE_HOURS * 3600,))
                    db.execute("UPDATE rocks SET source = NULL WHERE source IS NOT NULL AND received < ?",
                               (now - SOURCE_HOURS * 3600,))
                    db.execute("DELETE FROM counters WHERE day < ?", (utc_day(now - COUNTER_DAYS * 86400),))
                    db.execute("DELETE FROM hourly WHERE hour < ?", (utc_hour(now - HOURLY_DAYS * 86400),))
            finally:
                db.close()
        keep = {utc_day(now), utc_day(now - 86400)}
        with self.salt_lock:
            for day in [d for d in self.salts if d not in keep]:
                del self.salts[day]
            for key in [k for k in self.seen if k[1] != utc_day(now)]:
                del self.seen[key]

    # -- handing out

    def stations(self, now):
        """Each known station's state: from its readings of the last
        CONSENSUS_SECONDS where there are any (fresh), else its newest within
        STATION_DAYS; confirmed once another sender saw the same in that time.
        Where that reading has the capacity only - the line under it was not
        read, at Levski in 20 of 125 readings by 1 November 2026 - the
        workload is the one the capacity gives (LEVEL_FROM), and
        workload_read says so."""
        db = self._db()
        try:
            rows = db.execute(
                "SELECT id, station, capacity, workload, observed, source FROM observations"
                " WHERE known = 1 AND plausible = 1 AND observed >= ?"
                " AND (capacity IS NOT NULL OR workload IS NOT NULL) ORDER BY observed, id",
                (now - STATION_DAYS * 86400,)).fetchall()
            counts = {row["station"]: (row["seen"], row["quotes"]) for row in db.execute(
                "SELECT station, COUNT(*) AS seen, COUNT(method) AS quotes FROM observations"
                " WHERE known = 1 AND plausible = 1 AND observed >= ? GROUP BY station", (now - 86400,))}
        finally:
            db.close()
        by_station = collections.defaultdict(list)
        for row in rows:
            by_station[row["station"]].append(row)
        out = []
        for name in sorted(by_station):
            readings = by_station[name]
            recent = [r for r in readings if r["observed"] >= now - CONSENSUS_SECONDS]
            if recent:
                newest = {}
                for r in recent:    # in time order: each sender's last one stays
                    newest[r["source"] or "#%d" % r["id"]] = r
                state = consensus(list(newest.values()))
            else:
                last = readings[-1]
                state = {"capacity": last["capacity"], "workload": last["workload"], "observed": last["observed"],
                         "sources": 0, "confirmed": False}
            seen, quotes = counts.get(name, (0, 0))
            workload, read = state["workload"], state["workload"] is not None
            if not read and state["capacity"] is not None:
                workload = level_of(state["capacity"])
            out.append({"station": name, "capacity": state["capacity"], "workload": workload,
                        "workload_read": read,
                        "observed": state["observed"], "fresh": bool(recent), "sources": state["sources"],
                        "confirmed": state["confirmed"], "seen_24h": seen, "quotes_24h": quotes})
        return out

    def quotes(self, now, days, station=None):
        """The plausible quotes of known stations in the last `days`, the newest first"""
        db = self._db()
        try:
            sql = ("SELECT id, station, observed, capacity, workload, method, cost, seconds, game, bedrock"
                   " FROM observations WHERE method IS NOT NULL AND known = 1 AND plausible = 1 AND observed >= ?")
            args = [now - days * 86400]
            if station is not None:
                sql += " AND station = ?"
                args.append(station)
            sql += " ORDER BY observed DESC LIMIT ?"
            args.append(QUOTES_MAX)
            rows = db.execute(sql, args).fetchall()
            items = self._items(db, [row["id"] for row in rows])
        finally:
            db.close()
        return [{"station": row["station"], "observed": row["observed"], "capacity": row["capacity"],
                 "workload": row["workload"], "method": row["method"], "cost": row["cost"],
                 "seconds": row["seconds"], "game": row["game"], "bedrock": row["bedrock"],
                 "items": items[row["id"]]} for row in rows]

    @staticmethod
    def _items(db, ids):
        items = collections.defaultdict(list)
        for start in range(0, len(ids), 500):
            part = ids[start:start + 500]
            for it in db.execute(
                    "SELECT observation, material, quantity, quality, yield FROM quote_items"
                    " WHERE observation IN (%s) ORDER BY observation, position" % ",".join("?" * len(part)), part):
                items[it["observation"]].append({"material": it["material"], "quantity": it["quantity"],
                                                 "quality": it["quality"], "yield": it["yield"]})
        return items

    def export_csv(self):
        """Every reading kept, for anyone to work with - with whether it was
        taken as plausible, never with who sent it"""
        db = self._db()
        try:
            items = collections.defaultdict(list)
            for it in db.execute("SELECT observation, material, quantity, quality, yield FROM quote_items"
                                 " ORDER BY observation, position"):
                items[it["observation"]].append([it["material"], it["quantity"], it["quality"], it["yield"]])
            out = io.StringIO()
            w = csv.writer(out, lineterminator="\n")
            w.writerow(["observed", "station", "known", "plausible", "reason", "capacity", "workload", "method",
                        "cost", "seconds", "items", "game", "channel", "bedrock"])
            for row in db.execute("SELECT * FROM observations ORDER BY observed, id"):
                w.writerow([row["observed"], row["station"], row["known"], row["plausible"], row["reason"] or "",
                            row["capacity"], row["workload"], row["method"], row["cost"], row["seconds"],
                            json.dumps(items[row["id"]]) if row["id"] in items else "",
                            row["game"], row["channel"], row["bedrock"]])
            return out.getvalue()
        finally:
            db.close()

    # -- for whoever runs it

    def rocks_csv(self, out):
        """Every rock kept, as CSV into `out`, for whoever runs the service -
        with what did not fit, never who sent it; how many rocks"""
        db = self._db()
        try:
            w = csv.writer(out, lineterminator="\n")
            w.writerow(["id", "received", "observed", "bedrock", "game", "channel", "session", "sender_nth", "ore",
                        "mass", "scu", "resistance", "printed", "instability", "range_m", "ship", "place", "parts",
                        "implausible"])
            n = 0
            for row in db.execute("SELECT * FROM rocks ORDER BY id"):
                w.writerow([row["id"], utc_iso(row["received"]), utc_iso(row["observed"]), row["bedrock"],
                            row["game"], row["channel"], row["session"], row["sender_nth"], row["ore"],
                            _figure(row["mass"]),
                            _figure(row["scu"]), _figure(row["resistance"]), row["printed"],
                            _figure(row["instability"]), row["range_m"], row["ship"], row["place"],
                            parts_text(row["parts"]), row["implausible"]])
                n += 1
            return n
        finally:
            db.close()

    def purge(self, since, until, station=None, implausible_only=False, really=False):
        """The readings observed from `since` to `until` (UTC seconds), of one
        station or all, all of them or the implausible ones: how many, and
        with `really` they are taken out"""
        where = "observed >= ? AND observed <= ?"
        args = [since, until]
        if station:
            where += " AND station = ?"
            args.append(station)
        if implausible_only:
            where += " AND plausible = 0"
        with self.lock:
            db = self._db()
            try:
                with db:
                    found = db.execute("SELECT COUNT(*) FROM observations WHERE " + where, args).fetchone()[0]
                    if really and found:
                        db.execute("DELETE FROM quote_items WHERE observation IN (SELECT id FROM observations WHERE "
                                   + where + ")", args)
                        db.execute("DELETE FROM observations WHERE " + where, args)
                return found
            finally:
                db.close()

    def rejudge(self, now, really=False):
        """The implausible readings judged again by the rules as they stand
        (judge, ranges): how many there are and how many would pass, and with
        `really` those are taken as plausible - after a rule was found too
        strict. The day's counters keep what they counted."""
        ranges = self.ranges(now)
        db = self._db()
        try:
            rows = db.execute("SELECT * FROM observations WHERE plausible = 0 ORDER BY id").fetchall()
            items = collections.defaultdict(list)
            for item in db.execute("SELECT * FROM quote_items WHERE observation IN"
                                   " (SELECT id FROM observations WHERE plausible = 0) ORDER BY observation, position"):
                items[item["observation"]].append({"material": item["material"], "quantity": item["quantity"],
                                                   "quality": item["quality"], "yield": item["yield"]})
        finally:
            db.close()
        passing = []
        for row in rows:
            quote = None
            if row["method"]:
                quote = {"method": row["method"], "cost": row["cost"], "seconds": row["seconds"],
                         "items": items[row["id"]]}
            report = {"station": row["station"], "capacity": row["capacity"], "workload": row["workload"],
                      "quote": quote}
            if not judge(report, ranges):
                passing.append(row["id"])
        if really and passing:
            with self.lock:
                db = self._db()
                try:
                    with db:
                        db.executemany("UPDATE observations SET plausible = 1, reason = NULL WHERE id = ?",
                                       [(i,) for i in passing])
                finally:
                    db.close()
        return len(rows), len(passing)

    def backup(self, target):
        """A consistent copy while the service runs (SQLite's online backup)"""
        src = self._db()
        dst = sqlite3.connect(target)
        try:
            src.backup(dst)
        finally:
            dst.close()
            src.close()


# ---------------------------------------------------------------- the limit

class Limiter:
    """Reports per address and from everyone over the last hour, in memory,
    the address salted and hashed with a salt that dies with the process"""

    def __init__(self, per_address, overall):
        self.per_address = per_address
        self.overall = overall
        self.salt = secrets.token_bytes(16)
        self.lock = threading.Lock()
        self.seen = {}
        self.everyone = collections.deque()

    def allow(self, address, now, n=1):
        """Whether n more fit into the last hour - a report, or the rocks of
        a batch - and if so, they are counted"""
        key = hashlib.sha256(self.salt + address.encode("utf-8", "replace")).digest()
        hour = now - 3600
        with self.lock:
            while self.everyone and self.everyone[0] <= hour:
                self.everyone.popleft()
            times = self.seen.setdefault(key, collections.deque())
            while times and times[0] <= hour:
                times.popleft()
            if len(times) + n > self.per_address or len(self.everyone) + n > self.overall:
                return False
            times.extend([now] * n)
            self.everyone.extend([now] * n)
            if len(self.seen) > 10000:
                for k in [k for k, v in self.seen.items() if not v or v[-1] <= hour]:
                    del self.seen[k]
            return True


class Misfits:
    """The readings that did not fit over the last hour, per install and per
    address - in memory, salted and hashed with a salt that dies with the
    process: where either reaches MISFITS_PER_HOUR, its reports are refused
    for MISFIT_HOLD_SECONDS"""

    def __init__(self, per_hour=MISFITS_PER_HOUR, hold=MISFIT_HOLD_SECONDS):
        self.per_hour = per_hour
        self.hold = hold
        self.salt = secrets.token_bytes(16)
        self.lock = threading.Lock()
        self.seen = {}
        self.until = {}

    def _key(self, who):
        return hashlib.sha256(self.salt + who.encode("utf-8", "replace")).digest()

    def add(self, keys, now):
        hour = now - 3600
        with self.lock:
            for key in map(self._key, keys):
                times = self.seen.setdefault(key, collections.deque())
                while times and times[0] <= hour:
                    times.popleft()
                times.append(now)
                if len(times) >= self.per_hour:
                    self.until[key] = now + self.hold
                    times.clear()
            if len(self.seen) > 10000 or len(self.until) > 10000:
                for k in [k for k, v in self.seen.items() if not v or v[-1] <= hour]:
                    del self.seen[k]
                for k in [k for k, t in self.until.items() if t <= now]:
                    del self.until[k]

    def held(self, keys, now):
        with self.lock:
            return any(self.until.get(key, 0) > now for key in map(self._key, keys))


# ---------------------------------------------------------------- the service

class Handler(BaseHTTPRequestHandler):
    server_version = "bedrock-api"
    sys_version = ""
    store = None
    limiter = None
    rock_limiter = None
    rock_count_limiter = None
    misfits = None

    def log_message(self, fmt, *args):
        # the request and the answer, never the address or the agent
        sys.stderr.write("%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), fmt % args))

    def _route(self):
        path = urlsplit(self.path).path
        if not path.startswith(PREFIX + "/"):
            return None
        return path[len(PREFIX):]

    def _send(self, code, payload, kind="application/json; charset=utf-8"):
        body = payload if isinstance(payload, bytes) else json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _fail(self, code, why):
        self._send(code, {"ok": False, "error": why})

    def _install(self):
        number = (self.headers.get("X-Bedrock-Install") or "").strip().lower()
        return number if INSTALL.match(number) else None

    def _who(self, address):
        """Whom to count a request as: its install, by the number it sends,
        else its address"""
        install = self._install()
        return "install:" + install if install else address

    def _count(self, now, route):
        """The request, counted: what it asked for, the Bedrock version and
        platform its User-Agent names, and - for the stations - whether this
        install asked already today (by the senders' token, in memory only).
        Only what came through nginx (X-Real-IP): the container's own health
        check and the admin view are no players."""
        address = self.headers.get("X-Real-IP")
        if not address:
            return
        method = "GET" if self.command == "HEAD" else self.command
        names = ["req:" + ROUTE_NAMES.get((method, route), "other")]
        agent = AGENT.match(self.headers.get("User-Agent", ""))
        if agent:
            names += ["agent:" + agent.group(1), "platform:" + agent.group(2).lower()]
        else:
            names.append("agent:other")
        if route == "/v1/refinery/stations" and self.store.first_today(
                "clients", self.store.source(self._who(address), now), now):
            names.append("clients")
        try:
            self.store.tally(now, *names)
        except sqlite3.Error as e:    # a count never stands in the way of an answer
            sys.stderr.write("count: %s\n" % e)

    def do_GET(self):
        now = int(time.time())
        route = self._route()
        query = parse_qs(urlsplit(self.path).query)
        if route == "/v1/stats":
            # the admin view's, asked from inside the containers' network:
            # whatever comes through nginx carries X-Real-IP, and is not told
            # this exists
            if self.headers.get("X-Real-IP"):
                return self._fail(404, "not found")
            try:
                days = int(query.get("days", ["90"])[0])
            except ValueError:
                return self._fail(400, "days is not a number")
            return self._send(200, self.store.stats(now, max(1, min(COUNTER_DAYS, days))))
        self._count(now, route)
        if route == "/v1/health":
            today = self.store.counters(utc_day(now)).get(utc_day(now), {})
            return self._send(200, {"ok": True, "version": API_VERSION, "observations": self.store.count(),
                                    "rocks": self.store.rock_count(),
                                    "today": {name: today.get(name, 0) for name in
                                              ("accepted", "duplicate", "implausible", "limited", "held",
                                               "rejected")}})
        if route == "/v1/refinery/stations":
            return self._send(200, {"generated": now, "days": STATION_DAYS, "window": CONSENSUS_SECONDS,
                                    "stations": self.store.stations(now)})
        if route == "/v1/refinery/quotes":
            try:
                days = int(query.get("days", ["14"])[0])
            except ValueError:
                return self._fail(400, "days is not a number")
            days = max(1, min(QUOTE_DAYS_MAX, days))
            station = query.get("station", [None])[0]
            if station is not None and not NAME.match(station):
                return self._fail(400, "station is not in the expected form")
            return self._send(200, {"generated": now, "days": days,
                                    "quotes": self.store.quotes(now, days, station)})
        if route == "/v1/refinery/export.csv":
            return self._send(200, self.store.export_csv().encode("utf-8"), "text/csv; charset=utf-8")
        if route == "/v1/rocks/export.csv":
            # the operator's, like /v1/stats: through nginx it is not there
            if self.headers.get("X-Real-IP"):
                return self._fail(404, "not found")
            out = io.StringIO()
            self.store.rocks_csv(out)
            return self._send(200, out.getvalue().encode("utf-8"), "text/csv; charset=utf-8")
        return self._fail(404, "not found")

    do_HEAD = do_GET

    def do_POST(self):
        now = int(time.time())
        route = self._route()
        self._count(now, route)
        # a refinery reading comes alone, the rocks in batches
        largest = {"/v1/refinery/observations": MAX_BODY, "/v1/rocks/observations": MAX_ROCK_BODY}.get(route)
        if largest is None:
            return self._fail(404, "not found")
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return self._fail(411, "length required")
        if length < 0 or length > largest:
            return self._fail(413, "too large")
        if self.headers.get("Content-Type", "").split(";")[0].strip().lower() != "application/json":
            return self._fail(415, "send application/json")
        # nginx puts the address it saw here; the service is reachable from
        # the containers' network only
        address = self.headers.get("X-Real-IP") or self.client_address[0]
        # held back by its install's number and by its address: a number is
        # easily made up
        install = self._install()
        misfit = [address] + (["install:" + install] if install else [])
        if route == "/v1/rocks/observations":
            return self._rocks(now, address, misfit, length)
        if self.misfits.held(misfit, now):
            self.store.tally(now, "held")
            return self._fail(429, "too many readings that did not fit, try again in a few minutes")
        if not self.limiter.allow(address, now):
            self.store.tally(now, "limited")
            return self._fail(429, "too many reports, try again later")
        raw = self.rfile.read(length)
        try:
            report = validate(json.loads(raw.decode("utf-8")), now)
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.misfits.add(misfit, now)
            self.store.tally(now, "rejected", "rejected:json")
            return self._fail(400, "not JSON")
        except Invalid as e:
            self.misfits.add(misfit, now)
            self.store.tally(now, "rejected", "rejected:" + e.code)
            return self._fail(400, str(e))
        source = self.store.source(address, now)
        if self.store.readings_by(source, report["station"], now) >= STATION_READINGS_PER_HOUR:
            self.store.tally(now, "limited")
            return self._fail(429, "too many readings of this station, try again later")
        reason = judge(report, self.store.ranges(now))
        stored = self.store.add(report, now, source, reason)
        if reason and stored:
            self.misfits.add(misfit, now)
        names = [("implausible" if reason else "accepted") if stored else "duplicate"]
        if self.store.first_today("senders", self.store.source(self._who(address), now), now):
            names.append("senders")
        self.store.tally(now, *names)
        # whether it was taken as plausible is not said: that would teach a
        # forger what passes
        return self._send(201 if stored else 200, {"ok": True, "stored": stored})

    def _rocks(self, now, address, misfit, length):
        """A batch of rocks (POST /v1/rocks/observations), kept whole where it
        fits the format; a rock whose figures do not fit together is marked
        (judge_rock), and the sender is neither told nor held back for it"""
        if self.misfits.held(misfit, now):
            self.store.tally(now, "rocks:held")
            return self._fail(429, "too many readings that did not fit, try again in a few minutes")
        if not self.rock_limiter.allow(address, now):
            self.store.tally(now, "rocks:limited")
            return self._fail(429, "too many reports, try again later")
        raw = self.rfile.read(length)
        try:
            batch = validate_rocks(json.loads(raw.decode("utf-8")), now)
        except (UnicodeDecodeError, json.JSONDecodeError, RecursionError):   # the last: nested too deep to parse
            self.misfits.add(misfit, now)
            self.store.tally(now, "rocks:rejected", "rocks:rejected:json")
            return self._fail(400, "not JSON")
        except Invalid as e:
            self.misfits.add(misfit, now)
            self.store.tally(now, "rocks:rejected", "rocks:rejected:" + e.code)
            return self._fail(400, str(e))
        # ...and the rocks in it, counted as rocks: a batch of fifty is fifty
        if not self.rock_count_limiter.allow(address, now, len(batch["rocks"])):
            self.store.tally(now, "rocks:limited")
            return self._fail(429, "too many rocks, try again later")
        reasons = [judge_rock(rock) for rock in batch["rocks"]]
        new = self.store.add_rocks(batch, now, self.store.source(address, now), reasons)
        stored = sum(new)
        counts = [("rocks:accepted", sum(1 for n, r in zip(new, reasons) if n and not r)),
                  ("rocks:implausible", sum(1 for n, r in zip(new, reasons) if n and r)),
                  ("rocks:duplicate", len(new) - stored)]
        if self.store.first_today("rock-senders", self.store.source(self._who(address), now), now):
            counts.append(("rock-senders", 1))
        self.store.tally_many(now, counts)
        return self._send(201 if stored else 200, {"ok": True, "stored": stored, "duplicates": len(new) - stored})


def serve(store, limiter, host="0.0.0.0", port=PORT, misfits=None, rock_limiter=None, rock_count_limiter=None):
    """The service, not started yet: serve_forever() on the result. A thread
    beside it prunes every ten minutes (Store.prune). The rocks have limits
    of their own: batches (ROCK_POSTS_PER_HOUR) and rocks (ROCKS_PER_HOUR)."""
    handler = type("BoundHandler", (Handler,), {
        "store": store, "limiter": limiter, "misfits": misfits or Misfits(),
        "rock_limiter": rock_limiter or Limiter(ROCK_POSTS_PER_HOUR, ROCK_POSTS_PER_HOUR_ALL),
        "rock_count_limiter": rock_count_limiter or Limiter(ROCKS_PER_HOUR, ROCKS_PER_HOUR_ALL)})
    server = ThreadingHTTPServer((host, port), handler)
    server.daemon_threads = True

    def prune():
        while True:
            try:
                store.prune(int(time.time()))
            except sqlite3.Error as e:
                sys.stderr.write("prune: %s\n" % e)
            time.sleep(600)

    threading.Thread(target=prune, daemon=True).start()
    return server


def parse_time(text):
    """UTC seconds from "1791709800" or "2026-10-11T08:30" (taken as UTC)"""
    if text.isdigit():
        return int(text)
    t = datetime.datetime.fromisoformat(text)
    if t.tzinfo is None:
        t = t.replace(tzinfo=datetime.timezone.utc)
    return int(t.timestamp())


def main(argv=None):
    parser = argparse.ArgumentParser(description="Bedrock's refinery observations")
    parser.add_argument("--backup", metavar="FILE", help="write a consistent copy of the database to FILE and exit")
    parser.add_argument("--rocks-export", metavar="FILE", help="write the rocks kept to FILE as CSV and exit")
    parser.add_argument("--stats", action="store_true", help="what came in over the last 14 days, and exit")
    parser.add_argument("--purge", action="store_true",
                        help="take readings out: those observed from --since to --until (UTC), of --station or "
                             "all, all or --implausible ones; says how many, and takes them out with --yes")
    parser.add_argument("--rejudge", action="store_true",
                        help="judge the implausible readings again by the rules as they stand: says how many "
                             "would pass, and takes them as plausible with --yes")
    parser.add_argument("--since", default="0")
    parser.add_argument("--until", default=None)
    parser.add_argument("--station")
    parser.add_argument("--implausible", action="store_true")
    parser.add_argument("--yes", action="store_true")
    args = parser.parse_args(argv)
    store = Store(DB_PATH)
    if args.backup:
        store.backup(args.backup)
        print("backed up to", args.backup)
        return 0
    if args.rocks_export:
        with open(args.rocks_export, "w", encoding="utf-8", newline="") as f:
            n = store.rocks_csv(f)
        print("%d rock%s written to %s" % (n, "" if n == 1 else "s", args.rocks_export))
        return 0
    if args.stats:
        for day, names in sorted(store.counters(utc_day(time.time() - 13 * 86400)).items()):
            print(day, "  ".join("%s %d" % (name, n) for name, n in sorted(names.items())))
        return 0
    if args.rejudge:
        total, passing = store.rejudge(int(time.time()), args.yes)
        print("%d of %d implausible reading%s %s" % (passing, total, "" if total == 1 else "s",
                                                      "taken as plausible" if args.yes and passing
                                                      else "would pass - again with --yes to take them"))
        return 0
    if args.purge:
        since = parse_time(args.since)
        until = parse_time(args.until) if args.until else int(time.time()) + OBSERVED_FUTURE
        found = store.purge(since, until, args.station, args.implausible, args.yes)
        print("%d reading%s %s" % (found, "" if found == 1 else "s", "taken out" if args.yes and found
                                   else "would be taken out - again with --yes to do it"))
        return 0
    server = serve(store, Limiter(POSTS_PER_HOUR, POSTS_PER_HOUR_ALL),
                   rock_limiter=Limiter(ROCK_POSTS_PER_HOUR, ROCK_POSTS_PER_HOUR_ALL),
                   rock_count_limiter=Limiter(ROCKS_PER_HOUR, ROCKS_PER_HOUR_ALL))
    print("bedrock-api: %s on port %d, %d stations known, %s" % (
        DB_PATH, PORT, len(STATIONS), "refining model loaded" if TABLES else "no refining model"), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
