#!/usr/bin/env python3
"""Gate: every parameter this module exposes must match 4K EQ 2's own table.

Parses kFourKParams from the vendored src/ported/daf-plugin/FourKEQParams.hpp
and fkq_params from src/dsp/fourk_params.h, and fails the build if a key is
renamed, invented, or given a range or default that upstream does not have.

Defaults may differ ONLY for keys listed in DEFAULT_DEPARTURES, each with the
reason recorded here — so a departure is a decision someone wrote down, not a
typo that survived review.

Keys upstream has that this module does not offer are reported but allowed:
that set is fixed in NOT_EXPOSED and must match exactly, so silently dropping
a real control fails too.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
UPSTREAM = ROOT / "src/ported/daf-plugin/FourKEQParams.hpp"
OURS = ROOT / "src/dsp/fourk_params.h"

# key -> (our default, reason). See the header comment in fourk_params.h.
DEFAULT_DEPARTURES = {
    "oversampling": (1.0, "2x rather than upstream's 4x; measured, see fourk_params.h"),
}

# Upstream keys deliberately without a knob, matching the desktop UI's own
# surface. Meters are output params; the rest are legacy or window state.
NOT_EXPOSED = {
    "saturation", "ms_mode", "spectrum_prepost", "show_graph",
    "out_peak_l", "out_peak_r",
}

# Matches C float literals in both tables, including upstream's trailing-dot
# form ("16.f") and plain ints ("15201"). Non-capturing inside, because the
# callers wrap it in their own group and index by position.
NUM = r"[-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)f?"


def parse_upstream():
    text = UPSTREAM.read_text()
    body = re.search(r"kFourKParams\[kParamCount\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not body:
        sys.exit("FAIL: could not find kFourKParams in " + str(UPSTREAM))
    out = {}
    for m in re.finditer(
            r'\{\s*"([a-z_0-9]+)"\s*,\s*(' + NUM + r')\s*,\s*(' + NUM + r')\s*,\s*(' + NUM + r')\s*\}',
            body.group(1)):
        key, lo, hi, dv = m.groups()
        out[key] = tuple(float(v.rstrip("f")) for v in (lo, hi, dv))
    return out


def parse_ours():
    text = OURS.read_text()
    body = re.search(r"fkq_params\[FKQ_PARAM_COUNT\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not body:
        sys.exit("FAIL: could not find fkq_params in " + str(OURS))
    out = {}
    for m in re.finditer(
            r'\{\s*"([a-z_0-9]+)"\s*,\s*"[^"]*"\s*,\s*FKQ_\w+\s*,\s*'
            r'(' + NUM + r')\s*,\s*(' + NUM + r')\s*,\s*(' + NUM + r')\s*,',
            body.group(1)):
        key, lo, hi, dv = m.groups()
        out[key] = tuple(float(v.rstrip("f")) for v in (lo, hi, dv))
    return out


def main():
    up, ours = parse_upstream(), parse_ours()
    if not up or not ours:
        sys.exit("FAIL: parsed an empty table (up=%d ours=%d)" % (len(up), len(ours)))

    errors = []

    invented = sorted(set(ours) - set(up))
    for k in invented:
        errors.append("invented key %r — not a 4K EQ 2 parameter" % k)

    missing = sorted(set(up) - set(ours) - NOT_EXPOSED)
    for k in missing:
        errors.append("upstream key %r is not exposed and is not in NOT_EXPOSED" % k)

    stale = sorted(NOT_EXPOSED - set(up))
    for k in stale:
        errors.append("NOT_EXPOSED lists %r, which upstream no longer has" % k)

    for k in sorted(set(ours) & set(up)):
        (ulo, uhi, udef), (olo, ohi, odef) = up[k], ours[k]
        if (olo, ohi) != (ulo, uhi):
            errors.append("%s range %g..%g != upstream %g..%g" % (k, olo, ohi, ulo, uhi))
        if odef != udef:
            want = DEFAULT_DEPARTURES.get(k)
            if want is None:
                errors.append("%s default %g != upstream %g (undeclared departure)" % (k, odef, udef))
            elif abs(want[0] - odef) > 1e-9:
                errors.append("%s default %g != the declared departure %g" % (k, odef, want[0]))

    for k, (val, _) in DEFAULT_DEPARTURES.items():
        if k in ours and k in up and ours[k][2] == up[k][2]:
            errors.append("%s is listed as a departure but now matches upstream — drop it" % k)

    if errors:
        print("param contract FAILED:")
        for e in errors:
            print("  - " + e)
        return 1

    print("param contract ok: %d exposed, all 1:1 with upstream" % len(ours))
    print("  not exposed (deliberate): " + ", ".join(sorted(NOT_EXPOSED)))
    for k, (val, why) in DEFAULT_DEPARTURES.items():
        print("  default departure: %s = %g — %s" % (k, val, why))
    return 0


if __name__ == "__main__":
    sys.exit(main())
