#include "ttc_app.h"

#include <string.h>

#include "ground_link.h"
#include "icd_crc16.h"
#include "icd_pack.h"

/* Mode behaviour (see ICD.md section 8):
 *   BOOT     radio and sensor init. Leaves on its own.
 *   SAFE     receive on, beacon at least every 60 s, QUEUE_TX and BURN refused.
 *            Entered on radio init failure; left automatically when the radio recovers.
 *   DETUMBLE treated exactly like NOMINAL (TT&C has no attitude role).
 *   NOMINAL  receive, beacon, transmit OBC packets, burn wire allowed.   */

/* ---------------- queues ---------------- */

static bool q_push(ttc_queue_t *q, const uint8_t *d, uint8_t n)
{
    if (q->count >= TTC_RX_QUEUE_DEPTH) return false;
    ttc_pkt_t *p = &q->q[(q->head + q->count) % TTC_RX_QUEUE_DEPTH];
    memcpy(p->data, d, n);
    p->len = n;
    q->count++;
    return true;
}

static bool q_pop(ttc_queue_t *q, uint8_t *d, uint8_t *n)
{
    if (!q->count) return false;
    ttc_pkt_t *p = &q->q[q->head];
    memcpy(d, p->data, p->len);
    *n = p->len;
    q->head = (uint8_t)((q->head + 1) % TTC_RX_QUEUE_DEPTH);
    q->count--;
    return true;
}

/* ---------------- helpers ---------------- */

static bool tx_allowed_mode(const ttc_t *t)
{
    uint8_t m = icd_node_mode(&t->node);
    return m == ICD_MODE_NOMINAL || m == ICD_MODE_DETUMBLE;
}

static void burn_stop(ttc_t *t, uint32_t now)
{
    if (t->burning) {
        ttc_hw_burn(false);
        t->burning = false;
        t->burn_done = true;
        uint32_t d = now - t->burn_start;
        t->last_burn_ms = d > 65535u ? 65535u : (uint16_t)d;
    }
}

static bool rbf_removed(void) { return ttc_hw_rbf_in() == (bool)TTC_RBF_REMOVED_LEVEL; }
static bool deployed(void) { return ttc_hw_deploy_sw() == (bool)TTC_DEPLOYED_LEVEL; }

static bool valid_ground_packet(const uint8_t *d, uint8_t n)
{
    if (n < GL_HDR_LEN + GL_CRC_LEN || n > GL_MAX_LEN) return false;
    if (d[GL_OFS_SAT] != GL_SAT_ID) return false;
    uint16_t crc = icd_crc16(d, (size_t)(n - 2));
    return (uint8_t)crc == d[n - 2] && (uint8_t)(crc >> 8) == d[n - 1];
}

uint8_t ttc_build_beacon(ttc_t *t, uint8_t *out)
{
    uint8_t st[ICD_GET_STATUS_RESP_LEN];
    icd_node_peek_status(&t->node, st);
    memset(out, 0, GL_BT_LEN);
    out[GL_OFS_SAT] = GL_SAT_ID;
    out[GL_OFS_TYPE] = GL_T_BEACON_TTC;
    out[GL_OFS_SEQ] = t->beacon_seq++;
    icd_put_u32(&out[GL_BT_OFS_UPTIME_S], t->node.now_ms / 1000u);
    out[GL_BT_OFS_MODE] = icd_node_mode(&t->node);
    out[GL_BT_OFS_STATUS_FLAGS] = st[ICD_STATUS_OFS_FLAGS];
    memcpy(&out[GL_BT_OFS_FAULTS], &st[ICD_STATUS_OFS_FAULTS], 2);
    icd_put_u16(&out[GL_BT_OFS_RX_COUNT], t->rx_count);
    icd_put_i16(&out[GL_BT_OFS_TEMP], t->temp);
    icd_put_i16(&out[GL_BT_OFS_LAST_RSSI], t->last_rssi);
    out[GL_BT_OFS_OBC_CONTACT] = icd_node_master_lost(&t->node) ? 0 : 1;
    uint16_t crc = icd_crc16(out, GL_BT_LEN - 2);
    icd_put_u16(&out[GL_BT_LEN - 2], crc);
    return GL_BT_LEN;
}

