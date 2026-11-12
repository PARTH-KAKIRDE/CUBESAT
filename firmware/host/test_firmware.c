/*
 * Host unit tests for the slave firmware. Run with:  python firmware/build.py test
 * They drive the real firmware code through the simulated I2C bus.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bdot.h"
#include "cubesat_icd.h"
#include "ground_link.h"
#include "icd_crc16.h"
#include "icd_link.h"
#include "icd_pack.h"
#include "sim_api.h"

static int g_fail, g_checks;
#define CHECK(c)                                                                \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(c)) {                                                             \
            g_fail++;                                                           \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);               \
        }                                                                       \
    } while (0)

typedef struct {
    uint8_t cmd, status, len;
    uint8_t data[ICD_MAX_PAYLOAD];
    int crc_ok;
} resp_t;

static int parse(const uint8_t *f, resp_t *r)
{
    r->cmd = f[0];
    r->status = f[1];
    r->len = f[2];
    if (r->len > ICD_MAX_PAYLOAD) return r->crc_ok = 0;
    memcpy(r->data, &f[3], r->len);
    uint16_t c = icd_crc16(f, (size_t)(3 + r->len));
    r->crc_ok = (f[3 + r->len] == (uint8_t)c) && (f[4 + r->len] == (uint8_t)(c >> 8));
    return r->crc_ok;
}

/* Full ICD transaction: write, wait 5 ms, read; BUSY retried 3x. */
static int xact(uint8_t addr, uint8_t cmd, const uint8_t *p, uint8_t n, resp_t *r)
{
    uint8_t f[ICD_CMD_MAX_LEN], rx[ICD_RESP_FRAME_LEN];
    uint8_t len = icd_build_command(f, cmd, p, n);
    if (sim_i2c_write(addr, f, len)) return -1;
    sim_run_ms(ICD_TURNAROUND_MS);
    for (int i = 0; i <= ICD_BUSY_RETRIES; i++) {
        if (sim_i2c_read(addr, rx, ICD_RESP_FRAME_LEN)) return -1;
        if (!parse(rx, r)) return -2;
        if (r->status != ICD_ST_BUSY) return 0;
        sim_run_ms(ICD_BUSY_RETRY_MS);
    }
    return -3;
}

static uint8_t status_of(uint8_t addr, uint8_t cmd, const uint8_t *p, uint8_t n)
{
    resp_t r;
    if (xact(addr, cmd, p, n, &r)) return 0xFF;
    return r.status;
}

/* ------------------------------------------------------------------ */

static void test_crc(void)
{
    puts("crc16");
    CHECK(icd_crc16((const uint8_t *)"123456789", 9) == ICD_CRC_CHECK);
    CHECK(icd_crc16((const uint8_t *)"", 0) == 0xFFFF);
}

