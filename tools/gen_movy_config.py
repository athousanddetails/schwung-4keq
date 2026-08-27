#!/usr/bin/env python3
"""Emit src/movy_config.json from src/dsp/fourk_params.h.

Movy renders a module's knob pages from this file, and its shape is NOT the
one this repo used to ship. The authoritative form (src/types/param.ts,
ModuleConfig) is:

    { "id", "name", "banks": [ { "name", "rows": [ [ KnobSlot|null x8 ] ] } ] }

with KnobSlot = { key, short, full, type, and options|min/max }.

What shipped in 1.0.0 was invented: a top-level "module" key instead of "id",
and "pages" holding bare parameter-name STRINGS instead of "banks" holding
rows of slot objects. Movy could not read a single control from it.

HARD RULE, and the reason banks are emitted one per page: a config bank is
exactly one page. buildConfigPages sets bankGroups one entry per BANK while
the UI indexes per PAGE, so a bank with two rows shifts every later page's
label. One single-row bank per page.

  --check   exit non-zero if the file on disk is not what this would write
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HDR = ROOT / "src/dsp/fourk_params.h"
OUT = ROOT / "src/movy_config.json"

SLOTS_PER_ROW = 8

# bank name -> the keys on that page, in knob order. Mirrors the knob arrays
# in src/dsp/fourk_eq_plugin.cpp; check_movy_pages() below fails if they drift.
BANKS = [
    ("Master", ["eq_type", "oversampling", "bypass", "auto_gain",
                "input_gain", "output_gain"]),
    ("LF",     ["lf_gain", "lf_freq", "lf_bell", "hpf_freq"]),
    ("LMF",    ["lm_gain", "lm_freq", "lm_q"]),
    ("HMF",    ["hm_gain", "hm_freq", "hm_q"]),
    ("HF",     ["hf_gain", "hf_freq", "hf_bell", "lpf_freq"]),
]

# Movy shows `full` where there is room, so it may name the band; `short` is
# the knob cell and comes from the C table, which is already page-scoped.
FULL = {
    "eq_type": "Type", "oversampling": "Oversample", "bypass": "Bypass",
    "auto_gain": "Auto Gain", "input_gain": "Input", "output_gain": "Output",
    "lf_gain": "LF Gain", "lf_freq": "LF Freq", "lf_bell": "LF Bell",
    "hpf_freq": "HPF", "lm_gain": "LMF Gain", "lm_freq": "LMF Freq",
    "lm_q": "LMF Q", "hm_gain": "HMF Gain", "hm_freq": "HMF Freq",
    "hm_q": "HMF Q", "hf_gain": "HF Gain", "hf_freq": "HF Freq",
    "hf_bell": "HF Bell", "lpf_freq": "LPF",
}

NUM = r"[-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)f?"


def parse_header():
    text = HDR.read_text()
    opts = {}
    for m in re.finditer(r"(fkq_opts_\w+)\[\d+\]\s*=\s*\{(.*?)\};", text, re.S):
        opts[m.group(1)] = re.findall(r'"([^"]*)"', m.group(2))
    body = re.search(r"fkq_params\[FKQ_PARAM_COUNT\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not body:
        sys.exit("FAIL: no fkq_params table in " + str(HDR))
    row = re.compile(
        r'\{\s*"([a-z_0-9]+)"\s*,\s*"([^"]*)"\s*,\s*FKQ_(\w+)\s*,\s*'
        r'(' + NUM + r')\s*,\s*(' + NUM + r')\s*,\s*(' + NUM + r')\s*,\s*'
        r'([A-Za-z_0-9]+)\s*,\s*(\d+)\s*,')
    out = {}
    for m in row.finditer(body.group(1)):
        key, name, ptype, lo, hi, dv, optname, nopt = m.groups()
        f = lambda s: float(s.rstrip("f"))
        slot = {"key": key, "short": name, "full": FULL.get(key, name)}
        if ptype == "ENUM":
            slot["type"] = "enum"
            slot["options"] = opts[optname][:int(nopt)]
        else:
            slot["type"] = "int" if ptype == "INT" else "float"
            slot["min"], slot["max"] = f(lo), f(hi)
        out[key] = slot
    return out


def build(slots):
    banks = []
    for name, keys in BANKS:
        missing = [k for k in keys if k not in slots]
        if missing:
            sys.exit("FAIL: banks reference unknown params: " + ", ".join(missing))
        row = [slots[k] for k in keys]
        row += [None] * (SLOTS_PER_ROW - len(row))   # null pads a slot
        banks.append({"name": name, "rows": [row]})  # exactly ONE row per bank
    return {"id": "4k-eq", "name": "4K EQ", "banks": banks}


def check_pages_match_plugin():
    """The banks here and the knob arrays in the shell must list the same keys.

    They are two hand-maintained copies of one layout, which is exactly how
    the last one rotted.
    """
    src = (ROOT / "src/dsp/fourk_eq_plugin.cpp").read_text()
    problems = []
    for arr, bank in [("fkq_knobs_master", "Master"), ("fkq_knobs_lf", "LF"),
                      ("fkq_knobs_lmf", "LMF"), ("fkq_knobs_hmf", "HMF"),
                      ("fkq_knobs_hf", "HF")]:
        m = re.search(arr + r"\[\d+\]\s*=\s*\{(.*?)\};", src, re.S)
        if not m:
            problems.append("no %s in the shell" % arr)
            continue
        got = re.findall(r'"([a-z_0-9]+)"', m.group(1))
        want = dict(BANKS)[bank]
        if got != want:
            problems.append("%s: shell has %s, movy bank has %s" % (bank, got, want))
    return problems


def main():
    problems = check_pages_match_plugin()
    if problems:
        print("FAILED: movy banks disagree with the plugin's knob pages:")
        for p in problems:
            print("  - " + p)
        return 1

    doc = build(parse_header())
    rendered = json.dumps(doc, indent=1) + "\n"

    for b in doc["banks"]:
        if len(b["rows"]) != 1:
            print("FAILED: bank %r has %d rows; a bank must be exactly one page"
                  % (b["name"], len(b["rows"])))
            return 1

    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text() != rendered:
            print("FAIL: src/movy_config.json is stale — regenerate with "
                  "tools/gen_movy_config.py")
            return 1
        print("movy_config ok: %d banks, one row each, matching the plugin"
              % len(doc["banks"]))
        return 0

    OUT.write_text(rendered)
    print("wrote %s: %d banks, one row each" % (OUT, len(doc["banks"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
