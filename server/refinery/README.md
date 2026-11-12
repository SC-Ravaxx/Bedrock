# Refinery observations

A small service that collects what Bedrock reads off refinery terminals and
hands it back per station. The game sets a refinery's cost and processing
time by its current workload, and only the terminal shows that workload
("Current capacity 106 %", "Refinery currently has a moderate workload. A
small surcharge will be added."). With readings from many players, Bedrock
can show how busy each refinery is and what it quotes.

Bedrock only sends refinery readings when the player has agreed to it.

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

Since 2.6.7, Bedrock also sends a random number it made for its install the
first time (`X-Bedrock-Install`; `stats=0` in its config leaves it out). The
service counts installs by it and holds back an install that sends too many
readings that do not fit (see Limits below). The number is not stored
either: it is held in memory, salted and hashed like the addresses.
Readings are still told apart by the address, since a number is easily
made up.

Requests that come through nginx are counted, by the hour (two weeks) and
by the day (180 days): what was asked for, the Bedrock version and platform
from the User-Agent (`Bedrock/2.6.6 (Windows)`), and how many installs asked
for the stations or sent a reading that day. Installs are told apart by
their number, or by the address for older versions, with the same daily
salt, kept in memory for the day; only the counts are stored.

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
   implausible, and it is never handed out. The answer is the same as for
   a plausible reading.
   - A workload level is accepted only at its own capacities: light under
     100 %, moderate 100 to 199 %, heavy 200 to 299 % and extreme from
     300 %, with 10 points either side. Light has no floor, and extreme's
     ceiling is twice the top of what other senders saw it at (the 90th
     percentile over 30 days, once 10 readings from 3 senders are in, and
     only while the levels' medians rise in the game's order).
   - A quote cannot yield more than was put in. Where Bedrock's refining
     model has the factors (`tables.json`), the yield has to match the
     model within 15 % (the model matched every work order measured), and
     cost and time have to stay within wide bands around it (0.1 to 20
     times). Those bands leave room for the workload surcharge and whatever
     else moves the cost, which still has to be learned: in October 2026
     terminals asked 0.26 to 9.6 times the model's cost.
3. **Several senders.** A station's current state is its newest reading
   of the last three hours, each sender's newest counting once - hours
   rather than minutes while few players send, as a workload moves slowly.
   It counts as `confirmed` once another sender saw the same (the same
   workload, the capacity within 15 points or 25 %). Where two or more
   senders agree on something else and outnumber the ones that back the
   newest reading, the newest of theirs stands instead.
4. **Limits.** One sender can report a station at most 12 times an hour,
   and an address can send at most 120 readings an hour in total. nginx
   adds its own limit in front. An install that sends five readings in an
   hour that are turned down or do not fit is held back: its reports are
   refused with `429` for ten minutes, and Bedrock waits that long before
   it sends again. This goes by its number and by its address, so a
   made-up number does not get around it.
5. **Cleaning up.** `--stats` shows what came in each day: readings
   accepted, duplicate, implausible, limited, held and rejected. `--purge` finds
   readings by time, station and plausibility, and removes them only when
   run again with `--yes`. `--rejudge` judges the implausible readings
   again by the rules as they stand, after one was found too strict, and
   takes those that pass as plausible with `--yes`.

## Rock scans

Bedrock also sends the rocks the player scanned, unless sharing is turned
off in Bedrock's Settings. They are kept to work out rock sizes, densities
and the game's formulas for resistance and instability.

For each rock: when it was scanned, the ore, mass, volume in SCU,
resistance and instability as the scan panel printed them, whether the
resistance was printed in colour (a laser on the rock had changed it), the
distance to the rock, the ship and the place, and the composition: each ore
with its share and quality, and the inert share. The Bedrock version and
the game's version and channel go with it, and a random number Bedrock
makes fresh at each start, so that one session's rocks can be looked at
together, such as a rock and the pieces it broke into. That number tells
neither installs nor players apart.

Nothing about the player is kept: no name, no account, no IP address and
no install number. Like a refinery reading, each rock gets a token to tell
senders apart and loses it after 48 hours. Each rock also keeps which of its
sender's rocks of that day (UTC) it was, 1 for the first, so that the
analysis can count no sender for more than so many rocks a day: one who
sends thousands weighs no more than a player who mined all day. The number
says nothing of who sent it. The rocks themselves are kept.

A batch that does not fit the format is turned down whole with `400`, and
counts towards holding an install back, as for refinery readings. A rock
whose figures do not fit together is kept but marked, and left out of the
analysis: bigger than the largest rock (59.22 SCU), lighter than its volume
at density 1 or heavier than the densest element its ore can hold (with
room for the panel's rounding), the panel's first figures before the game
knows what the rock is made of (density 1, no resistance, no instability;
ice aside), a composition that does not add up to 100 %, or a quality its
ore never comes out at (each ore has eight). The answer does not say which,
and such rocks do not hold the sender back, as a panel is easily misread.

An address can send 60 batches and 300 rocks an hour, and everyone together
20000 batches and 100000 rocks; more is turned down with `429`. A player
mining hard scans about 100 rocks an hour.

The rocks are not handed out: only the operator can download them, from
inside the containers' network (`/v1/rocks/export.csv`) or with
`python app.py --rocks-export FILE`.

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
`415` when the content type is not JSON, and `429` when a limit is reached
or the install is held back.

### `GET /v1/refinery/stations`

Each station's current state. It is `fresh` when made from readings of the
last three hours. Otherwise it is the newest plausible reading of the last
seven days, with `sources` 0. Where that reading has the capacity but not
the workload, `workload` is the level the capacity gives and
`workload_read` is `false`.

```json
{"generated": 1791710100, "days": 7, "window": 10800, "stations": [
  {"station": "Checkmate", "capacity": 106, "workload": "moderate",
   "workload_read": true, "observed": 1791709800, "fresh": true,
   "sources": 2, "confirmed": true, "seen_24h": 5, "quotes_24h": 2}
]}
```

### `GET /v1/refinery/quotes?days=14&station=Checkmate`

Plausible quotes from the last `days` days (1 to 60, 14 when not given),
newest first, optionally for one station, at most 5000.

### `GET /v1/refinery/export.csv`

Every reading stored, as CSV, with `plausible` and `reason`.

### `POST /v1/rocks/observations`

The rocks scanned, 1 to 50 in a batch, `Content-Type: application/json`, at
most 64 KB:

```json
{
  "v": 1,
  "bedrock": "3.0.3",
  "game": "4.10.2",
  "channel": "LIVE",
  "session": "9f3a1c2b7d4e6f80",
  "rocks": [
    {"observed": 1793000000, "ore": "Taranite", "mass": 25005, "scu": 32.38,
     "resistance": 36, "printed": "white", "instability": 302.71,
     "range": 27, "ship": "prospector", "place": "Pyro - Pyro II",
     "parts": [{"ore": "Taranite", "percent": 9.37, "quality": 957},
               {"ore": "Taranite", "percent": 38.51, "quality": 310},
               {"inert": true, "percent": 52.11}]}
  ]
}
```

- `session` is 16 lowercase hex characters, made fresh at each start of
  Bedrock.
- `observed` is UTC seconds. Rocks up to three days old are accepted.
- `ore` is one of Agricium, Aluminium, Aslarite, Beryl, Bexalite, Borase,
  Copper, Corundum, Gold, Hephaestanite, Ice, Iron, Laranite, Lindinium,
  Ouratite, Quantainium, Quartz, Riccite, Savrilium, Silicon, Stileron,
  Taranite, Tin, Titanium, Torite or Tungsten.
- `mass` is in kg, `scu` from 0.01 to 100, `resistance` in per cent as
  printed and `instability` as printed; either is `-1` when it was not read.
- `printed` is `white`, or `colour` when the panel printed the resistance
  green or red because a laser on the rock had changed it.
- `range` is the distance in metres (`0` when not known), `ship` is
  `prospector`, `mole` or `golem`, and `place` takes the characters of a
  station's name, up to 64.
- `parts` is the composition in the panel's order, up to 8 lines: an ore
  with its share in per cent and its quality (0 to 1000), or the inert
  share. It is empty when the list was not read whole.
- `game`, `channel`, `printed`, `range`, `ship`, `place` and `parts` are
  optional.

The answer is `201 {"ok": true, "stored": 2, "duplicates": 0}`, or `200`
with `"stored": 0` when every rock was already stored. Errors as for a
refinery reading; a `400` turns down the whole batch.

### `GET /v1/rocks/export.csv`

For the operator, from inside the containers' network only, like
`/v1/stats`. Every rock stored, as CSV, with why it was marked
(`implausible`) and which of its sender's rocks of the day it was
(`sender_nth`), but never the token. The same as
`python app.py --rocks-export FILE`.

### `GET /v1/health`

The number of readings and of rocks stored, and today's counts for the
readings: `{"ok": true, "version": 1, "observations": N, "rocks": N,
"today": {"accepted": …, "duplicate": …, "implausible": …, "limited": …,
"held": …, "rejected": …}}`

### `GET /v1/stats?days=90`

For the operator's admin page, from inside the containers' network only: a
request through nginx (it carries `X-Real-IP`) gets a 404. The counters by
day (`req:<route>`, `agent:<version>`, `platform:<os>`, `clients`,
`senders` and the outcomes above, and for rocks `rock-senders` and the
same outcomes as `rocks:accepted` and so on), the requests by hour, the
readings and quotes received by day, each station's state and how often it
was read.

## Running it

Python 3.12 or newer, nothing else:

    python app.py                      # port 8080, database /data/refinery.db
    python -m unittest test_app        # the tests, on a free port and a temporary database
    python app.py --stats
    python app.py --purge --since 2026-10-11T08:00 --until 2026-10-11T09:00 --station Checkmate
    python app.py --purge --since 2026-10-11T08:00 --implausible --yes
    python app.py --backup FILE        # a consistent copy while the service runs
    python app.py --rocks-export FILE  # the rock scans as CSV
    python make_tables.py              # tables.json again, after the refining headers change

The environment variables `BEDROCK_API_DB`, `BEDROCK_API_PORT`,
`BEDROCK_API_PREFIX`, `BEDROCK_API_STATIONS`, `BEDROCK_API_TABLES`,
`BEDROCK_API_POSTS_PER_HOUR`, `BEDROCK_API_POSTS_PER_HOUR_ALL`,
`BEDROCK_API_ROCK_POSTS_PER_HOUR`, `BEDROCK_API_ROCK_POSTS_PER_HOUR_ALL`,
`BEDROCK_API_ROCKS_PER_HOUR` and `BEDROCK_API_ROCKS_PER_HOUR_ALL` change the
defaults.

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