static void test_link_framing(void)
{
    puts("link framing");
    sim_init(1);
    uint8_t rx[40];
    resp_t r;

    /* PING works and the response is padded with zeros to 32 bytes. */
    CHECK(xact(ICD_ADDR_ADCS, ICD_CMD_PING, 0, 0, &r) == 0);
    CHECK(r.status == ICD_ST_OK && r.cmd == ICD_CMD_PING && r.len == 0);

    /* Reading 40 bytes: bytes after 32 are padding. Re-read = same frame. */
    sim_i2c_read(ICD_ADDR_ADCS, rx, 40);
    int pad_ok = 1;
    for (int i = 5; i < 40; i++) pad_ok &= rx[i] == 0;
    CHECK(pad_ok);
    uint8_t rx2[32];
    sim_i2c_read(ICD_ADDR_ADCS, rx2, 32);
    CHECK(memcmp(rx, rx2, 32) == 0);

    /* Read before the main loop ran -> BUSY with the command echoed. */
    uint8_t f[ICD_CMD_MAX_LEN];
    uint8_t n = icd_build_command(f, ICD_CMD_GET_VERSION, 0, 0);
    sim_i2c_write(ICD_ADDR_ADCS, f, n);
    sim_i2c_read(ICD_ADDR_ADCS, rx, 32);
    parse(rx, &r);
    CHECK(r.crc_ok && r.status == ICD_ST_BUSY && r.cmd == ICD_CMD_GET_VERSION);
    sim_run_ms(1);
    sim_i2c_read(ICD_ADDR_ADCS, rx, 32);
    parse(rx, &r);
    CHECK(r.status == ICD_ST_OK && r.len == 5 && r.data[3] == ICD_VERSION_MAJOR &&
          r.data[4] == ICD_VERSION_MINOR);

    /* (a) Corrupted CRC -> BAD_CRC, command never executed. */
    uint8_t mode[1] = {ICD_MODE_SAFE};
    n = icd_build_command(f, ICD_CMD_SET_MODE, mode, 1);
    f[n - 1] ^= 0x5A;
    sim_i2c_write(ICD_ADDR_ADCS, f, n);
    sim_run_ms(5);
    sim_i2c_read(ICD_ADDR_ADCS, rx, 32);
    parse(rx, &r);
    CHECK(r.status == ICD_ST_BAD_CRC);
    CHECK(sim_adcs_mode() == ICD_MODE_DETUMBLE); /* not changed */

    /* Payload bit flip also caught. */
    n = icd_build_command(f, ICD_CMD_SET_MODE, mode, 1);
    f[2] ^= 0x01;
    sim_i2c_write(ICD_ADDR_ADCS, f, n);
    sim_run_ms(5);
    sim_i2c_read(ICD_ADDR_ADCS, rx, 32);
    parse(rx, &r);
    CHECK(r.status == ICD_ST_BAD_CRC && sim_adcs_mode() == ICD_MODE_DETUMBLE);

    /* LEN byte disagrees with bytes received -> BAD_LENGTH. */
    n = icd_build_command(f, ICD_CMD_PING, 0, 0);
    f[1] = 3;
    sim_i2c_write(ICD_ADDR_ADCS, f, n);
    sim_run_ms(5);
    sim_i2c_read(ICD_ADDR_ADCS, rx, 32);
    parse(rx, &r);
    CHECK(r.status == ICD_ST_BAD_LENGTH);

    /* Too long (40 bytes) -> BAD_LENGTH. */
    uint8_t big[40];
    memset(big, 0x11, sizeof big);
    sim_i2c_write(ICD_ADDR_ADCS, big, 40);
    sim_run_ms(5);
    sim_i2c_read(ICD_ADDR_ADCS, rx, 32);
    parse(rx, &r);
    CHECK(r.status == ICD_ST_BAD_LENGTH);

    /* Unknown command. */
    CHECK(status_of(ICD_ADDR_ADCS, 0x7E, 0, 0) == ICD_ST_UNKNOWN_CMD);
    /* Known command, wrong payload length. */
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_PING, mode, 1) == ICD_ST_BAD_LENGTH);

    /* (b) Unplugged slave NACKs; replugged it answers again. */
    sim_set_present(ICD_ADDR_ADCS, 0);
    CHECK(xact(ICD_ADDR_ADCS, ICD_CMD_PING, 0, 0, &r) == -1);
    sim_set_present(ICD_ADDR_ADCS, 1);
    CHECK(xact(ICD_ADDR_ADCS, ICD_CMD_PING, 0, 0, &r) == 0 && r.status == ICD_ST_OK);

    /* (c) 10,000 PINGs, zero errors. */
    int errors = 0;
    for (int i = 0; i < 10000; i++) {
        if (xact(ICD_ADDR_TTC, ICD_CMD_PING, 0, 0, &r) != 0 || r.status != ICD_ST_OK) errors++;
    }
    CHECK(errors == 0);
}

static void test_status_and_modes(void)
{
    puts("status flags and mode rules");
    sim_init(2);
    resp_t r;

    CHECK(xact(ICD_ADDR_ADCS, ICD_CMD_GET_STATUS, 0, 0, &r) == 0 && r.len == 8);
    CHECK(r.data[ICD_STATUS_OFS_FLAGS] & ICD_SF_RESET_OCCURRED);
    CHECK(r.data[ICD_STATUS_OFS_MODE] == ICD_MODE_DETUMBLE); /* autonomous after boot */
    xact(ICD_ADDR_ADCS, ICD_CMD_GET_STATUS, 0, 0, &r);
    CHECK(!(r.data[ICD_STATUS_OFS_FLAGS] & ICD_SF_RESET_OCCURRED)); /* cleared by read */

    uint8_t m;
    m = ICD_MODE_BOOT;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_INVALID_MODE);
    m = 9;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_INVALID_MODE);
    m = ICD_MODE_NOMINAL;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);
    CHECK(sim_adcs_mode() == ICD_MODE_NOMINAL);
    m = ICD_MODE_SAFE;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);
    m = ICD_MODE_DETUMBLE;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);

    /* NO_MASTER_CONTACT after 30 s of silence; reported once, then cleared. */
    sim_run_ms(ICD_NO_MASTER_TIMEOUT_MS + 100);
    xact(ICD_ADDR_TTC, ICD_CMD_GET_STATUS, 0, 0, &r);
    CHECK(r.data[ICD_STATUS_OFS_FLAGS] & ICD_SF_NO_MASTER_CONTACT);
    xact(ICD_ADDR_TTC, ICD_CMD_GET_STATUS, 0, 0, &r);
    CHECK(!(r.data[ICD_STATUS_OFS_FLAGS] & ICD_SF_NO_MASTER_CONTACT));
    /* ADCS kept detumbling while the master was silent. */
    CHECK(sim_adcs_mode() == ICD_MODE_DETUMBLE);

    /* Uptime in seconds. */
    CHECK(icd_get_u32(&r.data[ICD_STATUS_OFS_UPTIME_S]) == sim_now() / 1000u);
}

