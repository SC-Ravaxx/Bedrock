# Refinery observations

A small service that collects what Bedrock reads off refinery terminals and
hands it back per station. The game sets a refinery's cost and processing
time by its current workload, and only the terminal shows that workload
("Current capacity 106 %", "Refinery currently has a moderate workload. A
small surcharge will be added."). With readings from many players, Bedrock
can show how busy each refinery is and what it quotes.

Bedrock only sends readings when the player has agreed to it.

## What is kept

For each reading: the station, when the terminal showed it, the capacity in
per cent and the workload level, and, when a work order was being set up,
its quote: method, cost, processing time and each material with its
quantity, quality and yield. The Bedrock version and the game version go
with it.

Nothing about the player is kept: no name, no account and no IP address.
To tell senders apart, each reading gets a token made from the address and
a salt. The salt exists only in the service's memory and changes every
day, so a token cannot be traced back to an address, not even with a copy
of the database. A reading loses its token after 48 hours. The rate limit
holds addresses in memory for an hour, salted and hashed. The log has the
request line and the status code only.

Requests that come through nginx are counted, by the hour (two weeks) and
by the day (180 days): what was asked for, the Bedrock version and platform
from the User-Agent (`Bedrock/2.6.6 (Windows)`), and how many installs asked
for the stations or sent a reading that day. Installs are told apart by the
same daily token, kept in memory for the day; only the counts are stored.

All readings can be downloaded by anyone as CSV (`/v1/refinery/export.csv`),
including whether each was taken as plausible, but never the token.

## Keeping false reports out

Anyone can send anything to an open service, and the client being open
source means it cannot prove a reading came from a real terminal. So
nothing is taken on its word:

1. **Format.** A reading has to look like what a terminal can show: known
   fields, numbers in range, a time within the last three days, a known
   workload level. Anything else is turned down with `400`.
2. **The game's rules.** A reading that does not fit is kept but marked
   implausible, and it is never handed out. The sender is not told, so a
   forger does not learn what passes.
   - A workload level is accepted only at the capacities other senders saw
     it at. The span is the 10th to the 90th percentile over 30 days, plus
     10 points either side. It is used once 10 readings from 3 senders are
     in, and only while the levels' medians rise in the game's order
     (light, moderate, heavy, extreme).
   - A quote cannot yield more than was put in. Where Bedrock's refining
     model has the factors (`tables.json`), the yield has to match the
     model within 15 % (the model matched every work order measured), and
     cost and time have to stay within wide bands around it (0.5 to 6 times
     and 0.1 to 20 times). Those bands leave room for the workload
     surcharge, which still has to be learned.
3. **Several senders.** A station's current state is its newest reading
   of the last three hours, each sender's newest counting once - hours
   rather than minutes while few players send, as a workload moves slowly.
   It counts as `confirmed` once another sender saw the same (the same
   workload, the capacity within 15 points or 25 %). Where two or more
   senders agree on something else and outnumber the ones that back the
   newest reading, the newest of theirs stands instead.
4. **Limits.** One sender can report a station at most 12 times an hour,
   and an address can send at most 120 readings an hour in total. nginx
   adds its own limit in front.
5. **Cleaning up.** `--stats` shows what came in each day: readings
   accepted, duplicate, implausible, limited and rejected. `--purge` finds
   readings by time, station and plausibility, and removes them only when
   run again with `--yes`.

## API

All paths start with `/bedrock/api`. Answers are JSON unless noted.

### `POST /v1/refinery/observations`

One reading, `Content-Type: application/json`, at most 16 KB:

```json
{
  "v": 1,
  "bedrock": "2.6.7",
  "game": "4.10.2",
  "channel": "LIVE",
  "observed": 1791709800,
  "station": "Checkmate",
  "capacity": 106,
  "workload": "moderate",
  "quote": {
    "method": "DINYX_SOLVENTATION",
    "cost": 4210,
    "seconds": 23040,
    "items": [
      {"material": "Iron (Ore)", "quantity": 1050, "quality": 874, "yield": 958}
    ]
  }
}
```

- `observed` is UTC seconds. Readings up to three days old are accepted, so
  Bedrock can send them after being offline.
- `station` is spelled as in `stations.txt`. A station that is not listed
  there is stored, but it is not shown until it is added.
- `workload` is one of `light`, `moderate`, `heavy` or `extreme`.
- Quantities and yields are in cSCU, cost in aUEC, and `method` is the
  game's key for the method.
- `capacity`, `workload` and `quote` are each optional, but a reading needs
  at least one of them.

The answer is `201 {"ok": true, "stored": true}`, or `200` with
`"stored": false` when the same reading was already stored. Errors: `400`
for a reading that does not fit the format, `413` when it is too large,
`415` when the content type is not JSON, and `429` when a limit is reached.

### `GET /v1/refinery/stations`

Each station's current state. It is `fresh` when made from readings of the
last three hours. Otherwise it is the newest plausible reading of the last
seven days, with `sources` 0.

```json
{"generated": 1791710100, "days": 7, "window": 10800, "stations": [
  {"station": "Checkmate", "capacity": 106, "workload": "moderate",
   "observed": 1791709800, "fresh": true, "sources": 2, "confirmed": true,
   "seen_24h": 5, "quotes_24h": 2}
]}
```

### `GET /v1/refinery/quotes?days=14&station=Checkmate`

Plausible quotes from the last `days` days (1 to 60, 14 when not given),
newest first, optionally for one station, at most 5000.

### `GET /v1/refinery/export.csv`

Every reading stored, as CSV, with `plausible` and `reason`.

### `GET /v1/health`

The number of readings stored, and today's counts:
`{"ok": true, "version": 1, "observations": N, "today": {"accepted": …,
"duplicate": …, "implausible": …, "limited": …, "rejected": …}}`

### `GET /v1/stats?days=90`

For the operator's admin page, from inside the containers' network only: a
request through nginx (it carries `X-Real-IP`) gets a 404. The counters by
day (`req:<route>`, `agent:<version>`, `platform:<os>`, `clients`,
`senders` and the outcomes above), the requests by hour, the readings and
quotes received by day, each station's state and how often it was read.

## Running it

Python 3.12 or newer, nothing else:

    python app.py                      # port 8080, database /data/refinery.db
    python -m unittest test_app        # the tests, on a free port and a temporary database
    python app.py --stats
    python app.py --purge --since 2026-10-11T08:00 --until 2026-10-11T09:00 --station Checkmate
    python app.py --purge --since 2026-10-11T08:00 --implausible --yes
    python app.py --backup FILE        # a consistent copy while the service runs
    python make_tables.py              # tables.json again, after the refining headers change

The environment variables `BEDROCK_API_DB`, `BEDROCK_API_PORT`,
`BEDROCK_API_PREFIX`, `BEDROCK_API_STATIONS`, `BEDROCK_API_TABLES`,
`BEDROCK_API_POSTS_PER_HOUR` and `BEDROCK_API_POSTS_PER_HOUR_ALL` change
the defaults.

## Deploying it behind nginx with Docker Compose

1. Copy `app.py`, `stations.txt`, `tables.json` and `Dockerfile` into a
   folder next to the compose file, for example `./bedrock-api`.
2. Add the service in `deploy/compose-service.yml` to the compose file,
   along with its volume, and run `docker compose up -d --build bedrock-api`.
3. Add the two parts of `deploy/nginx.conf` to the site's configuration.
   Then check it with `nginx -t` and reload.
4. Check it from outside:
   `https://the-forsaken.org/bedrock/api/v1/health`.
5. Add `deploy/backup.sh` to the daily backup.

The container runs as a user without privileges, with a read-only file
system apart from its data volume, and is reached only through nginx on the
stack's network. To look at the data or clean it up on the server, use
`docker exec bedrock-api python app.py --stats` or the same with `--purge`.
