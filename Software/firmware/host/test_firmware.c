/*
 * Host unit tests for the slave firmware. Run with:  python firmware/build.py test
 * They drive the real firmware code through the simulated I2C bus.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdot.h"
#include "cubesat_icd.h"
#include "ground_link.h"
#include "hmac_sha256.h"
#include "icd_crc16.h"
#include "icd_link.h"
#include "icd_pack.h"
#include "sim_api.h"
#include "ttc_app.h"
#include "ttc_hw.h"
#include "ttc_nv.h"

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


/* ---------------- v0.5 / review fixes ---------------- */

static int s_runs;
static uint8_t count_dispatch(void *ctx, uint8_t cmd, const uint8_t *req, uint8_t n, uint8_t *resp,
                              uint8_t *resp_len)
{
    (void)ctx; (void)cmd; (void)req; (void)n; (void)resp;
    *resp_len = 0;
    s_runs++;
    return ICD_ST_OK;
}

static void test_link_no_double_run(void)
{
    puts("link: an extra end-of-transfer after a read never re-runs the last command");
    icd_link_t l;
    icd_link_init(&l, count_dispatch, 0);
    uint8_t f[ICD_CMD_MAX_LEN], p[2] = {0xE8, 0x03};
    uint8_t n = icd_build_command(f, 0x47, p, 2); /* e.g. BURN_FIRE */
    s_runs = 0;
    icd_link_isr_write_begin(&l);
    for (int i = 0; i < n; i++) icd_link_isr_rx_byte(&l, f[i]);
    icd_link_isr_write_end(&l);
    CHECK(icd_link_poll(&l) == 1 && s_runs == 1);
    icd_link_isr_read_begin(&l);
    for (int i = 0; i < ICD_RESP_FRAME_LEN; i++) (void)icd_link_isr_tx_byte(&l);
    icd_link_isr_read_end(&l);   /* AF: the master NACKed the last byte */
    icd_link_isr_write_end(&l);  /* a driver that also reported STOP as a write end */
    CHECK(icd_link_poll(&l) == 0 && s_runs == 1);
}

static int tlm(uint8_t page, resp_t *r)
{
    return xact(ICD_ADDR_ADCS, ICD_CMD_GET_TELEMETRY, &page, 1, r) == 0 && r->status == ICD_ST_OK;
}

static void reboot_adcs(void)
{
    sim_reset_slave(ICD_ADDR_ADCS);
}

static void set_dwell(uint16_t dwell_s)
{
    uint8_t p[6];
    icd_put_u16(&p[0], ADCS_EXIT_RATE_DEFAULT);
    icd_put_u16(&p[2], ADCS_ENTER_RATE_DEFAULT);
    icd_put_u16(&p[4], dwell_s);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_DETUMBLE_THRESH, p, 6) == ICD_ST_OK);
}

static void test_adcs_gyro_offset(void)
{
    puts("adcs gyro offset: measured at boot when still, rejected when moving, commandable");
    resp_t r;
    const double bx = 2.0, by = -1.5, bz = 1.0; /* dps: |bias| = 2.69 > the 2.00 exit rate */
    sim_init(7);
    sim_adcs_set_rate_dps(0, 0, 0);
    sim_adcs_set_gyro_bias_dps(bx, by, bz);
    reboot_adcs();
    sim_run_ms(1000);
    CHECK(tlm(ADCS_TLM_PAGE_GYRO_CAL, &r) && r.len == ADCS_TLM3_LEN);
    CHECK(r.data[ADCS_TLM3_OFS_CAL_STATE] == ADCS_GCAL_RUNNING);
    /* The OBC sends its magnetometer offset right after boot: must not spoil it. */
    uint8_t mo[6];
    icd_put_i16(&mo[0], 1000);
    icd_put_i16(&mo[2], -500);
    icd_put_i16(&mo[4], 200);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_MAG_OFFSET, mo, 6) == ICD_ST_OK);
    sim_run_ms(5000);
    CHECK(tlm(ADCS_TLM_PAGE_GYRO_CAL, &r));
    CHECK(r.data[ADCS_TLM3_OFS_CAL_STATE] == ADCS_GCAL_AUTO);
    int16_t ox = icd_get_i16(&r.data[ADCS_TLM3_OFS_GYRO_OFS_X]);
    int16_t oy = icd_get_i16(&r.data[ADCS_TLM3_OFS_GYRO_OFS_Y]);
    int16_t oz = icd_get_i16(&r.data[ADCS_TLM3_OFS_GYRO_OFS_Z]);
    printf("  measured offset %d %d %d LSB (true %.0f %.0f %.0f)\n", ox, oy, oz, bx * 65.5, by * 65.5, bz * 65.5);
    CHECK(abs(ox - 131) <= 3 && abs(oy + 98) <= 3 && abs(oz - 66) <= 3);
    CHECK(r.data[ADCS_TLM3_OFS_CAL_SAMPLES] >= ADCS_GYRO_CAL_MIN_SAMPLES);
    CHECK(icd_get_u16(&r.data[ADCS_TLM3_OFS_GYRO_SPREAD]) <= ADCS_GYRO_CAL_MAX_SPREAD);
    CHECK(icd_get_u16(&r.data[ADCS_TLM3_OFS_MAG_SPREAD]) <= ADCS_GYRO_CAL_MAX_MAG_SPREAD);
    CHECK(tlm(ADCS_TLM_PAGE_SENSORS, &r));
    CHECK(abs(icd_get_i16(&r.data[ADCS_TLM0_OFS_GYRO_X])) <= 4); /* page 0 gyro: offset removed */
    set_dwell(2);
    sim_run_ms(4000);
    CHECK(tlm(ADCS_TLM_PAGE_CTRL, &r) && (r.data[ADCS_TLM2_OFS_CTRL_FLAGS] & ADCS_CF_DETUMBLED));

    /* Boot while tumbling: REJECTED, offset 0. Once the spin is gone the bias
     * alone keeps the rate above 2 dps: DETUMBLED never comes (the v0.4 bug). */
    sim_adcs_set_rate_dps(10, -8, 6);
    reboot_adcs();
    sim_run_ms(6000);
    CHECK(tlm(ADCS_TLM_PAGE_GYRO_CAL, &r));
    CHECK(r.data[ADCS_TLM3_OFS_CAL_STATE] == ADCS_GCAL_REJECTED);
    CHECK(icd_get_i16(&r.data[ADCS_TLM3_OFS_GYRO_OFS_X]) == 0);
    CHECK(icd_get_u16(&r.data[ADCS_TLM3_OFS_MAG_SPREAD]) > ADCS_GYRO_CAL_MAX_MAG_SPREAD);
    sim_adcs_set_rate_dps(0, 0, 0);
    set_dwell(2);
    sim_run_ms(4000);
    CHECK(tlm(ADCS_TLM_PAGE_CTRL, &r) && !(r.data[ADCS_TLM2_OFS_CTRL_FLAGS] & ADCS_CF_DETUMBLED));
    /* The OBC re-sends the stored value: fixed. */
    uint8_t g[6];
    icd_put_i16(&g[0], 131);
    icd_put_i16(&g[2], -98);
    icd_put_i16(&g[4], 66);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_GYRO_OFFSET, g, 6) == ICD_ST_OK);
    sim_run_ms(4000);
    CHECK(tlm(ADCS_TLM_PAGE_CTRL, &r) && (r.data[ADCS_TLM2_OFS_CTRL_FLAGS] & ADCS_CF_DETUMBLED));
    CHECK(tlm(ADCS_TLM_PAGE_GYRO_CAL, &r) && r.data[ADCS_TLM3_OFS_CAL_STATE] == ADCS_GCAL_COMMANDED);

    /* Limits. */
    icd_put_i16(&g[0], ADCS_GYRO_OFS_MAX + 1);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_GYRO_OFFSET, g, 6) == ICD_ST_BAD_PARAM);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_GYRO_OFFSET, g, 4) == ICD_ST_BAD_LENGTH);

    /* No IMU at boot: nothing to measure. */
    sim_adcs_set_faults(SIM_ADCS_FAIL_IMU);
    reboot_adcs();
    sim_run_ms(6000);
    CHECK(tlm(ADCS_TLM_PAGE_GYRO_CAL, &r) && r.data[ADCS_TLM3_OFS_CAL_STATE] == ADCS_GCAL_NONE);
    sim_adcs_set_faults(0);
}

static int mode_after(uint32_t ms)
{
    sim_run_ms(ms);
    return sim_adcs_mode();
}

static void test_adcs_autonomy(void)
{
    puts("adcs autonomy: dead gyro does not block recovery; a shorted driver backs off");
    resp_t r;
    sim_init(8);
    sim_run_ms(3000);
    /* Coils stop as soon as the mode changes, not at the end of the cycle. */
    CHECK(tlm(ADCS_TLM_PAGE_SUN_MTQ, &r));
    int16_t dx = icd_get_i16(&r.data[ADCS_TLM1_OFS_DUTY_X]), dy = icd_get_i16(&r.data[ADCS_TLM1_OFS_DUTY_Y]);
    CHECK(dx != 0 || dy != 0); /* tumbling: B-dot is driving */
    uint8_t m = ICD_MODE_NOMINAL;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);
    CHECK(tlm(ADCS_TLM_PAGE_SUN_MTQ, &r));
    CHECK(icd_get_i16(&r.data[ADCS_TLM1_OFS_DUTY_X]) == 0 && icd_get_i16(&r.data[ADCS_TLM1_OFS_DUTY_Y]) == 0 &&
          icd_get_i16(&r.data[ADCS_TLM1_OFS_DUTY_Z]) == 0);
    m = ICD_MODE_DETUMBLE;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);

    /* Gyro dead, then the magnetometer fails -> SAFE. The magnetometer comes
     * back; the gyro does not. B-dot does not need it: back to DETUMBLE. */
    sim_adcs_set_faults(SIM_ADCS_FAIL_IMU | SIM_ADCS_FAIL_MAG);
    CHECK(mode_after(5000) == ICD_MODE_SAFE);
    sim_adcs_set_faults(SIM_ADCS_FAIL_IMU);
    CHECK(mode_after(70000) == ICD_MODE_DETUMBLE);
    sim_adcs_set_faults(0);
    /* That was an automatic recovery, so a fault now would count as a repeat.
     * A commanded mode change ends the episode (and resets the wait). */
    m = ICD_MODE_NOMINAL;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);
    m = ICD_MODE_DETUMBLE;
    CHECK(status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);

    /* Shorted X driver: every automatic restart trips again. The time spent
     * in SAFE before each new attempt must double: 30, 60, 120, 240 s. */
    sim_adcs_set_faults(SIM_ADCS_FAIL_DRVX);
    uint32_t safe_since = 0, gaps[4];
    int k = 0, last = sim_adcs_mode();
    for (uint32_t t = 0; t < 700000u && k < 4; t += 100u) {
        sim_run_ms(100);
        int now_mode = sim_adcs_mode();
        if (now_mode == ICD_MODE_SAFE && last != ICD_MODE_SAFE) safe_since = sim_now();
        if (now_mode == ICD_MODE_DETUMBLE && last == ICD_MODE_SAFE && safe_since) gaps[k++] = sim_now() - safe_since;
        last = now_mode;
    }
    CHECK(k == 4);
    if (k == 4) {
        printf("  time in SAFE before each retry: %u %u %u %u s\n", gaps[0] / 1000, gaps[1] / 1000,
               gaps[2] / 1000, gaps[3] / 1000);
        CHECK(gaps[0] >= 30000u && gaps[0] < 40000u);
        for (int i = 1; i < 4; i++) CHECK(gaps[i] >= 2u * gaps[i - 1] - 2000u && gaps[i] <= 2u * gaps[i - 1] + 2000u);
    }
    /* Fault gone; after 10 clean minutes a new fault waits only 30 s again. */
    sim_adcs_set_faults(0);
    for (uint32_t t = 0; t < 600000u && sim_adcs_mode() != ICD_MODE_DETUMBLE; t += 1000u) sim_run_ms(1000);
    CHECK(sim_adcs_mode() == ICD_MODE_DETUMBLE);
    sim_run_ms(600000u + 5000u);
    sim_adcs_set_faults(SIM_ADCS_FAIL_DRVX);
    uint32_t t0 = sim_now();
    while (sim_adcs_mode() != ICD_MODE_SAFE && sim_now() - t0 < 10000u) sim_run_ms(100);
    CHECK(sim_adcs_mode() == ICD_MODE_SAFE);
    sim_adcs_set_faults(0);
    t0 = sim_now(); /* SAFE entered just now */
    while (sim_adcs_mode() != ICD_MODE_DETUMBLE && sim_now() - t0 < 200000u) sim_run_ms(100);
    printf("  after 10 clean minutes: back to DETUMBLE %u s after the next fault\n", (sim_now() - t0) / 1000);
    CHECK(sim_now() - t0 >= 29000u && sim_now() - t0 < 32000u);
}


