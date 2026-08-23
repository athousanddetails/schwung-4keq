/* 4K EQ — Schwung audio_fx module for Ableton Move.
 *
 * A thin audio_fx_api_v2 shell around duskaudio::FourKEQDSP, the
 * framework-free British console EQ core from 4K EQ 2 by Dusk Audio
 * (GPL-3.0). The vendored core in src/ported/ is unmodified upstream;
 * everything Move-specific lives in this file.
 *
 * The parameter surface is 1:1 with upstream's own table — see
 * fourk_params.h for why, and tools/check_upstream_params.py for the gate
 * that keeps it that way.
 *
 * Realtime rules: process_block never allocates, never logs, never touches
 * the filesystem. set_param and get_param run on the SAME SPI callback and
 * are held to the same rule; the one expensive thing either of them can do —
 * designing the response curve for the remote panel — is cached behind a
 * dirty flag so a poll costs a memcpy rather than a redesign.
 */

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "../host/plugin_api_v1.h"
#include "../host/audio_fx_api_v2.h"
#include "FourKEQDSP.hpp"
#include "FourKEQParams.hpp"
#include "FourKEQPresetRuntime.hpp"
#include "fourk_params.h"

static const host_api_v1_t *g_host = nullptr;

static void fkq_log(const char *msg)
{
    if (g_host && g_host->log) g_host->log(msg);
}

/* ------------------------------------------------------------------ */
/* Instance                                                            */
/* ------------------------------------------------------------------ */

/* Index 0 is INIT (upstream's flat default); 1..14 are kFactoryPresets in
 * order. Upstream's names are used verbatim — they are what the plugin calls
 * these programs, and the preset browser is a list, not a 5-char knob
 * readout, so there is no reason to abbreviate them. */
#define FKQ_PRESET_COUNT (kNumFactoryPresets + 1)

/* Response curve served to the remote panel. 128 points is one per column of
 * the Move's 128px display and plenty for a browser canvas. */
#define FKQ_CURVE_POINTS 128
#define FKQ_CURVE_F_LO   20.0f
#define FKQ_CURVE_F_HI   20000.0f

struct fkq_instance {
    duskaudio::FourKEQDSP dsp;
    std::atomic<float> values[FKQ_PARAM_COUNT];

    float bufL[MOVE_FRAMES_PER_BLOCK];
    float bufR[MOVE_FRAMES_PER_BLOCK];

    /* Current program, 0..FKQ_PRESET_COUNT-1. Not a DSP parameter (it is not
     * one upstream either); it is the preset browser's list_param. */
    std::atomic<int> preset { 0 };

    /* int16 output clipping. The Move's FX bus is 16-bit, so a hard boost
     * clips at the CONVERSION, not inside the EQ — upstream's rail sits at
     * +-1.5 and never sees it. Counted here so the UI can show it rather
     * than the module quietly limiting on the user's behalf. */
    std::atomic<int> clip_count { 0 };

    /* Curve cache. Rebuilt only when an EQ parameter moves. */
    std::atomic<bool> curve_dirty { true };
    char curve_buf[FKQ_CURVE_POINTS * 8 + 16];

    /* serve buffers (control thread only) */
    char chain_buf[8192];
    char state_buf[1024];
};

static float fkq_clamp(const fkq_param_t *p, float v)
{
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    return v;
}

static int fkq_param_index(const char *key)
{
    for (int i = 0; i < FKQ_PARAM_COUNT; i++)
        if (!strcmp(fkq_params[i].key, key))
            return i;
    return -1;
}

/* Push one shell value into the DSP core. One switch, no arithmetic: the
 * surface carries upstream's own units, so every case is a straight handoff.
 * That is the point of keeping the table 1:1 — a conversion here would be a
 * second place for the calibration to be wrong. */