/* ---------------- ICD callbacks ---------------- */

static uint16_t live_faults(void *ctx)
{
    ttc_t *t = (ttc_t *)ctx;
    uint16_t f = 0;
    if (!t->radio_ok) f |= TTC_FLT_RADIO_INIT;
    if (!t->temp_ok) f |= TTC_FLT_TEMP_SENSOR;
    return f;
}

static uint8_t extra_flags(void *ctx)
{
    ttc_t *t = (ttc_t *)ctx;
    return t->rxq.count ? ICD_SF_GROUND_PKT_PENDING : 0;
}

static uint8_t on_set_mode(void *ctx, uint8_t from, uint8_t to)
{
    ttc_t *t = (ttc_t *)ctx;
    (void)from;
    if (to == ICD_MODE_SAFE) {
        burn_stop(t, t->node.now_ms);
        t->armed = false;
    }
    return ICD_ST_OK;
}

static uint8_t get_telemetry(void *ctx, uint8_t page, uint8_t *o, uint8_t *len)
{
    ttc_t *t = (ttc_t *)ctx;
    if (page == TTC_TLM_PAGE_RADIO) {
        icd_put_u32(&o[TTC_TLM0_OFS_UPTIME_MS], t->node.now_ms);
        o[TTC_TLM0_OFS_RADIO_STATE] = t->radio_state;
        o[TTC_TLM0_OFS_RX_QUEUE] = t->rxq.count;
        o[TTC_TLM0_OFS_TX_QUEUE] = t->txq.count;
        uint8_t df = 0;
        if (ttc_hw_deploy_sw()) df |= TTC_DF_DEPLOY_SW;
        if (ttc_hw_rbf_in()) df |= TTC_DF_RBF_IN;
        if (t->armed) df |= TTC_DF_ARMED;
        if (t->burning) df |= TTC_DF_BURNING;
        if (t->burn_done) df |= TTC_DF_BURN_DONE;
        o[TTC_TLM0_OFS_DEPLOY_FLAGS] = df;
        icd_put_i16(&o[TTC_TLM0_OFS_LAST_RSSI], t->last_rssi);
        o[TTC_TLM0_OFS_LAST_SNR] = (uint8_t)t->last_snr;
        o[TTC_TLM0_OFS_MODE_REASON] = t->node.mode_reason;
        icd_put_u16(&o[TTC_TLM0_OFS_RX_COUNT], t->rx_count);
        icd_put_u16(&o[TTC_TLM0_OFS_TX_COUNT], t->tx_count);
        icd_put_u16(&o[TTC_TLM0_OFS_RX_CRC_ERR], t->rx_crc_err);
        icd_put_i16(&o[TTC_TLM0_OFS_BOARD_TEMP], t->temp);
        icd_put_u16(&o[TTC_TLM0_OFS_BEACON_PERIOD], t->beacon_period_s);
        icd_put_u16(&o[TTC_TLM0_OFS_OBC_I2C_RESETS], (uint16_t)t->node.link.stats.bus_resets);
        *len = TTC_TLM0_LEN;
        return ICD_ST_OK;
    }
    if (page == TTC_TLM_PAGE_COUNTERS) {
        icd_put_u16(&o[TTC_TLM1_OFS_TX_TIMEOUTS], t->tx_timeouts);
        icd_put_u16(&o[TTC_TLM1_OFS_RX_DROPPED], t->rx_dropped);
        icd_put_u16(&o[TTC_TLM1_OFS_TX_DROPPED], t->tx_dropped);
        icd_put_u16(&o[TTC_TLM1_OFS_BEACON_COUNT], t->beacon_count);
        o[TTC_TLM1_OFS_BURN_COUNT] = t->burn_count;
        o[TTC_TLM1_OFS_RESERVED] = 0;
        icd_put_u16(&o[TTC_TLM1_OFS_LAST_BURN_MS], t->last_burn_ms);
        *len = TTC_TLM1_LEN;
        return ICD_ST_OK;
    }
    return ICD_ST_BAD_PARAM;
}