static double angle_deg(const double a[3], const double b[3])
{
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    double na = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), nb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    d /= na * nb;
    if (d > 1) d = 1;
    if (d < -1) d = -1;
    return acos(d) * 180.0 / 3.14159265358979323846;
}

static void sun_page(resp_t *r, double v[3], uint8_t *flags)
{
    CHECK(tlm(ADCS_TLM_PAGE_SUN, r) && r->len == ADCS_TLM4_LEN);
    *flags = r->data[ADCS_TLM4_OFS_SUN_FLAGS];
    v[0] = icd_get_i16(&r->data[ADCS_TLM4_OFS_SUN_X]);
    v[1] = icd_get_i16(&r->data[ADCS_TLM4_OFS_SUN_Y]);
    v[2] = icd_get_i16(&r->data[ADCS_TLM4_OFS_SUN_Z]);
}

static void sun_cfg(uint16_t lit, uint16_t full)
{
    uint8_t p[4];
    icd_put_u16(&p[0], lit);
    icd_put_u16(&p[2], full);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_CONFIG, p, 4) == ICD_ST_OK);
}

static void test_adcs_sun(void)
{
#ifdef SUN_NO_MZ
    puts("adcs sun sensors (5-face build, -DSUN_NO_MZ): direction, eclipse, config");
    const uint8_t fitted = 0x1F;
#else
    puts("adcs sun sensors (6 faces): direction, eclipse, config");
    const uint8_t fitted = 0x3F;
#endif
    resp_t r;
    double v[3], truth[3];
    uint8_t fl;
    sim_init(9);
    uint8_t m = ICD_MODE_NOMINAL; /* coils off: nothing turns the satellite */
    status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1);
    /* A grid of Sun directions over the whole sphere, satellite held still. */
    sim_adcs_set_rate_dps(0, 0, 0);
    int n_direct = 0, n_blind = 0, n_dark = 0, wrong = 0;
    double worst = 0, worst_blind_deep = 0, worst_blind_horizon = 0;
    for (int el = -85; el <= 85; el += 10) {
        for (int az = 0; az < 360; az += 20) {
            double e = el * 3.14159265358979323846 / 180, a = az * 3.14159265358979323846 / 180;
            double want[3] = {cos(e) * cos(a), cos(e) * sin(a), sin(e)};
            sim_adcs_set_sun_body(want[0], want[1], want[2]);
            sim_run_ms(1100); /* one full control cycle with this attitude */
            sim_adcs_get_sun_body(truth);
            CHECK(angle_deg(truth, want) < 0.01);
            sun_page(&r, v, &fl);
            CHECK(r.data[ADCS_TLM4_OFS_FITTED_FACES] == fitted && r.data[ADCS_TLM4_OFS_DEAD_FACES] == 0);
            double side = fabs(truth[0]) > fabs(truth[1]) ? fabs(truth[0]) : fabs(truth[1]);
            if (fl & ADCS_SUNF_LIT) {
                if (!(fl & ADCS_SUNF_VECTOR_VALID) || (fl & ADCS_SUNF_PREDICTED)) { wrong++; continue; }
                double err = angle_deg(v, truth);
                if (fl & ADCS_SUNF_BLIND_SIDE) {
                    n_blind++;
                    if (truth[2] > 0.01) wrong++;
                    if (truth[2] < -0.5) { if (err > worst_blind_deep) worst_blind_deep = err; }
                    else if (err > worst_blind_horizon) worst_blind_horizon = err;
                } else {
                    n_direct++;
                    if (err > worst) worst = err;
                }
            } else {
                n_dark++; /* sunlight but no face lit: only in the blind cone of a missing diode */
                if (!(truth[2] < 0 && side < 0.45)) wrong++;
            }
        }
    }
    printf("  grid: %d direct (worst %.1f deg); %d inferred (worst %.1f deg deep, %.1f deg near the horizon); %d not lit\n",
           n_direct, worst, n_blind, worst_blind_deep, worst_blind_horizon, n_dark);
    CHECK(wrong == 0);
    CHECK(worst < 3.0);
#ifdef SUN_NO_MZ
    CHECK(n_direct == 9 * 18 && n_blind > 100 && worst_blind_deep < 5.0 && worst_blind_horizon < 20.0 && n_dark > 0);
#else
    CHECK(n_direct == 18 * 18 && n_blind == 0 && n_dark == 0); /* six faces: always measured directly */
#endif

    /* Healthy diodes, fresh tracking: going dark is an eclipse at once. */
    sim_adcs_set_sun_body(0.3, 0.8, 0.52);
    sim_run_ms(2200);
    sim_adcs_set_eclipse(1);
    sim_run_ms(1500);
    sun_page(&r, v, &fl);
    CHECK(fl == 0 && r.data[ADCS_TLM4_OFS_LIT_FACES] == 0 && icd_get_u16(&r.data[ADCS_TLM4_OFS_SUN_MAX]) < ADCS_SUN_NOISE);
    sim_adcs_set_eclipse(0);
    sim_run_ms(1500);
    sun_page(&r, v, &fl);
    CHECK(fl & ADCS_SUNF_LIT);

    /* Albedo threshold: in Earth shadow, glow from the sunlit Earth below
     * (0.35 x full, under lit = 0.4) must not read as daylight. */
    sim_adcs_set_eclipse(1);
    sim_adcs_set_albedo(0.35, -1, 0, 0); /* Earth toward -X, a face every build has */
    sim_run_ms(1500);
    sun_page(&r, v, &fl);
    CHECK(!(fl & ADCS_SUNF_LIT));
    sim_adcs_set_albedo(0.45, -1, 0, 0); /* brighter than lit: raise the threshold */
    sim_run_ms(1500);
    sun_page(&r, v, &fl);
    CHECK(fl & ADCS_SUNF_LIT);           /* this is what a too-low threshold does */
    sun_cfg(1500, 3000);
    sun_page(&r, v, &fl);
    CHECK(!(fl & ADCS_SUNF_LIT));
    sun_cfg(ADCS_SUN_LIT_DEFAULT, ADCS_SUN_FULL_DEFAULT);
    sim_adcs_set_eclipse(0);
    /* In daylight: Earth glow (0.3 x full) on the faces looking down. On the
     * face opposite the Sun it is ignored (brighter-of-pair); on a face that
     * also sees the Sun it adds a small tilt. */
    double worst_albedo = 0;
    sim_adcs_set_albedo(0.3, 0, 0, -1);
    for (int az = 0; az < 360; az += 30) {
        double a = az * 3.14159265358979323846 / 180;
        sim_adcs_set_sun_body(0.8 * cos(a), 0.8 * sin(a), 0.6);
        sim_run_ms(1100);
        sim_adcs_get_sun_body(truth);
        sun_page(&r, v, &fl);
        if ((fl & ADCS_SUNF_VECTOR_VALID) && angle_deg(v, truth) > worst_albedo) worst_albedo = angle_deg(v, truth);
    }
    printf("  albedo 0.3 x full from below (-Z), Sun 37 deg above the XY plane: worst %.1f deg\n", worst_albedo);
    CHECK(worst_albedo < 3.0);
    sim_adcs_set_albedo(0, 0, 0, 0);

    /* SET_SUN_CONFIG limits; new values show on page 4. */
    uint8_t p[4];
    icd_put_u16(&p[0], 2000);
    icd_put_u16(&p[2], 1500);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_CONFIG, p, 4) == ICD_ST_BAD_PARAM);
    icd_put_u16(&p[0], ADCS_SUN_NOISE);
    icd_put_u16(&p[2], 3000);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_CONFIG, p, 4) == ICD_ST_BAD_PARAM);
    icd_put_u16(&p[0], 1000);
    icd_put_u16(&p[2], 4096);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_CONFIG, p, 4) == ICD_ST_BAD_PARAM);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_CONFIG, p, 2) == ICD_ST_BAD_LENGTH);
    sun_cfg(1000, 3200);
    CHECK(tlm(ADCS_TLM_PAGE_SUN, &r));
    CHECK(icd_get_u16(&r.data[ADCS_TLM4_OFS_LIT_THRESHOLD]) == 1000 && icd_get_u16(&r.data[ADCS_TLM4_OFS_FULL_SCALE]) == 3200);
    sun_cfg(ADCS_SUN_LIT_DEFAULT, ADCS_SUN_FULL_DEFAULT);

    /* SET_SUN_DISABLED: the ground can switch a face off; the Sun is still found. */
    uint8_t mask = ADCS_FACE_PZ;
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_DISABLED, &mask, 1) == ICD_ST_OK);
    sim_adcs_set_sun_body(0.6, 0.3, 0.74); /* +X lit; z must come from |s| = 1 */
    sim_run_ms(1100);
    sim_adcs_get_sun_body(truth);
    sun_page(&r, v, &fl);
    CHECK(r.data[ADCS_TLM4_OFS_DEAD_FACES] == ADCS_FACE_PZ && !(r.data[ADCS_TLM4_OFS_LIT_FACES] & ADCS_FACE_PZ));
