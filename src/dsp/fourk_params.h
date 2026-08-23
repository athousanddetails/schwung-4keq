/* 4K EQ for Ableton Move (Schwung audio_fx module).
 *
 * The Move-facing parameter surface. Single source of truth: the DSP shell,
 * the chain_params JSON served via get_param, the state blob and ui_hierarchy
 * all iterate this table. movy_config.json mirrors it by hand —
 * tools/check_config.py fails the build if they drift.
 *
 * ONE-TO-ONE WITH UPSTREAM. Every key, range and default below is copied from
 * kFourKParams in src/ported/daf-plugin/FourKEQParams.hpp, which is 4K EQ 2's
 * own parameter table. Nothing here is renamed, collapsed, re-scaled or
 * invented: a value set on this module means exactly what the same key means
 * in the desktop plugin, which is what lets the port be checked against it.
 *
 * That is why HPF and LPF each keep TWO parameters — frequency and enable —
 * rather than folding the enable into the bottom of the frequency knob. The
 * fold looks tidier and is wrong: `*_freq` carries a dial-marking CONTROL
 * coordinate, not audible Hz, and the two are not the same number (Brown HPF
 * control 16 -> 10.16 Hz audible; 16 Hz audible -> control 38.39). Upstream
 * decides the enables from AUDIBLE Hz in FourKEQPresetRuntime.hpp, so deriving
 * an enable from the control value switches the filter on for presets that
 * want it out — seven of the fourteen factory presets.
 *
 * The five upstream keys deliberately NOT given a knob, matching what the
 * desktop UI itself offers:
 *   saturation        hidden legacy slot; 4K EQ 2 has fixed native
 *                     nonlinearity driven by Input, and its own UI does not
 *                     expose this. Left at upstream's 0 default by the shell.
 *   ms_mode           retired; the DSP is always normal stereo.
 *   spectrum_prepost  desktop analyser source.
 *   show_graph        desktop window state.
 *   out_peak_l/r      output params (meters) — served through get_param.
 *
 * Factory presets are NOT a parameter here either, because they are not one
 * upstream. They are served as a preset-browser level so the fourteen names
 * stay exactly as Dusk Audio wrote them, instead of being cut to fit a 5-char
 * knob readout.
 *
 * Layout: ONE PAGE PER BAND, which is how the console reads and how the
 * plugin's own channel strip is arranged. Six pages, 22 params, no control
 * more than one page from the band it belongs to:
 *
 *   LF      Gain  Freq  Bell   HPF    HPF On
 *   LMF     Gain  Freq  Q
 *   HMF     Gain  Freq  Q
 *   HF      Gain  Freq  Bell   LPF    LPF On
 *   MASTER  Type  Ovrsmpl  Bypass  AutoGain  In  Out
 *   PRESET  (browser level — not knobs)
 *
 * The filters sit with the band they act on: the high-pass on LF, the
 * low-pass on HF. That is where you reach for them, and it keeps FILTERS
 * from being a page you have to remember exists.
 *
 * Pages are deliberately not filled to eight. A band page holding only its
 * own three or five controls is the point — nothing to hunt through.
 *
 * The DSP core and its parameter semantics are 4K EQ 2 by Dusk Audio
 * (GPL-3.0, github.com/dusk-audio/dusk-audio-plugins); see src/ported/.
 */

#ifndef FOURK_PARAMS_H
#define FOURK_PARAMS_H

enum FkqParamIndex {
    /* ---- page 1: the EQ under one knob row ---- */
    FKQ_P_LF_GAIN = 0,
    FKQ_P_LM_GAIN,
    FKQ_P_HM_GAIN,
    FKQ_P_HF_GAIN,
    FKQ_P_HPF_FREQ,
    FKQ_P_LPF_FREQ,
    FKQ_P_INPUT_GAIN,
    FKQ_P_OUTPUT_GAIN,
    /* ---- page 2: where the bands sit ---- */
    FKQ_P_LF_FREQ,
    FKQ_P_LM_FREQ,
    FKQ_P_HM_FREQ,
    FKQ_P_HF_FREQ,
    FKQ_P_LM_Q,
    FKQ_P_HM_Q,
    FKQ_P_LF_BELL,
    FKQ_P_HF_BELL,
    /* ---- page 3: filters in/out, voicing, machine settings ---- */
    FKQ_P_HPF_ENABLED,
    FKQ_P_LPF_ENABLED,
    FKQ_P_EQ_TYPE,
    FKQ_P_AUTO_GAIN,
    FKQ_P_OVERSAMPLING,
    FKQ_P_BYPASS,
    FKQ_PARAM_COUNT,
    FKQ_VISIBLE_PARAM_COUNT = FKQ_PARAM_COUNT
};

