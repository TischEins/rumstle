#include "kastle_lfo.h"
#include <stdlib.h>

static const uint8_t kRunglerMap[8] = {0, 80, 120, 150, 180, 200, 220, 255};
/* wsMap curve-warp table from the firmware: {in points}, {out points} */
static const uint16_t kWsMap[10] = {0, 60, 127, 191, 255, 50, 127, 190, 230, 254};

/* curveMap(value, 5, wsMap) from firmware, incl. Arduino map()'s integer truncation */
static int curve_map(int value)
{
    int inMin = 0, inMax = 255, outMin = 0, outMax = 255;
    int i;
    for (i = 0; i < 4; i++) {
        if (value >= kWsMap[i] && value <= kWsMap[i + 1]) {
            inMin = kWsMap[i];     inMax = kWsMap[i + 1];
            outMin = kWsMap[5+i];  outMax = kWsMap[5+i+1];
            break;
        }
    }
    if (inMax == inMin) return outMin;
    return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin; /* truncating int div, matches AVR */
}

/* setFrequency(int _freq) from firmware, translated to a Hz value for the ISR */
static float compute_isr_freq(int raw)
{
    int freq = (2048 - (raw << 3)) + 20;
    int preScaler = ((freq >> 7) + 1) & 0xF; /* only 4 bits (CS13:10) actually get written */
    int compare, ocr1a;
    float divisor;
    if (preScaler == 0) return 0.0f; /* timer stopped */
    compare = freq & 0xFF;
    compare &= 0x7F; /* bitWrite(compare,7,0) */
    ocr1a = (compare + 128) & 0xFF;
    divisor = (float)(1 << (preScaler - 1)); /* AVR CS1x prescaler table */
    return 8000000.0f / ((float)(ocr1a + 1) * divisor);
}

void kastle_lfo_init(KastleLFO *l, float host_sample_rate)
{
    l->chaos_cv = 155; /* classic XOR-invert mode, same as an unpatched Bit In jack */
    l->acc = 0.0f;
    l->isr_freq = 0.0f;
    l->lfo_value = 0;
    l->lfo_flop = true;
    l->rungler_byte = 0xB7; /* fixed non-zero seed (firmware uses random() at boot) */
    l->rungler_out = kRunglerMap[0];
    l->out = 0;
    kastle_lfo_set_sample_rate(l, host_sample_rate);
    kastle_lfo_set_rate(l, 0.5f);
}

void kastle_lfo_set_sample_rate(KastleLFO *l, float host_sample_rate)
{
    l->host_sample_rate = host_sample_rate;
    if (l->isr_freq > 0.0f) l->ratio = l->isr_freq / host_sample_rate;
}

void kastle_lfo_set_rate(KastleLFO *l, float value01)
{
    int raw8;
    if (value01 < 0.0f) value01 = 0.0f;
    if (value01 > 1.0f) value01 = 1.0f;
    raw8 = (int)(value01 * 255.0f + 0.5f);
    l->isr_freq = compute_isr_freq(curve_map(raw8));
    l->ratio = l->isr_freq / l->host_sample_rate;
}

void kastle_lfo_reset(KastleLFO *l)
{
    l->lfo_value = 0;
    l->lfo_flop = false;
}

static inline void lfo_tick(KastleLFO *l)
{
    l->lfo_value = (uint8_t)(l->lfo_value + 1);
    if (l->lfo_value == 0) {
        int new_bit = (l->rungler_byte >> 7) & 1;
        int ro;
        l->lfo_flop = !l->lfo_flop;
        l->rungler_byte = (uint8_t)(l->rungler_byte << 1);

        if (l->chaos_cv < 150) {
            /* unchanged: deterministic feedback loop */
        } else if (l->chaos_cv > 162) {
            new_bit = rand() & 1; /* stand-in for free-running TCNT0 noise */
        } else {
            new_bit = new_bit ? 0 : 1; /* classic Rungler XOR-invert */
        }
        l->rungler_byte = (uint8_t)((l->rungler_byte & ~1) | (new_bit & 1));

        ro = 0;
        ro |= (l->rungler_byte & 1) << 0;
        ro |= ((l->rungler_byte >> 3) & 1) << 1;
        ro |= ((l->rungler_byte >> 5) & 1) << 2;
        l->rungler_out = kRunglerMap[ro];
    }
    l->out = l->lfo_flop ? (uint8_t)(255 - l->lfo_value) : l->lfo_value;
}

void kastle_lfo_process(KastleLFO *l, float *lfo_out, float *rungler_out, int nframes)
{
    int i;
    for (i = 0; i < nframes; i++) {
        l->acc += l->ratio;
        while (l->acc >= 1.0f) { lfo_tick(l); l->acc -= 1.0f; }
        if (lfo_out)     lfo_out[i]     = ((float)l->out - 128.0f) / 128.0f;
        if (rungler_out) rungler_out[i] = ((float)l->rungler_out - 128.0f) / 128.0f;
    }
}
