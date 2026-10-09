#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Fetch the pinned Chromium source tree (chromium/VERSION), sync its
# dependencies, apply the crnet patches and link //crnet into it.
#
# Usage: scripts/fetch-chromium.sh [CHROMIUM_DIR]
#
# Environment:
#   CRNET_JOBS          parallel jobs for gclient (default: nproc, max 64)
#   CRNET_INSTALL_DEPS  set to 1 to run build/install-build-deps.sh (sudo)
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CR_DIR="${1:-${ROOT_DIR}/chromium}"
VERSION="$(tr -d '[:space:]' < "${ROOT_DIR}/chromium/VERSION")"
PATCH_DIR="${ROOT_DIR}/chromium/patches"
JOBS="${CRNET_JOBS:-$(nproc)}"
[ "${JOBS}" -gt 64 ] && JOBS=64

mkdir -p "${CR_DIR}"
CR_DIR="$(cd "${CR_DIR}" && pwd)"
SRC_DIR="${CR_DIR}/src"
DT_DIR="${CR_DIR}/depot_tools"

log() { printf '[fetch-chromium] %s\n' "$*"; }

# 1) depot_tools.
if [ ! -d "${DT_DIR}/.git" ]; then
	log "cloning depot_tools"
	git clone -q https://chromium.googlesource.com/chromium/tools/depot_tools.git "${DT_DIR}"
fi
export PATH="${DT_DIR}:${PATH}"
export DEPOT_TOOLS_UPDATE=0

# 2) chromium/src at the pinned tag (shallow).
if [ ! -d "${SRC_DIR}/.git" ]; then
	log "fetching chromium/src ${VERSION}"
	git init -q "${SRC_DIR}"
	git -C "${SRC_DIR}" remote add origin https://chromium.googlesource.com/chromium/src.git
fi
cur="$(git -C "${SRC_DIR}" describe --tags --exact-match HEAD 2>/dev/null || true)"
if [ "${cur}" != "${VERSION}" ]; then
	git -C "${SRC_DIR}" fetch -q --depth 1 origin "refs/tags/${VERSION}:refs/tags/${VERSION}"
	git -C "${SRC_DIR}" checkout -q -f "refs/tags/${VERSION}"
fi
SRC_SHA="$(git -C "${SRC_DIR}" rev-parse HEAD)"

# 3) .gclient + dependency sync.
cat > "${CR_DIR}/.gclient" <<EOF
solutions = [{
  "name": "src",
  "url": "https://chromium.googlesource.com/chromium/src.git",
  "managed": False,
  "custom_deps": {},
  "custom_vars": {
    "checkout_nacl": False,
    "checkout_pgo_profiles": False,
  },
}]
target_os = ["linux"]
EOF

# Bootstrap gsutil once: parallel GCS deps otherwise race on its lock.
python3 "${DT_DIR}/gsutil.py" version > /dev/null

log "gclient sync (${JOBS} jobs)"
(cd "${CR_DIR}" && gclient sync --nohooks --no-history -D -j "${JOBS}" \
	--revision "src@${SRC_SHA}")

if [ "${CRNET_INSTALL_DEPS:-0}" = "1" ]; then
	log "installing build dependencies"
	sudo "${SRC_DIR}/build/install-build-deps.sh" --no-prompt --no-arm \
		--no-chromeos-fonts --no-nacl
fi

log "gclient runhooks"
(cd "${CR_DIR}" && gclient runhooks -j "${JOBS}")

# 4) Patches. Reset the touched files first so re-running is idempotent.
"${ROOT_DIR}/scripts/apply-patches.sh" "${SRC_DIR}"

# 5) Expose the crnet GN target as //crnet.
ln -sfn "${ROOT_DIR}/src" "${SRC_DIR}/crnet"

log "done: ${SRC_DIR} @ ${VERSION} (${SRC_SHA})"
