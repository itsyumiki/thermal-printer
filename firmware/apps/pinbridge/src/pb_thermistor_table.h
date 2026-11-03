/* Stock head-thermistor resistance table, extracted from the original firmware.
 * Source: decrypted stock app.bin (sha256 7ed879435a5a29d1606743c61cabdc965a34c590dae88f0d6f066ebb0b797fa7)
 * Flash address 0x01E148B0, 101 little-endian u32 entries, strictly descending.
 * Index i is the head temperature (i - 20) degC for the 30 kOhm @ 25 degC NTC class part;
 * entry 20 = 100000 ohm (0 degC), entry 45 = 30000 ohm (25 degC).
 * See thermal-printer/re/docs/thermistor.md and re/scripts/thermistor_model.py. */
#ifndef PB_THERMISTOR_TABLE_H
#define PB_THERMISTOR_TABLE_H
#include <stdint.h>
#define PB_THERM_TABLE_N 101u
static const uint32_t pb_therm_table[PB_THERM_TABLE_N] = {
    269000u, 256800u, 244600u, 232400u, 220200u, 208000u,
    202000u, 196000u, 190000u, 184000u, 178000u, 167200u,
    156400u, 145600u, 134800u, 124000u, 119200u, 114400u,
    109600u, 104800u, 100000u, 95600u, 91200u, 86800u,
    82400u, 78000u, 74400u, 70800u, 67200u, 63000u,
    60000u, 57420u, 54840u, 52260u, 49680u, 47100u,
    45180u, 43260u, 41340u, 39420u, 37500u, 36000u,
    34500u, 33000u, 31500u, 30000u, 28840u, 27680u,
    26520u, 25360u, 24200u, 23280u, 22360u, 21440u,
    20520u, 19600u, 18860u, 18120u, 17380u, 16640u,
    15900u, 15340u, 14780u, 14220u, 13660u, 13100u,
    12640u, 12180u, 11720u, 11260u, 10800u, 10422u,
    10044u, 9666u, 9288u, 8910u, 8610u, 8310u,
    8010u, 7710u, 7410u, 7168u, 6926u, 6684u,
    6442u, 6200u, 6002u, 5804u, 5606u, 5408u,
    5210u, 5048u, 4886u, 4724u, 4562u, 4400u,
    4268u, 4136u, 4004u, 3872u, 3740u,
};
#endif
