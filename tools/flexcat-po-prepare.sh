#!/usr/bin/env bash
# Prepare *.po for FlexCat after msgmerge.
#
# FlexCat compiles msgstr into AmigaGPT.catalog and does NOT honour gettext
# "#, fuzzy" markers. Guessed fuzzy msgstr must never reach FlexCat.
#
# - catalogs/german/deutsch.po: HARD FAIL if any fuzzy remains (no auto-strip).
# - other languages: fuzzy/empty → msgstr = msgid (English UI fallback).
#
# Usage: tools/flexcat-po-prepare.sh <po-file> ...
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="$ROOT/tools/flexcat_po_prepare.py"

fail=0
for po in "$@"; do
	[ -f "$po" ] || continue
	python3 "$PY" "$po" || fail=1
done
exit "$fail"
