/*
 * kastle_lfo.h — Portable C port of the Bastl Instruments Kastle v1.5
 * LFO/Rungler firmware core.
 *
 * Based on kastleSynth_LFO firmware by Vaclav Pelousek (2016),
 * https://github.com/bastl-instruments/kastle — open source CC BY-SA.
 * This derivative work is likewise licensed CC BY-SA 4.0.
 *
 * Design notes:
 *  - On the real hardware this runs on the second ATtiny85 alongside the
 *    VCO chip. A single timer ISR, whose rate is set by the RATE pot,
 *    drives two things off one clock: a triangle LFO (0..255, wrapping
 *    every 512 ticks) and a Rungler — Rob Hordijk's shift-register +
 *    feedback circuit — clocked every 256 ticks (twice per LFO cycle).
 *  - setFrequency()'s AVR Timer1 prescaler/compare math is reproduced
 *    exactly (including the 4-bit truncation of the prescaler field) so
 *    the RATE pot's response curve matches hardware, right up to the
 *    point where cranking it fully clockwise turns the "LFO" into an
 *    audio-rate tone — a known, intentional Kastle/Rungler quirk.
 *  - The Rungler's three feedback modes (loop / classic XOR-invert /
 *    noise) are selected by `chaos_cv`, standing in for the analog
 *    "Bit In" jack on real hardware. Not yet patchable from this port —
 *    defaults to the classic XOR-invert mode, same as an unpatched jack.
 *  - No allocations, no dependencies, no globals. Setters may be called
 *    from any thread; process on the render thread.
 */

#ifndef KASTLE_LFO_H
#define KASTLE_LFO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* --- control state --- */
    uint8_t  chaos_cv;       /* stand-in for the "Bit In" jack, 0..255. 150..162 = classic XOR mode */
    float    isr_freq;       /* derived ISR rate in Hz (can exceed audio rate at fast settings) */
    float    ratio;          /* isr_freq / host_sample_rate */
    float    acc;            /* resampler accumulator */
    float    host_sample_rate;

    /* --- core state --- */
    uint8_t  lfo_value;      /* uint8_t counter, wraps every 256 ticks */
    bool     lfo_flop;       /* half-cycle flag */
    uint8_t  rungler_byte;   /* shift register */
    uint8_t  rungler_out;    /* looked up through rungler_map, one of 8 fixed levels */
    uint8_t  out;            /* triangle LFO output, 0..255 */
} KastleLFO;

/* Initialize with the host sample rate, e.g. 44100 or 48000. */
void kastle_lfo_init(KastleLFO *l, float host_sample_rate);

/* Call if the host sample rate changes. */
void kastle_lfo_set_sample_rate(KastleLFO *l, float host_sample_rate);

/* Rate pot, normalized 0..1 — matches the firmware's RATE input exactly,
 * including the exponential-ish warp and the audio-rate top end. */
void kastle_lfo_set_rate(KastleLFO *l, float value01);

/* External LFO RST edge (patch point on real hardware, unused so far). */
void kastle_lfo_reset(KastleLFO *l);

/* Render nframes. lfo_out/rungler_out are each roughly -1..+1, non-interleaved.
 * Either may be NULL. */
void kastle_lfo_process(KastleLFO *l, float *lfo_out, float *rungler_out, int nframes);

#ifdef __cplusplus
}
#endif
#endif /* KASTLE_LFO_H */
