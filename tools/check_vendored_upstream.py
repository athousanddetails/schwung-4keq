#!/usr/bin/env python3
"""Gate: everything under src/ported/ must be byte-identical to upstream.

The whole claim of this repository is that the DSP is Dusk Audio's 4K EQ 2 and
not an approximation of it. That claim is only worth anything if something
checks it, so this does: clone dusk-audio-plugins, and compare every vendored
file with the upstream file it came from, byte for byte.

A difference is not necessarily wrong — upstream moves — but it must be a
decision someone made and can see, never a local edit that drifted in
unnoticed. There are no permitted exceptions: if a change is ever genuinely
needed, it belongs in the shell, not in src/ported/.

  --skip-clone   compare against an existing checkout (pass its path)
"""
import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
UPSTREAM_URL = "https://github.com/dusk-audio/dusk-audio-plugins"
# The revision src/ported/ was taken from. Compared against THIS, not against
# whatever main happens to be: upstream ships other plugins out of the same
# shared-daf directory, so tracking HEAD meant an unrelated Multi-Comp commit
# turned every build in this repo red. Bumping it is a deliberate act — take
# the files, prove the audio is unchanged, then move this.
REV_FILE = pathlib.Path(__file__).resolve().parent.parent / "src/ported/UPSTREAM_REV"

# vendored path (under src/ported/)  ->  upstream path (under the repo root)
VENDORED = {
    "shared-daf/dsp/FourKEQDSP.hpp":              "plugins/shared-daf/dsp/FourKEQDSP.hpp",
    "shared-daf/dsp/FourKEQDSP.cpp":              "plugins/shared-daf/dsp/FourKEQDSP.cpp",
    "shared-daf/dsp/FourKEQPairCorrection.inc":   "plugins/shared-daf/dsp/FourKEQPairCorrection.inc",
    "shared-daf/dsp/FourKEQFilterCalibration.inc":"plugins/shared-daf/dsp/FourKEQFilterCalibration.inc",
    "shared-daf/dsp/ConsoleSaturationCore.h":     "plugins/shared-daf/dsp/ConsoleSaturationCore.h",
    "shared-daf/dsp/DuskDenormals.hpp":           "plugins/shared-daf/dsp/DuskDenormals.hpp",
    "shared-daf/dsp/DuskSmoothed.hpp":            "plugins/shared-daf/dsp/DuskSmoothed.hpp",
    "shared-daf/dsp/DuskFilters.hpp":             "plugins/shared-daf/dsp/DuskFilters.hpp",
    "shared-daf/dsp/DuskOversampler.hpp":         "plugins/shared-daf/dsp/DuskOversampler.hpp",
    "shared-daf/dsp/DuskADAA.hpp":                "plugins/shared-daf/dsp/DuskADAA.hpp",
    "daf-plugin/FourKEQParams.hpp":               "plugins/4k-eq/daf-plugin/FourKEQParams.hpp",
    "daf-plugin/FourKEQPresetRuntime.hpp":        "plugins/4k-eq/daf-plugin/FourKEQPresetRuntime.hpp",
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ported = ROOT / "src/ported"

    # Every vendored file must be accounted for — an unlisted file under
    # src/ported/ is code claiming upstream provenance that nothing checks.
    on_disk = {
        str(p.relative_to(ported))
        for p in ported.rglob("*") if p.is_file()
    }
    on_disk.discard("UPSTREAM_REV")   # the marker itself, not vendored code
    unlisted = sorted(on_disk - set(VENDORED))
    if unlisted:
        print("FAILED: files under src/ported/ that this gate does not check:")
        for u in unlisted:
            print("  - " + u)
        return 1

    tmp = None
    if "--skip-clone" in sys.argv:
        up = pathlib.Path(sys.argv[sys.argv.index("--skip-clone") + 1])
    else:
        want = REV_FILE.read_text().strip() if REV_FILE.exists() else ""
        if not want:
            print("FAILED: src/ported/UPSTREAM_REV is missing or empty")
            return 1
        tmp = tempfile.mkdtemp(prefix="fkq-upstream-")
        up = pathlib.Path(tmp)
        r = subprocess.run(
            ["git", "clone", "-q", "--filter=blob:none", "--sparse",
             UPSTREAM_URL, str(up)],
            capture_output=True, text=True)
        if r.returncode == 0:
            subprocess.run(["git", "-C", str(up), "sparse-checkout", "set",
                            "plugins/shared-daf", "plugins/4k-eq"],
                           capture_output=True)
            co = subprocess.run(["git", "-C", str(up), "checkout", "-q", want],
                                capture_output=True, text=True)
            if co.returncode != 0:
                print("FAILED: upstream has no revision %s" % want)
                shutil.rmtree(tmp, ignore_errors=True)
                return 1
        if r.returncode != 0:
            print("  (skipped: upstream not reachable — %s)"
                  % (r.stderr.strip().splitlines() or ["?"])[-1])
            shutil.rmtree(tmp, ignore_errors=True)
            return 0

    rev = subprocess.run(["git", "-C", str(up), "rev-parse", "--short", "HEAD"],
                         capture_output=True, text=True).stdout.strip()

    bad, missing = [], []
    for ours_rel, theirs_rel in sorted(VENDORED.items()):
        ours, theirs = ported / ours_rel, up / theirs_rel
        if not ours.exists():
            missing.append("ours: " + ours_rel); continue
        if not theirs.exists():
            missing.append("upstream: " + theirs_rel); continue
        if sha(ours) != sha(theirs):
            bad.append(ours_rel)

    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)

    if missing:
        print("FAILED: files missing")
        for m in missing:
            print("  - " + m)
        return 1
    if bad:
        print("FAILED: vendored files differ from upstream @ %s" % rev)
        for b in bad:
            print("  - " + b)
        print("  src/ported/ is a verbatim copy. Put local changes in the shell.")
        return 1

    print("vendored DSP is byte-identical to upstream @ %s (%d files)"
          % (rev, len(VENDORED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