static void put_radio(const ttc_radio_cfg_t *c, uint8_t *o)
{
    icd_put_u32(&o[TTC_RADIO_OFS_FREQ_HZ], c->freq_hz);
    o[TTC_RADIO_OFS_SF] = c->sf;
    o[TTC_RADIO_OFS_BW] = c->bw;
    o[TTC_RADIO_OFS_CR] = c->cr;
    o[TTC_RADIO_OFS_POWER_DBM] = (uint8_t)c->power;
    o[TTC_RADIO_OFS_SYNC] = c->sync;
}

static uint8_t app_command(void *ctx, uint8_t cmd, const uint8_t *req, uint8_t n,
                           uint8_t *resp, uint8_t *resp_len)
{
    ttc_t *t = (ttc_t *)ctx;
    uint32_t now = t->node.now_ms;
    *resp_len = 0;
    switch (cmd) {
    case TTC_CMD_GET_RX:
        if (n != 0) return ICD_ST_BAD_LENGTH;
        return q_pop(&t->rxq, resp, resp_len) ? ICD_ST_OK : ICD_ST_NO_DATA;

    case TTC_CMD_QUEUE_TX:
        if (n == 0) return ICD_ST_BAD_LENGTH;
        if (!tx_allowed_mode(t)) return ICD_ST_INVALID_MODE;
        return q_push(&t->txq, req, n) ? ICD_ST_OK : ICD_ST_QUEUE_FULL;

    case TTC_CMD_SET_BEACON:
        memcpy(t->obc_beacon, req, n);
        t->obc_beacon_len = n;
        t->obc_beacon_time = now;
        return ICD_ST_OK;

    case TTC_CMD_SET_BEACON_PERIOD: {
        if (n != 2) return ICD_ST_BAD_LENGTH;
        uint16_t p = icd_get_u16(req);
        if (p != 0 && p < TTC_BEACON_PERIOD_MIN_S) return ICD_ST_BAD_PARAM;
        t->beacon_period_s = p;
        return ICD_ST_OK;
    }

    case TTC_CMD_SET_RADIO: {
        if (n != TTC_RADIO_LEN) return ICD_ST_BAD_LENGTH;
        ttc_radio_cfg_t c;
        c.freq_hz = icd_get_u32(&req[TTC_RADIO_OFS_FREQ_HZ]);
        c.sf = req[TTC_RADIO_OFS_SF];
        c.bw = req[TTC_RADIO_OFS_BW];
        c.cr = req[TTC_RADIO_OFS_CR];
        c.power = (int8_t)req[TTC_RADIO_OFS_POWER_DBM];
        c.sync = req[TTC_RADIO_OFS_SYNC];
        if (c.freq_hz < TTC_FREQ_MIN_HZ || c.freq_hz > TTC_FREQ_MAX_HZ || c.sf < 7 || c.sf > 12 ||
            c.bw > TTC_BW_500K || c.cr < 5 || c.cr > 8 || c.power < 2 || c.power > 17)
            return ICD_ST_BAD_PARAM;
        t->radio_pending = c;
        t->radio_cfg_pending = true; /* applied when the radio is idle */
        return ICD_ST_OK;
    }

    case TTC_CMD_GET_RADIO:
        if (n != 0) return ICD_ST_BAD_LENGTH;
        put_radio(&t->radio, resp);
        *resp_len = TTC_RADIO_LEN;
        return ICD_ST_OK;

    case TTC_CMD_BURN_ARM:
        if (n != 4) return ICD_ST_BAD_LENGTH;
        if (icd_get_u32(req) != TTC_BURN_ARM_KEY) return ICD_ST_BAD_PARAM;
        if (!tx_allowed_mode(t)) return ICD_ST_INVALID_MODE;
        t->armed = true;
        t->armed_until = now + TTC_BURN_ARM_WINDOW_MS;
        return ICD_ST_OK;

    case TTC_CMD_BURN_FIRE: {
        if (n != 2) return ICD_ST_BAD_LENGTH;
        uint16_t ms = icd_get_u16(req);
        if (ms == 0 || ms > TTC_BURN_MAX_MS) return ICD_ST_BAD_PARAM;
        if (!tx_allowed_mode(t)) return ICD_ST_INVALID_MODE;
        if (!t->armed || (int32_t)(now - t->armed_until) >= 0) return ICD_ST_INVALID_MODE;
        if (!rbf_removed()) return ICD_ST_HW_FAULT; /* remove-before-flight still in */
        t->armed = false;
        t->burning = true;
        t->burn_done = false;
        t->burn_start = now;
        t->burn_end = now + ms;
        t->burn_count++;
        ttc_hw_burn(true);
        return ICD_ST_OK;
    }

    default:
        return ICD_ST_UNKNOWN_CMD;
    }
}

