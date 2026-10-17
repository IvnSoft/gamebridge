#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Fetch the Adaptec GameBridge FX2 firmware images and driver INFs.
#
# These files are Adaptec copyright and are NOT redistributed with this project
# (see NOTICE). They are fetched from a public archive of the original driver
# CD, for the purpose of driving hardware you own.
#
#   makoaudB.rom  14264 bytes  -- 8051 image, FX2 rev B silicon
#   makoaudC.rom  16382 bytes  -- 8051 image, FX2LP (rev C), 0xFF-padded to 16 KB
#   avcgbfl.in_   loader INF   -- the file that identifies 03f3:009b as "Loader"
#   avcgbdr.in_   capture INF  -- binds 03f3:009c
#
set -euo pipefail

# PINNED, to a commit and to a hash per file. avcgbfl.sys is where the 8051
# firmware is extracted from, and that code ends up running on the device,
# uploaded by root or by the kernel -- so a moving HEAD and a first-byte check
# were not enough (found by the 2026-10-16 security review). The archive's last
# commit is from 2013; these hashes were taken from it and match the copies
# this project was developed against. Files already on disk are checked too.
SRC_COMMIT="f8f008f8ec1f0ad315fe052af7a68135e9bd95e5"
SRC="https://raw.githubusercontent.com/Dakkers/GameBridge/${SRC_COMMIT}/Driver"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/firmware"

declare -A SHA=(
	[makoaudB.rom]=cfd4a8ad9e814c65070b8d45ba56e576391df1d2945ee4ea000d0f81cd8df278
	[makoaudC.rom]=a4dd01c9a09dc17b72f2d3845ed52c2f145b9eda0b564929d5f6b751f898e7a6
	[avcgbfl.in_]=2cf94d6ff7b000e7b4c8249bb9492b0ae9a26f77b10b8a9431ba8c964d425b30
	[avcgbdr.in_]=e12031799b82d49cdf1c22824b530cc682e3361711a3cc66eb9aeba67f4db6bf
	[avcgbfl.sys]=de77c18126b1173a61cf461dd1412a85f04a0efa330c9fa35d6181e2883fd015
	[avcgbdr.sys]=81c2a9faf928320c230c87f572579dc1466016e6da2b4118b4a9bbce00a3e33b
)

mkdir -p "${DIR}"
for f in makoaudB.rom makoaudC.rom avcgbfl.in_ avcgbdr.in_ avcgbfl.sys avcgbdr.sys; do
	if [ -s "${DIR}/${f}" ]; then
		echo "have ${f} ($(stat -c%s "${DIR}/${f}") bytes)"
	else
		echo "fetching ${f}"
		curl -sSfL -o "${DIR}/${f}.part" "${SRC}/${f}"
		mv "${DIR}/${f}.part" "${DIR}/${f}"
	fi
	got=$(sha256sum "${DIR}/${f}" | cut -d' ' -f1)
	[ "${got}" = "${SHA[${f}]}" ] || {
		echo "ERROR: ${f} sha256 mismatch" >&2
		echo "  expected ${SHA[${f}]}" >&2
		echo "  got      ${got}" >&2
		echo "  delete it and run this again; if it persists, do not use it" >&2
		exit 1; }
done

# The XC2028/XC3028 tuner firmware, for the tuner work that is still pending.
# Not from Adaptec and not on the device: it is the Xceive image every xc2028
# board needs, and mainline's xc2028.ko loads it by this exact filename.
XC_SRC="https://raw.githubusercontent.com/armbian/firmware/master/xc3028-v27.fw"
XC_SHA="0d4cfd7974a19a94f4d42eb2d160d5a3e71cca1cfa61bd2cc063cf3f36ffd32e"
if [ -s "${DIR}/xc3028-v27.fw" ]; then
	echo "have xc3028-v27.fw ($(stat -c%s "${DIR}/xc3028-v27.fw") bytes)"
else
	echo "fetching xc3028-v27.fw"
	curl -sSfL -o "${DIR}/xc3028-v27.fw" "${XC_SRC}"
fi
got=$(sha256sum "${DIR}/xc3028-v27.fw" | cut -d' ' -f1)
[ "${got}" = "${XC_SHA}" ] || {
	echo "ERROR: xc3028-v27.fw sha256 mismatch" >&2
	echo "  expected ${XC_SHA}" >&2
	echo "  got      ${got}" >&2
	exit 1; }
head -c 15 "${DIR}/xc3028-v27.fw" | grep -q "xc2028 firmware" || {
	echo "ERROR: xc3028-v27.fw lacks the 'xc2028 firmware' header" >&2; exit 1; }

echo "firmware ready in ${DIR}"
