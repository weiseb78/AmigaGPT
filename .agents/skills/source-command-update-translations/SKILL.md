---
name: "source-command-update-translations"
description: "Migrated source command `update-translations`"
---

# source-command-update-translations

Use this skill when the user asks to run the migrated source command `update-translations`.

## Command Template

Use the `po-translation-updater` subagent to update gettext catalogs for this repository.

**Fork policy (weiseb78/AmigaGPT):** Only `catalogs/german/deutsch.po` is actively translated. For every other `catalogs/*/*.po`, keep `msgstr` = English `msgid` (UI fallback). Do not machine-translate other languages. FlexCat prepare hard-fails on fuzzy or empty `msgstr` in German (`tools/flexcat-po-prepare.sh`).

Requirements:

1. Sync every `catalogs/*/*.po` file from `catalogs/AmigaGPT.pot` (msgmerge / prepare).
2. Translate only new, empty, or fuzzy entries in **`catalogs/german/deutsch.po`**, preserving existing human translations, placeholders, `msgctxt`, formatting, and wrapping.
3. For all other languages: fill empty/new entries with `msgstr` = `msgid`; do not invent translations.
4. Validate on this fork with MorphOS ship (`bash ship-morphos.sh` or `make -f Makefile.MorphOS ship`), not `./build_os3.sh` (that is the upstream AmigaOS 3 path).
5. If the build fails because a localized `STRING_*` symbol is referenced in code but missing from `catalogs/AmigaGPT.pot`, inspect the source context, infer the best English string, add the missing `.pot` entry, update `.po` files per the fork policy above, and retry.
6. Prefer running the subagent in the background unless the user explicitly asks to watch it live.

Return the subagent's final summary, including updated languages, new catalog entries, build status, and anything needing human review.
