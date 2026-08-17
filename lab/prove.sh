#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
NETWORK="$(docker compose ps -q feeder | xargs docker inspect -f '{{range $k, $v := .NetworkSettings.Networks}}{{$k}}{{end}}' | head -1)"

read_pktid() {
  local host="$1"
  docker run --rm --network "$NETWORK" python:3.12-slim python3 - <<PY
import socket

host = "${host}"
port = 16000
s = socket.create_connection((host, port), timeout=5)
s.sendall(b"ID ringserver-feeder-probe:probe:1:linux\\r")
buf = b""
while b"\\r" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        break
    buf += chunk
s.sendall(b"POSITION SET LATEST\\r")
buf = b""
while b"\\r" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        break
    buf += chunk
line = buf.decode("ascii", errors="replace").split("\\r")[0]
parts = line.split()
if len(parts) >= 2 and parts[0] in ("OK", "ERROR"):
    print(parts[1])
else:
    print("0")
PY
}

id0="$(read_pktid rs0)"
id1="$(read_pktid rs1)"

echo "rs0 latest pktid: $id0"
echo "rs1 latest pktid: $id1"

if [[ -n "$id0" && -n "$id1" && "$id0" == "$id1" && "$id0" != "0" ]]; then
  echo "PASS: replicas share packet IDs"
  exit 0
fi

echo "FAIL: packet IDs differ or ring is empty"
exit 1
