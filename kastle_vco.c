/*
 * kastle_vco.c — Portable C port of the Bastl Instruments Kastle v1.5 VCO
 * See kastle_vco.h for provenance and license (CC BY-SA).
 *
 * The integer DSP below is a line-for-line transplant of the original
 * ATtiny85 firmware (ISR + synthesis() + setFrequency/setFrequency2),
 * with AVR registers replaced by struct state. Intentional quirks of the
 * original (wraparound, out-of-range table reads, negative comparisons)
 * are preserved, only made well-defined.
 */

#include "kastle_vco.h"
#include "kastle_noise_table.h"

/* Sine wavetable, identical to the firmware (128 half-wave points,
 * mirrored to 256 at init). Values from bastl-instruments/kastle. */
static const uint8_t sine_half[128] = {
  0,0,0,0,1,1,1,2,2,3,4,5,5,6,7,9,10,11,12,14,15,17,18,20,21,23,25,27,29,31,
  33,35,37,40,42,44,47,49,52,54,57,59,62,65,67,70,73,76,79,82,85,88,90,93,97,
  100,103,106,109,112,115,118,121,124,128,131,134,137,140,143,146,149,152,155,
  158,162,165,167,170,173,176,179,182,185,188,190,193,196,198,201,203,206,208,
  211,213,215,218,220,222,224,226,228,230,232,234,235,237,238,240,241,243,244,
  245,246,248,249,250,250,251,252,253,253,254,254,254,255,255,255
};

static uint8_t wavetable[256];
static bool wavetable_ready = false;

/* Harmonic multipliers for the T&H chord stack (firmware `multiplier[24]`). */
static const uint8_t freq_multiplier[24] = {
  2,2,2,3,1,2,4,4, 3,4,5,2,1,5,6,8, 3,8,7,8,7,6,8,16
};

static void build_wavetable(void) {
    for (int i = 0; i < 128; ++i) wavetable[i] = sine_half[i];
    wavetable[128] = 255;
    for (int i = 129; i < 256; ++i) wavetable[i] = wavetable[256 - i];
    wavetable_ready = true;
}

/* ---- firmware: setFrequency() ---- */
static void update_frequency1(KastleVCO *v) {
    int32_t input = v->adc_pitch;
    if (v->mode == KASTLE_MODE_NOISE)
        v->frequency1 = (uint16_t)(((input - 200) << 2) + 1);
    else
        v->frequency1 = (uint16_t)((input << 2) + 1);
}

/* ---- firmware: setFrequency2() ---- */
static void update_frequency2(KastleVCO *v) {
    int32_t input = v->adc_rate;
    if (v->mode == KASTLE_MODE_NOISE) {
        v->frequency2 = (uint16_t)(((((input - 300) << 2) + 1) / 2));
    } else if (v->mode == KASTLE_MODE_TAH) {
        uint8_t mi = v->adc_ws >> 5;                    /* 0..7 */
        v->frequency2 = (uint16_t)((input << 2) + 1);
        v->frequency4 = (uint16_t)((v->frequency2 + 1) * freq_multiplier[mi]);
        v->frequency5 = (uint16_t)((v->frequency2 - 3) * freq_multiplier[mi + 8]);
        v->frequency6 = (uint16_t)((v->frequency2 + 7) * freq_multiplier[mi + 16]);
    } else { /* FM */
        v->frequency2 = (uint16_t)((input << 2) + 1);
        v->frequency4 = v->frequency2;
        v->frequency5 = v->frequency2;
    }
}

static void update_all_frequencies(KastleVCO *v) {
    update_frequency1(v);
    update_frequency2(v);
}

