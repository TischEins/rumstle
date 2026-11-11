/*
 * kastle_vco.h — Portable C port of the Bastl Instruments Kastle v1.5 VCO
 *
 * Based on kastleSynthe_VCO_2 firmware by Vaclav Pelousek (2017),
 * https://github.com/bastl-instruments/kastle — open source CC BY-SA.
 * Refactoring reference: https://github.com/sx107/kastle-refactored
 *
 * This derivative work is likewise licensed CC BY-SA 4.0.
 *
 * Design notes:
 *  - The original runs its audio ISR at 15625 Hz (Timer1, PLL 32 MHz / 8 / 256)
 *    with 8-bit PWM output. This port runs the identical integer core at that
 *    internal rate and resamples to the host sample rate (AUM / CoreAudio).
 *  - Zero-order hold resampling by default: the aliasing IS the Kastle sound.
 *    Set `interpolate = 1` for a slightly tamer top end.
 *  - Two outputs, exactly like the hardware: out1 (main PWM) and out2
 *    (secondary waveform / chord / pseudo-noise). Map them to L/R or mix.
 *  - No allocations, no dependencies, no globals. Safe for the audio thread:
 *    call the setters from any thread (they only write ints), process on the
 *    render thread.
 */

#ifndef KASTLE_VCO_H
#define KASTLE_VCO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KASTLE_INTERNAL_RATE 15625.0f

typedef enum {
    KASTLE_MODE_FM    = 0,  /* phase modulation / phase distortion (mode pin floating) */
    KASTLE_MODE_NOISE = 1,  /* noise sample mangling (mode pin -> GND) */
    KASTLE_MODE_TAH   = 2   /* track & hold + chord stack (mode pin -> VCC) */
} KastleMode;

typedef struct {
    /* --- control state (written by setters) --- */
    uint16_t adc_pitch;      /* 0..1023, PITCH input */
    uint16_t adc_rate;       /* 0..1023, TIMBRE/osc2 input */
    uint8_t  adc_ws;         /* 0..255,  WAVESHAPE input (WS_2) */
    uint8_t  mode;           /* KastleMode */
    bool     xy_mode;        /* easter egg: out2 = out1 shifted 90 degrees */
    bool     interpolate;    /* 0 = zero-order hold (authentic), 1 = linear */
    bool     dc_block;       /* one-pole DC blocker on outputs (default on) */

    /* --- derived frequencies (parameterModified) --- */
    uint16_t frequency1, frequency2, frequency4, frequency5, frequency6;

    /* --- oscillator core state --- */
    uint16_t phase1, phase2, phase4, phase5, phase6;
    uint8_t  phase3;
    uint8_t  sample1, sample2;      /* current 8-bit PWM values */
    uint8_t  prev1, prev2;          /* previous, for interpolation */
    uint8_t  saw, lastSaw;          /* FM out2 saw core */
    uint8_t  phs;                   /* T&H held phase */

    /* --- resampler + filters --- */
    float ratio;                    /* internal rate / host rate */
    float acc;                      /* resampler accumulator */
    float dcx1L, dcy1L, dcx1R, dcy1R;
} KastleVCO;

/* Initialize with the host (CoreAudio) sample rate, e.g. 44100 or 48000. */
void kastle_vco_init(KastleVCO *v, float host_sample_rate);

/* Call if the host sample rate changes. */
void kastle_vco_set_sample_rate(KastleVCO *v, float host_sample_rate);

/* Parameters, normalized 0..1 (mapped to the original ADC ranges). */
void kastle_vco_set_pitch(KastleVCO *v, float value01);
void kastle_vco_set_rate(KastleVCO *v, float value01);      /* timbre / osc2 */
void kastle_vco_set_waveshape(KastleVCO *v, float value01);
void kastle_vco_set_mode(KastleVCO *v, KastleMode mode);
void kastle_vco_set_xy_mode(KastleVCO *v, bool on);

/* Render. out1 = main output, out2 = secondary. Either may be NULL.
 * Buffers are non-interleaved float, roughly -1..+1. */
void kastle_vco_process(KastleVCO *v, float *out1, float *out2, int nframes);

#ifdef __cplusplus
}
#endif
#endif /* KASTLE_VCO_H */