#ifndef SUN_NO_MZ
    CHECK((fl & ADCS_SUNF_VECTOR_VALID) && (fl & ADCS_SUNF_BLIND_SIDE) && angle_deg(v, truth) < 5.0);
#endif
    mask = 0x40;
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_DISABLED, &mask, 1) == ICD_ST_BAD_PARAM);
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_DISABLED, &mask, 0) == ICD_ST_BAD_LENGTH);
    mask = 0;
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_DISABLED, &mask, 1) == ICD_ST_OK);
}

static void test_adcs_sun_dead_diode(void)
{
    puts("adcs sun: a diode dies in orbit; gyro tracking tells 'hidden' from eclipse");
    resp_t r;
    double v[3], truth[3];
    uint8_t fl;
    sim_init(10);
    uint8_t m = ICD_MODE_NOMINAL;
    status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1);
    sim_adcs_set_rate_dps(0, 0, 0);
    sim_adcs_set_sun_body(0, 0.95, 0.31); /* Sun on +Y, 18 deg above the XY plane */
    sim_run_ms(2200);
    sim_adcs_set_sun_dead(ADCS_FACE_PX); /* the +X diode breaks */
    /* Spin about Z at 4 dps: the Sun walks from +Y over the broken +X face, on round. */
    sim_adcs_set_rate_dps(0, 0, 4);
    int learned_at = -1, hidden = 0, false_eclipse = 0;
    double worst_before = 0, worst_after = 0, worst_pred = 0;
    for (int t = 0; t < 120; t++) {
        sim_run_ms(1000);
        sim_adcs_get_sun_body(truth);
        sun_page(&r, v, &fl);
        if (learned_at < 0 && (r.data[ADCS_TLM4_OFS_DEAD_FACES] & ADCS_FACE_PX)) learned_at = t;
        if (!(fl & ADCS_SUNF_LIT) && !(fl & ADCS_SUNF_HIDDEN)) false_eclipse++; /* it is never really dark */
        if (fl & ADCS_SUNF_HIDDEN) hidden++;
        if (fl & ADCS_SUNF_VECTOR_VALID) {
            double e = angle_deg(v, truth);
            if (fl & ADCS_SUNF_PREDICTED) { if (e > worst_pred) worst_pred = e; }
            else if (learned_at < 0) { if (e > worst_before) worst_before = e; }
            else if (e > worst_after) worst_after = e;
        }
    }
    printf("  +X marked dead after %d s; %d s hidden (not eclipse); worst %.1f deg before it was found, %.1f deg after, "
           "%.1f deg tracked\n", learned_at, hidden, worst_before, worst_after, worst_pred);
    CHECK(learned_at >= 5 && learned_at < 25); /* only once the Sun really swings onto +X (~9 s) */
    CHECK(false_eclipse == 0 && hidden > 5);
    /* Before detection the missing light must reach 25% (deficit 0.75) to count, so the
     * direction can be ~30 deg off while the Sun creeps onto the dead face. */
    CHECK(worst_before < 35.0 && worst_after < 15.0 && worst_pred < 5.0);
    xact(ICD_ADDR_ADCS, ICD_CMD_GET_STATUS, 0, 0, &r);
    CHECK(icd_get_u16(&r.data[ICD_STATUS_OFS_FAULTS]) & ADCS_FLT_SUN_DIODE);


    /* Held with the Sun on the broken face: HIDDEN, even if an eclipse starts
     * (it cannot be told apart), until the tracking is too old (5 min). */
    /* Keep turning until the Sun is on the broken face, then stop: the gyro saw it all. */
    for (int k = 0; k < 2000; k++) {
        sim_run_ms(100);
        sim_adcs_get_sun_body(truth);
        if (truth[0] > 0.93) break;
    }
    sim_adcs_set_rate_dps(0, 0, 0);
    sim_run_ms(1100);
    sun_page(&r, v, &fl);
    CHECK(fl & ADCS_SUNF_HIDDEN);
    sim_adcs_set_eclipse(1);
    sim_run_ms(60000);
    sun_page(&r, v, &fl);
    CHECK(fl & ADCS_SUNF_HIDDEN);
    sim_run_ms(ADCS_SUN_TRACK_MAX_MS);
    sun_page(&r, v, &fl);
    CHECK(fl == 0); /* too old to trust: dark means eclipse again */
    sim_adcs_set_eclipse(0);

    /* The diode recovers: it reads lit again and is taken back. */
    sim_adcs_set_sun_dead(0);
    sim_run_ms(1100);
    sun_page(&r, v, &fl);
    CHECK((fl & ADCS_SUNF_LIT) && !(r.data[ADCS_TLM4_OFS_DEAD_FACES] & ADCS_FACE_PX));

    /* No gyro, no tracking: HIDDEN can no longer be claimed. +X switched off
     * by command, the Sun turned onto it (seen by the gyro): HIDDEN. Then the
     * IMU dies: within a second it can only say dark. */
    uint8_t mask = ADCS_FACE_PX;
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_DISABLED, &mask, 1) == ICD_ST_OK);
    sim_adcs_set_sun_body(0, 0.95, 0.31);
    sim_run_ms(2200);
    sim_adcs_set_rate_dps(0, 0, 4);
    for (int k = 0; k < 2000; k++) {
        sim_run_ms(100);
        sim_adcs_get_sun_body(truth);
        if (truth[0] > 0.93) break;
    }
    sim_adcs_set_rate_dps(0, 0, 0);
    sim_run_ms(1100);
    sun_page(&r, v, &fl);
    CHECK(fl & ADCS_SUNF_HIDDEN);
    sim_adcs_set_faults(SIM_ADCS_FAIL_IMU);
    sim_run_ms(1100);
    sun_page(&r, v, &fl);
    CHECK(fl == 0);
    sim_adcs_set_faults(0);
    mask = 0;
    CHECK(status_of(ICD_ADDR_ADCS, ADCS_CMD_SET_SUN_DISABLED, &mask, 1) == ICD_ST_OK);

    /* An eclipse with fresh tracking on healthy faces condemns no diode. */
    sim_adcs_set_sun_body(0.5, 0.5, 0.707);
    sim_run_ms(2200);
    sim_adcs_set_eclipse(1);
    sim_run_ms(20000);
    sim_adcs_set_eclipse(0);
    sim_run_ms(1100);
    sun_page(&r, v, &fl);
    CHECK(r.data[ADCS_TLM4_OFS_DEAD_FACES] == 0 && (fl & ADCS_SUNF_LIT));

    /* A wrong tracker (here: an instant jump no gyro could see) must be
     * corrected by the measurement, never condemn the good diodes. */
    sim_adcs_set_sun_body(-0.7, 0.1, 0.7);
    sim_run_ms(10000);
    sun_page(&r, v, &fl);
    CHECK(r.data[ADCS_TLM4_OFS_DEAD_FACES] == 0 && (fl & ADCS_SUNF_VECTOR_VALID) && !(fl & ADCS_SUNF_PREDICTED));

    /* A "minus" face dies (-Y): the missing light must go on the side the
     * gyro-tracked Sun favours, -Y, not the first candidate (+Y). */
    sim_init(11);
    status_of(ICD_ADDR_ADCS, ICD_CMD_SET_MODE, &m, 1);
    sim_adcs_set_rate_dps(0, 0, 0);
    sim_adcs_set_sun_body(0.95, 0, 0.31); /* Sun on +X */
    sim_run_ms(2200);
    sim_adcs_set_sun_dead(ADCS_FACE_MY);
    sim_adcs_set_rate_dps(0, 0, 4); /* +X walks over to -Y */
    int fe2 = 0;
    for (int t = 0; t < 60; t++) {
        sim_run_ms(1000);
        sun_page(&r, v, &fl);
        if (!(fl & ADCS_SUNF_LIT) && !(fl & ADCS_SUNF_HIDDEN)) fe2++;
    }
    CHECK(r.data[ADCS_TLM4_OFS_DEAD_FACES] == ADCS_FACE_MY && fe2 == 0);
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
    /* v0.9: the first thing TT&C sends is a boot EVENT. */
    sim_run_ms(300);
    CHECK(sim_ttc_downlink(buf) == GL_EV_LEN && buf[1] == GL_T_EVENT && buf[GL_EV_OFS_CMD] == GL_EV_BOOT &&
          buf[GL_EV_OFS_SOURCE] == GL_EVS_AUTO && icd_get_u16(&buf[GL_EV_OFS_NO]) == 1);

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

    /* SAFE (v0.8): command answers (ACK, TLM) still go down; anything else is refused. */
    uint8_t m = ICD_MODE_SAFE;
    status_of(ICD_ADDR_TTC, ICD_CMD_SET_MODE, &m, 1);
    sim_run_ms(3000);
    while (sim_ttc_downlink(buf)) {
    }
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX, hello, 5) == ICD_ST_OK); /* ACK */
    sim_run_ms(500);
    CHECK(sim_ttc_downlink(buf) == 5 && buf[1] == GL_T_ACK);
    uint8_t tlm[4] = {GL_SAT_ID, GL_T_TLM, 1, 0};
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX, tlm, 4) == ICD_ST_OK);
    uint8_t blk[6] = {GL_SAT_ID, GL_T_FILE, 1, 0, 0, 0};
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX, blk, 6) == ICD_ST_INVALID_MODE);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX, blk, 1) == ICD_ST_INVALID_MODE); /* no type byte */
    sim_run_ms(500);
    while (sim_ttc_downlink(buf)) {
    }

    /* Default radio (ICD v0.9): 433.3 MHz down, 433.8 MHz up, CR 4/8, 10 dBm, 13-byte layout.
     * Both 125 kHz channels inside 433.05-434.04 MHz with room for drift and Doppler. */
    CHECK(xact(ICD_ADDR_TTC, TTC_CMD_GET_RADIO, 0, 0, &r) == 0 && r.len == TTC_RADIO_LEN);
    CHECK(icd_get_u32(&r.data[TTC_RADIO_OFS_FREQ_DOWN_HZ]) == 433300000u);
    CHECK(icd_get_u32(&r.data[TTC_RADIO_OFS_FREQ_UP_HZ]) == 433800000u);
    CHECK(r.data[TTC_RADIO_OFS_CR] == 8 && r.data[TTC_RADIO_OFS_POWER_DBM] == 10);
    CHECK(TTC_FREQ_DOWN_DEFAULT_HZ - 62500 - 20000 >= 433050000);
    CHECK(TTC_FREQ_UP_DEFAULT_HZ + 62500 + 20000 <= 434040000);
    CHECK(TTC_FREQ_UP_DEFAULT_HZ > TTC_FREQ_DOWN_DEFAULT_HZ);
    /* Split frequency: listening on the uplink, transmitted on the downlink. */
    CHECK(sim_ttc_rx_freq() == TTC_FREQ_UP_DEFAULT_HZ);
    CHECK(sim_ttc_tx_freq() == TTC_FREQ_DOWN_DEFAULT_HZ);

    /* Radio config validation. */
    uint8_t rc[TTC_RADIO_LEN];
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_DOWN_HZ], 434000000);
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_UP_HZ], 435000000);
    rc[4] = 13; rc[5] = 1; rc[6] = 5; rc[7] = 10; rc[8] = 0x12;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_BAD_PARAM); /* SF 13 */
    rc[4] = 10;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, 9) == ICD_ST_BAD_LENGTH); /* old v0.7 layout */
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_UP_HZ], 434000000);   /* uplink = downlink */
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_BAD_PARAM);
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_UP_HZ], 433500000);   /* uplink below downlink */
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_BAD_PARAM);
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_UP_HZ], 438000001);   /* uplink above the band */
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_BAD_PARAM);
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_DOWN_HZ], 433049999); /* downlink below the band */
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_UP_HZ], 435000000);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_BAD_PARAM);
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_DOWN_HZ], 434000000);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_OK);
    sim_run_ms(100);
    CHECK(xact(ICD_ADDR_TTC, TTC_CMD_GET_RADIO, 0, 0, &r) == 0 && r.data[4] == 10);
    CHECK(icd_get_u32(&r.data[TTC_RADIO_OFS_FREQ_DOWN_HZ]) == 434000000u);
    CHECK(icd_get_u32(&r.data[TTC_RADIO_OFS_FREQ_UP_HZ]) == 435000000u);
    CHECK(sim_ttc_rx_freq() == 435000000u); /* re-initialised on the new uplink */

    /* Radio dead at boot -> SAFE; recovers -> NOMINAL by itself. */
    sim_init(6);
    sim_ttc_set_radio_ok(0);
    sim_reset_slave(ICD_ADDR_TTC);
    CHECK(sim_ttc_mode() == ICD_MODE_SAFE);
    sim_ttc_set_radio_ok(1);
    sim_run_ms(11000);
    CHECK(sim_ttc_mode() == ICD_MODE_NOMINAL);
}

