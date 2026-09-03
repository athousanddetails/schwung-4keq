#!/usr/bin/env python3
"""Emit src/module.json's chain_params from src/dsp/fourk_params.h.

module.json is what the HOST reads. The plugin also serves chain_params from
get_param, and a module whose two copies disagree fails in the worst way:
the plugin behaves, the host does not offer the control. Generating one from
the other removes the chance.

  --check   exit non-zero if the file on disk is not what this would write
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HDR = ROOT / "src/dsp/fourk_params.h"
OUT = ROOT / "src/module.json"

NUM = r"[-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)f?"


def parse_options(text):
    """name -> [option, ...] for each fkq_opts_* array."""
    out = {}
    for m in re.finditer(r"(fkq_opts_\w+)\[\d+\]\s*=\s*\{(.*?)\};", text, re.S):
        out[m.group(1)] = re.findall(r'"([^"]*)"', m.group(2))
    return out


def parse_params(text, opts, switches, noviz):
    body = re.search(r"fkq_params\[FKQ_PARAM_COUNT\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not body:
        sys.exit("FAIL: no fkq_params table in " + str(HDR))
    entries = []
    row = re.compile(
        r'\{\s*"([a-z_0-9]+)"\s*,\s*"([^"]*)"\s*,\s*FKQ_(\w+)\s*,\s*'
        r'(' + NUM + r')\s*,\s*(' + NUM + r')\s*,\s*(' + NUM + r')\s*,\s*'
        r'([A-Za-z_0-9]+)\s*,\s*(\d+)\s*,\s*'
        r'(0|"[^"]*")\s*,\s*(0|"[^"]*")\s*\}')
    for m in row.finditer(body.group(1)):
        key, name, ptype, lo, hi, dv, optname, nopt, unit, fmt = m.groups()
        f = lambda s: float(s.rstrip("f"))
        e = {"key": key, "name": name}
        if ptype == "ENUM":
            e["type"] = "enum"
            e["options"] = opts[optname][:int(nopt)]
            e["default"] = e["options"][int(f(dv))]
        else:
            e["type"] = "int" if ptype == "INT" else "float"
            e["min"], e["max"] = f(lo), f(hi)
            e["default"] = f(dv)
        if unit != "0":
            e["unit"] = unit.strip('"')
        if fmt != "0":
            e["display_format"] = fmt.strip('"')
        if key in switches:
            e["viz"] = {"kind": "switch"}
        elif key in noviz:
            e["viz"] = False
        entries.append(e)
    return entries


def parse_noviz(text):
    body = re.search(r"fkq_no_viz\[\]\s*=\s*\{(.*?)\};", text, re.S)
    return set(re.findall(r'"([a-z_0-9]+)"', body.group(1))) if body else set()


def parse_switches(text):
    body = re.search(r"fkq_switch_viz\[\]\s*=\s*\{(.*?)\};", text, re.S)
    return set(re.findall(r'"([a-z_0-9]+)"', body.group(1))) if body else set()


def parse_readouts(text):
    """The fkq_readouts table — read-only keys the remote panel needs.

    Parsed from the same header the plugin compiles, so module.json and the
    plugin's own chain_params cannot list a different set.
    """
    body = re.search(r"fkq_readouts\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not body:
        sys.exit("FAIL: no fkq_readouts table in " + str(HDR))
    out = []
    for m in re.finditer(r'\{\s*"([a-z_0-9]+)"\s*,\s*"(\w+)"\s*\}', body.group(1)):
        key, ptype = m.groups()
        out.append({"key": key, "name": key, "type": ptype, "access": "read"})
    return out


def build(entries):
    return {
        "id": "4k-eq",
        "name": "4K EQ",
        "abbrev": "4KEQ",
        "version": "1.0.3",
        "description": (
            "British console EQ: four calibrated bands with Brown and Black "
            "voicings, stepped high- and low-pass filters, shared-stage band "
            "interaction and native console nonlinearity."),
        "author": "4K EQ 2 by Dusk Audio (GPL-3.0); port: athousanddetails",
        "dsp": "4k-eq.so",
        "api_version": 2,
        "component_type": "audio_fx",
        "capabilities": {
            "audio_out": True,
            "audio_in": True,
            "midi_in": False,
            "midi_out": False,
            "chainable": True,
            "component_type": "audio_fx",
            "chain_params": entries,
        },
    }


def main():
    text = HDR.read_text()
    doc = build(parse_params(text, parse_options(text), parse_switches(text),
                            parse_noviz(text))
                + parse_readouts(text))
    rendered = json.dumps(doc, indent=1) + "\n"

    size = len(rendered.encode())
    if size > 8192:
        print("FAIL: module.json is %d bytes, over the 8 KB cap" % size)
        return 1

    if "--check" in sys.argv:
        if not OUT.exists():
            print("FAIL: %s does not exist; run without --check" % OUT)
            return 1
        if OUT.read_text() != rendered:
            print("FAIL: src/module.json is stale — regenerate with "
                  "tools/gen_module_json.py")
            return 1
        print("module.json ok: %d params, %d bytes (cap 8192)"
              % (len(doc["capabilities"]["chain_params"]), size))
        return 0

    OUT.write_text(rendered)
    print("wrote %s: %d params, %d bytes (cap 8192)"
          % (OUT, len(doc["capabilities"]["chain_params"]), size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