static void fkq_push(fkq_instance *inst, int idx, float v)
{
    duskaudio::FourKEQDSP &d = inst->dsp;
    switch (idx) {
    case FKQ_P_LF_GAIN:      d.setLfGain(v);                    break;
    case FKQ_P_LM_GAIN:      d.setLmGain(v);                    break;
    case FKQ_P_HM_GAIN:      d.setHmGain(v);                    break;
    case FKQ_P_HF_GAIN:      d.setHfGain(v);                    break;
    case FKQ_P_HPF_FREQ:     d.setHpfFreq(v);                   break;
    case FKQ_P_LPF_FREQ:     d.setLpfFreq(v);                   break;
    case FKQ_P_INPUT_GAIN:   d.setInputGainDb(v);               break;
    case FKQ_P_OUTPUT_GAIN:  d.setOutputGainDb(v);              break;
    case FKQ_P_LF_FREQ:      d.setLfFreq(v);                    break;
    case FKQ_P_LM_FREQ:      d.setLmFreq(v);                    break;
    case FKQ_P_HM_FREQ:      d.setHmFreq(v);                    break;
    case FKQ_P_HF_FREQ:      d.setHfFreq(v);                    break;
    case FKQ_P_LM_Q:         d.setLmQ(v);                       break;
    case FKQ_P_HM_Q:         d.setHmQ(v);                       break;
    case FKQ_P_LF_BELL:      d.setLfBell(v > 0.5f);             break;
    case FKQ_P_HF_BELL:      d.setHfBell(v > 0.5f);             break;
    case FKQ_P_HPF_ENABLED:  d.setHpfEnabled(v > 0.5f);         break;
    case FKQ_P_LPF_ENABLED:  d.setLpfEnabled(v > 0.5f);         break;
    case FKQ_P_EQ_TYPE:      d.setEqType(v > 0.5f ? 1 : 0);     break;
    case FKQ_P_AUTO_GAIN:    d.setAutoGain(v > 0.5f);           break;
    case FKQ_P_OVERSAMPLING: d.setOversampling((int)(v + 0.5f)); break;
    case FKQ_P_BYPASS:       d.setBypass(v > 0.5f);             break;
    default: break;
    }
}

static void fkq_set_index(fkq_instance *inst, int idx, float v)
{
    v = fkq_clamp(&fkq_params[idx], v);
    inst->values[idx].store(v, std::memory_order_relaxed);
    fkq_push(inst, idx, v);
    inst->curve_dirty.store(true, std::memory_order_relaxed);
}

/* Upstream ParamId -> our table index. Every id the preset runtime emits is
 * listed; anything else returns -1 and is ignored, which is what keeps a
 * future upstream preset field from silently landing on the wrong knob. */
static int fkq_index_for_upstream(uint32_t id)
{
    switch ((ParamId)id) {
    case kHpfFreq:    return FKQ_P_HPF_FREQ;
    case kHpfEnabled: return FKQ_P_HPF_ENABLED;
    case kLpfFreq:    return FKQ_P_LPF_FREQ;
    case kLpfEnabled: return FKQ_P_LPF_ENABLED;
    case kLfGain:     return FKQ_P_LF_GAIN;
    case kLfFreq:     return FKQ_P_LF_FREQ;
    case kLfBell:     return FKQ_P_LF_BELL;
    case kLmGain:     return FKQ_P_LM_GAIN;
    case kLmFreq:     return FKQ_P_LM_FREQ;
    case kLmQ:        return FKQ_P_LM_Q;
    case kHmGain:     return FKQ_P_HM_GAIN;
    case kHmFreq:     return FKQ_P_HM_FREQ;
    case kHmQ:        return FKQ_P_HM_Q;
    case kHfGain:     return FKQ_P_HF_GAIN;
    case kHfFreq:     return FKQ_P_HF_FREQ;
    case kHfBell:     return FKQ_P_HF_BELL;
    case kEqType:     return FKQ_P_EQ_TYPE;
    case kInputGain:  return FKQ_P_INPUT_GAIN;
    case kOutputGain: return FKQ_P_OUTPUT_GAIN;
    case kAutoGain:   return FKQ_P_AUTO_GAIN;
    default:          return -1;
    }
}

static const char *fkq_preset_name(int idx)
{
    if (idx <= 0) return "INIT";
    if (idx > kNumFactoryPresets) return "INIT";
    return kFactoryPresets[idx - 1].name;
}