/* ------------------------------------------------------------------ */
/* TT&C v0.9: HMAC, flash store, auto-deploy, emergency commands,      */
/* radio revert, long packets                                          */
/* ------------------------------------------------------------------ */

static int hex_eq(const uint8_t *d, const char *hex)
{
    for (int i = 0; hex[2 * i]; i++) {
        unsigned v;
        if (sscanf(&hex[2 * i], "%2x", &v) != 1 || d[i] != v) return 0;
    }
    return 1;
}

static void test_hmac_sha256(void)
{
    puts("ttc sha-256 / hmac-sha256 (FIPS 180-4, RFC 4231 vectors)");
    uint8_t h[SHA256_LEN], key[131];
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, (const uint8_t *)"abc", 3);
    sha256_final(&s, h);
    CHECK(hex_eq(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* 56 bytes: 2 blocks */
    sha256_init(&s);
    for (size_t i = 0; i < strlen(two); i++) sha256_update(&s, (const uint8_t *)&two[i], 1);
    sha256_final(&s, h);
    CHECK(hex_eq(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, h);
    CHECK(hex_eq(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, h);
    CHECK(hex_eq(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    memset(key, 0xaa, 131); /* key longer than a block: hashed first */
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(key, 131, (const uint8_t *)m6, strlen(m6), h);
    CHECK(hex_eq(h, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

static void test_ttc_nv(void)
{
    puts("ttc flash store: append-only records, torn writes, compaction");
    sim_init(11);
    ttc_nv_t nv;
    ttc_nv_load(&nv);
    CHECK(nv.ok && nv.records == 1 && ttc_nv_get(&nv, TTC_NV_INIT) == 1); /* TT&C's first boot marked it */
    CHECK(ttc_nv_get(&nv, TTC_NV_DEPLOYED) == 0);
    CHECK(ttc_nv_set(&nv, TTC_NV_ECMD_COUNTER, 41) && ttc_nv_set(&nv, TTC_NV_ECMD_COUNTER, 42));
    CHECK(ttc_nv_set(&nv, TTC_NV_DEPLOYED, 1));
    uint32_t progs = sim_ttc_nv_programs();
    CHECK(ttc_nv_set(&nv, TTC_NV_DEPLOYED, 1) && sim_ttc_nv_programs() == progs); /* unchanged: no write */
    ttc_nv_t b;
    ttc_nv_load(&b);
    CHECK(b.records == 4 && ttc_nv_get(&b, TTC_NV_ECMD_COUNTER) == 42 && ttc_nv_get(&b, TTC_NV_DEPLOYED) == 1);
    /* A record cut short by a power loss (value written, marker not) is skipped. */
    sim_ttc_nv_poke(b.next + 4, 99);
    ttc_nv_t c;
    ttc_nv_load(&c);
    CHECK(c.torn == 1 && ttc_nv_get(&c, TTC_NV_ECMD_COUNTER) == 42 && c.next == b.next + 8);
    CHECK(ttc_nv_set(&c, TTC_NV_ECMD_COUNTER, 43));
    ttc_nv_load(&c);
    CHECK(ttc_nv_get(&c, TTC_NV_ECMD_COUNTER) == 43);
    /* Fill past three-quarters: the next load compacts and keeps every value. */
    for (uint32_t i = 0; i < 60 && c.next + 8 <= c.size; i++) ttc_nv_set(&c, TTC_NV_OBC_WDT_MIN, 100 + i);
    uint32_t wdt = ttc_nv_get(&c, TTC_NV_OBC_WDT_MIN);
    CHECK(!ttc_nv_set(&c, TTC_NV_OBC_WDT_MIN, 7)); /* full: write fails, RAM still updated */
    CHECK(ttc_nv_get(&c, TTC_NV_OBC_WDT_MIN) == 7);
    ttc_nv_load(&c);
    CHECK(sim_ttc_nv_erases() == 1 && c.ok && c.next == 4 * 8); /* only the 4 non-zero keys rewritten */
    ttc_nv_load(&c);
    CHECK(c.records == 4 && ttc_nv_get(&c, TTC_NV_ECMD_COUNTER) == 43 && ttc_nv_get(&c, TTC_NV_DEPLOYED) == 1 &&
          ttc_nv_get(&c, TTC_NV_OBC_WDT_MIN) == wdt);
    sim_ttc_nv_fail(1);
    CHECK(!ttc_nv_set(&c, TTC_NV_ECMD_COUNTER, 50));
    sim_ttc_nv_fail(0);
}

static uint8_t df_now(void)
{
    resp_t r;
    if (xact(ICD_ADDR_TTC, ICD_CMD_GET_TELEMETRY, (const uint8_t[]){TTC_TLM_PAGE_RADIO}, 1, &r)) return 0xFF;
    return r.data[TTC_TLM0_OFS_DEPLOY_FLAGS];
}

static uint8_t auto_attempts(void)
{
    resp_t r;
    if (xact(ICD_ADDR_TTC, ICD_CMD_GET_TELEMETRY, (const uint8_t[]){TTC_TLM_PAGE_COUNTERS}, 1, &r)) return 0xFF;
    return r.data[TTC_TLM1_OFS_AUTO_ATTEMPTS];
}

static uint16_t ttc_faults(void)
{
    resp_t r;
    if (xact(ICD_ADDR_TTC, ICD_CMD_GET_STATUS, 0, 0, &r)) return 0xFFFF;
    return icd_get_u16(&r.data[ICD_STATUS_OFS_FAULTS]);
}

static void test_ttc_autodeploy(void)
{
    puts("ttc automatic antenna deployment with the flash flag");
    sim_init(12);
    /* Bench: RBF in, nothing ever burns. */
    sim_run_ms(200000);
    CHECK(sim_ttc_burn_total_ms() == 0 && (df_now() & TTC_DF_AUTO_ACTIVE) && auto_attempts() == 0);
    /* RBF out: burn after DELAY_S of continuous "out". */
    sim_ttc_set_pins(0, 1);
    sim_run_ms(TTC_AUTODEPLOY_DELAY_S * 1000 - 1000);
    CHECK(!sim_ttc_burn_on());
    sim_ttc_set_pins(0, 0); /* plug back in for a moment: the delay starts again */
    sim_run_ms(100);
    sim_ttc_set_pins(0, 1);
    sim_run_ms(TTC_AUTODEPLOY_DELAY_S * 1000 - 1000);
    CHECK(!sim_ttc_burn_on());
    sim_run_ms(1100);
    CHECK(sim_ttc_burn_on() && auto_attempts() == 1);
    sim_run_ms(TTC_AUTODEPLOY_BURN_MS + 100);
    uint32_t b1 = sim_ttc_burn_total_ms();
    CHECK(!sim_ttc_burn_on() && b1 >= TTC_AUTODEPLOY_BURN_MS - 10 && b1 <= TTC_AUTODEPLOY_BURN_MS + 10);
    /* Not deployed: the second try comes RETRY_S later. */
    sim_run_ms(TTC_AUTODEPLOY_RETRY_S * 1000 - 500);
    CHECK(!sim_ttc_burn_on());
    sim_run_ms(600);
    CHECK(sim_ttc_burn_on() && auto_attempts() == 2);
    /* A 10 ms glitch on the switch (burn current noise) does not end the burn. */
    sim_run_ms(500);
    sim_ttc_set_pins(1, 1);
    sim_run_ms(10);
    sim_ttc_set_pins(0, 1);
    sim_run_ms(5);
    CHECK(sim_ttc_burn_on());
    /* The OBC cannot extend a running automatic burn. */
    uint8_t kk[4];
    icd_put_u32(kk, TTC_BURN_ARM_KEY);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_BURN_ARM, kk, 4) == ICD_ST_OK);
    icd_put_u16(kk, 8000);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_BURN_FIRE, kk, 2) == ICD_ST_INVALID_MODE && sim_ttc_burn_on());
    /* The antenna opens during the burn: it stops after the 50 ms debounce, the flag 2 s later. */
    sim_run_ms(500);
    sim_ttc_set_pins(1, 1);
    sim_run_ms(TTC_DEPLOY_DEBOUNCE_MS - 5);
    CHECK(sim_ttc_burn_on());
    sim_run_ms(10);
    CHECK(!sim_ttc_burn_on());
    CHECK(!(df_now() & TTC_DF_DEPLOYED_FLAG));
    sim_run_ms(TTC_DEPLOYED_CONFIRM_MS + 10);
    CHECK((df_now() & TTC_DF_DEPLOYED_FLAG) && !(df_now() & TTC_DF_AUTO_ACTIVE));
    /* Every later boot checks the flag and never burns, even if the switch reads stowed. */
    for (int boot = 0; boot < 3; boot++) {
        sim_reset_slave(ICD_ADDR_TTC);
        sim_ttc_set_pins(0, 1);
        uint32_t before = sim_ttc_burn_total_ms();
        sim_run_ms(300000);
        CHECK(sim_ttc_burn_total_ms() == before && (df_now() & TTC_DF_DEPLOYED_FLAG) && auto_attempts() == 2);
    }
    /* DEPLOY_RESET (bench): only with the RBF plug in, and with the key. */
    uint8_t k[4];
    icd_put_u32(k, TTC_DEPLOY_RESET_KEY);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_DEPLOY_RESET, k, 4) == ICD_ST_INVALID_MODE);
    sim_ttc_set_pins(0, 0);
    icd_put_u32(k, TTC_BURN_ARM_KEY);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_DEPLOY_RESET, k, 4) == ICD_ST_BAD_PARAM);
    icd_put_u32(k, TTC_DEPLOY_RESET_KEY);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_DEPLOY_RESET, k, 3) == ICD_ST_BAD_LENGTH);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_DEPLOY_RESET, k, 4) == ICD_ST_OK);
    CHECK((df_now() & TTC_DF_AUTO_ACTIVE) && auto_attempts() == 0);

    /* Never deploys: exactly MAX_ATTEMPTS burns in the satellite's life, across reboots. */
    sim_init(13);
    sim_ttc_set_pins(0, 1);
    sim_run_ms(TTC_AUTODEPLOY_DELAY_S * 1000 + 2000); /* first burn under way */
    CHECK(sim_ttc_burn_on() && auto_attempts() == 1);
    sim_reset_slave(ICD_ADDR_TTC);                     /* brown-out mid-burn: still counted */
    CHECK(!sim_ttc_burn_on() && auto_attempts() == 1);
    sim_run_ms(400000);
    CHECK(auto_attempts() == TTC_AUTODEPLOY_MAX_ATTEMPTS && !(df_now() & TTC_DF_AUTO_ACTIVE));
    uint32_t tot = sim_ttc_burn_total_ms();
    sim_reset_slave(ICD_ADDR_TTC);
    sim_run_ms(400000);
    CHECK(sim_ttc_burn_total_ms() == tot);
    CHECK(tot >= 2000 + (TTC_AUTODEPLOY_MAX_ATTEMPTS - 1) * TTC_AUTODEPLOY_BURN_MS - 50 &&
          tot <= 2000 + (TTC_AUTODEPLOY_MAX_ATTEMPTS - 1) * TTC_AUTODEPLOY_BURN_MS + 50);

    /* Commanded SAFE (OBC: low battery) holds it; the RBF going back in stops a burn. */
    sim_init(14);
    sim_ttc_set_pins(0, 1);
    uint8_t m = ICD_MODE_SAFE;
    CHECK(status_of(ICD_ADDR_TTC, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);
    sim_run_ms(120000);
    CHECK(sim_ttc_burn_total_ms() == 0 && auto_attempts() == 0);
    m = ICD_MODE_NOMINAL;
    CHECK(status_of(ICD_ADDR_TTC, ICD_CMD_SET_MODE, &m, 1) == ICD_ST_OK);
    sim_run_ms(50);
    CHECK(sim_ttc_burn_on());
    sim_run_ms(1000);
    sim_ttc_set_pins(0, 0);
    sim_run_ms(2);
    CHECK(!sim_ttc_burn_on());
    /* Radio dead (SAFE by fault) does not hold it: the antenna is needed either way. */
    sim_init(15);
    sim_ttc_set_radio_ok(0);
    sim_reset_slave(ICD_ADDR_TTC);
    CHECK(sim_ttc_mode() == ICD_MODE_SAFE);
    sim_ttc_set_pins(0, 1);
    sim_run_ms(TTC_AUTODEPLOY_DELAY_S * 1000 + 100);
    CHECK(sim_ttc_burn_on());
    /* A flash failure is reported, and the RAM count still limits the burns. */
    sim_init(16);
    sim_ttc_nv_fail(1);
    sim_ttc_set_pins(0, 1);
    sim_run_ms(TTC_AUTODEPLOY_DELAY_S * 1000 + 100);
    CHECK(sim_ttc_burn_on() && (ttc_faults() & TTC_FLT_NV));
    sim_ttc_nv_fail(0);
}

