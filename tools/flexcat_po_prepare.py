#!/usr/bin/env python3
"""Prepare one .po file for FlexCat (no gettext fuzzy semantics).

FlexCat ships msgstr as-is and ignores "#, fuzzy". This script:
  - german/deutsch.po: HARD FAIL if any fuzzy flag OR empty msgstr is present
    (no strip, no write). A human must fix before FlexCat runs.
  - other .po: fuzzy entry → msgstr = msgid; strip fuzzy flags; empty msgstr → msgid
"""
from __future__ import annotations

import re
import sys
from pathlib import Path


ENTRY_RE = re.compile(
    r"(?P<head>(?:^#.*\n|^msgctxt .*\n)*)"
    r"msgid (?P<msgid>(?:\"(?:\\.|[^\"\\])*\"(?:\n\"(?:\\.|[^\"\\])*\")*))\n"
    r"msgstr (?P<msgstr>(?:\"(?:\\.|[^\"\\])*\"(?:\n\"(?:\\.|[^\"\\])*\")*))\n",
    re.MULTILINE,
)

FUZZY_FLAG_RE = re.compile(r"^#,.*\bfuzzy\b", re.MULTILINE)


def _is_empty_msgstr(msgstr_body: str) -> bool:
    parts = re.findall(r'"(?:\\.|[^"\\])*"', msgstr_body)
    if not parts:
        return True
    return all(p == '""' for p in parts)


def _msgid_is_header(msgid_body: str) -> bool:
    parts = re.findall(r'"(?:\\.|[^"\\])*"', msgid_body)
    return parts == ['""']


def _head_has_fuzzy(head: str) -> bool:
    return bool(FUZZY_FLAG_RE.search(head))


def _strip_fuzzy_from_head(head: str) -> str:
    out: list[str] = []
    for line in head.splitlines(keepends=True):
        if line.startswith("#,") and re.search(r"\bfuzzy\b", line):
            flags = [f.strip() for f in line[2:].strip().split(",")]
            flags = [f for f in flags if f and f != "fuzzy"]
            if flags:
                out.append("#, " + ", ".join(flags) + "\n")
            continue
        out.append(line)
    return "".join(out)


def _msgctxt_of(head: str) -> str:
    ctx = re.search(r'^msgctxt "([^"]*)"', head, re.MULTILINE)
    return ctx.group(1) if ctx else "(no msgctxt)"


def _fuzzy_msgctxts(text: str) -> list[str]:
    """Return msgctxt ids for entries that still carry a fuzzy flag."""
    found: list[str] = []
    for m in ENTRY_RE.finditer(text):
        if not _head_has_fuzzy(m.group("head")):
            continue
        found.append(_msgctxt_of(m.group("head")))
    if not found and FUZZY_FLAG_RE.search(text):
        found.append("(fuzzy flag outside entry match)")
    return found


def _empty_msgstr_msgctxts(text: str) -> list[str]:
    """Return msgctxt ids for non-header entries with empty msgstr."""
    found: list[str] = []
    for m in ENTRY_RE.finditer(text):
        if _msgid_is_header(m.group("msgid")):
            continue
        if _is_empty_msgstr(m.group("msgstr")):
            found.append(_msgctxt_of(m.group("head")))
    return found


def process_non_german(text: str) -> tuple[str, dict[str, int]]:
    stats = {"fuzzy_reset": 0, "empty_filled": 0, "fuzzy_stripped": 0}

    def repl(m: re.Match[str]) -> str:
        head = m.group("head")
        msgid = m.group("msgid")
        msgstr = m.group("msgstr")
        fuzzy = _head_has_fuzzy(head)

        if fuzzy:
            stats["fuzzy_stripped"] += 1
            if not _msgid_is_header(msgid):
                msgstr = msgid
                stats["fuzzy_reset"] += 1
            head = _strip_fuzzy_from_head(head)

        if not _msgid_is_header(msgid) and _is_empty_msgstr(msgstr):
            msgstr = msgid
            stats["empty_filled"] += 1

        return f"{head}msgid {msgid}\nmsgstr {msgstr}\n"

    new = ENTRY_RE.sub(repl, text)

    cleaned: list[str] = []
    for line in new.splitlines(keepends=True):
        if line.startswith("#,") and re.search(r"\bfuzzy\b", line):
            flags = [f.strip() for f in line[2:].strip().split(",")]
            flags = [f for f in flags if f and f != "fuzzy"]
            stats["fuzzy_stripped"] += 1
            if flags:
                cleaned.append("#, " + ", ".join(flags) + "\n")
            continue
        cleaned.append(line)
    return "".join(cleaned), stats


def _print_list(title: str, items: list[str]) -> None:
    print(title, file=sys.stderr)
    for ctx in items[:20]:
        print(f"  - {ctx}", file=sys.stderr)
    if len(items) > 20:
        print(f"  ... and {len(items) - 20} more", file=sys.stderr)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <file.po>", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")
    is_german = path.name == "deutsch.po" and "german" in path.parts

    if is_german:
        fail = 0
        fuzzies = _fuzzy_msgctxts(text)
        if fuzzies:
            print(
                f"ERROR: {path} has gettext fuzzy entries -- refusing to modify or compile.\n"
                "FlexCat ignores '#, fuzzy' and would ship those msgstr values into "
                "AmigaGPT.catalog.\n"
                "Fix each msgstr (or set msgstr = msgid), remove the fuzzy flag, then rebuild.",
                file=sys.stderr,
            )
            _print_list("Fuzzy entries:", fuzzies)
            fail = 1
        empties = _empty_msgstr_msgctxts(text)
        if empties:
            print(
                f"ERROR: {path} has empty msgstr entries -- refusing to modify or compile.\n"
                "FlexCat would ship blank UI text into AmigaGPT.catalog.\n"
                "Translate each entry (or temporarily set msgstr = msgid), then rebuild.",
                file=sys.stderr,
            )
            _print_list("Empty msgstr entries:", empties)
            fail = 1
        return fail

    text, stats = process_non_german(text)
    path.write_text(text, encoding="utf-8")

    bits = []
    if stats["fuzzy_reset"]:
        bits.append(f"reset {stats['fuzzy_reset']} fuzzy->msgid")
    if stats["empty_filled"]:
        bits.append(f"filled {stats['empty_filled']} empty<-msgid")
    if stats["fuzzy_stripped"] and not bits:
        bits.append(f"stripped {stats['fuzzy_stripped']} fuzzy flag(s)")
    if bits:
        print(f"{path}: " + ", ".join(bits))
    return 0


if __name__ == "__main__":
    sys.exit(main())