/* Recall a program.
 *
 * INIT restores every preset-owned parameter to upstream's default, exactly
 * as fkIsPresetParam defines that set — so it clears the EQ without touching
 * bypass or the oversampling choice, which are not the preset's business.
 *
 * Factory programs go through upstream's own FourKEQPresetRuntime, which is
 * the only thing that knows how to turn a preset's AUDIBLE Hz into the
 * control coordinate the parameter carries. Never open-code that conversion.
 */
static void fkq_apply_preset(fkq_instance *inst, int presetIdx)
{
    if (presetIdx < 0 || presetIdx >= FKQ_PRESET_COUNT) return;
    inst->preset.store(presetIdx, std::memory_order_relaxed);

    if (presetIdx == 0) {
        for (uint32_t up = 0; up < (uint32_t)kNumInputParams; up++) {
            if (!fkIsPresetParam(up)) continue;
            const int idx = fkq_index_for_upstream(up);
            if (idx >= 0) fkq_set_index(inst, idx, kFourKParams[up].def);
        }
        return;
    }

    forEachFourKEQFactoryPresetParam(presetIdx - 1, [inst](uint32_t up, float v) {
        const int idx = fkq_index_for_upstream(up);
        if (idx >= 0) fkq_set_index(inst, idx, v);
    });
}

/* ------------------------------------------------------------------ */
/* v2 entry points                                                     */
/* ------------------------------------------------------------------ */

static void *fkq_create_instance(const char * /*module_dir*/,
                                 const char * /*config_json*/)
{
    auto *inst = new (std::nothrow) fkq_instance();
    if (!inst) return nullptr;

    const int sr     = g_host ? g_host->sample_rate      : MOVE_SAMPLE_RATE;
    const int frames = g_host ? g_host->frames_per_block : MOVE_FRAMES_PER_BLOCK;

    /* Seed every value BEFORE prepare(): prepare() snapshots the parameters
     * to design its first set of coefficients, and a core prepared from
     * unseeded atomics would run one block of whatever zero means for each
     * of them. */
    for (int i = 0; i < FKQ_PARAM_COUNT; i++)
        fkq_set_index(inst, i, fkq_params[i].def);

    /* The five upstream keys this module gives no knob still have to be put
     * where upstream's own defaults put them, rather than left at whatever
     * the core's member initialisers happen to be. saturation is the one
     * that matters: consoleSatAmount() reads it on every block, so leaving
     * it unset would change the sound. */
    inst->dsp.setSaturation(kFourKParams[kSaturation].def);
    inst->dsp.setMsMode(kFourKParams[kMsMode].def > 0.5f);

    inst->dsp.prepare((double)sr,
                      frames > MOVE_FRAMES_PER_BLOCK ? frames : MOVE_FRAMES_PER_BLOCK);
    inst->dsp.reset();

    fkq_log("4k-eq: instance created");
    return inst;
}

static void fkq_destroy_instance(void *instance)
{
    delete (fkq_instance *)instance;
}