static void test_bdot_math(void)
{
    puts("b-dot math");
    int16_t now[3] = {400, 0, 0}, prev[3] = {0, 0, 0}, d[3], dbdt[3];
    /* dB = 400 LSB = 10 uT over 1 s -> 10 uT/s; gain 50 -> duty -500 */
    bdot_compute(now, prev, 1000, 50000, d, dbdt);
    CHECK(d[0] == -500 && d[1] == 0 && d[2] == 0);
    CHECK(dbdt[0] == 1000);
    /* Saturation keeps the direction: x:y = 2:1 */
    int16_t n2[3] = {800, 400, 0};
    bdot_compute(n2, prev, 1000, 300000, d, dbdt);
    CHECK(d[0] == -1000 && d[1] == -500);
}

static void test_adcs_detumble(void)
{
    puts("adcs detumble (simulated physics, ~50 min)");
    sim_init(3);
    double w0[3], w1[3];
    sim_adcs_get_rate_dps(w0);
    double n0 = sqrt(w0[0] * w0[0] + w0[1] * w0[1] + w0[2] * w0[2]);
    resp_t r;
    /* Keep the OBC "alive" so this tests the normal path. */
    for (int k = 0; k < 3000; k++) {
        sim_run_ms(1000);
        if (k % 10 == 0) xact(ICD_ADDR_ADCS, ICD_CMD_GET_STATUS, 0, 0, &r);
    }
    sim_adcs_get_rate_dps(w1);
    double n1 = sqrt(w1[0] * w1[0] + w1[1] * w1[1] + w1[2] * w1[2]);
    printf("  rate %.2f -> %.2f deg/s\n", n0, n1);
    CHECK(n1 < n0 * 0.3);
    CHECK(sim_adcs_dirty_mag_samples() == 0);

    uint8_t page = ADCS_TLM_PAGE_SENSORS;
    CHECK(xact(ICD_ADDR_ADCS, ICD_CMD_GET_TELEMETRY, &page, 1, &r) == 0);
    CHECK(r.status == ICD_ST_OK && r.len == ADCS_TLM0_LEN);
    uint16_t age = icd_get_u16(&r.data[ADCS_TLM0_OFS_MAG_AGE_MS]);
    CHECK(age < ADCS_PERIOD_MS_DEFAULT + 100);
    int16_t mx = icd_get_i16(&r.data[ADCS_TLM0_OFS_MAG_X]);
    int16_t mz = icd_get_i16(&r.data[ADCS_TLM0_OFS_MAG_Z]);
    double bnorm = sqrt((double)mx * mx + (double)mz * mz) * ADCS_MAG_NT_PER_LSB / 1000.0;
    CHECK(bnorm > 5.0 && bnorm < 80.0);
    page = 7;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_GET_TELEMETRY, &page, 1) == ICD_ST_BAD_PARAM);
}

