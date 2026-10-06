#!/usr/bin/env bash
# Jelly5 — show the console's log live (the app sends it as UDP to this machine).
#   scripts/log.sh            listen on 5555, also append to build/console.log
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${JELLY5_LOG_PORT:-5555}"
mkdir -p "${ROOT}/build"
echo "listening for the Jelly5 log on udp/${PORT} (Ctrl-C to stop)"
exec python3 -u - "${PORT}" "${ROOT}/build/console.log" <<'PY'
import socket, sys
port, path = int(sys.argv[1]), sys.argv[2]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
with open(path, "a", buffering=1) as f:
    while True:
        data, _ = s.recvfrom(65536)
        text = data.decode("utf-8", "replace")
        sys.stdout.write(text)
        f.write(text)
PY