/* ---- firmware: ISR(TIMER1_COMPA_vect) + synthesis(), one 15625 Hz tick ---- */
static void tick(KastleVCO *v) {
    v->prev1 = v->sample1;
    v->prev2 = v->sample2;

    /* ISR body */
    v->phase1 += v->frequency1;
    v->phase2 += v->frequency2;
    v->phase4 += v->frequency4;
    v->phase5 += v->frequency5;

    if (v->mode == KASTLE_MODE_FM) {
        uint8_t phs = (uint8_t)((v->phase1 +
            (uint16_t)(v->adc_ws * wavetable[v->phase2 >> 8])) >> 6);
        v->phs = phs;
        v->sample1 = wavetable[phs];
    } else {
        v->phase3 = (uint8_t)(v->phase2 >> 5);
        v->phase6 += v->frequency6;
    }

    /* loop()/synthesis() body */
    if (v->mode == KASTLE_MODE_FM) {
        if (v->xy_mode) {
            uint8_t phs90 = (uint8_t)(v->phs + 64);
            v->sample2 = wavetable[phs90];
        } else {
            v->lastSaw = v->saw;
            v->saw = (uint8_t)((((255 - (v->phase1 >> 8)) * v->adc_ws)) >> 8);
            v->sample2 = (uint8_t)(
                ((v->saw * wavetable[v->phase4 >> 8]) >> 8) +
                ((wavetable[v->phase5 >> 8] * (255 - v->adc_ws)) >> 8));
            if (v->lastSaw < v->saw) v->phase4 = (uint16_t)(64 << 8); /* hard sync */
            uint8_t shft = (uint8_t)(v->saw > v->lastSaw ? v->saw - v->lastSaw
                                                          : v->lastSaw - v->saw);
            if (shft > 3) v->phase5 += (uint16_t)(shft << 8);          /* soft sync */
        }
    }
    else if (v->mode == KASTLE_MODE_NOISE) {
        /* signed comparison intentional: for ws < 100 the phase pins to 0,
         * exactly like the AVR int-promotion behavior */
        if ((int32_t)(v->phase1 >> 2) >= (((int32_t)v->adc_ws - 100) << 5))
            v->phase1 = 0;
        uint16_t idx = (uint16_t)((v->phase1 >> 2) & (KASTLE_NOISE_TABLE_LEN - 1));
        uint8_t smp = (uint8_t)kastle_noise_table[idx];
        v->sample1 = (uint8_t)((smp * wavetable[v->phase2 >> 8]) >> 8);
        v->sample2 = wavetable[(uint8_t)(v->phase3 + (v->phase1 >> 8))];
    }
    else { /* KASTLE_MODE_TAH */
        if ((v->phase2 >> 8) > v->adc_ws) {
            v->phs = (uint8_t)(v->phase1 >> 8);
            v->sample1 = wavetable[v->phs];
        }
        if (v->xy_mode) {
            uint8_t phs90 = (uint8_t)(v->phs + 64);
            v->sample2 = wavetable[phs90];
        } else {
            v->sample2 = (uint8_t)((wavetable[v->phase2 >> 8] +
                                    wavetable[v->phase4 >> 8] +
                                    wavetable[v->phase5 >> 8] +
                                    wavetable[v->phase6 >> 8]) >> 2);
        }
    }
}

/* ---- public API ---- */

void kastle_vco_init(KastleVCO *v, float host_sample_rate) {
    if (!wavetable_ready) build_wavetable();
    KastleVCO zero = {0};
    *v = zero;
    v->mode = KASTLE_MODE_FM;
    v->adc_pitch = 512;
    v->adc_rate  = 512;
    v->adc_ws    = 128;
    v->dc_block  = true;
    v->interpolate = false;
    kastle_vco_set_sample_rate(v, host_sample_rate);
    update_all_frequencies(v);
}

void kastle_vco_set_sample_rate(KastleVCO *v, float host_sample_rate) {
    if (host_sample_rate < 8000.0f) host_sample_rate = 44100.0f;
    v->ratio = KASTLE_INTERNAL_RATE / host_sample_rate;
    v->acc = 0.0f;
}

static uint16_t clamp10(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return (uint16_t)(x * 1023.0f + 0.5f);
}

void kastle_vco_set_pitch(KastleVCO *v, float x) {
    v->adc_pitch = clamp10(x);
    update_frequency1(v);
}

void kastle_vco_set_rate(KastleVCO *v, float x) {
    v->adc_rate = clamp10(x);
    update_frequency2(v);
}

void kastle_vco_set_waveshape(KastleVCO *v, float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    v->adc_ws = (uint8_t)(x * 255.0f + 0.5f);
    /* WS feeds the T&H multiplier table -> refresh derived freqs */
    update_frequency2(v);
}

void kastle_vco_set_mode(KastleVCO *v, KastleMode mode) {
    v->mode = (uint8_t)mode;
    update_all_frequencies(v);
}

void kastle_vco_set_xy_mode(KastleVCO *v, bool on) { v->xy_mode = on; }

static inline float dcblock(float in, float *x1, float *y1) {
    /* y[n] = x[n] - x[n-1] + 0.995 y[n-1] */
    float y = in - *x1 + 0.995f * *y1;
    *x1 = in; *y1 = y;
    return y;
}

void kastle_vco_process(KastleVCO *v, float *out1, float *out2, int nframes) {
    for (int i = 0; i < nframes; ++i) {
        v->acc += v->ratio;
        while (v->acc >= 1.0f) {
            tick(v);
            v->acc -= 1.0f;
        }
        float s1, s2;
        if (v->interpolate) {
            float f = v->acc; /* 0..1 position between prev and current */
            s1 = ((1.0f - f) * v->prev1 + f * v->sample1 - 128.0f) * (1.0f / 128.0f);
            s2 = ((1.0f - f) * v->prev2 + f * v->sample2 - 128.0f) * (1.0f / 128.0f);
        } else {
            s1 = (v->sample1 - 128.0f) * (1.0f / 128.0f);
            s2 = (v->sample2 - 128.0f) * (1.0f / 128.0f);
        }
        if (v->dc_block) {
            s1 = dcblock(s1, &v->dcx1L, &v->dcy1L);
            s2 = dcblock(s2, &v->dcx1R, &v->dcy1R);
        }
        if (out1) out1[i] = s1;
        if (out2) out2[i] = s2;
    }
}