static void test_adcs_commands_and_faults(void)
{
    puts("adcs commands and faults");
    sim_init(4);
    resp_t r;
    uint8_t p[8];

    /* MTQ_MANUAL only in NOMINAL. */
    icd_put_i16(&p[0], 500);
    icd_put_i16(&p[2], 0);
    icd_put_i16(&p[4], -500);
    icd_put_u16(&p[6], 2000);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_MTQ_MANUAL, p, 8) == ICD_ST_INVALID_MODE);
    uint8_t m = ICD_MODE_NOMINAL;
    status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_MTQ_MANUAL, p, 8) == ICD_ST_OK);
    sim_run_ms(1500);
    uint8_t page = ADCS_TLM_PAGE_SUN_MTQ;
    xact(ICD_ADDR_ADCS, ICD_CMD_GET_TELEMETRY, &page, 1, &r);
    CHECK(icd_get_i16(&r.data[ADCS_TLM1_OFS_DUTY_X]) == 500);
    sim_run_ms(2000);
    xact(ICD_ADDR_ADCS, ICD_CMD_GET_TELEMETRY, &page, 1, &r);
    CHECK(icd_get_i16(&r.data[ADCS_TLM1_OFS_DUTY_X]) == 0); /* expired */
    CHECK(sim_adcs_dirty_mag_samples() == 0);
    icd_put_i16(&p[0], 1500);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_MTQ_MANUAL, p, 8) == ICD_ST_BAD_PARAM);

    /* Config round trip. */
    icd_put_u32(p, 123456);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_BDOT_GAIN, p, 4) == ICD_ST_OK);
    icd_put_u32(p, ADCS_BDOT_GAIN_MAX + 1);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_BDOT_GAIN, p, 4) == ICD_ST_BAD_PARAM);
    icd_put_u16(&p[0], 300);
    icd_put_u16(&p[2], 200); /* enter <= exit: refused */
    icd_put_u16(&p[4], 10);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_DETUMBLE_THRESH, p, 6) == ICD_ST_BAD_PARAM);
    icd_put_u16(&p[0], 500);
    icd_put_u16(&p[2], 30);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_BDOT_TIMING, p, 4) == ICD_ST_OK);
    CHECK(xact(ICD_ADDR_ADCS, ADCS_CMD_GET_CONFIG, 0, 0, &r) == 0 && r.len == ADCS_CFG_LEN);
    CHECK(icd_get_u32(&r.data[ADCS_CFG_OFS_GAIN]) == 123456);
    CHECK(icd_get_u16(&r.data[ADCS_CFG_OFS_PERIOD_MS]) == 500);

    /* Magnetometer failure in DETUMBLE -> SAFE, fault flag set. */
    m = ICD_MODE_DETUMBLE;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);
    sim_adcs_set_faults(SIM_ADCS_FAIL_MAG);
    sim_run_ms(5000);
    CHECK(sim_adcs_mode() == ICD_MODE_SAFE);
    xact(ICD_ADDR_ADCS, ICD_CMD_GET_STATUS, 0, 0, &r);
    CHECK(icd_get_u16(&r.data[ICD_STATUS_OFS_FAULTS]) & ADCS_FLT_MAG);
    CHECK(r.data[ICD_STATUS_OFS_FLAGS] & ICD_SF_FAULT_ACTIVE);
    /* Cannot command DETUMBLE without a magnetometer. */
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_HW_FAULT);

    /* Fault goes away; OBC silent for 30 s -> ADCS resumes detumble alone. */
    sim_adcs_set_faults(0);
    sim_run_ms(70000);
    CHECK(sim_adcs_mode() == ICD_MODE_DETUMBLE);

    /* Driver fault persists -> SAFE. */
    sim_adcs_set_faults(SIM_ADCS_FAIL_DRVY);
    sim_run_ms(5000);
    CHECK(sim_adcs_mode() == ICD_MODE_SAFE);
    xact(ICD_ADDR_ADCS, ICD_CMD_GET_STATUS, 0, 0, &r);
    CHECK(icd_get_u16(&r.data[ICD_STATUS_OFS_FAULTS]) & ADCS_FLT_DRVY); /* sticky */
    sim_adcs_set_faults(0);
    xact(ICD_ADDR_ADCS, ICD_CMD_GET_STATUS, 0, 0, &r);
    CHECK(!(icd_get_u16(&r.data[ICD_STATUS_OFS_FAULTS]) & ADCS_FLT_DRVY)); /* cleared after read */
}

static uint8_t mk_ground(uint8_t *o, uint8_t sat, uint8_t seq)
{
    o[0] = sat;
    o[1] = GL_T_CMD;
    o[2] = seq;
    o[3] = GL_TGT_OBC;
    o[4] = GL_OBC_PING;
    o[5] = 0;
    uint16_t c = icd_crc16(o, 6);
    o[6] = (uint8_t)c;
    o[7] = (uint8_t)(c >> 8);
    return 8;
}

