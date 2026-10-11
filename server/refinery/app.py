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
request line and the answer only.

Since anyone can send anything, nothing sent is taken on its word:
  - a reading has to look like what a terminal shows (validate)
  - and fit what the game does: a workload level only at the capacities
    other senders saw it at, a quote's yield, cost and time against
    Bedrock's refining model (judge)
  - a station's state is the median of several senders, outliers left out,
    and it is confirmed only once two senders agree (consensus)
  - one sender reports a station a few times an hour at most
  - what slipped through can be found and taken out (--purge, --stats)

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
MAX_BODY = 16 * 1024
STATION_DAYS = 7              # /stations: how old a station's last reading may be
QUOTE_DAYS_MAX = 60           # /quotes: the most days asked for at once
QUOTES_MAX = 5000
OBSERVED_PAST = 3 * 86400     # a reading sent this late is still taken (Bedrock was offline)
OBSERVED_FUTURE = 600         # ...and one this far ahead (a clock a little fast)

CONSENSUS_SECONDS = 1800      # the readings that make a station's state now
OUTLIER_POINTS = 15           # a capacity this far from the senders' median - or 25 % of it - is left out
STATION_READINGS_PER_HOUR = 12  # from one sender, for one station
SOURCE_HOURS = 48             # how long a reading keeps the token that tells senders apart
RANGE_DAYS = 30               # a workload level's capacity span is learned over this long
RANGE_MIN_READINGS = 10       # ...from this many readings
RANGE_MIN_SOURCES = 3         # ...by this many senders
RANGE_MARGIN = 10             # percentage points beyond the span that are still taken
YIELD_BAND = (0.85, 1.15)     # a quote's yield against the model, where its factors are known
COST_BAND = (0.5, 6.0)        # cost likewise, at a station whose cost is known: no surcharge to a large one
TIME_BAND = (0.1, 20.0)       # time, far looser: how the workload moves it is still to be learned
COUNTER_DAYS = 180

# The game's own four (refinery_ui_Capacity_Low/Medium/High/Extreme), in
# order: light - no surcharge, moderate - small, heavy - moderate, extreme - large
WORKLOADS = ("light", "moderate", "heavy", "extreme")
CHANNELS = ("LIVE", "PTU", "EPTU", "TECH-PREVIEW", "HOTFIX")
NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 .'()\-]{0,63}$")
METHOD = re.compile(r"^[A-Z][A-Z_]{2,39}$")
VERSION = re.compile(r"^\d{1,3}(\.\d{1,4}){1,3}$")


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
        "capacity": _number(body.get("capacity"), 0, 999, "capacity", optional=True),
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


def judge(r, ranges, tables=None):
    """'' where a reading fits what the game does, else what does not.

    A workload level is taken only at the capacities other senders saw it
    at (ranges, Store.ranges). A quote cannot yield more than was put in;
    where Bedrock's model has the factors, its yield has to come out as the
    model says - it did on every work order measured - and its cost and
    time within the bands the workload can move them."""
    tables = TABLES if tables is None else tables
    cap, level = r["capacity"], r["workload"]
    if cap is not None and level is not None and level in ranges:
        low, high = ranges[level]
        if cap < low - RANGE_MARGIN or cap > high + RANGE_MARGIN:
            return "capacity %d %% where %s was seen at %d-%d %%" % (cap, level, low, high)
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


def consensus(readings):
    """One station's state from its readings in CONSENSUS_SECONDS, one per
    sender: the median capacity with the outliers left out, the workload
    most senders saw (the newest of a tie)"""
    caps = [r["capacity"] for r in readings if r["capacity"] is not None]
    capacity = None
    if caps:
        middle = statistics.median(caps)
        kept = [c for c in caps if abs(c - middle) <= max(OUTLIER_POINTS, 0.25 * middle)]
        capacity = int(round(statistics.median(kept)))
    levels = collections.Counter(r["workload"] for r in readings if r["workload"])
    workload = None
    if levels:
        most = max(levels.values())
        tied = {level for level, n in levels.items() if n == most}
        workload = next(r["workload"] for r in sorted(readings, key=lambda r: -r["observed"])
                        if r["workload"] in tied)
    return {"capacity": capacity, "workload": workload, "observed": max(r["observed"] for r in readings),
            "sources": len(readings)}


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
    name TEXT NOT NULL,               -- accepted, duplicate, implausible, limited, rejected, rejected:<field>
    count INTEGER NOT NULL,
    PRIMARY KEY (day, name)
);
"""


class Store:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()   # one writer at a time
        self.salts = {}                # UTC day -> salt; in memory only, so the tokens cannot be undone
        self.salt_lock = threading.Lock()
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
        only while the levels' medians rise in the game's order; else {} and
        nothing is judged by them"""
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

    def tally(self, now, *names):
        with self.lock:
            db = self._db()
            try:
                with db:
                    for name in names:
                        db.execute("INSERT INTO counters VALUES (?, ?, 1)"
                                   " ON CONFLICT (day, name) DO UPDATE SET count = count + 1", (utc_day(now), name))
            finally:
                db.close()

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

    def prune(self, now):
        """Readings lose their sender's token after SOURCE_HOURS, counters go
        after COUNTER_DAYS, and only today's and yesterday's salt stay"""
        with self.lock:
            db = self._db()
            try:
                with db:
                    db.execute("UPDATE observations SET source = NULL WHERE source IS NOT NULL AND received < ?",
                               (now - SOURCE_HOURS * 3600,))
                    db.execute("DELETE FROM counters WHERE day < ?", (utc_day(now - COUNTER_DAYS * 86400),))
            finally:
                db.close()
        keep = {utc_day(now), utc_day(now - 86400)}
        with self.salt_lock:
            for day in [d for d in self.salts if d not in keep]:
                del self.salts[day]

    # -- handing out

    def stations(self, now):
        """Each known station's state: from its readings of the last
        CONSENSUS_SECONDS where there are any (fresh), else its newest within
        STATION_DAYS; confirmed once two senders agree on it now"""
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
                         "sources": 0}
            seen, quotes = counts.get(name, (0, 0))
            out.append({"station": name, "capacity": state["capacity"], "workload": state["workload"],
                        "observed": state["observed"], "fresh": bool(recent), "sources": state["sources"],
                        "confirmed": state["sources"] >= 2, "seen_24h": seen, "quotes_24h": quotes})
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

    def allow(self, address, now):
        key = hashlib.sha256(self.salt + address.encode("utf-8", "replace")).digest()
        hour = now - 3600
        with self.lock:
            while self.everyone and self.everyone[0] <= hour:
                self.everyone.popleft()
            times = self.seen.setdefault(key, collections.deque())
            while times and times[0] <= hour:
                times.popleft()
            if len(times) >= self.per_address or len(self.everyone) >= self.overall:
                return False
            times.append(now)
            self.everyone.append(now)
            if len(self.seen) > 10000:
                for k in [k for k, v in self.seen.items() if not v or v[-1] <= hour]:
                    del self.seen[k]
            return True


