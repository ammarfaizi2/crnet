#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
#
# (Re)apply chromium/patches/*.patch onto a Chromium src tree. Files
# touched by the patches are restored to HEAD first, so this is
# idempotent.
#
# Usage: scripts/apply-patches.sh CHROMIUM_SRC_DIR
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${1:?usage: $0 CHROMIUM_SRC_DIR}"
PATCH_DIR="${ROOT_DIR}/chromium/patches"

shopt -s nullglob
patches=("${PATCH_DIR}"/*.patch)
[ "${#patches[@]}" -eq 0 ] && exit 0

for p in "${patches[@]}"; do
	git -C "${SRC_DIR}" apply --numstat "${p}" | awk '{print $3}' |
		while read -r f; do
			git -C "${SRC_DIR}" checkout -q HEAD -- "${f}" 2>/dev/null || true
		done
done

for p in "${patches[@]}"; do
	printf '[apply-patches] %s\n' "$(basename "${p}")"
	git -C "${SRC_DIR}" apply "${p}"
done
