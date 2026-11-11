#ifndef KASTLE_NOISE_TABLE_H
#define KASTLE_NOISE_TABLE_H
#define KASTLE_NOISE_SAMPLE_CELLS 629   /* original TR_HH sample length */
#define KASTLE_NOISE_TABLE_LEN 16384    /* extended, covers full phase range */
extern const signed char kastle_noise_table[KASTLE_NOISE_TABLE_LEN];
#endif
