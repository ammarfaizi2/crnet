#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Usage: run_c_smoke.sh PYTHON SERVER_PY SMOKE_BIN
set -euo pipefail
coproc SRV { exec "$1" -I "$2"; }
trap 'kill ${SRV_PID} 2>/dev/null || true' EXIT
read -r line <&"${SRV[0]}"
port=$(printf '%s' "$line" | "$1" -c 'import json,sys; print(json.load(sys.stdin)["https"])')
"$3" "https://localhost:${port}/echo"