/* The two dial positions the console prints as OUT. Upstream's preset
 * runtime uses exactly these thresholds to decide a program's filter
 * enables, so the dial and a recalled preset cannot disagree. */
#define FKQ_HPF_OUT_HZ 16.0f
#define FKQ_LPF_OUT_HZ 15201.0f

typedef enum { FKQ_FLOAT, FKQ_INT, FKQ_ENUM } fkq_type_t;

typedef struct {
    const char  *key;
    const char  *name;
    fkq_type_t   type;
    float        min, max, def;
    const char *const *options;  /* FKQ_ENUM only */
    int          n_options;
    const char  *unit;
    const char  *display_format;
} fkq_param_t;

/* Enum labels. Brown/Black and 1x/2x/4x are upstream's own kEqTypeLabels and
 * kOversampleLabels, verbatim. The booleans render Off/On; upstream declares
 * them kParameterIsBoolean and names them "LF Bell" / "HPF Enabled", so
 * Off/On is what those names mean. */
static const char *const fkq_opts_offon[2] = { "Off", "On" };
static const char *const fkq_opts_type[2]  = { "Brown", "Black" };
static const char *const fkq_opts_os[3]    = { "1x", "2x", "4x" };

/* Ranges and defaults are kFourKParams, unchanged, with ONE departure:
 *
 *   oversampling defaults to 2x here, where upstream ships 4x.
 *
 * The full 0..2 range and all three labels are kept, so nothing is taken away
 * — only the power-on choice differs, and it is measured rather than assumed.
 * On Move, 4x buys nothing audible over 1x: the response differs by at most
 * 0.05 dB on an 8 kHz shelf and 0.25 dB on a 16 kHz shelf. What 1x does cost
 * is aliasing from the always-on console nonlinearity (consoleSatAmount()
 * returns 0.25 native in Brown, 0.50 in Black, even with saturation at zero):
 * bin-centred FFT puts 9-14 kHz tones at -84 dBc in Brown and -71 dBc in
 * Black at -3 dBFS. 2x returns all of that to below -120 dBc for 23 samples
 * (0.52 ms) of latency and a fraction of a percent of a core. Brown at 1x is
 * clean enough for anything; Black at 1x is the case that grits.
 *
 * Everything else — including auto_gain Off — is upstream's own default and
 * is kept BECAUSE it is upstream's: the reference console has no compensation
 * stage, and matching it is what makes this port checkable. The Move's int16
 * output bus will clip before the EQ does if you boost hard; that is what the
 * clip flag in get_param and the Out trim are for.
 */
static const fkq_param_t fkq_params[FKQ_PARAM_COUNT] = {
    { "lf_gain",      "GAIN",     FKQ_FLOAT,   -15,    15,     0, 0, 0, "dB", ".1f" },
    { "lm_gain",      "GAIN",    FKQ_FLOAT,   -15,    15,     0, 0, 0, "dB", ".1f" },
    { "hm_gain",      "GAIN",    FKQ_FLOAT,   -15,    15,     0, 0, 0, "dB", ".1f" },
    { "hf_gain",      "GAIN",     FKQ_FLOAT,   -15,    15,     0, 0, 0, "dB", ".1f" },
    { "hpf_freq",     "HPF",    FKQ_FLOAT,    16,   350,    16, 0, 0, "Hz", ".0f" },
    { "lpf_freq",     "LPF",    FKQ_FLOAT,  3000, 15201, 15201, 0, 0, "Hz", ".0f" },
    { "input_gain",   "IN",     FKQ_FLOAT,   -12,    12,     0, 0, 0, "dB", ".1f" },
    { "output_gain",  "OUT",    FKQ_FLOAT,   -12,    12,     0, 0, 0, "dB", ".1f" },

    { "lf_freq",      "FREQ",  FKQ_FLOAT,    30,   450,   200, 0, 0, "Hz", ".0f" },
    { "lm_freq",      "FREQ", FKQ_FLOAT,   200,  2500,  1000, 0, 0, "Hz", ".0f" },
    { "hm_freq",      "FREQ", FKQ_FLOAT,   600,  7000,  3000, 0, 0, "Hz", ".0f" },
    { "hf_freq",      "FREQ",  FKQ_FLOAT,  1500, 16000,  8000, 0, 0, "Hz", ".0f" },
    { "lm_q",         "Q",  FKQ_FLOAT,  0.5f,     3,  1.5f, 0, 0, 0,    ".2f" },
    { "hm_q",         "Q",  FKQ_FLOAT,  0.5f,     3,  1.5f, 0, 0, 0,    ".2f" },
    { "lf_bell",      "BELL",  FKQ_ENUM,      0,     1,     0, fkq_opts_offon, 2, 0, 0 },
    { "hf_bell",      "BELL",  FKQ_ENUM,      0,     1,     0, fkq_opts_offon, 2, 0, 0 },

    { "hpf_enabled",  "HPF ON", FKQ_ENUM,      0,     1,     0, fkq_opts_offon, 2, 0, 0 },
    { "lpf_enabled",  "LPF ON", FKQ_ENUM,      0,     1,     0, fkq_opts_offon, 2, 0, 0 },
    { "eq_type",      "TYPE",   FKQ_ENUM,      0,     1,     0, fkq_opts_type,  2, 0, 0 },
    { "auto_gain",    "A.GAIN", FKQ_ENUM,      0,     1,     0, fkq_opts_offon, 2, 0, 0 },
    { "oversampling", "OVRSMP", FKQ_ENUM,      0,     2,     1, fkq_opts_os,    3, 0, 0 },
    { "bypass",       "BYPASS", FKQ_ENUM,      0,     1,     0, fkq_opts_offon, 2, 0, 0 },
};


