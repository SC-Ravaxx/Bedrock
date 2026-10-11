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


class ServiceTest(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.store = app.Store(os.path.join(self.folder.name, "refinery.db"))
        self.limiter = app.Limiter(per_address=5, overall=100)
        self.server = app.serve(self.store, self.limiter, host="127.0.0.1", port=0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d%s" % (self.server.server_address[1], app.PREFIX)
        self.now = int(time.time())

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.folder.cleanup()

    def call(self, path, body=None, kind="application/json", address=None, raw=None):
        data = raw if raw is not None else (None if body is None else json.dumps(body).encode("utf-8"))
        request = urllib.request.Request(self.base + path, data=data, method="POST" if data is not None else "GET")
        if data is not None:
            request.add_header("Content-Type", kind)
        if address:
            request.add_header("X-Real-IP", address)
        try:
            with urllib.request.urlopen(request, timeout=10) as answer:
                text = answer.read().decode("utf-8")
                return answer.status, (json.loads(text) if answer.headers.get_content_type() == "application/json"
                                       else text)
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read().decode("utf-8"))

    def reading(self, **changes):
        r = {"v": 1, "bedrock": "2.6.7", "game": "4.10.2", "channel": "LIVE", "observed": self.now - 60,
             "station": "Checkmate", "capacity": 106, "workload": "moderate"}
        r.update(changes)
        return r

    def quote(self, **changes):
        q = {"method": "DINYX_SOLVENTATION", "cost": 4210, "seconds": 23040,
             "items": [{"material": "Iron (Ore)", "quantity": 1050, "quality": 874, "yield": 958}]}
        q.update(changes)
        return q

    def test_health(self):
        self.assertEqual(self.call("/v1/health"), (200, {"ok": True, "version": 1, "observations": 0}))

    def test_a_reading_is_kept_once_and_shows_as_the_station_now(self):
        self.assertEqual(self.call("/v1/refinery/observations", self.reading()), (201, {"ok": True, "stored": True}))
        self.assertEqual(self.call("/v1/refinery/observations", self.reading()), (200, {"ok": True, "stored": False}))
        # an older reading of the same station does not take the place of the newer
        self.call("/v1/refinery/observations", self.reading(observed=self.now - 3600, capacity=40, workload="light"))
        code, answer = self.call("/v1/refinery/stations")
        self.assertEqual(code, 200)
        self.assertEqual(answer["stations"], [{"station": "Checkmate", "capacity": 106, "workload": "moderate",
                                               "observed": self.now - 60, "seen_24h": 2, "quotes_24h": 0}])

    def test_a_quote_comes_back_with_its_items(self):
        self.assertEqual(self.call("/v1/refinery/observations", self.reading(quote=self.quote()))[0], 201)
        code, answer = self.call("/v1/refinery/quotes?days=7")
        self.assertEqual(code, 200)
        self.assertEqual(len(answer["quotes"]), 1)
        q = answer["quotes"][0]
        self.assertEqual((q["station"], q["method"], q["cost"], q["seconds"], q["capacity"]),
                         ("Checkmate", "DINYX_SOLVENTATION", 4210, 23040, 106))
        self.assertEqual(q["items"], [{"material": "Iron (Ore)", "quantity": 1050, "quality": 874, "yield": 958}])
        self.assertEqual(self.call("/v1/refinery/quotes?station=Levski")[1]["quotes"], [])
        self.assertEqual(self.call("/v1/refinery/quotes?days=x")[0], 400)

    def test_a_station_not_listed_is_kept_but_not_shown(self):
        self.assertEqual(self.call("/v1/refinery/observations", self.reading(station="New Refinery 7"))[0], 201)
        self.assertEqual(self.call("/v1/refinery/stations")[1]["stations"], [])
        code, text = self.call("/v1/refinery/export.csv")
        self.assertEqual(code, 200)
        lines = text.strip().split("\n")
        self.assertEqual(lines[0], "observed,station,known,capacity,workload,method,cost,seconds,items,game,"
                                   "channel,bedrock")
        self.assertIn("New Refinery 7,0,106,moderate", lines[1])

    def test_what_a_terminal_cannot_show_is_turned_down(self):
        bad = [
            self.reading(station="<script>"),
            self.reading(capacity=5000),
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
            code, answer = self.call("/v1/refinery/observations", body, address="10.0.0.%d" % bad.index(body))
            self.assertEqual(code, 400, body)
            self.assertFalse(answer["ok"])
        self.assertEqual(self.store.count(), 0)

    def test_not_json_too_large_or_the_wrong_kind(self):
        self.assertEqual(self.call("/v1/refinery/observations", raw=b"{not json", address="10.1.0.1")[0], 400)
        self.assertEqual(self.call("/v1/refinery/observations", self.reading(), kind="text/plain")[0], 415)
        self.assertEqual(self.call("/v1/refinery/observations", raw=b"x" * (app.MAX_BODY + 1))[0], 413)

    def test_the_rate_limit_is_per_address(self):
        for i in range(5):
            code, _ = self.call("/v1/refinery/observations", self.reading(observed=self.now - 100 - i),
                                address="192.0.2.1")
            self.assertEqual(code, 201)
        self.assertEqual(self.call("/v1/refinery/observations", self.reading(observed=self.now - 200),
                                   address="192.0.2.1")[0], 429)
        self.assertEqual(self.call("/v1/refinery/observations", self.reading(observed=self.now - 200),
                                   address="192.0.2.2")[0], 201)
        # nothing of the address is held as it was
        self.assertTrue(all(len(k) == 32 for k in self.limiter.seen))

    def test_the_log_names_no_address(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.call("/v1/refinery/observations", self.reading(), address="198.51.100.7")
            self.call("/v1/health")
        self.assertIn("POST", err.getvalue())
        self.assertNotIn("198.51.100.7", err.getvalue())
        self.assertNotIn("127.0.0.1", err.getvalue())

    def test_unknown_paths(self):
        self.assertEqual(self.call("/v1/nothing")[0], 404)
        self.assertEqual(self.call("/v1/refinery/stations", self.reading())[0], 404)

    def test_backup_is_a_whole_copy(self):
        self.call("/v1/refinery/observations", self.reading(quote=self.quote()))
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