/* ---- emergency commands ---- */

static uint32_t s_ectr = 101; /* above the test build's counter floor (100) */

static uint8_t mk_ecmd(uint8_t *o, uint8_t seq, uint8_t tgt, uint8_t cmd, const uint8_t *a, uint8_t an,
                       uint32_t ctr)
{
    o[0] = GL_SAT_ID;
    o[1] = GL_T_ECMD;
    o[2] = seq;
    o[3] = tgt;
    o[4] = cmd;
    o[5] = an;
    memcpy(&o[6], a, an);
    icd_put_u32(&o[6 + an], ctr);
    uint8_t h[SHA256_LEN];
    hmac_sha256(ttc_ecmd_key, ttc_ecmd_key_len, o, (size_t)(10 + an), h);
    memcpy(&o[10 + an], h, 4);
    uint16_t c = icd_crc16(o, (size_t)(14 + an));
    icd_put_u16(&o[14 + an], c);
    return (uint8_t)(16 + an);
}

static void drain(void)
{
    uint8_t b[255];
    while (sim_ttc_downlink(b)) {
    }
}

/* EVENTs seen by the last ecmd_raw(), with the frequency each went out on. */
static struct {
    int n;
    uint8_t cmd[8], tgt[8], result[8], src[8], eseq[8];
    uint16_t detail[8], no[8];
    uint32_t freq[8];
} s_ev;
static uint32_t s_ack_freq;

static void take_event(const uint8_t *b, int len)
{
    uint16_t c = icd_crc16(b, (size_t)(len - 2));
    CHECK(len == GL_EV_LEN && b[len - 2] == (uint8_t)c && b[len - 1] == (uint8_t)(c >> 8));
    if (s_ev.n >= 8) return;
    int i = s_ev.n++;
    s_ev.cmd[i] = b[GL_EV_OFS_CMD];
    s_ev.tgt[i] = b[GL_EV_OFS_TARGET];
    s_ev.result[i] = b[GL_EV_OFS_RESULT];
    s_ev.src[i] = b[GL_EV_OFS_SOURCE];
    s_ev.eseq[i] = b[GL_EV_OFS_ECMD_SEQ];
    s_ev.detail[i] = icd_get_u16(&b[GL_EV_OFS_DETAIL]);
    s_ev.no[i] = icd_get_u16(&b[GL_EV_OFS_NO]);
    s_ev.freq[i] = sim_ttc_tx_freq();
}

/* Run for ms, 1 ms at a time, sorting what goes down. Returns the first
 * emergency ACK's status (0xFF: none), its data into d. */
static uint8_t watch(uint32_t ms, uint8_t *d, uint8_t *dn)
{
    uint8_t b[255], st = 0xFF;
    for (uint32_t t = 0; t < ms; t++) {
        sim_run_ms(1);
        int len;
        while ((len = sim_ttc_downlink(b)) > 0) {
            if (b[1] == GL_T_EVENT) {
                take_event(b, len);
            } else if (b[1] == GL_T_ACK && (b[GL_ACK_OFS_TARGET] & GL_TGT_EMERGENCY) && st == 0xFF) {
                s_ack_freq = sim_ttc_tx_freq();
                uint16_t c = icd_crc16(b, (size_t)(len - 2));
                CHECK(b[len - 2] == (uint8_t)c && b[len - 1] == (uint8_t)(c >> 8));
                st = b[GL_ACK_OFS_STATUS];
                if (d) memcpy(d, &b[GL_ACK_OFS_DATA], b[GL_ACK_OFS_LEN]);
                if (dn) *dn = b[GL_ACK_OFS_LEN];
            }
        }
    }
    return st;
}

/* Uplink one ECMD; return the emergency ACK's status (0xFF: none), data into d. */
static uint8_t ecmd_raw(const uint8_t *pkt, uint8_t n, uint8_t *d, uint8_t *dn)
{
    drain();
    memset(&s_ev, 0, sizeof s_ev);
    sim_ttc_uplink(pkt, n, -80);
    return watch(300, d, dn);
}

static uint8_t ecmd(uint8_t tgt, uint8_t cmd, const uint8_t *a, uint8_t an, uint8_t *d, uint8_t *dn)
{
    uint8_t p[24];
    uint32_t ctr = s_ectr++;
    return ecmd_raw(p, mk_ecmd(p, (uint8_t)ctr, tgt, cmd, a, an, ctr), d, dn);
}

