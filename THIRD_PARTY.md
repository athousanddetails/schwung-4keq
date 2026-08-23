# Third-party components

## 4K EQ 2 — Dusk Audio

The entire DSP core is Dusk Audio's, vendored **unmodified** under
`src/ported/` from
[dusk-audio/dusk-audio-plugins](https://github.com/dusk-audio/dusk-audio-plugins),
GPL-3.0-or-later:

| File | Upstream path |
| --- | --- |
| `src/ported/shared-daf/dsp/FourKEQDSP.{hpp,cpp}` | `plugins/shared-daf/dsp/` |
| `src/ported/shared-daf/dsp/FourKEQPairCorrection.inc` | `plugins/shared-daf/dsp/` |
| `src/ported/shared-daf/dsp/FourKEQFilterCalibration.inc` | `plugins/shared-daf/dsp/` |
| `src/ported/shared-daf/dsp/ConsoleSaturationCore.h` | `plugins/shared-daf/dsp/` |
| `src/ported/shared-daf/dsp/Dusk{Denormals,Smoothed,Filters,Oversampler,ADAA}.hpp` | `plugins/shared-daf/dsp/` |
| `src/ported/daf-plugin/FourKEQParams.hpp` | `plugins/4k-eq/daf-plugin/` |
| `src/ported/daf-plugin/FourKEQPresetRuntime.hpp` | `plugins/4k-eq/daf-plugin/` |

Every band law, the Brown and Black calibration tables, the shared-stage pair
corrections, the console nonlinearity and the fourteen factory programs are
theirs. This repository adds only the Move-specific shell around them.

The parameter surface is kept 1:1 with upstream's own `kFourKParams` —
`tools/check_upstream_params.py` fails the build if a key, range or default
drifts, and records the single intentional departure.

4K EQ 2 is an independent emulation inspired by classic British console EQs.
It is not affiliated with or endorsed by any hardware or software manufacturer.

## Schwung

Host headers under `src/host/` (`plugin_api_v1.h`, `audio_fx_api_v2.h`) and the
module conventions come from
[charlesvestal/schwung](https://github.com/charlesvestal/schwung) by Charles
Vestal and contributors.

## Licence

This module is GPL-3.0-or-later, matching upstream. See `LICENSE`.
