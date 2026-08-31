#!/usr/bin/env python3
"""Gate: src/help.json is in the shape the device's Help viewer actually reads.

This file shipped in 1.0.0 and 1.0.1 doing nothing at all. The loader's whole
test is:

    if (helpData.children) helpMap[id] = helpData.children;

It reads ONLY a top-level `children` array. The file used `pages`, so it
parsed, failed that one check, and was dropped — no warning, no log line, and
the viewer said "No help content available for this module." Reported by the
Schwung maintainer, who found twelve modules in the same state.

Three things are checked, because all three fail silently on the device:

  shape   a top-level `children` array; every node either a BRANCH
          (title + children) or a LEAF (title + lines), lines being an array
          of strings, one per display line — not a paragraph.

  width   lines are drawn, never wrapped and never truncated: print() walks
          the string a glyph at a time and anything past x=127 is dropped by
          set_pixel. The font is proportional ("." is 3px, "W" is 6px) so
          this is a budget, not a hard count. 20 characters is what Schwung's
          own modules hold to (6W6 tops out at 21, median 18).

  charset the bitmap font carries printable ASCII plus exactly the ten
          accented/symbol glyphs below. Anything else has no glyph AND no
          fallback: it renders as a 1px gap, so "amount ±12 semis" silently
          becomes "amount 12 semis".
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HELP = ROOT / "src/help.json"

MAX_LINE = 20
EXTRA_GLYPHS = set("ÄÖÜäöü€†‡°")

# Topics the device pages need covered. "Overview" is not a page; the rest are
# the levels the plugin publishes, so a renamed or dropped page shows up here.
REQUIRED_TOPICS = {"Main", "LF", "LMF", "HMF", "HF", "Preset"}
RETIRED_TOPICS = {"Bands", "Setup"}


def walk(node, path, errors, titles):
    title = node.get("title")
    if not isinstance(title, str) or not title:
        errors.append("%s: node has no title" % path)
        return
    titles.add(title)
    here = "%s/%s" % (path, title)

    has_children = "children" in node
    has_lines = "lines" in node
    if has_children and has_lines:
        errors.append("%s: node has both children and lines; it must be one or the other" % here)
    if not has_children and not has_lines:
        errors.append("%s: node has neither children nor lines" % here)

    if has_children:
        if not isinstance(node["children"], list):
            errors.append("%s: children is not an array" % here)
            return
        for c in node["children"]:
            walk(c, here, errors, titles)
        return

    lines = node["lines"]
    if not isinstance(lines, list):
        errors.append("%s: lines is not an array (it must be one string per display line)" % here)
        return
    for i, ln in enumerate(lines):
        if not isinstance(ln, str):
            errors.append("%s: lines[%d] is %s, not a string" % (here, i, type(ln).__name__))
            continue
        if len(ln) > MAX_LINE:
            errors.append("%s: lines[%d] is %d chars, over the ~%d budget — the tail is "
                          "dropped at x=127 with no ellipsis: %r"
                          % (here, i, len(ln), MAX_LINE, ln))
        for ch in ln:
            if ch in EXTRA_GLYPHS:
                continue
            if not (32 <= ord(ch) <= 126):
                errors.append("%s: lines[%d] has %r (U+%04X), which the bitmap font has no "
                              "glyph for — it renders as a 1px gap: %r"
                              % (here, i, ch, ord(ch), ln))


def main():
    if not HELP.exists():
        print("FAILED: %s does not exist" % HELP)
        return 1
    try:
        doc = json.loads(HELP.read_text())
    except ValueError as e:
        print("FAILED: help.json is not valid JSON: %s" % e)
        return 1

    errors = []
    if "children" not in doc:
        print("FAILED: help.json has no top-level \"children\" array.")
        print("  The loader's entire test is `if (helpData.children)`. Without it the")
        print("  file is parsed, dropped, and the viewer says there is no help —")
        print("  silently. Top-level keys present: %s" % sorted(doc.keys()))
        return 1
    if not isinstance(doc["children"], list) or not doc["children"]:
        print("FAILED: top-level children is not a non-empty array")
        return 1

    titles = set()
    for node in doc["children"]:
        walk(node, "", errors, titles)

    missing = sorted(REQUIRED_TOPICS - titles)
    for m in missing:
        errors.append("no topic covers the %s page" % m)
    for r in sorted(RETIRED_TOPICS & titles):
        errors.append("topic %r describes a page that no longer exists" % r)

    if errors:
        print("help.json FAILED (%d):" % len(errors))
        for e in errors:
            print("  - " + e)
        return 1

    leaves = sum(1 for n in doc["children"] if "lines" in n)
    lines = [l for n in doc["children"] for l in n.get("lines", []) if l]
    print("help.json ok: %d topics, %d lines, longest %d chars (budget %d), ASCII-safe"
          % (leaves, len(lines), max(len(l) for l in lines), MAX_LINE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