static void test_ttc_emergency(void)
{
    puts("ttc emergency commands: executed by TT&C itself, no OBC");
    sim_init(20);
    s_ectr = 101;
    uint8_t d[16], dn = 0, p[24], n, k[5];
    resp_t r;

    /* PING answers with no OBC on the bus at all. */
    sim_run_ms(40000);
    CHECK(ecmd(GL_TGT_TTC, GL_E_PING, 0, 0, d, &dn) == ICD_ST_OK && dn == GL_EP_LEN);
    CHECK(icd_get_u32(&d[GL_EP_OFS_UPTIME_S]) >= 40 && d[GL_EP_OFS_MODE] == ICD_MODE_NOMINAL);
    CHECK(icd_get_u16(&d[GL_EP_OFS_OBC_SILENT_S]) >= 40 && (d[GL_EP_OFS_DEPLOY_FLAGS] & TTC_DF_AUTO_ACTIVE));
    /* An ECMD never reaches the OBC queue; a normal CMD still does. */
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_GET_RX, 0, 0) == ICD_ST_NO_DATA);

    /* Reset the Pi: bad key refused, good key pulses RUN for 200 ms. */
    icd_put_u32(k, 0x12345678);
    CHECK(ecmd(GL_TGT_OBC, GL_E_OBC_RESET, k, 4, 0, 0) == ICD_ST_BAD_PARAM && sim_ttc_line_pulses(TTC_LINE_OBC_RUN) == 0);
    icd_put_u32(k, GL_REBOOT_KEY);
    n = mk_ecmd(p, 9, GL_TGT_OBC, GL_E_OBC_RESET, k, 4, s_ectr++);
    CHECK(ecmd_raw(p, n, 0, 0) == ICD_ST_OK);
    sim_run_ms(300);
    CHECK(sim_ttc_line_pulses(TTC_LINE_OBC_RUN) == 1 && !sim_ttc_line(TTC_LINE_OBC_RUN));
    CHECK(sim_ttc_line_ms(TTC_LINE_OBC_RUN) >= GL_E_OBC_RESET_PULSE_MS - 2 &&
          sim_ttc_line_ms(TTC_LINE_OBC_RUN) <= GL_E_OBC_RESET_PULSE_MS + 2);
    /* The same packet again (ACK lost): same ACK, not executed again. */
    CHECK(ecmd_raw(p, n, 0, 0) == ICD_ST_OK && sim_ttc_line_pulses(TTC_LINE_OBC_RUN) == 1);
    /* Replay of an older one, a forged MAC, a bad length: no answer, nothing done. */
    uint8_t old[24];
    uint8_t on = mk_ecmd(old, 1, GL_TGT_OBC, GL_E_OBC_RESET, k, 4, 102);
    CHECK(ecmd_raw(old, on, 0, 0) == 0xFF);
    n = mk_ecmd(p, 10, GL_TGT_OBC, GL_E_OBC_RESET, k, 4, s_ectr++);
    p[10 + 4] ^= 1;                          /* MAC byte */
    uint16_t c = icd_crc16(p, (size_t)(n - 2));
    icd_put_u16(&p[n - 2], c);
    CHECK(ecmd_raw(p, n, 0, 0) == 0xFF && sim_ttc_line_pulses(TTC_LINE_OBC_RUN) == 1);
    n = mk_ecmd(p, 11, GL_TGT_TTC, GL_E_PING, 0, 0, s_ectr++);
    p[5] = 1;                                /* LEN no longer matches */
    c = icd_crc16(p, (size_t)(n - 2));
    icd_put_u16(&p[n - 2], c);
    CHECK(ecmd_raw(p, n, 0, 0) == 0xFF);

    /* Power-cycle the Pi (GLOBAL_EN low 3 s), reset ADCS (NRST 20 ms). */
    icd_put_u32(k, GL_REBOOT_KEY);
    k[4] = 0;
    CHECK(ecmd(GL_TGT_OBC, GL_E_OBC_POWER_CYCLE, k, 5, 0, 0) == ICD_ST_BAD_PARAM);
    k[4] = GL_E_POWER_OFF_MAX_S + 1;
    CHECK(ecmd(GL_TGT_OBC, GL_E_OBC_POWER_CYCLE, k, 5, 0, 0) == ICD_ST_BAD_PARAM);
    k[4] = 3;
    CHECK(ecmd(GL_TGT_OBC, GL_E_OBC_POWER_CYCLE, k, 4, 0, 0) == ICD_ST_BAD_LENGTH);
    CHECK(ecmd(GL_TGT_OBC, GL_E_OBC_POWER_CYCLE, k, 5, 0, 0) == ICD_ST_OK);
    sim_run_ms(3000);
    CHECK(!sim_ttc_line(TTC_LINE_OBC_GLOBAL_EN) && sim_ttc_line_ms(TTC_LINE_OBC_GLOBAL_EN) >= 2998 &&
          sim_ttc_line_ms(TTC_LINE_OBC_GLOBAL_EN) <= 3002);
    CHECK(ecmd(GL_TGT_ADCS, GL_E_ADCS_RESET, k, 4, 0, 0) == ICD_ST_OK);
    CHECK(sim_ttc_line_pulses(TTC_LINE_ADCS_NRST) == 1 && sim_ttc_line_ms(TTC_LINE_ADCS_NRST) >= 18 &&
          sim_ttc_line_ms(TTC_LINE_ADCS_NRST) <= 22);
    CHECK(ecmd(GL_TGT_EPS, 0x01, 0, 0, 0, 0) == ICD_ST_UNKNOWN_CMD);
    CHECK(ecmd(GL_TGT_TTC, 0x7E, 0, 0, 0, 0) == ICD_ST_UNKNOWN_CMD);

    /* Burn from the ground: RBF must be out, works in any mode but commanded SAFE. */
    uint8_t ms[2];
    icd_put_u16(ms, 100);
    CHECK(ecmd(GL_TGT_TTC, GL_E_BURN, ms, 2, 0, 0) == ICD_ST_HW_FAULT && sim_ttc_burn_total_ms() == 0);
    sim_ttc_set_pins(0, 1);
    icd_put_u16(ms, TTC_BURN_MAX_MS + 1);
    CHECK(ecmd(GL_TGT_TTC, GL_E_BURN, ms, 2, 0, 0) == ICD_ST_BAD_PARAM);
    icd_put_u16(ms, 100);
    CHECK(ecmd(GL_TGT_TTC, GL_E_BURN, ms, 2, 0, 0) == ICD_ST_OK);
    sim_run_ms(200);
    CHECK(sim_ttc_burn_total_ms() >= 98 && sim_ttc_burn_total_ms() <= 102);
    sim_ttc_set_pins(0, 0);

    /* Beacon off, then radio silence: only emergency ACKs go out. */
    uint8_t pp[2] = {0, 0};
    CHECK(ecmd(GL_TGT_TTC, GL_E_BEACON_PERIOD, pp, 2, 0, 0) == ICD_ST_OK);
    drain();
    sim_run_ms(65000);
    uint8_t b[255];
    CHECK(sim_ttc_downlink(b) == 0);
    icd_put_u16(pp, 30);
    CHECK(ecmd(GL_TGT_TTC, GL_E_BEACON_PERIOD, pp, 2, 0, 0) == ICD_ST_OK);
    icd_put_u16(pp, GL_E_SILENCE_MAX_MIN + 1);
    CHECK(ecmd(GL_TGT_TTC, GL_E_TX_SILENCE, pp, 2, 0, 0) == ICD_ST_BAD_PARAM);
    icd_put_u16(pp, 1);
    CHECK(ecmd(GL_TGT_TTC, GL_E_TX_SILENCE, pp, 2, 0, 0) == ICD_ST_OK);
    uint8_t hello[5] = {GL_SAT_ID, GL_T_ACK, 1, 2, 3};
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX, hello, 5) == ICD_ST_OK);
    CHECK(ecmd(GL_TGT_TTC, GL_E_PING, 0, 0, d, &dn) == ICD_ST_OK && d[GL_EP_OFS_SILENCE_MIN] == 1);
    sim_run_ms(45000);
    CHECK(sim_ttc_downlink(b) == 0);         /* no beacon, the OBC packet is held */
    sim_run_ms(50000);                       /* silence over: the held packet, then the next beacon on schedule */
    int got_hello = 0, got_beacon = 0, len;
    while ((len = sim_ttc_downlink(b)) > 0) {
        if (len == 5 && memcmp(b, hello, 5) == 0) got_hello = 1;
        if (b[1] == GL_T_BEACON_TTC) got_beacon = 1;
    }
    CHECK(got_hello && got_beacon);

    /* Back to the default radio: the ACK goes out on the old settings first. */
    uint8_t rc[TTC_RADIO_LEN];
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_DOWN_HZ], 434000000);
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_UP_HZ], 435000000);
    rc[4] = 10; rc[5] = 1; rc[6] = 5; rc[7] = 10; rc[8] = 0x12;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_OK);
    sim_run_ms(100);
    CHECK(ecmd(GL_TGT_TTC, GL_E_RADIO_DEFAULTS, 0, 0, d, &dn) == ICD_ST_OK && dn == 1 &&
          d[0] == GL_EA_REPORT_FOLLOWS);
    CHECK(s_ack_freq == 434000000u);           /* the ACK on the settings the ground used */
    CHECK(s_ev.n == 1 && s_ev.cmd[0] == GL_E_RADIO_DEFAULTS && s_ev.result[0] == ICD_ST_OK &&
          s_ev.freq[0] == TTC_FREQ_DOWN_DEFAULT_HZ); /* the report on the new ones */
    CHECK(sim_ttc_rx_freq() == TTC_FREQ_UP_DEFAULT_HZ);
    CHECK(xact(ICD_ADDR_TTC, TTC_CMD_GET_RADIO, 0, 0, &r) == 0 && r.data[TTC_RADIO_OFS_SF] == TTC_SF_DEFAULT);

    /* Reset TT&C: ACK first, then the reboot; the counter survives it. */
    icd_put_u32(k, 0);
    CHECK(ecmd(GL_TGT_TTC, GL_E_TTC_RESET, k, 4, 0, 0) == ICD_ST_BAD_PARAM && sim_ttc_resets() == 0);
    icd_put_u32(k, GL_REBOOT_KEY);
    n = mk_ecmd(p, 30, GL_TGT_TTC, GL_E_TTC_RESET, k, 4, s_ectr++);
    CHECK(ecmd_raw(p, n, 0, 0) == ICD_ST_OK && sim_ttc_resets() == 1);
    CHECK(ecmd_raw(p, n, 0, 0) == 0xFF && sim_ttc_resets() == 1); /* replay after reboot: refused */
    CHECK(xact(ICD_ADDR_TTC, ICD_CMD_GET_STATUS, 0, 0, &r) == 0 && (r.data[ICD_STATUS_OFS_FLAGS] & ICD_SF_RESET_OCCURRED));
    CHECK(ecmd(GL_TGT_TTC, GL_E_PING, 0, 0, d, &dn) == ICD_ST_OK);

    /* OBC watchdog: off by default; set, it resets the Pi, then power-cycles it. */
    icd_put_u16(pp, 3);
    CHECK(ecmd(GL_TGT_OBC, GL_E_OBC_WATCHDOG, pp, 2, 0, 0) == ICD_ST_BAD_PARAM);
    icd_put_u16(pp, 5);
    CHECK(ecmd(GL_TGT_OBC, GL_E_OBC_WATCHDOG, pp, 2, 0, 0) == ICD_ST_OK);
    sim_reset_slave(ICD_ADDR_TTC);           /* kept in flash */
    CHECK(ecmd(GL_TGT_TTC, GL_E_PING, 0, 0, d, &dn) == ICD_ST_OK && icd_get_u16(&d[GL_EP_OFS_WDT_MIN]) == 5);
    uint32_t run0 = sim_ttc_line_pulses(TTC_LINE_OBC_RUN), pwr0 = sim_ttc_line_pulses(TTC_LINE_OBC_GLOBAL_EN);
    sim_run_ms(5 * 60000 - 2000);
    CHECK(sim_ttc_line_pulses(TTC_LINE_OBC_RUN) == run0);
    sim_run_ms(3000);
    CHECK(sim_ttc_line_pulses(TTC_LINE_OBC_RUN) == run0 + 1 && sim_ttc_line_pulses(TTC_LINE_OBC_GLOBAL_EN) == pwr0);
    sim_run_ms(5 * 60000);
    CHECK(sim_ttc_line_pulses(TTC_LINE_OBC_GLOBAL_EN) == pwr0 + 1);
    sim_run_ms(30 * 60000);                  /* gives up after GL_E_WDT_MAX_ACTIONS */
    CHECK(sim_ttc_line_pulses(TTC_LINE_OBC_RUN) + sim_ttc_line_pulses(TTC_LINE_OBC_GLOBAL_EN) ==
          run0 + pwr0 + GL_E_WDT_MAX_ACTIONS);
    CHECK(ecmd(GL_TGT_TTC, GL_E_PING, 0, 0, d, &dn) == ICD_ST_OK && d[GL_EP_OFS_WDT_ACTIONS] == GL_E_WDT_MAX_ACTIONS);
    /* The OBC talks again: the count starts over. Regular I2C keeps it quiet. */
    for (int i = 0; i < 40; i++) {
        status_of(ICD_ADDR_TTC, ICD_CMD_PING, 0, 0);
        sim_run_ms(10000);
    }
    CHECK(ecmd(GL_TGT_TTC, GL_E_PING, 0, 0, d, &dn) == ICD_ST_OK && d[GL_EP_OFS_WDT_ACTIONS] == 0);
    uint32_t total = sim_ttc_line_pulses(TTC_LINE_OBC_RUN) + sim_ttc_line_pulses(TTC_LINE_OBC_GLOBAL_EN);
    CHECK(total == run0 + pwr0 + GL_E_WDT_MAX_ACTIONS);
    icd_put_u16(pp, 0);
    CHECK(ecmd(GL_TGT_OBC, GL_E_OBC_WATCHDOG, pp, 2, 0, 0) == ICD_ST_OK);
    sim_run_ms(20 * 60000);
    CHECK(sim_ttc_line_pulses(TTC_LINE_OBC_RUN) + sim_ttc_line_pulses(TTC_LINE_OBC_GLOBAL_EN) == total);

    /* Works in commanded SAFE too. */
    uint8_t m = ICD_MODE_SAFE;
    status_of(ICD_ADDR_TTC, ICD_CMD_SET_MODE, &m, 1);
    CHECK(ecmd(GL_TGT_TTC, GL_E_PING, 0, 0, d, &dn) == ICD_ST_OK && d[GL_EP_OFS_MODE] == ICD_MODE_SAFE);
}

