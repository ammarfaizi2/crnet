#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Check that a Chromium src tree carries chromium/patches/*.patch: every
# file the patches touch must equal its committed version with all the
# patches applied in order. Patches may build on each other, so checking
# them one by one (git apply --reverse --check) is not enough.
#
# Usage: scripts/check-patches.sh CHROMIUM_SRC_DIR WORK_DIR
# Exits 0 when the tree is patched, 1 (naming the patches) when not.
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${1:?usage: $0 CHROMIUM_SRC_DIR WORK_DIR}"
WORK_DIR="${2:?usage: $0 CHROMIUM_SRC_DIR WORK_DIR}"
PATCH_DIR="${ROOT_DIR}/chromium/patches"

shopt -s nullglob
patches=("${PATCH_DIR}"/*.patch)
[ "${#patches[@]}" -eq 0 ] && exit 0

rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"
WORK_DIR="$(cd "${WORK_DIR}" && pwd)"
mapfile -t files < <(for p in "${patches[@]}"; do
	git -C "${SRC_DIR}" apply --numstat "${p}" | awk '{print $3}'
done | sort -u)

for f in "${files[@]}"; do
	mkdir -p "${WORK_DIR}/$(dirname "${f}")"
	git -C "${SRC_DIR}" show "HEAD:${f}" > "${WORK_DIR}/${f}"
done
# Outside any repository (the work directory may sit inside one, where
# git apply would only touch paths below the current directory).
export GIT_CEILING_DIRECTORIES="$(dirname "${WORK_DIR}")"
for p in "${patches[@]}"; do
	if ! (cd "${WORK_DIR}" && git apply "${p}"); then
		echo "check-patches: $(basename "${p}") does not apply to the committed Chromium sources" >&2
		exit 1
	fi
done
for f in "${files[@]}"; do
	if ! cmp -s "${WORK_DIR}/${f}" "${SRC_DIR}/${f}"; then
		echo "check-patches: ${f} differs from the patched version" >&2
		exit 1
	fi
done
