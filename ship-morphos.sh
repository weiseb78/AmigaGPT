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

# Cursor overlays *.catalog with empty/unreadable tmpfs; binaries are gitignored.
# Rebuild into /tmp so package-morphos-cross.sh can ship real FlexCat catalogs.
need_catalog_rebuild=0
if ! (test -r "$ROOT/catalogs/german/AmigaGPT.catalog" 2>/dev/null); then
  need_catalog_rebuild=1
elif [[ ! -s "$ROOT/catalogs/german/AmigaGPT.catalog" ]]; then
  need_catalog_rebuild=1
fi
if [[ "$need_catalog_rebuild" -eq 1 ]]; then
  AMIGAGPT_CATALOGS_DIR="/tmp/amigagpt-catalogs"
  rm -rf "$AMIGAGPT_CATALOGS_DIR"
  mkdir -p "$AMIGAGPT_CATALOGS_DIR"
  if [[ ! -x "${FLEXCAT:-}" ]]; then
    log "flexcat missing — cannot rebuild catalogs for overlay"
    exit 1
  fi
  log "rebuilding catalogs via flexcat → $AMIGAGPT_CATALOGS_DIR (Cursor overlay)"
  shopt -s nullglob
  for po in "$ROOT"/catalogs/*/*.po; do
    lang=$(basename "$(dirname "$po")")
    mkdir -p "$AMIGAGPT_CATALOGS_DIR/$lang"
    bash "$ROOT/tools/flexcat-po-prepare.sh" "$po" >/dev/null
    # FlexCat warns on missing version/revision in some .po files but still writes.
    "$FLEXCAT" POFILE "$po" CATALOG "$AMIGAGPT_CATALOGS_DIR/$lang/AmigaGPT.catalog" >/dev/null || true
    if [[ ! -s "$AMIGAGPT_CATALOGS_DIR/$lang/AmigaGPT.catalog" ]]; then
      log "flexcat produced empty catalog for $lang"
      exit 1
    fi
  done
  shopt -u nullglob
  export AMIGAGPT_CATALOGS_DIR
fi

log "make ship (build + package + Z:)…"
make -C "$ROOT" -f Makefile.MorphOS ship "${MAKE_EXTRA[@]}" "$@"
