#!/usr/bin/env bash
# make + daemon + package-morphos-cross.sh — default MorphOS delivery (see .cursor/rules).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/flexcat-env.sh
source "$ROOT/tools/flexcat-env.sh"
log() { printf '==> %s\n' "$*"; }

log "flexcat: $FLEXCAT"

MAKE_EXTRA=()
# Keep make EXECUTABLE_DIR and package AMIGAGPT_OUT_DIR in lockstep.
# A stale AMIGAGPT_OUT_DIR from a previous shell used to package an old /tmp
# binary while make linked into out/ (Cursor overlay) — About then showed the
# wrong build.
if [[ -n "${AMIGAGPT_OUT_DIR:-}" ]]; then
  mkdir -p "$AMIGAGPT_OUT_DIR"
  MAKE_EXTRA+=(EXECUTABLE_DIR="$AMIGAGPT_OUT_DIR")
  log "AMIGAGPT_OUT_DIR=$AMIGAGPT_OUT_DIR (EXECUTABLE_DIR synced)"
elif ! (touch "$ROOT/out/.write_test" 2>/dev/null && rm -f "$ROOT/out/.write_test"); then
  ALT_OUT="/tmp/amigagpt-morphos-out"
  mkdir -p "$ALT_OUT"
  log "out/ not writable (Cursor .cursorignore overlay) — using $ALT_OUT"
  export AMIGAGPT_OUT_DIR="$ALT_OUT"
  MAKE_EXTRA+=(EXECUTABLE_DIR="$ALT_OUT")
fi

if ! (test -r "$ROOT/catalogs/german/AmigaGPT.catalog" 2>/dev/null); then
  AMIGAGPT_CATALOGS_DIR="/tmp/amigagpt-catalogs"
  rm -rf "$AMIGAGPT_CATALOGS_DIR"
  mkdir -p "$AMIGAGPT_CATALOGS_DIR"
  git -C "$ROOT" archive HEAD catalogs | tar -x -C "$AMIGAGPT_CATALOGS_DIR"
  export AMIGAGPT_CATALOGS_DIR
  log "catalogs from git archive (Cursor .cursorignore overlay)"
fi

log "make ship (build + package + Z:)…"
make -C "$ROOT" -f Makefile.MorphOS ship "${MAKE_EXTRA[@]}" "$@"