static void test_ttc_radio_revert(void)
{
    puts("ttc radio settings: kept only if the ground is heard on them");
    sim_init(21);
    resp_t r;
    uint8_t rc[TTC_RADIO_LEN], pkt[24];
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_DOWN_HZ], 434000000);
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_UP_HZ], 435000000);
    rc[4] = 10; rc[5] = 1; rc[6] = 5; rc[7] = 10; rc[8] = 0x12;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_OK);
    sim_run_ms(TTC_RADIO_CONFIRM_MS - 1000);
    CHECK(sim_ttc_rx_freq() == 435000000u);
    sim_run_ms(2000);                        /* nobody answered: back to the defaults */
    CHECK(sim_ttc_rx_freq() == TTC_FREQ_UP_DEFAULT_HZ);
    CHECK(xact(ICD_ADDR_TTC, TTC_CMD_GET_RADIO, 0, 0, &r) == 0 && r.data[TTC_RADIO_OFS_SF] == TTC_SF_DEFAULT);
    CHECK(ttc_faults() & TTC_FLT_RADIO_REVERTED);
    /* This time the ground is heard on the new settings: they stay. */
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_OK);
    sim_run_ms(60000);
    sim_ttc_uplink(pkt, mk_ground(pkt, GL_SAT_ID, 1), -90);
    sim_run_ms(TTC_RADIO_CONFIRM_MS + 10000);
    CHECK(sim_ttc_rx_freq() == 435000000u);
    /* A third setting nobody hears: back to the second (the last good one), not the defaults. */
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_UP_HZ], 436000000);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_OK);
    sim_run_ms(1000);
    CHECK(sim_ttc_rx_freq() == 436000000u);
    sim_run_ms(TTC_RADIO_CONFIRM_MS + 1000);
    CHECK(sim_ttc_rx_freq() == 435000000u);
}

static void test_ttc_long_packets(void)
{
    puts("ttc QUEUE_TX_PART: long FILE packets, own queue sizes, airtime");
    sim_init(22);
    uint8_t big[255], part[24], b[255];
    for (int i = 0; i < 255; i++) big[i] = (uint8_t)(i * 7 + 3);
    big[0] = GL_SAT_ID;
    big[1] = GL_T_FILE;
    sim_run_ms(6000);
    drain();
    /* 207 bytes in parts of 22. */
    int n = 207;
    for (int off = 0; off < n; off += TTC_TXP_MAX_DATA) {
        int dn = n - off < TTC_TXP_MAX_DATA ? n - off : TTC_TXP_MAX_DATA;
        part[0] = off + dn == n ? TTC_TXP_F_LAST : 0;
        part[1] = (uint8_t)off;
        memcpy(&part[2], &big[off], (size_t)dn);
        CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, (uint8_t)(dn + 2)) == ICD_ST_OK);
    }
    sim_run_ms(500);
    CHECK(sim_ttc_downlink(b) == n && memcmp(b, big, (size_t)n) == 0);
    /* A part missing: refused, and the half-built packet is gone. */
    part[0] = 0; part[1] = 0; memcpy(&part[2], big, 22);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 24) == ICD_ST_OK);
    part[1] = 44;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 24) == ICD_ST_BAD_PARAM);
    part[1] = 22; part[0] = TTC_TXP_F_LAST;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 24) == ICD_ST_BAD_PARAM);
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 2) == ICD_ST_BAD_LENGTH);
    /* 255 bytes is the most; one byte more is refused. */
    int over = 0;
    for (int off = 0; off <= 255; off += 22) {
        part[0] = 0; part[1] = (uint8_t)off;
        uint8_t st = status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 24);
        if (st == ICD_ST_BAD_LENGTH) over = 1;
        if (st != ICD_ST_OK) break;
    }
    CHECK(over);
    /* The TX queue holds TTC_TX_QUEUE_DEPTH long packets (plus the one on air). */
    int ok = 0, full = 0;
    for (int i = 0; i < TTC_TX_QUEUE_DEPTH + 3; i++) {
        part[0] = 0; part[1] = 0;
        status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 24);
        part[0] = TTC_TXP_F_LAST; part[1] = 22;
        uint8_t st = status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 24);
        if (st == ICD_ST_OK) ok++;
        if (st == ICD_ST_QUEUE_FULL) full++;
    }
    CHECK(ok >= TTC_TX_QUEUE_DEPTH && ok <= TTC_TX_QUEUE_DEPTH + 1 && full >= 2);
    sim_run_ms(5000);
    int got = 0;
    while ((n = sim_ttc_downlink(b)) > 0)
        if (n == 44) got++;
    CHECK(got == ok);
    /* Refused in SAFE (bulk data). */
    uint8_t m = ICD_MODE_SAFE;
    status_of(ICD_ADDR_TTC, ICD_CMD_SET_MODE, &m, 1);
    part[0] = 0; part[1] = 0;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 24) == ICD_ST_INVALID_MODE);
    /* LoRa airtime (Semtech formula): 24 B 280 ms, 255 B 1951 ms at SF9/125 kHz/CR 4/8. */
    ttc_radio_cfg_t c = {TTC_FREQ_DOWN_DEFAULT_HZ, TTC_FREQ_UP_DEFAULT_HZ, 9, TTC_BW_125K, 8, 10, 0x12};
    CHECK(ttc_airtime_ms(&c, 24) == 280 && ttc_airtime_ms(&c, 255) == 1951);
    c.sf = 12; c.bw = TTC_BW_62K5;  /* low data rate optimisation on: 255 B takes 28.07 s */
    CHECK(ttc_airtime_ms(&c, 255) == 28066);
}

static int find_ev(uint8_t cmd)
{
    for (int i = 0; i < s_ev.n; i++)
        if (s_ev.cmd[i] == cmd) return i;
    return -1;
}

