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
    test_link_no_double_run();
    test_adcs_gyro_offset();
    test_adcs_autonomy();
    test_adcs_sun();
    test_adcs_sun_dead_diode();
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