/* Read-only readouts.
 *
 * These are NOT controls and are deliberately absent from fkq_params above,
 * which is the 1:1 mirror of upstream's parameter table. They exist because
 * the remote panel can only ever see keys the plugin lists in chain_params —
 * the manager seeds and re-reads exactly that set, so a value the DSP merely
 * computes never reaches a browser at all. Declaring them "access":"read"
 * gets them across without making them editable, and none of them appears in
 * any level's knobs[], so the stock knob grid never plans a cell for one.
 *
 * out_peak_l / out_peak_r are upstream's own output parameters. The rest are
 * shell-level: in_peak_l/r read the core's input meters, clip counts samples
 * clamped at the Move's int16 conversion (which happens outside the EQ
 * entirely), and curve is the response the panel draws.
 *
 * curve is type "string" on purpose: param_meta.mjs treats string as an
 * opaque kind, so nothing tries to render 128 comma-separated decibels as a
 * dial.
 */
/* Params drawn as an on/off switch rather than a dial.
 *
 * The detector already infers a switch over all six, and gets it right — but
 * inference is documented as a fallback, not the contract, and a declaration
 * is the only way to correct it if the heuristic ever changes. `validate.mjs`
 * reports each guess as `viz-inferred`; declaring them turns those into
 * confirmations.
 *
 * eq_type is deliberately NOT here. It is a two-option enum like the others,
 * but Brown/Black is a voicing choice, not an on/off state, and an on/off
 * switch graphic would say something false about it. The detector agrees —
 * it does not claim eq_type — so leaving it undeclared keeps it an honest
 * enum cell. */
static const char *const fkq_switch_viz[] = {
    "lf_bell", "hf_bell", "hpf_enabled", "lpf_enabled", "auto_gain", "bypass",
};
#define FKQ_SWITCH_VIZ_COUNT ((int)(sizeof fkq_switch_viz / sizeof fkq_switch_viz[0]))

/* Params that must NOT get a graphic, because the detector's guess is wrong.
 *
 * A band's FREQ sitting next to its Q looks exactly like a filter's cutoff
 * next to its resonance, and the detector says "filter" for LMF and HMF. It
 * would then draw a lowpass response over a parametric BELL — a picture that
 * states something false about the control. The docs are explicit that a
 * declaration is the only way to correct a detector, and that a wrong graphic
 * is worse than an honest dial. There is no "bell" kind to declare instead,
 * so these four decline one. */
static const char *const fkq_no_viz[] = {
    "lm_freq", "lm_q", "hm_freq", "hm_q",
};
#define FKQ_NO_VIZ_COUNT ((int)(sizeof fkq_no_viz / sizeof fkq_no_viz[0]))

static inline int fkq_is_no_viz(const char *key)
{
    for (int i = 0; i < FKQ_NO_VIZ_COUNT; i++) {
        const char *a = fkq_no_viz[i], *b = key;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

static inline int fkq_is_switch_viz(const char *key)
{
    for (int i = 0; i < FKQ_SWITCH_VIZ_COUNT; i++) {
        const char *a = fkq_switch_viz[i], *b = key;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

typedef struct {
    const char *key;
    const char *type;   /* chain_params type: "float" / "int" / "string" */
} fkq_readout_t;

static const fkq_readout_t fkq_readouts[] = {
    { "in_peak_l",  "float"  },
    { "in_peak_r",  "float"  },
    { "out_peak_l", "float"  },
    { "out_peak_r", "float"  },
    { "clip",       "int"    },
};
#define FKQ_READOUT_COUNT ((int)(sizeof fkq_readouts / sizeof fkq_readouts[0]))

#endif /* FOURK_PARAMS_H */