static icd_node_app_t s_app = {
    .fw_major = TTC_FW_MAJOR,
    .fw_minor = TTC_FW_MINOR,
    .fw_patch = TTC_FW_PATCH,
    .live_faults = live_faults,
    .extra_flags = extra_flags,
    .on_set_mode = on_set_mode,
    .get_telemetry = get_telemetry,
    .app_command = app_command,
};

/* ---------------- tasks ---------------- */

static void radio_start(ttc_t *t, uint32_t now)
{
    t->radio_ok = ttc_hw_radio_init(&t->radio);
    if (t->radio_ok) {
        ttc_hw_radio_rx();
        t->radio_state = TTC_RS_RX;
    } else {
        t->radio_state = TTC_RS_OFF;
        t->radio_retry_at = now + TTC_RADIO_RETRY_MS;
    }
}

static void radio_task(ttc_t *t, uint32_t now)
{
    if (!t->radio_ok) {
        if ((int32_t)(now - t->radio_retry_at) < 0) return;
        radio_start(t, now);
        if (t->radio_ok && icd_node_mode(&t->node) == ICD_MODE_SAFE &&
            t->node.mode_reason == ICD_MR_FAULT)
            icd_node_set_mode(&t->node, ICD_MODE_NOMINAL, ICD_MR_AUTO);
        return;
    }

    uint8_t buf[TTC_RADIO_BUF];
    uint8_t len = 0;
    int16_t rssi = 0;
    int8_t snr = 0;
    int ev = ttc_hw_radio_poll(buf, &len, &rssi, &snr);
    if (ev == TTC_EV_RX) {
        t->last_rssi = rssi;
        t->last_snr = snr;
        t->led_until = now + 50u;
        if (!valid_ground_packet(buf, len)) {
            t->rx_dropped++; /* not ours, or corrupted */
        } else {
            t->rx_count++;
            if (!q_push(&t->rxq, buf, len)) {
                t->rx_dropped++;
                icd_node_raise_fault(&t->node, TTC_FLT_RX_OVERFLOW);
            }
        }
    } else if (ev == TTC_EV_RX_CRC_ERR) {
        t->rx_crc_err++;
    } else if (ev == TTC_EV_TX_DONE && t->radio_state == TTC_RS_TX) {
        t->tx_count++;
        ttc_hw_radio_rx();
        t->radio_state = TTC_RS_RX;
    }

    if (t->radio_state == TTC_RS_TX) {
        if ((uint32_t)(now - t->tx_start) >= TTC_TX_TIMEOUT_MS) {
            t->tx_timeouts++;
            icd_node_raise_fault(&t->node, TTC_FLT_RADIO_TX_TIMEOUT);
            radio_start(t, now);
        }
        return;
    }

    /* Radio is receiving and idle: apply new settings, then transmit. */
    if (t->radio_cfg_pending) {
        t->radio_cfg_pending = false;
        t->radio = t->radio_pending;
        radio_start(t, now);
        if (!t->radio_ok) return;
    }

    uint8_t pkt[ICD_MAX_PAYLOAD];
    uint8_t n = 0;
    if (t->beacon_due) {
        t->beacon_due = false;
        uint32_t max_age = (uint32_t)t->beacon_period_s * 1000u * TTC_OBC_BEACON_MAX_AGE;
        if (t->beacon_period_s == 0) max_age = 3u * TTC_BEACON_PERIOD_SAFE_MIN_S * 1000u;
        bool obc_fresh = t->obc_beacon_len > 0 && !icd_node_master_lost(&t->node) &&
                         (uint32_t)(now - t->obc_beacon_time) <= max_age;
        if (obc_fresh) {
            memcpy(pkt, t->obc_beacon, t->obc_beacon_len);
            n = t->obc_beacon_len;
        } else {
            n = ttc_build_beacon(t, pkt);
        }
        t->beacon_count++;
    } else if (!q_pop(&t->txq, pkt, &n)) {
        return;
    }
#ifdef TTC_TX_INHIBIT
    /* Bench build without an antenna: never key the transmitter. */
    (void)pkt;
    t->tx_dropped++;
    return;
#endif
    if (ttc_hw_radio_tx(pkt, n)) {
        t->radio_state = TTC_RS_TX;
        t->tx_start = now;
        t->led_until = now + 50u;
    } else {
        t->tx_dropped++;
        icd_node_raise_fault(&t->node, TTC_FLT_TX_OVERFLOW);
    }
}