static void test_ttc_emergency_reports(void)
{
    puts("ttc emergency priority and execution reports (EVENT packets)");
    sim_init(23);
    s_ectr = 1000;
    uint8_t d[16], dn = 0, k[5], p[24], b[255];
    sim_run_ms(1000);
    drain();

    /* Deferred action: ACK says "report follows", the EVENT says it was done. */
    icd_put_u32(k, GL_REBOOT_KEY);
    uint8_t n = mk_ecmd(p, 0x51, GL_TGT_OBC, GL_E_OBC_RESET, k, 4, s_ectr++);
    drain();
    memset(&s_ev, 0, sizeof s_ev);
    sim_ttc_uplink(p, n, -80);
    CHECK(watch(1000, d, &dn) == ICD_ST_OK && dn == 1 && d[0] == GL_EA_REPORT_FOLLOWS);
    int i = find_ev(GL_E_OBC_RESET);
    CHECK(i >= 0 && s_ev.src[i] == GL_EVS_ECMD && s_ev.tgt[i] == GL_TGT_OBC && s_ev.eseq[i] == 0x51 &&
          s_ev.result[i] == ICD_ST_OK);
    /* The Pi talks again 20 s later: "OBC back", detail = 20 s. */
    sim_run_ms(19000);
    memset(&s_ev, 0, sizeof s_ev);
    status_of(ICD_ADDR_TTC, ICD_CMD_PING, 0, 0);
    watch(500, 0, 0);
    i = find_ev(GL_EV_OBC_BACK);
    CHECK(i >= 0 && s_ev.eseq[i] == 0x51 && s_ev.detail[i] >= 19 && s_ev.detail[i] <= 21);
    /* Power cycle with no Pi coming back: "not back" after GL_EV_OBC_WAIT_S. */
    k[4] = 2;
    memset(&s_ev, 0, sizeof s_ev);
    CHECK(ecmd(GL_TGT_OBC, GL_E_OBC_POWER_CYCLE, k, 5, d, &dn) == ICD_ST_OK && d[0] == GL_EA_REPORT_FOLLOWS);
    memset(&s_ev, 0, sizeof s_ev);
    watch(2500, 0, 0);
    CHECK(find_ev(GL_E_OBC_POWER_CYCLE) >= 0);
    memset(&s_ev, 0, sizeof s_ev);
    watch(GL_EV_OBC_WAIT_S * 1000, 0, 0);
    i = find_ev(GL_EV_OBC_NOT_BACK);
    CHECK(i >= 0 && s_ev.result[i] == GL_ST_COMM_FAULT);
    /* ADCS reset. */
    CHECK(ecmd(GL_TGT_ADCS, GL_E_ADCS_RESET, k, 4, 0, 0) == ICD_ST_OK);
    CHECK(find_ev(GL_E_ADCS_RESET) >= 0);
    /* Immediate commands: the ACK is the report (no data byte, no EVENT). */
    uint8_t pp[2] = {30, 0};
    CHECK(ecmd(GL_TGT_TTC, GL_E_BEACON_PERIOD, pp, 2, d, &dn) == ICD_ST_OK && dn == 0 && s_ev.n == 0);

    /* Burn: OK with the time burned; cut short by the RBF going back in: HW_FAULT. */
    sim_ttc_set_pins(0, 1);
    uint8_t ms[2];
    icd_put_u16(ms, 300);
    CHECK(ecmd(GL_TGT_TTC, GL_E_BURN, ms, 2, d, &dn) == ICD_ST_OK && d[0] == GL_EA_REPORT_FOLLOWS);
    memset(&s_ev, 0, sizeof s_ev);
    watch(400, 0, 0);
    i = find_ev(GL_E_BURN);
    CHECK(i >= 0 && s_ev.result[i] == ICD_ST_OK && s_ev.detail[i] >= 298 && s_ev.detail[i] <= 302);
    icd_put_u16(ms, 3000);
    CHECK(ecmd(GL_TGT_TTC, GL_E_BURN, ms, 2, 0, 0) == ICD_ST_OK);
    sim_ttc_set_pins(0, 0);
    memset(&s_ev, 0, sizeof s_ev);
    watch(300, 0, 0);
    i = find_ev(GL_E_BURN);
    CHECK(i >= 0 && s_ev.result[i] == ICD_ST_HW_FAULT && s_ev.detail[i] < 1000);

    /* Radio silence end is reported. */
    icd_put_u16(pp, 1);
    CHECK(ecmd(GL_TGT_TTC, GL_E_TX_SILENCE, pp, 2, 0, 0) == ICD_ST_OK);
    memset(&s_ev, 0, sizeof s_ev);
    watch(61000, 0, 0);
    i = find_ev(GL_EV_SILENCE_END);
    CHECK(i >= 0 && s_ev.src[i] == GL_EVS_ECMD);

    /* GET_EVENTS reads the last three back, newest first. */
    CHECK(ecmd(GL_TGT_TTC, GL_E_GET_EVENTS, 0, 0, d, &dn) == ICD_ST_OK && dn == GL_EVL_COUNT * GL_EVL_ENTRY_LEN);
    uint16_t n0 = icd_get_u16(&d[0]), n1 = icd_get_u16(&d[5]), n2 = icd_get_u16(&d[10]);
    CHECK(n0 == n1 + 1 && n1 == n2 + 1 && d[3] == GL_EV_SILENCE_END && d[2] == GL_TGT_TTC);

    /* TT&C reset: reported after the reboot, from flash, with the command's SEQ. */
    icd_put_u32(k, GL_REBOOT_KEY);
    n = mk_ecmd(p, 0x77, GL_TGT_TTC, GL_E_TTC_RESET, k, 4, s_ectr++);
    drain();
    memset(&s_ev, 0, sizeof s_ev);
    sim_ttc_uplink(p, n, -80);
    CHECK(watch(1000, d, &dn) == ICD_ST_OK && d[0] == GL_EA_REPORT_FOLLOWS && sim_ttc_resets() == 1);
    i = find_ev(GL_E_TTC_RESET);
    CHECK(i >= 0 && s_ev.eseq[i] == 0x77 && s_ev.src[i] == GL_EVS_ECMD && s_ev.no[i] == 1 &&
          find_ev(GL_EV_BOOT) < 0);
    /* An ordinary reboot says BOOT. */
    sim_reset_slave(ICD_ADDR_TTC);
    memset(&s_ev, 0, sizeof s_ev);
    watch(300, 0, 0);
    CHECK(s_ev.n == 1 && s_ev.cmd[0] == GL_EV_BOOT && s_ev.src[0] == GL_EVS_AUTO);

    /* Autonomous actions are reported too: radio revert, auto-deploy burn and flag. */
    uint8_t rc[TTC_RADIO_LEN];
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_DOWN_HZ], 434000000);
    icd_put_u32(&rc[TTC_RADIO_OFS_FREQ_UP_HZ], 435000000);
    rc[4] = 10; rc[5] = 1; rc[6] = 5; rc[7] = 10; rc[8] = 0x12;
    CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_SET_RADIO, rc, TTC_RADIO_LEN) == ICD_ST_OK);
    memset(&s_ev, 0, sizeof s_ev);
    watch(TTC_RADIO_CONFIRM_MS + 2000, 0, 0);
    i = find_ev(GL_EV_RADIO_REVERTED);
    CHECK(i >= 0 && s_ev.src[i] == GL_EVS_AUTO && s_ev.freq[i] == TTC_FREQ_DOWN_DEFAULT_HZ);
    sim_ttc_set_pins(0, 1);
    memset(&s_ev, 0, sizeof s_ev);
    watch(TTC_AUTODEPLOY_DELAY_S * 1000 + 1000, 0, 0);
    sim_ttc_set_pins(1, 1);                   /* opens during the first burn */
    watch(TTC_DEPLOYED_CONFIRM_MS + 500, 0, 0);
    i = find_ev(GL_E_BURN);
    int j = find_ev(GL_EV_DEPLOYED);
    CHECK(i >= 0 && s_ev.src[i] == GL_EVS_AUTO && s_ev.result[i] == ICD_ST_OK && j > i &&
          (s_ev.detail[j] & TTC_DF_DEPLOYED_FLAG));

    /* Flash wiped (full-chip erase) on a satellite whose antenna is out: TT&C
     * says so, finds the antenna deployed again from the switch, never burns,
     * and still refuses commands older than its build (counter floor). */
    sim_ttc_nv_wipe();
    sim_ttc_set_pins(1, 1);
    uint32_t burned = sim_ttc_burn_total_ms();
    sim_reset_slave(ICD_ADDR_TTC);
    memset(&s_ev, 0, sizeof s_ev);
    watch(TTC_DEPLOYED_CONFIRM_MS + 500, 0, 0);
    CHECK(find_ev(GL_EV_NV_BLANK) >= 0 && find_ev(GL_EV_DEPLOYED) >= 0 && (df_now() & TTC_DF_DEPLOYED_FLAG));
    watch(TTC_AUTODEPLOY_DELAY_S * 1000 + 10000, 0, 0);
    CHECK(sim_ttc_burn_total_ms() == burned);
    n = mk_ecmd(p, 3, GL_TGT_TTC, GL_E_PING, 0, 0, 100);   /* "recorded before the build" */
    CHECK(ecmd_raw(p, n, 0, 0) == 0xFF);
    n = mk_ecmd(p, 3, GL_TGT_TTC, GL_E_PING, 0, 0, 101);   /* newer than the build: the floor allows it */
    CHECK(ecmd_raw(p, n, 0, 0) == ICD_ST_OK);
    sim_reset_slave(ICD_ADDR_TTC);                          /* marked now: no second BLANK */
    memset(&s_ev, 0, sizeof s_ev);
    watch(300, 0, 0);
    CHECK(find_ev(GL_EV_NV_BLANK) < 0);

    /* Two ECMDs back to back: both answered (v0.9 single slot lost the first). */
    uint8_t p2[24], n2b;
    n = mk_ecmd(p, 1, GL_TGT_TTC, GL_E_PING, 0, 0, s_ectr++);
    n2b = mk_ecmd(p2, 2, GL_TGT_TTC, GL_E_PING, 0, 0, s_ectr++);
    drain();
    sim_ttc_uplink(p, n, -80);
    sim_ttc_uplink(p2, n2b, -80);
    sim_run_ms(1000);
    int acks = 0, len;
    while ((len = sim_ttc_downlink(b)) > 0)
        if (b[1] == GL_T_ACK && (b[GL_ACK_OFS_TARGET] & GL_TGT_EMERGENCY)) acks++;
    CHECK(acks == 2);

    /* Priority on the air: half-duplex radio, a full queue of long FILE packets
     * going out; the ground repeats its ECMD every 100 ms. The listen gap lets
     * it in, and its ACK goes out ahead of the rest of the queue. */
    sim_ttc_set_half_duplex(1);
    uint8_t part[24] = {0};
    for (int q = 0; q < TTC_TX_QUEUE_DEPTH; q++) {
        part[0] = 0; part[1] = 0; part[2] = GL_SAT_ID; part[3] = GL_T_FILE;
        status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 24);
        part[0] = TTC_TXP_F_LAST; part[1] = 22;
        status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX_PART, part, 24);
    }
    n = mk_ecmd(p, 9, GL_TGT_TTC, GL_E_PING, 0, 0, s_ectr++);
    int files_before = 0, got = 0;
    for (int t = 0; t < 20000 && !got; t++) {
        if (t % 100 == 0) sim_ttc_uplink(p, n, -80);
        sim_run_ms(1);
        while ((len = sim_ttc_downlink(b)) > 0) {
            if (b[1] == GL_T_FILE) files_before++;
            if (b[1] == GL_T_ACK && (b[GL_ACK_OFS_TARGET] & GL_TGT_EMERGENCY)) got = 1;
        }
    }
    CHECK(got && files_before < TTC_TX_QUEUE_DEPTH && sim_ttc_uplink_lost() > 0);
    printf("  ECMD answered after %d of %d queued FILE packets (%u uplink tries lost while transmitting)\n",
           files_before, TTC_TX_QUEUE_DEPTH, sim_ttc_uplink_lost());
    sim_ttc_set_half_duplex(0);
    sim_run_ms(5000);                           /* let the rest of the FILE burst go out */

    /* Interference that makes the modem "detect a signal" all the time: each
     * transmission waits at most TTC_RX_HOLD_MAX_MS, it is never blocked. */
    drain();
    sim_ttc_set_rx_noise(1);
    uint8_t hello[5] = {GL_SAT_ID, GL_T_ACK, 1, 2, 3};
    for (int q = 0; q < 3; q++) CHECK(status_of(ICD_ADDR_TTC, TTC_CMD_QUEUE_TX, hello, 5) == ICD_ST_OK);
    int sent = 0;
    for (int t = 0; t < 3 * (TTC_RX_HOLD_MAX_MS + 300) && sent < 3; t++) {
        sim_run_ms(1);
        while ((len = sim_ttc_downlink(b)) > 0)
            if (len == 5) sent++;
    }
    CHECK(sent == 3);
    sim_ttc_set_rx_noise(0);
}

int main(void)
{
    test_crc();
    test_link_framing();
    test_status_and_modes();
    test_bdot_math();
    test_adcs_commands_and_faults();
    test_ttc();
    test_hmac_sha256();
    test_ttc_nv();
    test_ttc_autodeploy();
    test_ttc_emergency();
    test_ttc_emergency_reports();
    test_ttc_radio_revert();
    test_ttc_long_packets();
    test_adcs_detumble();
    test_link_no_double_run();
    test_adcs_gyro_offset();
    test_adcs_autonomy();
    test_adcs_sun();
    test_adcs_sun_dead_diode();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
