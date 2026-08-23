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

Upstream and replica services use pinned `ghcr.io/platformfuzz/ringserver`
tags. A ringserver `v*` release dispatches this repo to open a bump PR so
Integration re-runs against the new serve image.

Upstream ringserver scans `lab/miniseed/`. The feeder writes to `rs0` and `rs1`.
`prove.sh` checks both replicas expose the same latest DataLink packet ID.

### GitHub App for ringserver pin PRs

**Bump ringserver** (daily cron, `workflow_dispatch`, or `repository_dispatch` type
`ringserver-released`) opens a compose pin PR when GHCR has a newer `x.y.z` tag.
Org policy does not let Actions create PRs with `GITHUB_TOKEN`, so the job uses the
same private GitHub App as `seiscomp-gui` / `seiscomp-base`.

1. Open the existing bump App under
   [platformfuzz GitHub Apps](https://github.com/organizations/platformfuzz/settings/apps)
   (see `seiscomp-gui` README for create steps if you still need a new App).
2. **Install / configure** → add **`ringserver-feeder`** (Contents + Pull requests:
   Read and write). Keep the App installed on `seiscomp-gui` as well.
3. In **this** repo: **Settings → Secrets and variables → Actions** (secrets, not
   variables):
   - `SEISCOMP_BUMP_APP_CLIENT_ID` = Client ID (not App ID)
   - `SEISCOMP_BUMP_APP_PRIVATE_KEY` = full PEM
4. Copy the same two secrets onto **`platformfuzz/ringserver`**. Its
   `notify-feeder.yml` mints an App token scoped to `ringserver-feeder` and sends
   `repository_dispatch` on `v*` tags. The App does not need to be installed on
   `ringserver`.

After both secrets exist here, **Actions → Bump ringserver → Run workflow** to
confirm `create-github-app-token` and `gh pr create` succeed.

## Build

```bash
docker build -t ringserver-feeder:test .
```
