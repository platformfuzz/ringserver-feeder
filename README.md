# ringserver-feeder

![CI](https://github.com/platformfuzz/ringserver-feeder/actions/workflows/ci.yml/badge.svg)
![Build and Release](https://github.com/platformfuzz/ringserver-feeder/actions/workflows/build-and-release.yml/badge.svg)

SeedLink client that writes the same DataLink packet ID to every ringserver replica.

Pulls upstream SeedLink and fans each miniSEED record out with DataLink 1.1
`WRITE` flag `I` so NLB clients can resume on any replica. Derived from EarthScope
slink2dali; uses libdali `dl_write_id()`.

**Package:** [ghcr.io/platformfuzz/ringserver-feeder](https://github.com/platformfuzz/ringserver-feeder/pkgs/container/ringserver-feeder)

## Run

```bash
docker pull ghcr.io/platformfuzz/ringserver-feeder:latest
docker run --rm \
  -e FEEDER_SEEDLINK_HOST=upstream:18000 \
  -e FEEDER_DATALINK_HOSTS=rs0:16000,rs1:16000 \
  -v feeder-data:/data \
  ghcr.io/platformfuzz/ringserver-feeder:latest
```

| Variable | Purpose |
| --- | --- |
| `FEEDER_SEEDLINK_HOST` | Upstream SeedLink `host:port` |
| `FEEDER_DATALINK_HOSTS` | Comma-separated DataLink targets |
| `FEEDER_STATE_FILE` | SeedLink resume state (default `/data/seedlink.state`) |
| `FEEDER_PKTID_FILE` | Last written packet ID (default `/data/pktid.state`) |
| `FEEDER_LOCK_FILE` | Single-writer lock (default `/data/feeder.lock`) |

slink2dali-style flags (`-s`, `-S`, `-l`, `-x`, `-nd`, `-nt`, `-k`) also work.

## Lab

```bash
cd lab
docker compose up --build -d
./prove.sh
docker compose down
```

Upstream ringserver scans `lab/miniseed/`. The feeder writes to `rs0` and `rs1`.
`prove.sh` checks both replicas expose the same latest DataLink packet ID.

## Build

```bash
docker build -t ringserver-feeder:test .
```
