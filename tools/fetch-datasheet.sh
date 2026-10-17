#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Fetch the Conexant CX25840/1/2/3 production data sheet.
#
# 289 pages including the complete register map. This is the document that turns
# a captured register write into a meaning -- mainline cx25840-core.c writes many
# of these registers but names almost none of them, and guessing at bit positions
# from the driver alone had me stuck three times in a row here.
#
# Not committed: it is Conexant's document, and at 2.5 MB it does not belong in
# the history. docs/cx25840-registers.md carries the address/name index derived
# from it, which is enough to navigate.
set -euo pipefail

URL="https://pdf.datasheet.live/4d9b57e011f2eae5cc30f0d72b575c9e.pdf"
OUT="${1:-cx25840-datasheet.pdf}"

# Pinned: the 289-page Conexant document this mirror served on 2026-10-16,
# twice, identically. The mirror is flaky -- the same day it also served a
# 9.5 KB HTML page instead -- so a failure here is usually that, not a changed
# document. The download goes to a temporary file and replaces ${OUT} only if
# it matches.
SHA="838910e91f1e6e9d23e27df1321f6798b818aa13600f418dcd80b7f360e2c69e"
echo "fetching CX25840 data sheet -> ${OUT}"
TMP="$(mktemp "${OUT}.XXXXXX")"
trap 'rm -f "${TMP}"' EXIT
curl -sSfL --max-time 180 -o "${TMP}" "${URL}"

if ! head -c 5 "${TMP}" | grep -q '%PDF'; then
    echo "ERROR: that URL served HTML, not a PDF (it does at times; retry)." >&2
    echo "Search for: Conexant CX25840 'Production Data Sheet' 102284B" >&2
    exit 1
fi
got=$(sha256sum "${TMP}" | cut -d' ' -f1)
[ "${got}" = "${SHA}" ] || {
    echo "ERROR: sha256 mismatch: expected ${SHA}, got ${got}" >&2
    exit 1; }
mv "${TMP}" "${OUT}"
echo "ok: $(stat -c%s "${OUT}") bytes, sha256 verified"
command -v pdftotext >/dev/null && \
    echo "extract text with: pdftotext -layout ${OUT} cx25840.txt"
