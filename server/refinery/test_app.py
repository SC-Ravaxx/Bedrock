"""The service, run for real on a free port with a database in a temporary
folder: python -m unittest test_app (from this folder)"""

import contextlib
import io
import json
import os
import sqlite3
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

import app

HUR = "HUR-L1 Green Glade Station"


class ServiceTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.store = app.Store(os.path.join(self.folder.name, "refinery.db"))
        self.limiter = app.Limiter(per_address=1000, overall=100000)
        self.rock_limiter = app.Limiter(per_address=1000, overall=100000)
        self.rock_count_limiter = app.Limiter(per_address=100000, overall=1000000)
        self.server = app.serve(self.store, self.limiter, host="127.0.0.1", port=0, rock_limiter=self.rock_limiter,
                                rock_count_limiter=self.rock_count_limiter)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d%s" % (self.server.server_address[1], app.PREFIX)
        self.now = int(time.time())
        self.tick = 0

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.folder.cleanup()

    def call(self, path, body=None, kind="application/json", address="203.0.113.1", raw=None, install=None):
        data = raw if raw is not None else (None if body is None else json.dumps(body).encode("utf-8"))
        request = urllib.request.Request(self.base + path, data=data, method="POST" if data is not None else "GET")
        if data is not None:
            request.add_header("Content-Type", kind)
            request.add_header("X-Real-IP", address)
        if install:
            request.add_header("X-Bedrock-Install", install)
        try:
            with urllib.request.urlopen(request, timeout=10) as answer:
                text = answer.read().decode("utf-8")
                return answer.status, (json.loads(text) if answer.headers.get_content_type() == "application/json"
                                       else text)
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read().decode("utf-8"))

    def send(self, address="203.0.113.1", install=None, **changes):
        return self.call("/v1/refinery/observations", self.reading(**changes), address=address, install=install)

    def reading(self, **changes):
        # each a little earlier than the last, so no two are the same reading
        self.tick += 1
        r = {"v": 1, "bedrock": "2.6.7", "game": "4.10.2", "channel": "LIVE", "observed": self.now - 60 - self.tick,
             "station": "Checkmate", "capacity": 106, "workload": "moderate"}
        r.update(changes)
        return r

    def quote(self, **changes):
        # Iron at HUR-L1 by Dinyx: the model yields 1000 * 0.45 * 0.95 * 1.0 = 427.5,
        # costs 1000 * 0.0661463 * 1.05 = 69.5 and takes 1000 * 0.858521 * 0.96 * 12 = 9890 s
        q = {"method": "DINYX_SOLVENTATION", "cost": 80, "seconds": 12000,
             "items": [{"material": "Iron (Ore)", "quantity": 1000, "quality": 874, "yield": 428}]}
        q.update(changes)
        return q

    def stations(self):
        return {s["station"]: s for s in self.call("/v1/refinery/stations")[1]["stations"]}

    def rows(self):
        db = sqlite3.connect(self.store.path)
        db.row_factory = sqlite3.Row
        try:
            return db.execute("SELECT * FROM observations ORDER BY id").fetchall()
        finally:
            db.close()

    # ---------------------------------------------------------------- basics

    def test_health_counts_what_came_in(self):
        self.send()
        self.send(station="<script>")
        code, answer = self.call("/v1/health")
        self.assertEqual(code, 200)
        self.assertEqual(answer["observations"], 1)
        self.assertEqual(answer["today"], {"accepted": 1, "duplicate": 0, "implausible": 0, "limited": 0,
                                           "held": 0, "rejected": 1})

    def test_a_reading_is_kept_once(self):
        body = self.reading()
        self.assertEqual(self.call("/v1/refinery/observations", body), (201, {"ok": True, "stored": True}))
        self.assertEqual(self.call("/v1/refinery/observations", body), (200, {"ok": True, "stored": False}))

    def test_what_a_terminal_cannot_show_is_turned_down(self):
        bad = [
            self.reading(station="<script>"),
            self.reading(capacity=2000000),
            self.reading(capacity=True),
            self.reading(observed=self.now - 10 * 86400),
            self.reading(observed=self.now + 3600),
            self.reading(workload="busy"),
            self.reading(channel="DEV"),
            self.reading(v=2),
            self.reading(bedrock="latest"),
            self.reading(capacity=None, workload=None),
            self.reading(quote=self.quote(items=[])),
            self.reading(quote=self.quote(method="dinyx")),
            self.reading(quote=self.quote(seconds=0)),
            self.reading(quote=self.quote(items=[{"material": "Iron (Ore)", "quantity": -5}])),
            [1, 2, 3],
        ]
        # (each from an address of its own: five from one would hold it back)
        for i, body in enumerate(bad):
            code, answer = self.call("/v1/refinery/observations", body, address="192.0.2.%d" % i)
            self.assertEqual(code, 400, body)
            self.assertFalse(answer["ok"])
        self.assertEqual(self.store.count(), 0)
        self.assertEqual(self.store.counters(app.utc_day(self.now))[app.utc_day(self.now)]["rejected"], len(bad))

    def test_not_json_too_large_or_the_wrong_kind(self):
        self.assertEqual(self.call("/v1/refinery/observations", raw=b"{not json")[0], 400)
        self.assertEqual(self.call("/v1/refinery/observations", self.reading(), kind="text/plain")[0], 415)
        self.assertEqual(self.call("/v1/refinery/observations", raw=b"x" * (app.MAX_BODY + 1))[0], 413)

    def test_unknown_paths(self):
        self.assertEqual(self.call("/v1/nothing")[0], 404)
        self.assertEqual(self.call("/v1/refinery/stations", self.reading())[0], 404)

    # ---------------------------------------------------------------- layer 3: what the game does

    def test_a_quote_that_fits_the_model_comes_back_with_its_items(self):
        self.assertEqual(self.send(station=HUR, quote=self.quote())[0], 201)
        quotes = self.call("/v1/refinery/quotes?days=7")[1]["quotes"]
        self.assertEqual(len(quotes), 1)
        self.assertEqual((quotes[0]["station"], quotes[0]["method"], quotes[0]["cost"], quotes[0]["seconds"]),
                         (HUR, "DINYX_SOLVENTATION", 80, 12000))
        self.assertEqual(quotes[0]["items"], [{"material": "Iron (Ore)", "quantity": 1000, "quality": 874,
                                               "yield": 428}])
        self.assertEqual(self.call("/v1/refinery/quotes?station=Levski")[1]["quotes"], [])
        self.assertEqual(self.call("/v1/refinery/quotes?days=x")[0], 400)

    def test_a_quote_against_the_model_is_kept_but_not_handed_out(self):
        made_up = [
            self.quote(items=[{"material": "Iron (Ore)", "quantity": 1000, "yield": 300}]),   # 0.70 of the model
            self.quote(cost=5000),                                                            # 72 times
            self.quote(seconds=10),                                                           # 0.001 times
            self.quote(items=[{"material": "Iron (Ore)", "quantity": 100, "yield": 900}]),    # more than put in
        ]
        for q in made_up:
            self.assertEqual(self.send(station=HUR, quote=q)[0], 201)   # the sender is not told
        self.assertEqual(self.call("/v1/refinery/quotes")[1]["quotes"], [])
        self.assertEqual([r["plausible"] for r in self.rows()], [0, 0, 0, 0])
        self.assertIn("yields 300 where the model has 428", self.rows()[0]["reason"])
        lines = self.call("/v1/refinery/export.csv")[1].strip().split("\n")
        self.assertEqual(len(lines), 5)
        self.assertTrue(lines[0].startswith("observed,station,known,plausible,reason,"))
        self.assertNotIn("source", lines[0])

    def test_a_material_the_model_does_not_cover_is_judged_by_the_plain_rules_only(self):
        salvage = {"method": "DINYX_SOLVENTATION", "cost": 99999, "seconds": 23000,
                   "items": [{"material": "Construction Salvage", "quantity": 9390, "yield": 9000}]}
        self.assertEqual(self.send(quote=salvage)[0], 201)
        self.assertEqual(self.rows()[0]["plausible"], 1)

    def test_a_workload_level_is_taken_only_at_its_capacities(self):
        # ten readings of "moderate" between 90 and 126 %, from five senders
        for i in range(10):
            self.send(address="198.51.100.%d" % (i % 5), station=HUR if i % 2 else "Checkmate",
                      capacity=90 + 4 * i, workload="moderate")
        self.assertEqual(self.store.ranges(self.now), {"moderate": (94, 122)})
        # what was seen is no bound: 175 % is moderate all the same (MIC-L5, 1 November 2026)
        self.send(address="198.51.100.9", capacity=20, workload="moderate")
        self.send(address="198.51.100.9", station=HUR, capacity=175, workload="moderate")
        rows = self.rows()
        self.assertEqual((rows[-2]["plausible"], rows[-1]["plausible"]), (0, 1))
        self.assertEqual(rows[-2]["reason"], "capacity 20 % where moderate is 100-199 %")

    def test_the_lowest_level_has_no_floor_and_the_highest_no_ceiling_near_what_was_seen(self):
        # the spans of 22 October 2026
        ranges = {"light": (26, 87), "moderate": (105, 162), "extreme": (317, 613)}
        at = lambda capacity, level: {"station": "Levski", "capacity": capacity, "workload": level, "quote": None}
        self.assertEqual(app.judge(at(7, "light"), ranges), "")       # Stanton Gateway (Pyro), quiet
        self.assertEqual(app.judge(at(631, "extreme"), ranges), "")   # Levski, busy
        self.assertEqual(app.judge(at(1200, "extreme"), ranges), "")
        self.assertEqual(app.judge(at(10796, "extreme"), ranges), "capacity 10796 % where extreme was seen up to 613 %")
        self.assertEqual(app.judge(at(10796, "extreme"), {}), "")     # nothing seen yet: no ceiling at all
        self.assertEqual(app.judge(at(150, "light"), ranges), "capacity 150 % where light is under 100 %")
        self.assertEqual(app.judge(at(250, "extreme"), ranges), "capacity 250 % where extreme is from 300 %")
        self.assertEqual(app.judge(at(80, "moderate"), ranges), "capacity 80 % where moderate is 100-199 %")
        self.assertIn("capacity 1 %", app.judge(at(1, "extreme"), ranges))   # Levski, misread on 25 October 2026

    def test_what_others_saw_does_not_move_where_a_level_begins(self):
        # 1 November 2026: Levski's 700 to 1000 % had put extreme's span at 421-908 %,
        # and readings of Green Glade, Wide Forest and Ambitious Dream were turned away
        ranges = {"light": (12, 87), "moderate": (105, 154), "heavy": (228, 278), "extreme": (421, 908)}
        at = lambda capacity, level: {"station": HUR, "capacity": capacity, "workload": level, "quote": None}
        for capacity, level in ((389, "extreme"), (303, "extreme"), (206, "heavy"), (175, "moderate"),
                                (99, "moderate"), (290, "extreme"), (209, "moderate")):
            self.assertEqual(app.judge(at(capacity, level), ranges), "", (capacity, level))
        self.assertEqual(app.judge(at(278, "moderate"), ranges), "capacity 278 % where moderate is 100-199 %")
        self.assertEqual(app.judge(at(289, "extreme"), ranges), "capacity 289 % where extreme is from 300 %")
        self.assertEqual(app.judge(at(210, "moderate"), ranges), "capacity 210 % where moderate is 100-199 %")
        self.assertEqual([app.level_of(c) for c in (0, 99, 100, 199, 200, 299, 300, 931)],
                         ["light", "light", "moderate", "moderate", "heavy", "heavy", "extreme", "extreme"])

    def test_a_quote_may_cost_far_off_the_model_but_not_absurdly(self):
        # the model has 69.5 for this one; terminals asked 0.26 to 9.6 times the model in October 2026
        for cost in (625, 18, 2085):     # 9, a quarter, 30 times
            self.assertEqual(self.send(station=HUR, quote=self.quote(cost=cost))[0], 201)
        self.assertEqual([r["plausible"] for r in self.rows()], [1, 1, 0])

    def test_implausible_readings_are_judged_again_by_the_rules_as_they_stand(self):
        self.send(station=HUR, quote=self.quote(cost=625))
        self.send(station=HUR, quote=self.quote(cost=5000))
        db = sqlite3.connect(self.store.path)
        try:
            with db:   # as the old rules had them
                db.execute("UPDATE observations SET plausible = 0, reason = 'cost 625 where the model has 70'"
                           " WHERE cost = 625")
        finally:
            db.close()
        self.assertEqual(self.store.rejudge(self.now), (2, 1))
        self.assertEqual([r["plausible"] for r in self.rows()], [0, 0])
        self.assertEqual(self.store.rejudge(self.now, really=True), (2, 1))
        self.assertEqual([(r["plausible"], r["reason"]) for r in self.rows()],
                         [(1, None), (0, "cost 5000 where the model has 69")])

    def test_readings_the_learned_spans_turned_away_pass_when_judged_again(self):
        self.send(station=HUR, capacity=389, workload="extreme")
        self.send(station=HUR, capacity=1, workload="extreme")
        db = sqlite3.connect(self.store.path)
        try:
            with db:   # as the spans of 1 November 2026 had it
                db.execute("UPDATE observations SET plausible = 0,"
                           " reason = 'capacity 389 % where extreme was seen at 421-908 %' WHERE capacity = 389")
        finally:
            db.close()
        self.assertEqual(self.store.rejudge(self.now, really=True), (2, 1))
        self.assertEqual([(r["capacity"], r["plausible"], r["reason"]) for r in self.rows()],
                         [(389, 1, None), (1, 0, "capacity 1 % where extreme is from 300 %")])
        self.assertEqual((self.stations()[HUR]["capacity"], self.stations()[HUR]["workload"]), (389, "extreme"))

    def test_levels_that_contradict_the_game_order_judge_nothing(self):
        for i in range(10):
            self.send(address="192.0.2.%d" % i, capacity=150, workload="light")
            self.send(address="192.0.2.%d" % i, station=HUR, capacity=50, workload="moderate")
        db = sqlite3.connect(self.store.path)
        try:
            with db:   # taken, as rules before LEVEL_FROM could have
                db.execute("UPDATE observations SET plausible = 1, reason = NULL")
        finally:
            db.close()
        self.assertEqual(self.store.ranges(self.now), {})

    # ---------------------------------------------------------------- layer 4: several senders

    def test_a_station_now_is_its_newest_reading_backed_by_others(self):
        # sent in turn, each a little older than the one before
        self.send(address="10.0.0.1", capacity=100)
        self.send(address="10.0.0.2", capacity=110)
        self.send(address="10.0.0.3", capacity=300, workload="extreme")
        s = self.stations()["Checkmate"]
        self.assertEqual((s["capacity"], s["workload"], s["sources"], s["confirmed"], s["fresh"]),
                         (100, "moderate", 3, True, True))

    def test_one_newest_reading_against_two_that_agree_is_outvoted(self):
        self.send(address="10.0.0.1", capacity=100, observed=self.now - 600)
        self.send(address="10.0.0.2", capacity=105, observed=self.now - 500)
        self.send(address="10.0.0.3", capacity=300, workload="extreme", observed=self.now - 60)
        s = self.stations()["Checkmate"]
        self.assertEqual((s["capacity"], s["workload"], s["sources"], s["confirmed"], s["observed"]),
                         (105, "moderate", 3, True, self.now - 500))

    def test_a_reading_hours_old_still_confirms(self):
        self.send(address="10.0.0.1", capacity=103, observed=self.now - 2 * 3600)
        self.send(address="10.0.0.2", capacity=124, observed=self.now - 60)
        s = self.stations()["Checkmate"]
        self.assertEqual((s["capacity"], s["workload"], s["sources"], s["confirmed"]), (124, "moderate", 2, True))

    def test_one_sender_counts_once_and_alone_does_not_confirm(self):
        self.send(address="10.0.0.1", capacity=100, observed=self.now - 600)
        self.send(address="10.0.0.1", capacity=240, observed=self.now - 300, workload="heavy")
        s = self.stations()["Checkmate"]
        self.assertEqual((s["capacity"], s["workload"], s["sources"], s["confirmed"]), (240, "heavy", 1, False))

    def test_a_station_read_without_its_workload_shows_the_one_its_capacity_gives(self):
        # Levski, 1 November 2026: 982 % extreme, then 931 % with the line under it not read
        self.send(station="Levski", capacity=982, workload="extreme", observed=self.now - 2700)
        self.send(station="Levski", capacity=931, workload=None, observed=self.now - 60)
        s = self.stations()["Levski"]
        self.assertEqual((s["capacity"], s["workload"], s["workload_read"]), (931, "extreme", False))
        self.send(capacity=106)
        s = self.stations()["Checkmate"]
        self.assertEqual((s["capacity"], s["workload"], s["workload_read"]), (106, "moderate", True))
        self.send(station=HUR, capacity=None, workload="heavy")
        s = self.stations()[HUR]
        self.assertEqual((s["capacity"], s["workload"], s["workload_read"]), (None, "heavy", True))

    def test_an_old_reading_shows_as_not_fresh(self):
        self.send(capacity=80, workload="light", observed=self.now - 4 * 3600)
        s = self.stations()["Checkmate"]
        self.assertEqual((s["capacity"], s["fresh"], s["sources"], s["confirmed"]), (80, False, 0, False))

    def test_a_station_not_listed_is_kept_but_not_shown(self):
        self.assertEqual(self.send(station="New Refinery 7")[0], 201)
        self.assertEqual(self.stations(), {})
        self.assertEqual(self.rows()[0]["known"], 0)

    def test_one_sender_reports_a_station_only_so_often(self):
        for i in range(app.STATION_READINGS_PER_HOUR):
            self.assertEqual(self.send(address="10.9.9.9")[0], 201)
        self.assertEqual(self.send(address="10.9.9.9")[0], 429)
        self.assertEqual(self.send(address="10.9.9.9", station=HUR)[0], 201)
        self.assertEqual(self.send(address="10.9.9.8")[0], 201)

    def test_the_rate_limit_is_per_address(self):
        self.limiter.per_address = 3
        for i in range(3):
            self.assertEqual(self.send(address="192.0.2.1")[0], 201)
        self.assertEqual(self.send(address="192.0.2.1")[0], 429)
        self.assertEqual(self.send(address="192.0.2.2")[0], 201)
        self.assertTrue(all(len(k) == 32 for k in self.limiter.seen))

    # ---------------------------------------------------------------- layer 5: senders apart, no addresses

    def test_senders_are_told_apart_by_a_token_that_names_no_address(self):
        self.send(address="198.51.100.7")
        self.send(address="198.51.100.7", station=HUR)
        self.send(address="198.51.100.8")
        a, b, c = [r["source"] for r in self.rows()]
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)
        self.assertEqual(len(a), 16)
        with open(self.store.path, "rb") as f:
            self.assertNotIn(b"198.51.100", f.read())

    def test_tokens_and_salts_go_after_two_days(self):
        self.send()
        self.store.prune(self.now + 3600)
        self.assertIsNotNone(self.rows()[0]["source"])
        self.store.prune(self.now + 49 * 3600)
        self.assertIsNone(self.rows()[0]["source"])
        self.assertNotIn(app.utc_day(self.now), self.store.salts)

    def test_the_log_names_no_address(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.send(address="198.51.100.7")
            self.call("/v1/health")
        self.assertIn("POST", err.getvalue())
        self.assertNotIn("198.51.100.7", err.getvalue())
        self.assertNotIn("127.0.0.1", err.getvalue())

    # ---------------------------------------------------------------- layer 7: taking out, keeping

    def test_purge_counts_first_and_takes_out_only_when_told(self):
        self.send(station=HUR, quote=self.quote(cost=5000))       # implausible
        self.send(station=HUR, quote=self.quote())                 # plausible
        self.send()
        since, until = self.now - 3600, self.now
        self.assertEqual(self.store.purge(since, until), 3)
        self.assertEqual(self.store.purge(since, until, station=HUR, implausible_only=True), 1)
        self.assertEqual(self.store.count(), 3)
        self.assertEqual(self.store.purge(since, until, station=HUR, implausible_only=True, really=True), 1)
        self.assertEqual(self.store.count(), 2)
        db = sqlite3.connect(self.store.path)
        try:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM quote_items").fetchone()[0], 1)
        finally:
            db.close()

    def test_purge_and_stats_from_the_command_line(self):
        self.send()
        out = io.StringIO()
        app.DB_PATH, kept = self.store.path, app.DB_PATH
        try:
            with contextlib.redirect_stdout(out):
                app.main(["--purge", "--since", str(self.now - 3600)])
                app.main(["--stats"])
                app.main(["--purge", "--since", "2026-01-01T00:00", "--yes"])
        finally:
            app.DB_PATH = kept
        text = out.getvalue()
        self.assertIn("1 reading would be taken out", text)
        self.assertIn("accepted 1", text)
        self.assertIn("1 reading taken out", text)
        self.assertEqual(self.store.count(), 0)

    def test_backup_is_a_whole_copy(self):
        self.send(station=HUR, quote=self.quote())
        target = os.path.join(self.folder.name, "copy.db")
        self.store.backup(target)
        db = sqlite3.connect(target)
        try:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM observations").fetchone()[0], 1)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM quote_items").fetchone()[0], 1)
        finally:
            db.close()

    # ---------------------------------------------------------------- what was asked, counted

    def get(self, path, address=None, agent=None, install=None):
        request = urllib.request.Request(self.base + path)
        if address:
            request.add_header("X-Real-IP", address)
        if agent:
            request.add_header("User-Agent", agent)
        if install:
            request.add_header("X-Bedrock-Install", install)
        try:
            with urllib.request.urlopen(request, timeout=10) as answer:
                return answer.status, json.loads(answer.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read().decode("utf-8"))

    def test_requests_are_counted_by_route_version_and_install(self):
        windows, linux = "Bedrock/2.6.6 (Windows)", "Bedrock/2.6.5.2 (Linux)"
        self.get("/v1/refinery/stations", "198.51.100.7", windows)
        self.get("/v1/refinery/stations", "198.51.100.7", windows)
        self.get("/v1/refinery/stations", "198.51.100.8", linux)
        self.get("/v1/refinery/quotes", "198.51.100.8", "curl/8.0")
        self.get("/v1/refinery/stations")          # from inside: the health check, the admin view
        self.send(address="198.51.100.7")
        self.send(address="198.51.100.7", station=HUR)
        code, stats = self.get("/v1/stats")
        self.assertEqual(code, 200)
        today = stats["counters"][app.utc_day(time.time())]
        self.assertEqual(today["req:stations"], 3)
        self.assertEqual(today["clients"], 2)
        self.assertEqual(today["req:quotes"], 1)
        self.assertEqual(today["req:observations"], 2)
        self.assertEqual(today["senders"], 1)
        self.assertEqual((today["agent:2.6.6"], today["agent:2.6.5.2"]), (2, 1))
        self.assertEqual((today["platform:windows"], today["platform:linux"]), (2, 1))
        self.assertEqual(today["agent:other"], 3)  # curl, and the readings' Python
        self.assertEqual(stats["hours"][app.utc_hour(time.time())]["req:stations"], 3)
        self.assertEqual(stats["came_in"][app.utc_day(time.time())]["readings"], 2)
        self.assertEqual(stats["read"]["Checkmate"]["readings"], 1)
        with open(self.store.path, "rb") as f:
            self.assertNotIn(b"198.51.100", f.read())

    def test_the_stats_are_not_handed_out_through_nginx(self):
        self.assertEqual(self.get("/v1/stats", "198.51.100.7")[0], 404)
        code, stats = self.get("/v1/stats?days=7")
        self.assertEqual((code, stats["days"]), (200, 7))
        self.assertEqual(self.get("/v1/stats?days=x")[0], 400)

    def test_hourly_counts_go_after_two_weeks_and_the_installs_seen_after_the_day(self):
        self.get("/v1/refinery/stations", "198.51.100.7")
        self.store.prune(self.now + 15 * 86400)
        db = sqlite3.connect(self.store.path)
        try:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM hourly").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM counters WHERE name = 'req:stations'").fetchone()[0], 1)
        finally:
            db.close()
        self.assertEqual(self.store.seen, {})

    # ---------------------------------------------------------------- the install's number

    ONE, OTHER = "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210"

    def test_installs_are_counted_by_their_number_and_readings_told_apart_by_the_address(self):
        self.send(address="198.51.100.7", install=self.ONE)
        self.send(address="198.51.100.7", install=self.OTHER)
        self.send(address="198.51.100.7", install="NOT-A-NUMBER")   # as an older Bedrock: by its address
        self.get("/v1/refinery/stations", "198.51.100.7", install=self.ONE)
        self.get("/v1/refinery/stations", "198.51.100.7", install=self.OTHER)
        self.get("/v1/refinery/stations", "198.51.100.7", install=self.ONE)
        today = self.get("/v1/stats")[1]["counters"][app.utc_day(time.time())]
        self.assertEqual((today["senders"], today["clients"]), (3, 2))
        # a number is easily made up: the readings keep their address's token,
        # so one address does not pass for several senders
        self.assertEqual(len({r["source"] for r in self.rows()}), 1)
        kept = b"".join(open(p, "rb").read() for p in (self.store.path, self.store.path + "-wal")
                        if os.path.exists(p))
        self.assertNotIn(self.ONE.encode(), kept)
        self.assertNotIn(b"198.51.100", kept)

    def test_an_install_that_sends_what_does_not_fit_is_held_back(self):
        for _ in range(app.MISFITS_PER_HOUR):
            self.assertEqual(self.send(install=self.ONE, station=HUR, quote=self.quote(cost=5000))[0], 201)
        code, answer = self.send(install=self.ONE)
        self.assertEqual(code, 429)
        self.assertIn("did not fit", answer["error"])
        # ...from another address too, and so is its address with another number
        self.assertEqual(self.send(address="198.51.100.9", install=self.ONE)[0], 429)
        self.assertEqual(self.send(install=self.OTHER)[0], 429)
        self.assertEqual(self.send(address="198.51.100.9", install=self.OTHER)[0], 201)
        today = self.get("/v1/stats")[1]["counters"][app.utc_day(time.time())]
        self.assertEqual((today["implausible"], today["held"], today["accepted"]), (app.MISFITS_PER_HOUR, 3, 1))

    def test_what_did_not_fit_holds_back_for_ten_minutes_and_is_forgotten_after_an_hour(self):
        misfits = app.Misfits(per_hour=3, hold=600)
        misfits.add(["203.0.113.1", "install:x"], 1000)
        misfits.add(["203.0.113.1", "install:x"], 1000)
        self.assertFalse(misfits.held(["203.0.113.1", "install:x"], 1001))
        misfits.add(["198.51.100.9", "install:x"], 1002)                  # x's third, from elsewhere
        self.assertTrue(misfits.held(["192.0.2.5", "install:x"], 1003))   # by its number
        self.assertFalse(misfits.held(["203.0.113.1", "install:y"], 1003))
        self.assertFalse(misfits.held(["192.0.2.5", "install:x"], 1602))  # ten minutes on
        misfits.add(["203.0.113.1"], 4700)                                # its two of an hour ago are gone
        self.assertFalse(misfits.held(["203.0.113.1"], 4701))
        for _ in range(3):
            misfits.add(["203.0.113.1"], 4800)                            # turned down, or not: the same
        self.assertTrue(misfits.held(["203.0.113.1", "install:z"], 4801))

    # ---------------------------------------------------------------- rock scans

    SESSION = "9f3a1c2b7d4e6f80"
    BARE = ",".join([""] * 8)    # resistance to implausible, none of them read

    def rock(self, **changes):
        # Taranite in Pyro as a Prospector's panel showed it, each a little
        # earlier than the last, so no two are the same rock
        self.tick += 1
        r = {"observed": self.now - 60 - self.tick, "ore": "Taranite", "mass": 25005, "scu": 32.38,
             "resistance": 36, "printed": "white", "instability": 302.71, "range": 27, "ship": "prospector",
             "place": "Pyro - Pyro II",
             "parts": [{"ore": "Taranite", "percent": 9.37, "quality": 957},
                       {"ore": "Taranite", "percent": 38.51, "quality": 310},
                       {"inert": True, "percent": 52.11}]}
        r.update(changes)
        return r

    def batch(self, rocks, **changes):
        b = {"v": 1, "bedrock": "3.0.3", "game": "4.10.2", "channel": "LIVE", "session": self.SESSION, "rocks": rocks}
        b.update(changes)
        return b

    def send_rocks(self, rocks, address="203.0.113.1", install=None, **changes):
        return self.call("/v1/rocks/observations", self.batch(rocks, **changes), address=address, install=install)

    def rock_rows(self):
        db = sqlite3.connect(self.store.path)
        db.row_factory = sqlite3.Row
        try:
            return db.execute("SELECT * FROM rocks ORDER BY id").fetchall()
        finally:
            db.close()

    def today(self):
        return self.store.counters(app.utc_day(time.time()))[app.utc_day(time.time())]

    def test_a_batch_of_rocks_is_kept_once(self):
        body = self.batch([self.rock(), self.rock()])
        self.assertEqual(self.call("/v1/rocks/observations", body), (201, {"ok": True, "stored": 2, "duplicates": 0}))
        self.assertEqual(self.call("/v1/rocks/observations", body), (200, {"ok": True, "stored": 0, "duplicates": 2}))
        rows = self.rock_rows()
        self.assertEqual(len(rows), 2)
        r = rows[0]
        self.assertEqual((r["observed"], r["session"], r["bedrock"], r["game"], r["channel"], r["ore"], r["mass"],
                          r["scu"], r["resistance"], r["printed"], r["instability"], r["range_m"], r["ship"],
                          r["place"], r["implausible"]),
                         (body["rocks"][0]["observed"], self.SESSION, "3.0.3", "4.10.2", "LIVE", "Taranite", 25005,
                          32.38, 36, "white", 302.71, 27, "prospector", "Pyro - Pyro II", None))
        self.assertEqual(r["parts"], '[{"ore":"Taranite","percent":9.37,"quality":957},'
                                     '{"ore":"Taranite","percent":38.51,"quality":310},{"inert":true,"percent":52.11}]')
        self.assertEqual(len(r["source"]), 16)
        today = self.get("/v1/stats")[1]["counters"][app.utc_day(time.time())]
        self.assertEqual((today["req:rocks"], today["rocks:accepted"], today["rocks:duplicate"], today["rock-senders"]),
                         (2, 2, 2, 1))
        self.assertNotIn("rocks:implausible", today)
        self.assertNotIn("accepted", today)     # the refinery's own counts stay apart

    def test_what_was_not_read_is_kept_as_nothing(self):
        bare = {"observed": self.now - 30, "ore": "Quartz", "mass": 9520, "scu": 26.4, "resistance": -1,
                "instability": -1}
        blank = self.rock(resistance=-1, instability=-1, printed="", range=0, ship="", place="", parts=[])
        # at density 1, but -1 is not read, not 0: no first picture of the panel
        unread = self.rock(mass=4629, scu=32.4, resistance=-1, instability=-1, parts=[])
        self.assertEqual(self.send_rocks([bare, blank, unread], game=None, channel=None)[0], 201)
        for r in self.rock_rows():
            self.assertEqual((r["game"], r["channel"], r["resistance"], r["instability"], r["implausible"]),
                             (None, None, None, None, None))
        self.assertEqual([(r["printed"], r["range_m"], r["ship"], r["place"], r["parts"]) for r in self.rock_rows()[:2]],
                         [(None, None, None, None, None)] * 2)

    def test_a_batch_that_does_not_fit_the_format_is_turned_down_whole(self):
        bad = [
            self.batch([self.rock()], v=2),
            self.batch([self.rock()], session="9F3A1C2B7D4E6F80"),
            self.batch([self.rock()], session="9f3a1c2b7d4e6f8"),
            self.batch([self.rock()], session=None),
            self.batch([self.rock() for _ in range(51)]),
            self.batch([]),
            self.batch([self.rock(), self.rock(ore="Unobtainium")]),     # one rock wrong, none kept
            self.batch([self.rock(observed=self.now - 4 * 86400)]),
            self.batch([self.rock(mass=0)]),
            self.batch([self.rock(scu="32.38")]),
            self.batch([self.rock(resistance=-2)]),
            self.batch([self.rock(instability=None)]),
            self.batch([self.rock(printed="green")]),
            self.batch([self.rock(ship="carrack")]),
            self.batch([self.rock(range=2001)]),
            self.batch([self.rock(place="Pyro " + "I" * 60)]),
            self.batch([self.rock(parts=[{"inert": True, "percent": 11}] * 9)]),
            self.batch([self.rock(parts=[{"ore": "Taranite", "percent": 9.37}])]),
            self.batch([self.rock(parts=[{"ore": "Hadanite", "percent": 9.37, "quality": 500}])]),
            self.batch([self.rock(parts=[{"inert": "yes", "percent": 52.11}])]),
            [self.rock()],
        ]
        # (each from an address of its own: five from one would hold it back)
        for i, body in enumerate(bad):
            code, answer = self.call("/v1/rocks/observations", body, address="192.0.2.%d" % i)
            self.assertEqual(code, 400, json.dumps(body)[:200])
            self.assertFalse(answer["ok"])
        self.assertEqual(self.call("/v1/rocks/observations", raw=b"{not json", address="192.0.2.99")[0], 400)
        self.assertEqual(self.call("/v1/rocks/observations", raw=b"[" * 60000, address="192.0.2.98")[0], 400)
        self.assertEqual(self.store.rock_count(), 0)
        today = self.today()
        self.assertEqual(today["rocks:rejected"], len(bad) + 2)
        self.assertEqual((today["rocks:rejected:session"], today["rocks:rejected:ore"], today["rocks:rejected:json"]),
                         (3, 2, 2))
        self.assertNotIn("rejected", today)

    def test_a_batch_may_be_larger_than_a_refinery_reading_up_to_64_kb(self):
        body = json.dumps(self.batch([self.rock() for _ in range(50)])).encode("utf-8")
        self.assertGreater(len(body), app.MAX_BODY)
        self.assertEqual(self.call("/v1/refinery/observations", raw=body)[0], 413)    # its 16 KB stay
        self.assertEqual(self.call("/v1/rocks/observations", raw=body),
                         (201, {"ok": True, "stored": 50, "duplicates": 0}))
        self.assertEqual(self.call("/v1/rocks/observations", raw=b"x" * (app.MAX_ROCK_BODY + 1))[0], 413)
        self.assertEqual(self.call("/v1/rocks/observations", self.batch([self.rock()]), kind="text/plain")[0], 415)

    def test_rocks_whose_figures_do_not_fit_together_are_kept_but_marked(self):
        rocks = [
            self.rock(),                                                              # as the panel read it
            self.rock(ore="Ice", mass=76, scu=0.54, resistance=0, instability=0, parts=[]),   # ice: density 1
            self.rock(mass=90000, scu=60),
            self.rock(mass=1000, scu=10),                                             # density 0.7
            self.rock(ore="Ice", mass=1000, scu=1, parts=[]),                         # density 7
            self.rock(mass=4629, scu=32.4, resistance=0, instability=0, parts=[]),    # the panel's first picture
            self.rock(parts=[{"ore": "Taranite", "percent": 9.37, "quality": 957}, {"inert": True, "percent": 52.11}]),
            # 958: no Taranite comes out at that
            self.rock(parts=[{"ore": "Taranite", "percent": 9.37, "quality": 958},
                             {"ore": "Taranite", "percent": 38.51, "quality": 310}, {"inert": True, "percent": 52.11}]),
        ]
        # the sender is not told
        self.assertEqual(self.send_rocks(rocks), (201, {"ok": True, "stored": 8, "duplicates": 0}))
        self.assertEqual([r["implausible"] for r in self.rock_rows()],
                         [None, None, "bigger than any rock", "lighter than its volume", "heavier than its parts",
                          "not scanned yet", "parts do not add up", "a quality its ore does not have"])
        today = self.today()
        self.assertEqual((today["rocks:accepted"], today["rocks:implausible"]), (2, 6))

    def test_rocks_at_the_edges_of_what_the_game_makes_fit(self):
        def at(ore, mass, scu, resistance=20, instability=50, parts=()):
            return app.judge_rock({"ore": ore, "mass": mass, "scu": scu, "resistance": resistance,
                                   "instability": instability, "parts": list(parts)})
        self.assertEqual(at("Gold", 30100, 10), "")                  # all of it gold: density 21.07
        self.assertEqual(at("Gold", 30800, 10), "heavier than its parts")
        self.assertEqual(at("Quartz", 143, 1.0), "")                 # density 1: 142.857 kg, printed as 143
        self.assertEqual(at("Quartz", 138, 1.0), "lighter than its volume")
        self.assertEqual(at("Ice", 1, 0.01, 0, 0), "")               # the smallest figures the panel shows
        self.assertEqual(at("Quartz", 3571, 10, 0, 0), "")           # no resistance, but scanned: density 2.5
        self.assertEqual(at("Quartz", 1429, 10, 0, 0), "not scanned yet")
        self.assertEqual(at("Quartz", 1429, 10, 0, 1), "")
        self.assertEqual(at("Taranite", 82908, 59.22), "")           # the largest rock model, at density 9.8
        self.assertEqual(at("Iron", 7000, 10, parts=[{"inert": True, "percent": 100.4}]), "")
        self.assertEqual(at("Iron", 7000, 10, parts=[{"inert": True, "percent": 97.9}]), "parts do not add up")
        # each quality one its ore has: Iron at 664 and 325, as a panel showed them
        iron = [{"ore": "Iron", "percent": 10.55, "quality": 664}, {"ore": "Iron", "percent": 36.10, "quality": 325},
                {"inert": True, "percent": 53.35}]
        self.assertEqual(at("Iron", 7000, 10, parts=iron), "")
        iron[1] = dict(iron[1], quality=326)
        self.assertEqual(at("Iron", 7000, 10, parts=iron), "a quality its ore does not have")
        self.assertEqual(sorted(app.ORE_QUALITIES), sorted(app.ROCK_ORES))
        self.assertTrue(all(len(set(values)) == 8 for values in app.ORE_QUALITIES.values()))

    def test_rocks_that_do_not_fit_hold_no_one_back_but_batches_turned_down_do(self):
        for _ in range(app.MISFITS_PER_HOUR + 1):
            self.assertEqual(self.send_rocks([self.rock(mass=1000, scu=10)], install=self.ONE)[0], 201)
        for _ in range(app.MISFITS_PER_HOUR):
            self.assertEqual(self.send_rocks([self.rock()], install=self.ONE, v=2)[0], 400)
        code, answer = self.send_rocks([self.rock()], install=self.ONE)
        self.assertEqual(code, 429)
        self.assertIn("did not fit", answer["error"])
        # ...and its refinery readings with them: an install is held back once
        self.assertEqual(self.send(install=self.ONE)[0], 429)
        today = self.today()
        self.assertEqual((today["rocks:implausible"], today["rocks:rejected"], today["rocks:held"], today["held"]),
                         (app.MISFITS_PER_HOUR + 1, app.MISFITS_PER_HOUR, 1, 1))

    def test_rocks_have_a_limit_of_their_own(self):
        self.rock_limiter.per_address = 3
        for _ in range(3):
            self.assertEqual(self.send_rocks([self.rock()], address="192.0.2.1")[0], 201)
        self.assertEqual(self.send_rocks([self.rock()], address="192.0.2.1"),
                         (429, {"ok": False, "error": "too many reports, try again later"}))
        self.assertEqual(self.send_rocks([self.rock()], address="192.0.2.2")[0], 201)
        self.assertEqual(self.send(address="192.0.2.1")[0], 201)    # the refinery's limit is another
        today = self.today()
        self.assertEqual(today["rocks:limited"], 1)
        self.assertNotIn("limited", today)

    def test_rocks_are_counted_as_rocks_as_well(self):
        self.rock_count_limiter.per_address = 5
        self.assertEqual(self.send_rocks([self.rock() for _ in range(3)], address="192.0.2.1")[0], 201)
        # three more would be six: the whole batch waits
        self.assertEqual(self.send_rocks([self.rock() for _ in range(3)], address="192.0.2.1"),
                         (429, {"ok": False, "error": "too many rocks, try again later"}))
        self.assertEqual(self.send_rocks([self.rock() for _ in range(2)], address="192.0.2.1")[0], 201)
        self.assertEqual(self.send_rocks([self.rock()], address="192.0.2.1")[0], 429)
        self.assertEqual(self.send_rocks([self.rock() for _ in range(5)], address="192.0.2.2")[0], 201)
        self.assertEqual(self.store.rock_count(), 10)
        self.assertEqual(self.today()["rocks:limited"], 2)
        # ...and an hour on, the first three are let go, the two after them not yet
        limiter = app.Limiter(per_address=5, overall=100)
        self.assertTrue(limiter.allow("192.0.2.1", 1000, 3))
        self.assertFalse(limiter.allow("192.0.2.1", 1001, 3))
        self.assertTrue(limiter.allow("192.0.2.1", 1001, 2))
        self.assertTrue(limiter.allow("192.0.2.1", 4600, 3))
        self.assertFalse(limiter.allow("192.0.2.1", 4600, 1))

    def test_each_rock_says_which_of_its_senders_rocks_of_the_day_it_was(self):
        first = [self.rock(), self.rock()]
        self.send_rocks(first, address="198.51.100.7")
        self.send_rocks(first + [self.rock()], address="198.51.100.7")     # the two again: not counted again
        self.send_rocks([self.rock()], address="198.51.100.8")
        self.send_rocks([self.rock()], address="198.51.100.7", install=self.ONE)
        self.assertEqual([r["sender_nth"] for r in self.rock_rows()], [1, 2, 3, 1, 4])
        # another day, another token: counted from one again
        tomorrow = self.now + 86400
        batch = app.validate_rocks(self.batch([self.rock()]), self.now)
        self.store.add_rocks(batch, tomorrow, self.store.source("198.51.100.7", tomorrow), [""])
        self.assertEqual(self.rock_rows()[-1]["sender_nth"], 1)

    def test_rocks_lose_the_senders_token_after_two_days_and_stay(self):
        self.send_rocks([self.rock()], address="198.51.100.7", install=self.ONE)
        self.send(address="198.51.100.7")
        self.assertEqual(self.rock_rows()[0]["source"], self.rows()[0]["source"])    # a reading's token
        self.store.prune(self.now + 3600)
        self.assertIsNotNone(self.rock_rows()[0]["source"])
        self.store.prune(self.now + 49 * 3600)
        self.assertIsNone(self.rock_rows()[0]["source"])
        self.assertEqual(self.store.rock_count(), 1)
        kept = b""
        for path in (self.store.path, self.store.path + "-wal"):
            if os.path.exists(path):
                with open(path, "rb") as f:
                    kept += f.read()
        self.assertNotIn(b"198.51.100", kept)
        self.assertNotIn(self.ONE.encode(), kept)

    def test_the_rocks_are_for_the_operator_alone(self):
        rock = self.rock()
        ice = self.rock(ore="Ice", mass=76, scu=0.54, resistance=-1, instability=-1, printed="", range=0, ship="",
                        place="", parts=[])
        self.send_rocks([rock, ice])
        self.assertEqual(self.get("/v1/rocks/export.csv", "198.51.100.7"), (404, {"ok": False, "error": "not found"}))
        code, text = self.call("/v1/rocks/export.csv")
        self.assertEqual(code, 200)
        lines = text.split("\n")
        self.assertEqual(lines[0], "id,received,observed,bedrock,game,channel,session,sender_nth,ore,mass,scu,"
                                   "resistance,printed,instability,range_m,ship,place,parts,implausible")
        self.assertEqual(lines[1].split(",", 3)[2:], [
            app.utc_iso(rock["observed"]), "3.0.3,4.10.2,LIVE,9f3a1c2b7d4e6f80,1,Taranite,25005,32.38,36,white,"
            "302.71,27,prospector,Pyro - Pyro II,Taranite 9.37 957; Taranite 38.51 310; inert 52.11,"])
        self.assertTrue(lines[2].endswith(",Ice,76,0.54," + self.BARE), lines[2])
        self.assertEqual(len(lines), 4)
        self.assertEqual(self.today()["req:rocks-export"], 1)       # the one through nginx
        # ...and the same from the command line
        target = os.path.join(self.folder.name, "rocks.csv")
        out = io.StringIO()
        app.DB_PATH, kept = self.store.path, app.DB_PATH
        try:
            with contextlib.redirect_stdout(out):
                app.main(["--rocks-export", target])
        finally:
            app.DB_PATH = kept
        self.assertIn("2 rocks written to", out.getvalue())
        with open(target, encoding="utf-8", newline="") as f:
            self.assertEqual(f.read(), text)

    def test_health_counts_the_rocks(self):
        self.send_rocks([self.rock(), self.rock()])
        code, answer = self.call("/v1/health")
        self.assertEqual((code, answer["observations"], answer["rocks"]), (200, 0, 2))

    def test_a_database_from_before_the_rocks_gets_their_table(self):
        self.send()
        db = sqlite3.connect(self.store.path)
        try:
            with db:
                db.execute("DROP TABLE rocks")
        finally:
            db.close()
        app.Store(self.store.path)
        self.assertEqual(self.send_rocks([self.rock()])[0], 201)
        self.assertEqual((self.store.count(), self.store.rock_count()), (1, 1))


if __name__ == "__main__":
    unittest.main()