static void beacon_task(ttc_t *t, uint32_t now)
{
    uint32_t p = t->beacon_period_s;
    if (icd_node_mode(&t->node) == ICD_MODE_SAFE) {
        /* In SAFE the beacon is never off and never faster than 60 s. */
        if (p < TTC_BEACON_PERIOD_SAFE_MIN_S) p = TTC_BEACON_PERIOD_SAFE_MIN_S;
    }
    if (p == 0) return;
    if ((uint32_t)(now - t->last_beacon) >= p * 1000u) {
        t->last_beacon = now;
        t->beacon_due = true;
    }
}

static void burn_task(ttc_t *t, uint32_t now)
{
    if (t->armed && (int32_t)(now - t->armed_until) >= 0) t->armed = false;
    if (!t->burning) return;
    /* Stop at the deadline, as soon as the antenna reports deployed, or if
     * the mode no longer allows it. */
    if ((int32_t)(now - t->burn_end) >= 0 || deployed() || !tx_allowed_mode(t)) burn_stop(t, now);
}

static void housekeeping_task(ttc_t *t, uint32_t now)
{
    if ((uint32_t)(now - t->last_temp) >= TTC_TEMP_PERIOD_MS) {
        t->last_temp = now;
        t->temp_ok = ttc_hw_temp_read(&t->temp);
    }
    uint16_t e = ttc_hw_local_i2c_errors();
    if (e != t->local_i2c_seen) {
        t->local_i2c_seen = e;
        icd_node_raise_fault(&t->node, TTC_FLT_LOCAL_I2C);
    }
    uint32_t r = t->node.link.stats.bus_resets;
    if (r != t->obc_resets_seen) {
        t->obc_resets_seen = r;
        icd_node_raise_fault(&t->node, TTC_FLT_OBC_I2C);
    }
    /* LED: flash on radio activity, otherwise 1 s heartbeat (50 ms on). */
    bool on = (int32_t)(now - t->led_until) < 0 || (now % 1000u) < 50u;
    ttc_hw_led(on);
}

void ttc_init(ttc_t *t, uint32_t now)
{
    memset(t, 0, sizeof *t);
    s_app.ctx = t;
    icd_node_init(&t->node, &s_app, now);
    t->radio.freq_hz = TTC_FREQ_DEFAULT_HZ;
    t->radio.sf = TTC_SF_DEFAULT;
    t->radio.bw = TTC_BW_DEFAULT;
    t->radio.cr = TTC_CR_DEFAULT;
    t->radio.power = TTC_POWER_DEFAULT_DBM;
    t->radio.sync = TTC_SYNC_DEFAULT;
    t->beacon_period_s = TTC_BEACON_PERIOD_DEFAULT_S;
    t->last_rssi = -200;

    ttc_hw_init();
    ttc_hw_burn(false);
    t->temp_ok = ttc_hw_temp_read(&t->temp);
    t->last_temp = now;
    t->local_i2c_seen = ttc_hw_local_i2c_errors();
    radio_start(t, now);
    /* First beacon soon after boot, then every period. */
    t->last_beacon = now - (uint32_t)t->beacon_period_s * 1000u + 5000u;

    if (t->radio_ok)
        icd_node_set_mode(&t->node, ICD_MODE_NOMINAL, ICD_MR_AUTO);
    else
        icd_node_set_mode(&t->node, ICD_MODE_SAFE, ICD_MR_FAULT);
}

void ttc_step(ttc_t *t, uint32_t now)
{
    icd_node_poll(&t->node, now);
    beacon_task(t, now);
    radio_task(t, now);
    burn_task(t, now);
    housekeeping_task(t, now);
}
