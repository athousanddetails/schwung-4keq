/* 4K EQ loader test.
 *
 * dlopen()s the real 4k-eq.so exactly as Schwung's chain host does, then
 * checks the layer the host actually consumes. Built for aarch64 and run ON
 * the Move — this is the go/no-go gate before wiring it into a set.
 *
 * The rule this file exists to enforce: prove a parameter APPLIES, do not
 * prove that setting it failed to crash. Every control is set away from its
 * default and the rendered audio must change; a control whose band is
 * inactive is given its prerequisite first, because a 0 dB band is bypassed
 * outright by the core and its frequency genuinely does nothing.
 *
 *   ./fkq_loadtest ./4k-eq.so [module.json]
 */

#include <dlfcn.h>
#include <cmath>
#include <cstdarg>   /* va_start/va_end: libc++ pulls these in via
                      * <cstdio>, libstdc++ does not, so omitting it
                      * builds on the Mac and fails in the container. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>

#include "plugin_api_v1.h"
#include "audio_fx_api_v2.h"

static int g_fail = 0;
static int g_checks = 0;

static void ok(bool cond, const char *fmt, ...)
{
    va_list ap;
    char msg[512];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", msg);
    } else {
        printf("  FAIL %s\n", msg);
        g_fail++;
    }
}

/* ---- host stub ------------------------------------------------------- */

static void host_log(const char *msg) { printf("       [dsp] %s\n", msg); }

static host_api_v1_t g_host_api;

/* ---- test signal ----------------------------------------------------- */

/* 512 frames of a fixed multi-tone at a realistic master-bus level. Enough
 * low, mid and high content that every band has something to act on. */
static const int kFrames = 512;

static void fill_signal(std::vector<int16_t> &buf, float peak)
{
    buf.resize((size_t)kFrames * 2);
    for (int i = 0; i < kFrames; i++) {
        const double t = (double)i / 44100.0;
        double s = 0.42 * std::sin(2 * M_PI *   80.0 * t)
                 + 0.30 * std::sin(2 * M_PI *  650.0 * t)
                 + 0.22 * std::sin(2 * M_PI * 3200.0 * t)
                 + 0.16 * std::sin(2 * M_PI * 9000.0 * t);
        s *= peak;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        const int16_t v = (int16_t)(s * 32767.0);
        buf[(size_t)i * 2] = v;
        buf[(size_t)i * 2 + 1] = v;
    }
}

/* FNV-1a over the rendered block: a cheap fingerprint that changes if any
 * sample changes.
 *
 * kWarmBlocks is not a round number picked for comfort. Comparing two
 * fingerprints for EQUALITY is comparing recursive filter state, and the
 * slowest pole in the chain is the console saturator's DC blocker at 5 Hz —
 * a time constant of about 32 ms. Eight blocks (93 ms, ~3 tau) leaves a
 * residual well above an int16 LSB, so two instances holding identical
 * parameters still rendered different bytes and the INIT check failed
 * against a module that was in fact correct. Thirty-two blocks is 372 ms,
 * about 11.6 tau, which puts the residual below the LSB and makes the
 * fingerprint a property of the SETTINGS rather than of the history. */
static const int kWarmBlocks = 32;

