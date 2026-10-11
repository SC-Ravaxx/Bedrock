#!/usr/bin/env python3
"""Bedrock's refinery observations.

Bedrock reads a refinery terminal while the player stands at it: the
station's workload ("Current capacity 106 %" and the line under it) and,
when a work order is set up, the quote - method, materials, cost and
processing time. The game sets cost and time by that workload, and only
the terminal shows it. Players who agreed send what their Bedrock read
here, and every Bedrock can ask how busy each refinery was when last seen
and what it quoted.

Nothing of the player is kept: no name, no account, no address. An address
is held in memory, salted and hashed, for an hour's rate limit only.

Standard library only, SQLite beside it. See README.md.
"""

import argparse
import collections
import csv
import hashlib
import io
import json
import os
import re
import secrets
import sqlite3
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

API_VERSION = 1
PREFIX = os.environ.get("BEDROCK_API_PREFIX", "/bedrock/api").rstrip("/")
DB_PATH = os.environ.get("BEDROCK_API_DB", "/data/refinery.db")
PORT = int(os.environ.get("BEDROCK_API_PORT", "8080"))
POSTS_PER_HOUR = int(os.environ.get("BEDROCK_API_POSTS_PER_HOUR", "120"))          # from one address
POSTS_PER_HOUR_ALL = int(os.environ.get("BEDROCK_API_POSTS_PER_HOUR_ALL", "20000"))  # from everyone
MAX_BODY = 16 * 1024
STATION_DAYS = 7            # /stations: how old a station's last reading may be
QUOTE_DAYS_MAX = 60         # /quotes: the most days asked for at once
QUOTES_MAX = 5000
OBSERVED_PAST = 3 * 86400   # a reading sent this late is still taken (Bedrock was offline)
OBSERVED_FUTURE = 600       # ...and one this far ahead (a clock a little fast)

# The game's own four (refinery_ui_Capacity_Low/Medium/High/Extreme):
# light - no surcharge, moderate - small, heavy - moderate, extreme - large
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


STATIONS = load_stations(os.environ.get(
    "BEDROCK_API_STATIONS", os.path.join(os.path.dirname(os.path.abspath(__file__)), "stations.txt")))


# ---------------------------------------------------------------- a report

class Invalid(ValueError):
    pass


def _number(value, low, high, what, optional=False):
    if value is None and optional:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value != value:
        raise Invalid(what + " is not a number")
    if value < low or value > high:
        raise Invalid("%s is out of range (%s to %s)" % (what, low, high))
    return int(round(value))


def _text(value, pattern, what, optional=False):
    if value is None and optional:
        return None
    if not isinstance(value, str) or not pattern.match(value):
        raise Invalid(what + " is not in the expected form")
    return value


def validate(body, now):
    """A report as Bedrock sends it (README.md), checked and put in order;
    Invalid where anything in it is not what a terminal can show"""
    if not isinstance(body, dict):
        raise Invalid("the report is not a JSON object")
    if body.get("v") != API_VERSION:
        raise Invalid("unknown report version")
    channel = body.get("channel")
    if channel is not None and channel not in CHANNELS:
        raise Invalid("unknown channel")
    workload = body.get("workload")
    if workload is not None and workload not in WORKLOADS:
        raise Invalid("unknown workload")
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
            raise Invalid("quote is not an object")
        items = quote.get("items")
        if not isinstance(items, list) or not 1 <= len(items) <= 30:
            raise Invalid("a quote has 1 to 30 items")
        q = {
            "method": _text(quote.get("method"), METHOD, "method"),
            "cost": _number(quote.get("cost"), 0, 10 ** 9, "cost"),
            "seconds": _number(quote.get("seconds"), 1, 30 * 86400, "seconds"),
            "items": [],
        }
        for item in items:
            if not isinstance(item, dict):
                raise Invalid("an item is not an object")
            q["items"].append({
                "material": _text(item.get("material"), NAME, "material"),
                "quantity": _number(item.get("quantity"), 1, 10 ** 7, "quantity"),
                "quality": _number(item.get("quality"), 0, 1000, "quality", optional=True),
                "yield": _number(item.get("yield"), 0, 10 ** 7, "yield", optional=True),
            })
        out["quote"] = q
    if out["capacity"] is None and out["workload"] is None and out["quote"] is None:
        raise Invalid("nothing read: no capacity, workload or quote")
    return out


# ---------------------------------------------------------------- the store

SCHEMA = """
CREATE TABLE IF NOT EXISTS observations (
    id INTEGER PRIMARY KEY,
    digest TEXT NOT NULL UNIQUE,      -- the same reading sent twice is kept once
    received INTEGER NOT NULL,        -- server time, UTC seconds
    observed INTEGER NOT NULL,        -- when the terminal showed it
    station TEXT NOT NULL,
    known INTEGER NOT NULL,           -- the station is in stations.txt
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
CREATE TABLE IF NOT EXISTS quote_items (
    observation INTEGER NOT NULL REFERENCES observations (id),
    position INTEGER NOT NULL,
    material TEXT NOT NULL,
    quantity INTEGER NOT NULL,        -- cSCU put in
    quality INTEGER,
    yield INTEGER,                    -- cSCU it comes out at
    PRIMARY KEY (observation, position)
);
"""