static void fkq_process_block(void *instance, int16_t *audio_inout, int frames)
{
    auto *inst = (fkq_instance *)instance;
    if (!inst || !audio_inout || frames <= 0) return;

    int clipped = 0;
    int16_t *p = audio_inout;
    while (frames > 0) {
        const int n = frames > MOVE_FRAMES_PER_BLOCK ? MOVE_FRAMES_PER_BLOCK : frames;

        for (int i = 0; i < n; i++) {
            inst->bufL[i] = (float)p[i * 2]     * (1.0f / 32768.0f);
            inst->bufR[i] = (float)p[i * 2 + 1] * (1.0f / 32768.0f);
        }

        const float *ins[2] = { inst->bufL, inst->bufR };
        float *outs[2]      = { inst->bufL, inst->bufR };
        inst->dsp.processBlock(ins, outs, 2, n);

        /* Scale OUT by the same 32768 the input was scaled by, not 32767.
         * The asymmetric pair is the usual idiom and it is not identity:
         * 1000 -> 1000/32768 -> *32767 -> 999.97 -> 999, losing an LSB on
         * every sample. That silently costs this module the one thing its
         * bypass is supposed to guarantee — a bit-exact dry path — and the
         * loadtest caught it. 32768 is a power of two, so the round trip is
         * exact in float; only the positive rail needs the clamp. */
        for (int i = 0; i < n; i++) {
            float l = inst->bufL[i] * 32768.0f;
            float r = inst->bufR[i] * 32768.0f;
            if (l > 32767.0f)  { l = 32767.0f;  clipped++; }
            else if (l < -32768.0f) { l = -32768.0f; clipped++; }
            if (r > 32767.0f)  { r = 32767.0f;  clipped++; }
            else if (r < -32768.0f) { r = -32768.0f; clipped++; }
            p[i * 2]     = (int16_t)l;
            p[i * 2 + 1] = (int16_t)r;
        }

        p += n * 2;
        frames -= n;
    }

    if (clipped)
        inst->clip_count.fetch_add(clipped, std::memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* set                                                                 */
/* ------------------------------------------------------------------ */

static int fkq_enum_index(const fkq_param_t *prm, const char *val)
{
    for (int i = 0; i < prm->n_options; i++)
        if (!strcmp(prm->options[i], val))
            return i;
    for (int i = 0; i < prm->n_options; i++) {
        const char *a = prm->options[i], *b = val;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return -1;
}

static int fkq_json_number(const char *json, const char *key, float *out)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    *out = (float)atof(p);
    return 0;
}

static void fkq_set_param(void *instance, const char *key, const char *val)
{
    auto *inst = (fkq_instance *)instance;
    if (!inst || !key || !val) return;

    const char *colon = strrchr(key, ':');
    if (colon) key = colon + 1;

    if (!strcmp(key, "state")) {
        float v;
        if (fkq_json_number(val, "fkq", &v) != 0) return;
        for (int i = 0; i < FKQ_PARAM_COUNT; i++)
            if (fkq_json_number(val, fkq_params[i].key, &v) == 0)
                fkq_set_index(inst, i, v);
        /* Restore the browser's position WITHOUT recalling — the values in
         * the blob are the truth, and re-applying the program on top would
         * discard any edit made after it was recalled. */
        if (fkq_json_number(val, "preset", &v) == 0) {
            int pi = (int)(v + 0.5f);
            if (pi < 0) pi = 0;
            if (pi >= FKQ_PRESET_COUNT) pi = FKQ_PRESET_COUNT - 1;
            inst->preset.store(pi, std::memory_order_relaxed);
        }
        return;
    }

    /* The preset browser's list_param. */
    if (!strcmp(key, "preset")) {
        fkq_apply_preset(inst, atoi(val));
        return;
    }

    const int idx = fkq_param_index(key);
    if (idx < 0) return;
    const fkq_param_t *prm = &fkq_params[idx];

    float v;
    if (prm->type == FKQ_ENUM) {
        const int oi = fkq_enum_index(prm, val);
        v = oi >= 0 ? (float)oi : (float)atof(val);
    } else {
        v = (float)atof(val);
    }
    fkq_set_index(inst, idx, v);
}

/* ------------------------------------------------------------------ */
/* get                                                                 */
/* ------------------------------------------------------------------ */

static int fkq_write_str(char *buf, int buf_len, const char *s)
{
    int n = (int)strlen(s);
    if (n >= buf_len) n = buf_len - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    return n;
}

/* One page per band — see the layout note in fourk_params.h.
 *
 * Each array is that band's own controls and nothing else, so a page never
 * mixes two bands and you are never more than one page from the thing you
 * are turning. The high-pass rides with LF and the low-pass with HF, because
 * that is the end of the spectrum each one acts on; a separate FILTERS page
 * would be one more page to remember.
 *
 * Gain first on every band page, then Freq, then the band's third control.
 * Same order on all four, so the hand learns one shape. */
static const char *const fkq_knobs_lf[5] = {
    "lf_gain", "lf_freq", "lf_bell", "hpf_freq", "hpf_enabled",
};
static const char *const fkq_knobs_lmf[3] = {
    "lm_gain", "lm_freq", "lm_q",
};
static const char *const fkq_knobs_hmf[3] = {
    "hm_gain", "hm_freq", "hm_q",
};
static const char *const fkq_knobs_hf[5] = {
    "hf_gain", "hf_freq", "hf_bell", "lpf_freq", "lpf_enabled",
};
static const char *const fkq_knobs_master[6] = {
    "eq_type", "oversampling", "bypass", "auto_gain",
    "input_gain", "output_gain",
};

static const char *fkq_type_name(fkq_type_t t)
{
    return t == FKQ_ENUM ? "enum" : (t == FKQ_INT ? "int" : "float");
}

/* Writes the parameter array shared by chain_params and ui_hierarchy. */
static int fkq_write_param_array(char *o, size_t cap)
{
    size_t w = 0;
    w += (size_t)snprintf(o + w, cap - w, "[");
    for (int i = 0; i < FKQ_VISIBLE_PARAM_COUNT; i++) {
        const fkq_param_t *p = &fkq_params[i];
        if (i) w += (size_t)snprintf(o + w, cap - w, ",");
        w += (size_t)snprintf(o + w, cap - w,
                              "{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\"",
                              p->key, p->name, fkq_type_name(p->type));
        if (p->type == FKQ_ENUM) {
            w += (size_t)snprintf(o + w, cap - w, ",\"options\":[");
            for (int j = 0; j < p->n_options; j++)
                w += (size_t)snprintf(o + w, cap - w, "%s\"%s\"",
                                      j ? "," : "", p->options[j]);
            w += (size_t)snprintf(o + w, cap - w, "],\"default\":%d",
                                  (int)(p->def + 0.5f));
        } else {
            w += (size_t)snprintf(o + w, cap - w,
                                  ",\"min\":%.6g,\"max\":%.6g,\"default\":%.6g",
                                  (double)p->min, (double)p->max, (double)p->def);
        }
        if (p->unit)
            w += (size_t)snprintf(o + w, cap - w, ",\"unit\":\"%s\"", p->unit);
        if (p->display_format)
            w += (size_t)snprintf(o + w, cap - w, ",\"display_format\":\"%s\"",
                                  p->display_format);
        if (fkq_is_switch_viz(p->key))
            w += (size_t)snprintf(o + w, cap - w, ",\"viz\":{\"kind\":\"switch\"}");
        else if (fkq_is_no_viz(p->key))
            w += (size_t)snprintf(o + w, cap - w, ",\"viz\":false");
        w += (size_t)snprintf(o + w, cap - w, "}");
        if (w >= cap - 64) return -1;
    }
    /* Read-only readouts. Listed here because chain_params is the ONLY set of
     * keys the manager pushes to a remote panel; they are on no page. */
    for (int i = 0; i < FKQ_READOUT_COUNT; i++) {
        w += (size_t)snprintf(o + w, cap - w,
                              ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\""
                              ",\"access\":\"read\"}",
                              fkq_readouts[i].key, fkq_readouts[i].key,
                              fkq_readouts[i].type);
        if (w >= cap - 64) return -1;
    }
    w += (size_t)snprintf(o + w, cap - w, "]");
    return w >= cap ? -1 : (int)w;
}

static size_t fkq_write_level(char *o, size_t cap, size_t w,
                              const char *key, const char *name,
                              const char *const *knobs, int n_knobs,
                              bool last)
{
    w += (size_t)snprintf(o + w, cap - w,
                          "\"%s\":{\"name\":\"%s\",\"children\":null,\"knobs\":[",
                          key, name);
    for (int i = 0; i < n_knobs; i++)
        w += (size_t)snprintf(o + w, cap - w, "%s\"%s\"", i ? "," : "", knobs[i]);
    w += (size_t)snprintf(o + w, cap - w, "],\"params\":[");
    for (int i = 0; i < n_knobs; i++) {
        const int idx = fkq_param_index(knobs[i]);
        if (idx < 0) continue;
        w += (size_t)snprintf(o + w, cap - w, "%s{\"key\":\"%s\",\"label\":\"%s\"}",
                              i ? "," : "", fkq_params[idx].key, fkq_params[idx].name);
    }
    w += (size_t)snprintf(o + w, cap - w, "]}%s", last ? "" : ",");
    return w;
}

/* Rebuild the cached response curve if a parameter has moved since the last
 * read. designCurve() itself is under a microsecond; the 128-point sweep is
 * the cost, which is why it is not paid per poll. */
static void fkq_refresh_curve(fkq_instance *inst)
{
    if (!inst->curve_dirty.exchange(false, std::memory_order_relaxed))
        return;

    duskaudio::FourKEQDSP::CurveControls c;
    c.baseSampleRate = g_host ? (double)g_host->sample_rate : (double)MOVE_SAMPLE_RATE;
    c.oversampling = inst->values[FKQ_P_OVERSAMPLING].load(std::memory_order_relaxed);
    c.black        = inst->values[FKQ_P_EQ_TYPE].load(std::memory_order_relaxed) > 0.5f;
    c.hpfEnabled   = inst->values[FKQ_P_HPF_ENABLED].load(std::memory_order_relaxed) > 0.5f;
    c.lpfEnabled   = inst->values[FKQ_P_LPF_ENABLED].load(std::memory_order_relaxed) > 0.5f;
    c.hpfFreq      = inst->values[FKQ_P_HPF_FREQ].load(std::memory_order_relaxed);
    c.lpfFreq      = inst->values[FKQ_P_LPF_FREQ].load(std::memory_order_relaxed);
    c.lfGain       = inst->values[FKQ_P_LF_GAIN].load(std::memory_order_relaxed);
    c.lfFreq       = inst->values[FKQ_P_LF_FREQ].load(std::memory_order_relaxed);
    c.lfBell       = inst->values[FKQ_P_LF_BELL].load(std::memory_order_relaxed);
    c.lmGain       = inst->values[FKQ_P_LM_GAIN].load(std::memory_order_relaxed);
    c.lmFreq       = inst->values[FKQ_P_LM_FREQ].load(std::memory_order_relaxed);
    c.lmQ          = inst->values[FKQ_P_LM_Q].load(std::memory_order_relaxed);
    c.hmGain       = inst->values[FKQ_P_HM_GAIN].load(std::memory_order_relaxed);
    c.hmFreq       = inst->values[FKQ_P_HM_FREQ].load(std::memory_order_relaxed);
    c.hmQ          = inst->values[FKQ_P_HM_Q].load(std::memory_order_relaxed);
    c.hfGain       = inst->values[FKQ_P_HF_GAIN].load(std::memory_order_relaxed);
    c.hfFreq       = inst->values[FKQ_P_HF_FREQ].load(std::memory_order_relaxed);
    c.hfBell       = inst->values[FKQ_P_HF_BELL].load(std::memory_order_relaxed);
    /* Upstream's default, not a knob here — but designCurve() must be told
     * the same value processBlock() runs at or the drawn curve is not the
     * one you are hearing. */
    c.saturation   = kFourKParams[kSaturation].def;

    const duskaudio::FourKEQDSP::CurveCoeffs d =
        duskaudio::FourKEQDSP::designCurve(c);

    const float ratio = FKQ_CURVE_F_HI / FKQ_CURVE_F_LO;
    char *o = inst->curve_buf;
    const size_t cap = sizeof inst->curve_buf;
    size_t w = 0;
    for (int i = 0; i < FKQ_CURVE_POINTS; i++) {
        const float f = FKQ_CURVE_F_LO *
            std::pow(ratio, (float)i / (float)(FKQ_CURVE_POINTS - 1));
        w += (size_t)snprintf(o + w, cap - w, "%s%.2f", i ? "," : "",
                              (double)duskaudio::FourKEQDSP::curveDbAt(d, f));
        if (w >= cap - 12) break;
    }
    o[w < cap ? w : cap - 1] = 0;
}

static int fkq_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    auto *inst = (fkq_instance *)instance;
    if (!inst || !key || !buf || buf_len <= 1) return -1;

    const char *colon = strrchr(key, ':');
    if (colon) key = colon + 1;

    if (!strcmp(key, "name"))
        return fkq_write_str(buf, buf_len, "4K EQ");

    if (!strcmp(key, "chain_params")) {
        if (fkq_write_param_array(inst->chain_buf, sizeof inst->chain_buf) < 0)
            return -1;
        return fkq_write_str(buf, buf_len, inst->chain_buf);
    }

    if (!strcmp(key, "ui_hierarchy")) {
        char *o = inst->chain_buf;
        const size_t cap = sizeof inst->chain_buf;
        size_t w = (size_t)snprintf(o, cap, "{\"modes\":null,\"levels\":{");
        /* LF is root. It is the first band, and a root that is itself a band
         * keeps every page the same kind of thing — no "Main" page that is
         * really a summary of five others. */
        w += (size_t)snprintf(o + w, cap - w,
            "\"root\":{\"name\":\"LF\",\"children\":null,\"knobs\":[");
        for (int i = 0; i < 5; i++)
            w += (size_t)snprintf(o + w, cap - w, "%s\"%s\"", i ? "," : "",
                                  fkq_knobs_lf[i]);
        w += (size_t)snprintf(o + w, cap - w, "],\"params\":[");
        for (int i = 0; i < 5; i++) {
            const int idx = fkq_param_index(fkq_knobs_lf[i]);
            if (idx < 0) continue;
            w += (size_t)snprintf(o + w, cap - w, "%s{\"key\":\"%s\",\"label\":\"%s\"}",
                                  i ? "," : "", fkq_params[idx].key, fkq_params[idx].name);
        }
        w += (size_t)snprintf(o + w, cap - w,
            ",{\"level\":\"lmf\",\"label\":\"LMF\"}"
            ",{\"level\":\"hmf\",\"label\":\"HMF\"}"
            ",{\"level\":\"hf\",\"label\":\"HF\"}"
            ",{\"level\":\"master\",\"label\":\"MASTER\"}"
            ",{\"level\":\"presets\",\"label\":\"PRESET\"}]},");
        if (w >= cap - 2) return -1;

        w = fkq_write_level(o, cap, w, "lmf", "LMF", fkq_knobs_lmf, 3, false);
        w = fkq_write_level(o, cap, w, "hmf", "HMF", fkq_knobs_hmf, 3, false);
        w = fkq_write_level(o, cap, w, "hf", "HF", fkq_knobs_hf, 5, false);
        w = fkq_write_level(o, cap, w, "master", "MASTER", fkq_knobs_master, 6, false);
        /* Preset browser. list_param / count_param / name_param get their own
         * page kind; deliberately no "knobs" here — a selector listed as a
         * knob is ignored by the planner and would be dead travel anyway. */
        w += (size_t)snprintf(o + w, cap - w,
            "\"presets\":{\"name\":\"PRESET\",\"children\":null,"
            "\"list_param\":\"preset\",\"count_param\":\"preset_count\","
            "\"name_param\":\"preset_name\"}");
        w += (size_t)snprintf(o + w, cap - w, "}}");
        if (w >= cap) return -1;
        return fkq_write_str(buf, buf_len, o);
    }

    if (!strcmp(key, "state")) {
        char *o = inst->state_buf;
        const size_t cap = sizeof inst->state_buf;
        size_t w = (size_t)snprintf(o, cap, "{\"fkq\":1");
        for (int i = 0; i < FKQ_PARAM_COUNT; i++) {
            w += (size_t)snprintf(o + w, cap - w, ",\"%s\":%.6g",
                                  fkq_params[i].key,
                                  (double)inst->values[i].load(std::memory_order_relaxed));
            if (w >= cap - 48) return -1;
        }
        w += (size_t)snprintf(o + w, cap - w, ",\"preset\":%d",
                              inst->preset.load(std::memory_order_relaxed));
        if (w >= cap - 8) return -1;
        w += (size_t)snprintf(o + w, cap - w, "}");
        return fkq_write_str(buf, buf_len, o);
    }

    /* ---- preset browser ---- */
    if (!strcmp(key, "preset"))
        return snprintf(buf, buf_len, "%d", inst->preset.load(std::memory_order_relaxed));
    if (!strcmp(key, "preset_count"))
        return snprintf(buf, buf_len, "%d", FKQ_PRESET_COUNT);
    if (!strcmp(key, "preset_name"))
        return fkq_write_str(buf, buf_len,
                             fkq_preset_name(inst->preset.load(std::memory_order_relaxed)));

    /* ---- meters, linear peak 0..~2, ~300 ms release ---- */
    if (!strcmp(key, "in_peak_l"))  return snprintf(buf, buf_len, "%.4f", (double)inst->dsp.getInputPeakL());
    if (!strcmp(key, "in_peak_r"))  return snprintf(buf, buf_len, "%.4f", (double)inst->dsp.getInputPeakR());
    if (!strcmp(key, "out_peak_l")) return snprintf(buf, buf_len, "%.4f", (double)inst->dsp.getOutputPeakL());
    if (!strcmp(key, "out_peak_r")) return snprintf(buf, buf_len, "%.4f", (double)inst->dsp.getOutputPeakR());

    /* Samples clamped at the int16 conversion since the last read, and
     * cleared BY the read: the UI wants "did it clip since I last looked",
     * and a free-running total answers a different question. */
    if (!strcmp(key, "clip"))
        return snprintf(buf, buf_len, "%d", inst->clip_count.exchange(0, std::memory_order_relaxed));

    /* Latency the oversampler is currently adding, in base-rate samples. */
    if (!strcmp(key, "latency"))
        return snprintf(buf, buf_len, "%d", inst->dsp.getLatencySamples());

    /* Calibrated audible centre of each band, in Hz — see fourk_readouts. */
    {
        using FK = duskaudio::FourKEQDSP;
        const bool black = inst->values[FKQ_P_EQ_TYPE].load(std::memory_order_relaxed) > 0.5f;
        struct { const char *k; int fi, gi, bi; FK::Band band; } bands[4] = {
            { "lf_hz", FKQ_P_LF_FREQ, FKQ_P_LF_GAIN, FKQ_P_LF_BELL, FK::Band::LF },
            { "lm_hz", FKQ_P_LM_FREQ, FKQ_P_LM_GAIN, -1,            FK::Band::LM },
            { "hm_hz", FKQ_P_HM_FREQ, FKQ_P_HM_GAIN, -1,            FK::Band::HM },
            { "hf_hz", FKQ_P_HF_FREQ, FKQ_P_HF_GAIN, FKQ_P_HF_BELL, FK::Band::HF },
        };
        for (int i = 0; i < 4; i++) {
            if (strcmp(key, bands[i].k)) continue;
            /* The mid bands are always bell; only LF and HF have the switch. */
            const bool bell = bands[i].bi < 0
                ? true
                : inst->values[bands[i].bi].load(std::memory_order_relaxed) > 0.5f;
            const float hz = FK::calibratedEqFrequency(
                inst->values[bands[i].fi].load(std::memory_order_relaxed),
                inst->values[bands[i].gi].load(std::memory_order_relaxed),
                bands[i].band, black, bell);
            return snprintf(buf, buf_len, "%.1f", (double)hz);
        }
    }

    /* The drawn response, dB at FKQ_CURVE_POINTS log-spaced points from
     * 20 Hz to 20 kHz. Computed by the DSP's own designCurve(), so the panel
     * cannot draw a curve the audio path disagrees with. */
    if (!strcmp(key, "curve")) {
        fkq_refresh_curve(inst);
        return fkq_write_str(buf, buf_len, inst->curve_buf);
    }
    if (!strcmp(key, "curve_points"))
        return snprintf(buf, buf_len, "%d", FKQ_CURVE_POINTS);

    const int idx = fkq_param_index(key);
    if (idx < 0) return -1;
    const fkq_param_t *prm = &fkq_params[idx];
    const float v = inst->values[idx].load(std::memory_order_relaxed);
    if (prm->type == FKQ_ENUM) {
        int oi = (int)(v + 0.5f);
        if (oi < 0) oi = 0;
        if (oi >= prm->n_options) oi = prm->n_options - 1;
        return fkq_write_str(buf, buf_len, prm->options[oi]);
    }
    return snprintf(buf, buf_len, "%.6g", (double)v);
}

/* ------------------------------------------------------------------ */

static audio_fx_api_v2_t g_fx_api_v2;

extern "C" audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    memset(&g_fx_api_v2, 0, sizeof g_fx_api_v2);
    g_fx_api_v2.api_version      = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = fkq_create_instance;
    g_fx_api_v2.destroy_instance = fkq_destroy_instance;
    g_fx_api_v2.process_block    = fkq_process_block;
    g_fx_api_v2.set_param        = fkq_set_param;
    g_fx_api_v2.get_param        = fkq_get_param;
    g_fx_api_v2.on_midi          = nullptr;

    fkq_log("4k-eq: 4K EQ 2 (Dusk Audio) audio_fx v2 initialized");
    return &g_fx_api_v2;
}