static void test_ttc(void)
{
    puts("ttc radio, queues, beacon, burn wire");
    sim_init(5);
    resp_t r;
    uint8_t pkt[24], buf[255];

    CHECK(sim_ttc_mode() == ICD_MODE_NOMINAL);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_GET_RX, 0, 0) == ICD_ST_NO_DATA);

    /* A valid ground packet becomes pending; a foreign one is dropped. */
    sim_ttc_uplink(pkt, mk_ground(pkt, GL_SAT_ID, 7), -90);
    sim_ttc_uplink(pkt, mk_ground(pkt, 0x42, 8), -95);
    sim_run_ms(10);
    xact(ICD_ADDR_TTC, ICD_CMD_GET_STATUS, 0, 0, &r);
    CHECK(r.data[ICD_STATUS_OFS_FLAGS] & ICD_SF_GROUND_PKT_PENDING);
    CHECK(xact(ICD_ADDR_TTC, TTC_CMD_GET_RX, 0, 0, &r) == 0 && r.status == ICD_ST_OK);
    CHECK(r.len == 8 && r.data[2] == 7);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_GET_RX, 0, 0) == ICD_ST_NO_DATA);

    /* QUEUE_TX goes out on the radio. Drain the early beacon first. */
    sim_run_ms(6000);
    while (sim_ttc_downlink(buf)) {
    }
    uint8_t hello[5] = {GL_SAT_ID, GL_T_ACK, 1, 2, 3};
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX, hello, 5) == ICD_ST_OK);
    sim_run_ms(200);
    CHECK(sim_ttc_downlink(buf) == 5 && memcmp(buf, hello, 5) == 0);
    /* Queue full -> QUEUE_FULL (radio still busy with the first). */
    int full = 0;
    for (int i = 0; i < 12; i++)
        if (status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX, hello, 5) == ICD_ST_QUEUE_FULL) full = 1;
    CHECK(full);
    sim_run_ms(3000);
    while (sim_ttc_downlink(buf)) {
    }

    /* Beacon: TT&C's own when no OBC beacon was set. */
    sim_run_ms(31000);
    int n = sim_ttc_downlink(buf);
    CHECK(n == GL_BT_LEN && buf[1] == GL_T_BEACON_TTC);
    /* OBC beacon replaces it once set. */
    uint8_t ob[GL_BO_LEN] = {GL_SAT_ID, GL_T_BEACON_OBC};
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_BEACON, ob, GL_BO_LEN) == ICD_ST_OK);
    sim_run_ms(31000);
    n = sim_ttc_downlink(buf);
    CHECK(n == GL_BO_LEN && buf[1] == GL_T_BEACON_OBC);

    /* Burn wire: needs ARM key, RBF removed, and stops at the deadline. */
    uint8_t d[4];
    icd_put_u16(d, 2000);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_BURN_FIRE, d, 2) == ICD_ST_INVALID_MODE); /* not armed */
    icd_put_u32(d, 0x12345678);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_BURN_ARM, d, 4) == ICD_ST_BAD_PARAM);
    icd_put_u32(d, TTC_BURN_ARM_KEY);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_BURN_ARM, d, 4) == ICD_ST_OK);
    sim_ttc_set_pins(0, 0); /* RBF inserted */
    icd_put_u16(d, 2000);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_BURN_FIRE, d, 2) == ICD_ST_HW_FAULT);
    sim_ttc_set_pins(0, 1);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_BURN_FIRE, d, 2) == ICD_ST_OK);
    CHECK(sim_ttc_burn_on());
    sim_run_ms(2100);
    CHECK(!sim_ttc_burn_on());
    uint32_t total = sim_ttc_burn_total_ms();
    CHECK(total >= 1990 && total <= 2010);
    /* Re-fire needs a new ARM. */
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_BURN_FIRE, d, 2) == ICD_ST_INVALID_MODE);

    /* SAFE refuses QUEUE_TX. */
    uint8_t m = ICD_MODE_SAFE;
    status_of(ICD_ADDR_TTC, ICD_CMD_SET_MODE, &m, 1);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX, hello, 5) == ICD_ST_INVALID_MODE);

    /* Radio config validation. */
    uint8_t rc[TTC_RADIO_LEN];
    icd_put_u32(rc, 436000000);
    rc[4] = 13; rc[5] = 1; rc[6] = 5; rc[7] = 10; rc[8] = 0x12;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_BAD_PARAM);
    rc[4] = 10;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_OK);
    sim_run_ms(100);
    CHECK(xact(ICD_ADDR_TTC, TTC_CMD_GET_RADIO, 0, 0, &r) == 0 && r.data[4] == 10);

    /* Radio dead at boot -> SAFE; recovers -> NOMINAL by itself. */
    sim_init(6);
    sim_ttc_set_radio_ok(0);
    sim_reset_slave(ICD_ADDR_TTC);
    CHECK(sim_ttc_mode() == ICD_MODE_SAFE);
    sim_ttc_set_radio_ok(1);
    sim_run_ms(11000);
    CHECK(sim_ttc_mode() == ICD_MODE_NOMINAL);
}

int main(void)
{
    test_crc();
    test_link_framing();
    test_status_and_modes();
    test_bdot_math();
    test_adcs_commands_and_faults();
    test_ttc();
    test_adcs_detumble();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