# ---------------------------------------------------------------- the service

class Handler(BaseHTTPRequestHandler):
    server_version = "bedrock-api"
    sys_version = ""
    store = None
    limiter = None

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

    def do_GET(self):
        now = int(time.time())
        route = self._route()
        query = parse_qs(urlsplit(self.path).query)
        if route == "/v1/health":
            today = self.store.counters(utc_day(now)).get(utc_day(now), {})
            return self._send(200, {"ok": True, "version": API_VERSION, "observations": self.store.count(),
                                    "today": {name: today.get(name, 0) for name in
                                              ("accepted", "duplicate", "implausible", "limited", "rejected")}})
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
        return self._fail(404, "not found")

    do_HEAD = do_GET

    def do_POST(self):
        now = int(time.time())
        if self._route() != "/v1/refinery/observations":
            return self._fail(404, "not found")
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return self._fail(411, "length required")
        if length < 0 or length > MAX_BODY:
            return self._fail(413, "too large")
        if self.headers.get("Content-Type", "").split(";")[0].strip().lower() != "application/json":
            return self._fail(415, "send application/json")
        # nginx puts the address it saw here; the service is reachable from
        # the containers' network only
        address = self.headers.get("X-Real-IP") or self.client_address[0]
        if not self.limiter.allow(address, now):
            self.store.tally(now, "limited")
            return self._fail(429, "too many reports, try again later")
        raw = self.rfile.read(length)
        try:
            report = validate(json.loads(raw.decode("utf-8")), now)
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.store.tally(now, "rejected", "rejected:json")
            return self._fail(400, "not JSON")
        except Invalid as e:
            self.store.tally(now, "rejected", "rejected:" + e.code)
            return self._fail(400, str(e))
        source = self.store.source(address, now)
        if self.store.readings_by(source, report["station"], now) >= STATION_READINGS_PER_HOUR:
            self.store.tally(now, "limited")
            return self._fail(429, "too many readings of this station, try again later")
        reason = judge(report, self.store.ranges(now))
        stored = self.store.add(report, now, source, reason)
        self.store.tally(now, ("implausible" if reason else "accepted") if stored else "duplicate")
        # whether it was taken as plausible is not said: that would teach a
        # forger what passes
        return self._send(201 if stored else 200, {"ok": True, "stored": stored})


def serve(store, limiter, host="0.0.0.0", port=PORT):
    """The service, not started yet: serve_forever() on the result. A thread
    beside it prunes every ten minutes (Store.prune)."""
    handler = type("BoundHandler", (Handler,), {"store": store, "limiter": limiter})
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
    parser.add_argument("--stats", action="store_true", help="what came in over the last 14 days, and exit")
    parser.add_argument("--purge", action="store_true",
                        help="take readings out: those observed from --since to --until (UTC), of --station or "
                             "all, all or --implausible ones; says how many, and takes them out with --yes")
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
    if args.stats:
        for day, names in sorted(store.counters(utc_day(time.time() - 13 * 86400)).items()):
            print(day, "  ".join("%s %d" % (name, n) for name, n in sorted(names.items())))
        return 0
    if args.purge:
        since = parse_time(args.since)
        until = parse_time(args.until) if args.until else int(time.time()) + OBSERVED_FUTURE
        found = store.purge(since, until, args.station, args.implausible, args.yes)
        print("%d reading%s %s" % (found, "" if found == 1 else "s", "taken out" if args.yes and found
                                   else "would be taken out - again with --yes to do it"))
        return 0
    server = serve(store, Limiter(POSTS_PER_HOUR, POSTS_PER_HOUR_ALL))
    print("bedrock-api: %s on port %d, %d stations known, %s" % (
        DB_PATH, PORT, len(STATIONS), "refining model loaded" if TABLES else "no refining model"), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
