/* TEST ONLY emergency-command key for the host tests and the simulation:
 * bytes 0x00..0x1F. The real key comes from TTC_KEY_FILE (firmware/build.py). */
#include <stdint.h>

const uint8_t ttc_ecmd_key[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                  16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
const uint8_t ttc_ecmd_key_len = 32;
const uint32_t ttc_ecmd_ctr_floor = 100; /* tests: counters 1..100 count as "recorded before the build" */