static uint64_t render_hash(audio_fx_api_v2_t *api, void *inst, float peak,
                            double *rms_out = nullptr)
{
    std::vector<int16_t> sig;
    for (int warm = 0; warm < kWarmBlocks; warm++) {
        fill_signal(sig, peak);
        api->process_block(inst, sig.data(), kFrames);
    }
    fill_signal(sig, peak);
    api->process_block(inst, sig.data(), kFrames);

    uint64_t h = 1469598103934665603ULL;
    double acc = 0.0;
    const uint8_t *p = (const uint8_t *)sig.data();
    for (size_t i = 0; i < sig.size() * sizeof(int16_t); i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    for (size_t i = 0; i < sig.size(); i++) {
        const double v = sig[i] / 32768.0;
        acc += v * v;
    }
    if (rms_out) *rms_out = std::sqrt(acc / (double)sig.size());
    return h;
}

static std::string get(audio_fx_api_v2_t *api, void *inst, const char *key)
{
    char buf[16384];
    const int n = api->get_param(inst, key, buf, (int)sizeof buf);
    if (n < 0) return std::string();
    buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;
    return std::string(buf);
}

static void set(audio_fx_api_v2_t *api, void *inst, const char *key, const char *val)
{
    api->set_param(inst, key, val);
}

/* ---- the control surface, and what each control needs to be audible --- */

struct Probe {
    const char *key;
    const char *value;      /* set away from default */
    const char *prereq_key; /* may be null */
    const char *prereq_val;
};

/* A band at exactly 0 dB is bypassed and reset by the core, so its frequency,
 * Q and shape are genuinely inert until the band has gain. Same for the
 * filters and their enables. Giving each probe its prerequisite is what makes
 * "the audio changed" a real assertion rather than a coin flip. */
static const Probe kProbes[] = {
    { "lf_gain",      "8",     nullptr,       nullptr },
    { "lm_gain",      "-8",    nullptr,       nullptr },
    { "hm_gain",      "8",     nullptr,       nullptr },
    { "hf_gain",      "8",     nullptr,       nullptr },
    { "hpf_freq",     "300",   "hpf_enabled", "On"    },
    { "lpf_freq",     "4000",  "lpf_enabled", "On"    },
    { "input_gain",   "9",     nullptr,       nullptr },
    { "output_gain",  "-9",    nullptr,       nullptr },
    { "lf_freq",      "450",   "lf_gain",     "10"    },
    { "lm_freq",      "2500",  "lm_gain",     "10"    },
    { "hm_freq",      "7000",  "hm_gain",     "10"    },
    { "hf_freq",      "16000", "hf_gain",     "10"    },
    { "lm_q",         "3",     "lm_gain",     "10"    },
    { "hm_q",         "3",     "hm_gain",     "10"    },
    { "lf_bell",      "On",    "lf_gain",     "10"    },
    { "hf_bell",      "On",    "hf_gain",     "10"    },
    /* Both enables are now DERIVED from their dial (the console's OUT
     * detent), so the prerequisite that raises the dial already switches them
     * on. Probing them "On" therefore sets what is already set and nothing
     * moves — which is correct behaviour and a useless assertion. Probe the
     * other direction instead: with the dial raised, switching the enable
     * OFF must take the filter out and change the audio. That also keeps the
     * independent-set path under test, which automation relies on. */
    { "hpf_enabled",  "Off",   "hpf_freq",    "300"   },
    { "lpf_enabled",  "Off",   "lpf_freq",    "4000"  },
    { "eq_type",      "Black", "lm_gain",     "10"    },
    { "auto_gain",    "On",    "lf_gain",     "12"    },
    { "oversampling", "4x",    "hf_gain",     "12"    },
    { "bypass",       "On",    "lm_gain",     "12"    },
};
static const int kNumProbes = (int)(sizeof kProbes / sizeof kProbes[0]);

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s ./4k-eq.so [module.json]\n", argv[0]);
        return 2;
    }

    printf("== dlopen ==\n");
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("  FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    ok(true, "dlopen %s", argv[1]);

    auto init = (audio_fx_init_v2_fn)dlsym(h, AUDIO_FX_INIT_V2_SYMBOL);
    ok(init != nullptr, "resolve %s", AUDIO_FX_INIT_V2_SYMBOL);
    if (!init) return 1;

    memset(&g_host_api, 0, sizeof g_host_api);
    g_host_api.sample_rate      = MOVE_SAMPLE_RATE;
    g_host_api.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    g_host_api.log              = host_log;

    audio_fx_api_v2_t *api = init(&g_host_api);
    ok(api != nullptr, "init returned an api");
    if (!api) return 1;
    ok(api->api_version == AUDIO_FX_API_VERSION_2, "api_version == 2");
    ok(api->create_instance && api->destroy_instance && api->process_block
       && api->set_param && api->get_param, "all required entry points present");

    void *inst = api->create_instance(".", nullptr);
    ok(inst != nullptr, "create_instance");
    if (!inst) return 1;

    printf("\n== contracts ==\n");
    const std::string cp = get(api, inst, "chain_params");
    ok(!cp.empty() && cp[0] == '[', "chain_params served and is an array");
    {
        int missing = 0;
        for (int i = 0; i < kNumProbes; i++) {
            const std::string pat = std::string("\"key\":\"") + kProbes[i].key + "\"";
            if (cp.find(pat) == std::string::npos) {
                printf("       chain_params is missing %s\n", kProbes[i].key);
                missing++;
            }
        }
        ok(missing == 0, "chain_params declares all %d controls", kNumProbes);
    }

    /* module.json vs the live plugin.
     *
     * These are two independent copies of the same list and the host reads
     * the FILE, not the plugin. A key the plugin serves but the file omits is
     * a control the host never offers, with nothing failing anywhere to say
     * so — a known way to ship a module that looks fine and is not. Compare
     * the key sets rather than trusting that a generator was re-run. */
    if (argc > 2) {
        FILE *f = fopen(argv[2], "rb");
        ok(f != nullptr, "module.json opens (%s)", argv[2]);
        if (f) {
            std::string mj;
            char chunk[4096];
            size_t got;
            while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) mj.append(chunk, got);
            fclose(f);

            /* keys the plugin serves */
            std::vector<std::string> served;
            for (size_t at = cp.find("\"key\":\""); at != std::string::npos;
                 at = cp.find("\"key\":\"", at + 1)) {
                const size_t b = at + 7, e = cp.find('"', b);
                if (e == std::string::npos) break;
                served.push_back(cp.substr(b, e - b));
            }
            int absent = 0;
            for (const std::string &k : served) {
                const std::string pat = "\"key\": \"" + k + "\"";
                const std::string pat2 = "\"key\":\"" + k + "\"";
                if (mj.find(pat) == std::string::npos && mj.find(pat2) == std::string::npos) {
                    printf("       module.json omits %s\n", k.c_str());
                    absent++;
                }
            }
            ok(!served.empty(), "plugin serves %d chain_params keys", (int)served.size());
            ok(absent == 0, "module.json declares every key the plugin serves");
        }
    }

    const std::string uh = get(api, inst, "ui_hierarchy");
    /* One page per band. Check every level is present AND that each band's
     * own controls live on it — a hierarchy that merely parses can still put
     * a control on the wrong page, which is the mistake worth catching. */
    {
        static const char *const kLevels[] = { "root", "lf", "lmf", "hmf", "hf", "presets" };
        for (const char *lv : kLevels) {
            const std::string pat = std::string("\"") + lv + "\":{";
            ok(uh.find(pat) != std::string::npos, "ui_hierarchy has level %s", lv);
        }
        /* every control must appear exactly once across all the knobs[] */
        int placed = 0, dupes = 0;
        for (int i = 0; i < kNumProbes; i++) {
            const std::string pat = std::string("\"") + kProbes[i].key + "\"";
            int n = 0;
            for (size_t at = uh.find("\"knobs\":["); at != std::string::npos;
                 at = uh.find("\"knobs\":[", at + 1)) {
                const size_t end = uh.find(']', at);
                if (end == std::string::npos) break;
                const std::string span = uh.substr(at, end - at);
                for (size_t f = span.find(pat); f != std::string::npos; f = span.find(pat, f + 1))
                    n++;
            }
            const bool derived = !strcmp(kProbes[i].key, "hpf_enabled")
                              || !strcmp(kProbes[i].key, "lpf_enabled");
            if (derived) {
                /* Folded into the filter dial, like the console's OUT detent.
                 * They must NOT have a knob — a switch beside the dial is the
                 * thing that let an HPF sit at 168 Hz doing nothing. */
                if (n != 0) { printf("       %s still has a knob\n", kProbes[i].key); dupes++; }
                continue;
            }
            if (n == 1) placed++;
            else if (n > 1) { printf("       %s is on %d pages\n", kProbes[i].key, n); dupes++; }
            else printf("       %s is on NO page\n", kProbes[i].key);
        }
        ok(placed == kNumProbes - 2 && dupes == 0,
           "all %d controls sit on exactly one page, both enables derived (%d placed)",
           kNumProbes - 2, placed);
    }
    ok(uh.find("\"list_param\":\"preset\"") != std::string::npos,
       "ui_hierarchy declares the preset browser");
    /* A selector listed in knobs[] is ignored by the planner and would be
     * dead travel; make sure we never start doing it. Scan every knobs[]
     * array rather than doing arithmetic on a single offset — the offset
     * version was off by one and "passed" for the wrong reason. */
    {
        bool preset_in_knobs = false;
        for (size_t at = uh.find("\"knobs\":["); at != std::string::npos;
             at = uh.find("\"knobs\":[", at + 1)) {
            const size_t end = uh.find(']', at);
            if (end == std::string::npos) break;
            if (uh.substr(at, end - at).find("\"preset\"") != std::string::npos)
                preset_in_knobs = true;
        }
        ok(!preset_in_knobs, "preset is never listed as a knob");
    }

    printf("\n== every control changes the audio ==\n");
    const uint64_t base = render_hash(api, inst, 0.5f);
    for (int i = 0; i < kNumProbes; i++) {
        const Probe &p = kProbes[i];
        /* fresh instance per probe: no leakage from the previous one */
        void *ti = api->create_instance(".", nullptr);
        if (!ti) { ok(false, "%s: create_instance", p.key); continue; }
        if (p.prereq_key) set(api, ti, p.prereq_key, p.prereq_val);
        const uint64_t before = render_hash(api, ti, 0.5f);
        set(api, ti, p.key, p.value);
        double rms = 0.0;
        const uint64_t after = render_hash(api, ti, 0.5f, &rms);
        ok(before != after, "%-13s %-6s -> audio changed (rms %.4f)", p.key, p.value, rms);
        /* and it reads back as what we set */
        const std::string rb = get(api, ti, p.key);
        ok(!rb.empty(), "%-13s reads back (%s)", p.key, rb.c_str());
        api->destroy_instance(ti);
    }
    (void)base;

    printf("\n== bypass is a bit-exact dry path ==\n");
    {
        void *ti = api->create_instance(".", nullptr);
        set(api, ti, "lf_gain", "12");
        set(api, ti, "hf_gain", "-12");
        set(api, ti, "bypass", "On");
        /* settle the 30 ms crossfade before comparing */
        std::vector<int16_t> sig;
        for (int w = 0; w < 64; w++) { fill_signal(sig, 0.5f); api->process_block(ti, sig.data(), kFrames); }
        std::vector<int16_t> in;
        fill_signal(in, 0.5f);
        std::vector<int16_t> out = in;
        api->process_block(ti, out.data(), kFrames);
        ok(memcmp(in.data(), out.data(), in.size() * sizeof(int16_t)) == 0,
           "bypassed output is byte-identical to input");
        ok(atoi(get(api, ti, "latency").c_str()) == 0,
           "bypassed latency reports 0");
        api->destroy_instance(ti);
    }

    printf("\n== state round-trip ==\n");
    {
        void *a = api->create_instance(".", nullptr);
        set(api, a, "lf_gain", "6.5");
        set(api, a, "lm_freq", "1750");
        set(api, a, "eq_type", "Black");
        set(api, a, "hpf_enabled", "On");
        set(api, a, "hpf_freq", "120");
        const std::string blob = get(api, a, "state");
        const uint64_t ha = render_hash(api, a, 0.5f);

        void *b = api->create_instance(".", nullptr);
        set(api, b, "state", blob.c_str());
        const uint64_t hb = render_hash(api, b, 0.5f);
        ok(!blob.empty() && blob[0] == '{', "state is a JSON object");
        ok(ha == hb, "a restored instance renders identically");
        ok(get(api, b, "lf_gain") == get(api, a, "lf_gain"), "lf_gain survived");
        ok(get(api, b, "eq_type") == "Black", "eq_type survived as an enum name");
        api->destroy_instance(a);
        api->destroy_instance(b);
    }

    printf("\n== factory programs ==\n");
    {
        void *ti = api->create_instance(".", nullptr);
        const int n = atoi(get(api, ti, "preset_count").c_str());
        ok(n == 15, "preset_count is 15 (INIT + 14 factory)");
        uint64_t flat = 0;
        for (int i = 0; i < n; i++) {
            char v[16];   /* 8 tripped -Wformat-truncation on aarch64 g++:
                           * it cannot know i is 0..14 */
            snprintf(v, sizeof v, "%d", i);
            set(api, ti, "preset", v);
            const std::string nm = get(api, ti, "preset_name");
            const uint64_t hh = render_hash(api, ti, 0.5f);
            if (i == 0) { flat = hh; ok(nm == "INIT", "preset 0 is INIT"); }
            else ok(hh != flat && !nm.empty(), "preset %2d %-24s changes the audio", i, nm.c_str());
        }
        /* INIT must actually return to flat */
        set(api, ti, "preset", "0");
        ok(render_hash(api, ti, 0.5f) == flat, "INIT returns to the flat default");
        api->destroy_instance(ti);
    }

    printf("\n== int16 clip reporting ==\n");
    {
        void *ti = api->create_instance(".", nullptr);
        render_hash(api, ti, 0.5f);
        get(api, ti, "clip");                      /* clear */
        ok(atoi(get(api, ti, "clip").c_str()) == 0, "no clipping at -6 dBFS, flat");
        set(api, ti, "lf_gain", "15");
        set(api, ti, "input_gain", "12");
        render_hash(api, ti, 0.99f);
        ok(atoi(get(api, ti, "clip").c_str()) > 0, "hard boost on a hot input reports clipping");
        ok(atoi(get(api, ti, "clip").c_str()) == 0, "reading clip clears it");
        api->destroy_instance(ti);
    }

    printf("\n== realtime factor ==\n");
    {
        /* Four states per oversampling mode, because "CPU usage" is not one
         * number and the interesting part is what you pay when you are NOT
         * using the EQ.
         *
         * The console nonlinearity can never be switched off — upstream's
         * consoleSatAmount() returns 0.25 native in Brown and 0.50 in Black
         * even with the saturation control at zero — so it is inside EVERY
         * row below, bypass included is the only exception. FLAT is therefore
         * very close to "the saturator, the gains and the meters", which is
         * what a master-bus insert sitting at unity actually costs.
         *
         * A band at exactly 0 dB is bypassed and reset by the core, and a
         * pair-correction only runs when BOTH its members are non-zero, so
         * FLAT really does skip the biquads rather than running them at
         * unity. */
        struct State { const char *name; int bands; bool filters; bool byp; };
        static const State kStates[] = {
            { "bypass",   0, false, true  },
            { "flat",     0, false, false },
            { "2 bands",  2, false, false },
            { "all-live", 4, true,  false },
        };
        const char *names[3] = { "1x", "2x", "4x" };
        for (int m = 0; m < 3; m++) {
          for (const State &st : kStates) {
            void *ti = api->create_instance(".", nullptr);
            set(api, ti, "oversampling", names[m]);
            if (st.byp) set(api, ti, "bypass", "On");
            if (st.filters) {
                set(api, ti, "hpf_freq", "80");
                set(api, ti, "lpf_freq", "12000");
            }
            if (st.bands >= 2) { set(api, ti, "lf_gain", "6");  set(api, ti, "hf_gain", "3"); }
            if (st.bands >= 4) { set(api, ti, "lm_gain", "-4"); set(api, ti, "hm_gain", "5"); }

            std::vector<int16_t> sig;
            fill_signal(sig, 0.5f);
            const int blocks = 44100 * 5 / MOVE_FRAMES_PER_BLOCK;   /* 5 s */
            auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; b++)
                api->process_block(ti, sig.data(), MOVE_FRAMES_PER_BLOCK);
            auto t1 = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(t1 - t0).count();
            const double audio = (double)blocks * MOVE_FRAMES_PER_BLOCK / 44100.0;
            const double frac = secs / audio;
            const double per_block_us = secs / blocks * 1e6;
            printf("  --   %-2s %-9s: %5.2f%% of one core, %6.1f us/block\n",
                   names[m], st.name, frac * 100.0, per_block_us);
            /* The SPI callback budget is ~900 us for EVERYTHING in the chain. */
            if (st.bands == 4)
                ok(per_block_us < 400.0, "%s worst case stays under 400 us per block", names[m]);
            api->destroy_instance(ti);
          }
        }
    }

    printf("\n== the snapshot a remote panel reads ==\n");
    {
        void *ti = api->create_instance(".", nullptr);
        set(api, ti, "lf_gain", "12");

        /* THE READOUTS MUST BE IN THE STATE BLOB.
         *
         * schwung-manager pushes values to a browser by reading "<comp>:state"
         * (fetchAllParams), never by walking chain_params. A readout the plugin
         * only serves from get_param is therefore invisible to a remote panel —
         * which is exactly how the meters, the clip flag, the band centres and
         * the curve all arrived as `undefined` in the browser while every local
         * test passed. Declaring them in chain_params was not enough. */
        const std::string blob = get(api, ti, "state");
        ok(!blob.empty() && blob[0] == '{' && blob[blob.size()-1] == '}',
           "state blob is complete JSON (%d bytes, not truncated)", (int)blob.size());
        static const char *const kMustBeInState[] = {
            "in_peak_l", "in_peak_r", "out_peak_l", "out_peak_r", "clip",
            "build",
        };
        int missing = 0;
        for (const char *k : kMustBeInState) {
            const std::string pat = std::string("\"") + k + "\":";
            if (blob.find(pat) == std::string::npos) {
                printf("       state blob omits %s — a remote panel cannot see it\n", k);
                missing++;
            }
        }
        ok(missing == 0, "state blob carries every readout the panel needs");
        api->destroy_instance(ti);
    }

    printf("\n== the filter dial carries OUT ==\n");
    {
        /* Turning the dial off its OUT endpoint must engage the filter, and
         * back to the endpoint must disengage it — one control, as on the
         * console. Checked through set_param, and checked in the AUDIO too,
         * because an enable flag that flips without changing the sound would
         * pass a state-only test. */
        void *ti = api->create_instance(".", nullptr);
        ok(get(api, ti, "hpf_enabled") == "Off", "HPF starts out of circuit");
        const uint64_t flat = render_hash(api, ti, 0.5f);
        set(api, ti, "hpf_freq", "250");
        ok(get(api, ti, "hpf_enabled") == "On", "turning HPF up engages it");
        ok(render_hash(api, ti, 0.5f) != flat, "and the audio changes");
        set(api, ti, "hpf_freq", "16");
        ok(get(api, ti, "hpf_enabled") == "Off", "back to OUT disengages it");
        ok(render_hash(api, ti, 0.5f) == flat, "and the audio returns to flat");

        ok(get(api, ti, "lpf_enabled") == "Off", "LPF starts out of circuit");
        set(api, ti, "lpf_freq", "5000");
        ok(get(api, ti, "lpf_enabled") == "On", "turning LPF down engages it");
        set(api, ti, "lpf_freq", "15201");
        ok(get(api, ti, "lpf_enabled") == "Off", "back to OUT disengages it");

        /* The enables stay independently settable for automation. */
        set(api, ti, "hpf_freq", "120");
        set(api, ti, "hpf_enabled", "Off");
        ok(get(api, ti, "hpf_enabled") == "Off",
           "hpf_enabled can still be set directly, dial untouched");
        ok(get(api, ti, "hpf_freq") == "120", "and the dial keeps its value");
        api->destroy_instance(ti);
    }

    printf("\n== a restored state is literal, not re-derived ==\n");
    {
        /* A blob holding a raised dial AND a disengaged filter must come back
         * exactly that way. If the dial's fan-out ran during restore it would
         * switch the filter on and quietly change a saved set. */
        void *a = api->create_instance(".", nullptr);
        set(api, a, "hpf_freq", "250");
        set(api, a, "hpf_enabled", "Off");
        const std::string blob = get(api, a, "state");
        void *b = api->create_instance(".", nullptr);
        set(api, b, "state", blob.c_str());
        ok(get(api, b, "hpf_freq") == "250", "restored dial is 250");
        ok(get(api, b, "hpf_enabled") == "Off", "restored enable stayed Off");
        api->destroy_instance(a); api->destroy_instance(b);
    }

    api->destroy_instance(inst);
    dlclose(h);

    printf("\n== %d checks, %d failed ==\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