class Store:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()   # one writer at a time
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

    def add(self, r, now):
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
                        "INSERT OR IGNORE INTO observations (digest, received, observed, station, known, capacity,"
                        " workload, game, channel, bedrock, method, cost, seconds)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                        (digest, now, r["observed"], r["station"], int(r["station"] in STATIONS), r["capacity"],
                         r["workload"], r["game"], r["channel"], r["bedrock"], q.get("method"), q.get("cost"),
                         q.get("seconds")))
                    if cur.rowcount == 0:
                        return False
                    for i, item in enumerate(q.get("items", [])):
                        db.execute("INSERT INTO quote_items VALUES (?, ?, ?, ?, ?, ?)",
                                   (cur.lastrowid, i, item["material"], item["quantity"], item["quality"],
                                    item["yield"]))
                return True
            finally:
                db.close()

    def count(self):
        db = self._db()
        try:
            return db.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
        finally:
            db.close()

    def stations(self, now):
        """Each known station's last reading of its workload in STATION_DAYS,
        with how many readings and quotes it had in the last day"""
        db = self._db()
        try:
            latest = {}
            for row in db.execute(
                    "SELECT station, capacity, workload, observed FROM observations"
                    " WHERE known = 1 AND observed >= ? AND (capacity IS NOT NULL OR workload IS NOT NULL)"
                    " ORDER BY observed", (now - STATION_DAYS * 86400,)):
                latest[row["station"]] = row
            counts = {row["station"]: (row["seen"], row["quotes"]) for row in db.execute(
                "SELECT station, COUNT(*) AS seen, COUNT(method) AS quotes FROM observations"
                " WHERE known = 1 AND observed >= ? GROUP BY station", (now - 86400,))}
        finally:
            db.close()
        out = []
        for name in sorted(latest):
            row = latest[name]
            seen, quotes = counts.get(name, (0, 0))
            out.append({"station": name, "capacity": row["capacity"], "workload": row["workload"],
                        "observed": row["observed"], "seen_24h": seen, "quotes_24h": quotes})
        return out

    def quotes(self, now, days, station=None):
        """The quotes of known stations in the last `days`, the newest first"""
        db = self._db()
        try:
            sql = ("SELECT id, station, observed, capacity, workload, method, cost, seconds, game, bedrock"
                   " FROM observations WHERE method IS NOT NULL AND known = 1 AND observed >= ?")
            args = [now - days * 86400]
            if station is not None:
                sql += " AND station = ?"
                args.append(station)
            sql += " ORDER BY observed DESC LIMIT ?"
            args.append(QUOTES_MAX)
            rows = db.execute(sql, args).fetchall()
            items = collections.defaultdict(list)
            ids = [row["id"] for row in rows]
            for start in range(0, len(ids), 500):
                part = ids[start:start + 500]
                for it in db.execute(
                        "SELECT observation, material, quantity, quality, yield FROM quote_items"
                        " WHERE observation IN (%s) ORDER BY observation, position" % ",".join("?" * len(part)),
                        part):
                    items[it["observation"]].append({"material": it["material"], "quantity": it["quantity"],
                                                     "quality": it["quality"], "yield": it["yield"]})
        finally:
            db.close()
        return [{"station": row["station"], "observed": row["observed"], "capacity": row["capacity"],
                 "workload": row["workload"], "method": row["method"], "cost": row["cost"],
                 "seconds": row["seconds"], "game": row["game"], "bedrock": row["bedrock"],
                 "items": items[row["id"]]} for row in rows]

    def export_csv(self):
        """Every reading kept, for anyone to work with"""
        db = self._db()
        try:
            items = collections.defaultdict(list)
            for it in db.execute("SELECT observation, material, quantity, quality, yield FROM quote_items"
                                 " ORDER BY observation, position"):
                items[it["observation"]].append([it["material"], it["quantity"], it["quality"], it["yield"]])
            out = io.StringIO()
            w = csv.writer(out, lineterminator="\n")
            w.writerow(["observed", "station", "known", "capacity", "workload", "method", "cost", "seconds",
                        "items", "game", "channel", "bedrock"])
            for row in db.execute("SELECT * FROM observations ORDER BY observed, id"):
                w.writerow([row["observed"], row["station"], row["known"], row["capacity"], row["workload"],
                            row["method"], row["cost"], row["seconds"],
                            json.dumps(items[row["id"]]) if row["id"] in items else "",
                            row["game"], row["channel"], row["bedrock"]])
            return out.getvalue()
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
    """Reports per address and from everyone over the last hour, in memory.
    The address goes in salted and hashed with a salt that dies with the
    process: what is held could not name anyone even if it were read."""

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
            return self._send(200, {"ok": True, "version": API_VERSION, "observations": self.store.count()})
        if route == "/v1/refinery/stations":
            return self._send(200, {"generated": now, "days": STATION_DAYS, "stations": self.store.stations(now)})
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
            return self._fail(429, "too many reports, try again later")
        raw = self.rfile.read(length)
        try:
            report = validate(json.loads(raw.decode("utf-8")), now)
        except (UnicodeDecodeError, json.JSONDecodeError):
            return self._fail(400, "not JSON")
        except Invalid as e:
            return self._fail(400, str(e))
        stored = self.store.add(report, now)
        return self._send(201 if stored else 200, {"ok": True, "stored": stored})


def serve(store, limiter, host="0.0.0.0", port=PORT):
    """The service, not started yet: serve_forever() on the result"""
    handler = type("BoundHandler", (Handler,), {"store": store, "limiter": limiter})
    server = ThreadingHTTPServer((host, port), handler)
    server.daemon_threads = True
    return server


def main(argv=None):
    parser = argparse.ArgumentParser(description="Bedrock's refinery observations")
    parser.add_argument("--backup", metavar="FILE",
                        help="write a consistent copy of the database to FILE and exit")
    args = parser.parse_args(argv)
    store = Store(DB_PATH)
    if args.backup:
        store.backup(args.backup)
        print("backed up to", args.backup)
        return 0
    server = serve(store, Limiter(POSTS_PER_HOUR, POSTS_PER_HOUR_ALL))
    print("bedrock-api: %s on port %d, %d stations known" % (DB_PATH, PORT, len(STATIONS)), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
