#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# Stop feeder so DataLink read probes are not blocked by open write connections.
docker compose stop feeder >/dev/null 2>&1 || true

python3 - <<'PY'
import subprocess
import sys

try:
    from datalink_client import DataLink
except ImportError:
    subprocess.check_call([sys.executable, "-m", "pip", "install", "-q", "datalink-client"])
    from datalink_client import DataLink

compose_dir = "."
ids: dict[str, int] = {}

for service in ("rs0", "rs1"):
    cid = subprocess.check_output(
        ["docker", "compose", "ps", "-q", service],
        cwd=compose_dir,
        text=True,
    ).strip()
    if not cid:
        print(f"FAIL: {service} container not running")
        sys.exit(1)

    ip = subprocess.check_output(
        ["docker", "inspect", "-f", "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", cid],
        text=True,
    ).strip()

    with DataLink(ip, 16000) as dl:
        pktid = dl.set_position_latest()
        ids[service] = int(pktid)
        print(f"{service} latest pktid: {pktid}")

id0, id1 = ids["rs0"], ids["rs1"]
if id0 > 0 and id1 > 0 and id0 == id1:
    print("PASS: replicas share packet IDs")
    sys.exit(0)

print("FAIL: packet IDs differ or ring is empty")
sys.exit(1)
PY
