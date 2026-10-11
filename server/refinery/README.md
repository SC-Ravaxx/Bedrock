# Refinery observations

A small service that collects what Bedrock reads off refinery terminals and
hands it back per station. The game sets a refinery's cost and processing
time by its current workload, and only the terminal shows that workload
("Current capacity 106 %", "Refinery currently has a moderate workload. A
small surcharge will be added."). With readings from many players, Bedrock
can show how busy each refinery was when last seen and what it quoted.

Bedrock only sends readings when the player has agreed to it.

## What is kept

For each reading: the station, when the terminal showed it, the capacity in
per cent and the workload level, and, when a work order was being set up,
its quote: method, cost, processing time and each material with its
quantity, quality and yield. The Bedrock version and the game version go
with it.

Nothing about the player is kept: no name, no account and no IP address.
For the rate limit, an address is held in memory for an hour, salted and
hashed with a salt that is lost when the service restarts. The log has the
request line and the status code only.

All readings can be downloaded by anyone as CSV (`/v1/refinery/export.csv`).

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
- `workload` is one of `light`, `moderate`, `heavy` or `extreme`, the game's
  four levels.
- Quantities and yields are in cSCU, cost in aUEC, `method` is the game's
  key for the method.
- `capacity`, `workload` and `quote` are each optional, but a reading needs
  at least one of them.

The answer is `201 {"ok": true, "stored": true}`, or `200` with
`"stored": false` when the same reading was already stored. Errors: `400`
for a reading that does not fit the format, `413` when it is too large, `415`
when the content type is not JSON, and `429` when one address sends more than
120 readings an hour.

### `GET /v1/refinery/stations`

Each station's newest workload reading from the last seven days, with the
number of readings and quotes in the last 24 hours:

```json
{"generated": 1791710100, "days": 7, "stations": [
  {"station": "Checkmate", "capacity": 106, "workload": "moderate",
   "observed": 1791709800, "seen_24h": 5, "quotes_24h": 2}
]}
```

### `GET /v1/refinery/quotes?days=14&station=Checkmate`

Quotes from the last `days` days (1 to 60, 14 when not given), newest first,
optionally for one station, at most 5000.

### `GET /v1/refinery/export.csv`

Every reading stored, as CSV.

### `GET /v1/health`

`{"ok": true, "version": 1, "observations": N}`

## Running it

Python 3.12 or newer, nothing else:

    python app.py                      # port 8080, database /data/refinery.db
    python -m unittest test_app        # the tests, on a free port and a temporary database

The environment variables `BEDROCK_API_DB`, `BEDROCK_API_PORT`,
`BEDROCK_API_PREFIX`, `BEDROCK_API_STATIONS`, `BEDROCK_API_POSTS_PER_HOUR`
and `BEDROCK_API_POSTS_PER_HOUR_ALL` change the defaults.
`python app.py --backup FILE` writes a consistent copy of the database while
the service is running.

## Deploying it behind nginx with Docker Compose

1. Copy `app.py`, `stations.txt` and `Dockerfile` into a folder next to the
   compose file, for example `./bedrock-api`.
2. Add the service in `deploy/compose-service.yml` to the compose file,
   along with its volume, and run `docker compose up -d --build bedrock-api`.
3. Add the two parts of `deploy/nginx.conf` to the site's configuration. Then
   check it with `nginx -t` and reload.
4. Check it from outside:
   `https://the-forsaken.org/bedrock/api/v1/health`.
5. Add `deploy/backup.sh` to the daily backup.

The container runs as a user without privileges, with a read-only file
system apart from its data volume, and is reached only through nginx on the
stack's network.
