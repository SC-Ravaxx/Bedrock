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
        self.server = app.serve(self.store, self.limiter, host="127.0.0.1", port=0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d%s" % (self.server.server_address[1], app.PREFIX)
        self.now = int(time.time())
        self.tick = 0

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.folder.cleanup()

    def call(self, path, body=None, kind="application/json", address="203.0.113.1", raw=None):
        data = raw if raw is not None else (None if body is None else json.dumps(body).encode("utf-8"))
        request = urllib.request.Request(self.base + path, data=data, method="POST" if data is not None else "GET")
        if data is not None:
            request.add_header("Content-Type", kind)
            request.add_header("X-Real-IP", address)
        try:
            with urllib.request.urlopen(request, timeout=10) as answer:
                text = answer.read().decode("utf-8")
                return answer.status, (json.loads(text) if answer.headers.get_content_type() == "application/json"
                                       else text)
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read().decode("utf-8"))

    def send(self, address="203.0.113.1", **changes):
        return self.call("/v1/refinery/observations", self.reading(**changes), address=address)

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
                                           "rejected": 1})

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
        for body in bad:
            code, answer = self.call("/v1/refinery/observations", body)
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

    def test_a_workload_level_is_taken_only_where_others_saw_it(self):
        # ten readings of "moderate" between 90 and 126 %, from five senders
        for i in range(10):
            self.send(address="198.51.100.%d" % (i % 5), station=HUR if i % 2 else "Checkmate",
                      capacity=90 + 4 * i, workload="moderate")
        self.assertEqual(self.store.ranges(self.now), {"moderate": (94, 122)})
        self.send(address="198.51.100.9", capacity=20, workload="moderate")
        self.send(address="198.51.100.9", station=HUR, capacity=128, workload="moderate")
        rows = self.rows()
        self.assertEqual((rows[-2]["plausible"], rows[-1]["plausible"]), (0, 1))
        self.assertIn("capacity 20 % where moderate was seen at 94-122 %", rows[-2]["reason"])

    def test_levels_that_contradict_the_game_order_judge_nothing(self):
        for i in range(10):
            self.send(address="192.0.2.%d" % i, capacity=150, workload="light")
            self.send(address="192.0.2.%d" % i, station=HUR, capacity=50, workload="moderate")
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
        self.send(address="10.0.0.1", capacity=140, observed=self.now - 300, workload="heavy")
        s = self.stations()["Checkmate"]
        self.assertEqual((s["capacity"], s["workload"], s["sources"], s["confirmed"]), (140, "heavy", 1, False))

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


if __name__ == "__main__":
    unittest.main()
