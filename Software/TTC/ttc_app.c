#include "ttc_app.h"

#include <string.h>

#include "hmac_sha256.h"
#include "icd_crc16.h"
#include "icd_pack.h"

/* Mode behaviour (see ICD.md section 8):
 *   BOOT     radio and sensor init. Leaves on its own.
 *   SAFE     receive on, beacon at least every 60 s, OBC burn commands refused.
 *            QUEUE_TX takes only ACK and TLM packets (v0.8), so every ground
 *            command is still acknowledged; bulk downlink (FILE) is refused.
 *            Entered on radio init failure; left automatically when the radio recovers.
 *            Commanded SAFE (OBC: low battery) also holds automatic deployment.
 *   DETUMBLE treated exactly like NOMINAL (TT&C has no attitude role).
 *   NOMINAL  receive, beacon, transmit OBC packets, burn wire allowed.
 * Emergency commands (GL_T_ECMD, v0.9) work in every mode and need no OBC. */

/* ---------------- queues ---------------- */

static void q_setup(ttc_queue_t *q, uint8_t *data, uint8_t *lens, uint16_t slot, uint8_t depth)
{
    q->data = data;
    q->lens = lens;
    q->slot = slot;
    q->depth = depth;
    q->head = q->count = 0;
}

static bool q_push(ttc_queue_t *q, const uint8_t *d, uint16_t n)
{
    if (q->count >= q->depth || n == 0 || n > q->slot) return false;
    uint8_t i = (uint8_t)((q->head + q->count) % q->depth);
    memcpy(&q->data[(uint32_t)i * q->slot], d, n);
    q->lens[i] = (uint8_t)n;
    q->count++;
    return true;
}

static bool q_pop(ttc_queue_t *q, uint8_t *d, uint8_t *n)
{
    if (!q->count) return false;
    memcpy(d, &q->data[(uint32_t)q->head * q->slot], q->lens[q->head]);
    *n = q->lens[q->head];
    q->head = (uint8_t)((q->head + 1) % q->depth);
    q->count--;
    return true;
}

/* ---------------- helpers ---------------- */

static bool tx_allowed_mode(const ttc_t *t)
{
    uint8_t m = icd_node_mode(&t->node);
    return m == ICD_MODE_NOMINAL || m == ICD_MODE_DETUMBLE;
}

/* SAFE commanded by the OBC (low battery): holds automatic and emergency burns too. */
static bool commanded_safe(const ttc_t *t)
{
    return icd_node_mode(&t->node) == ICD_MODE_SAFE && t->node.mode_reason == ICD_MR_COMMAND;
}

/* Command answers that must go down in every mode (ICD v0.8). */
static bool is_cmd_answer(const uint8_t *d, uint8_t n)
{
    return n > GL_OFS_TYPE && (d[GL_OFS_TYPE] == GL_T_ACK || d[GL_OFS_TYPE] == GL_T_TLM);
}

static void seal(uint8_t *o, uint8_t n_body);

/* Report a finished emergency action (or one TT&C took by itself) to the
 * ground: an EVENT packet in the priority queue, and an entry in the log
 * that E_GET_EVENTS reads back. */
static void emit_event(ttc_t *t, uint8_t src, uint8_t tgt, uint8_t cmd, uint8_t result, uint8_t eseq,
                       uint16_t detail)
{
    t->ev_no++;
    uint8_t o[GL_EV_LEN];
    o[GL_OFS_SAT] = GL_SAT_ID;
    o[GL_OFS_TYPE] = GL_T_EVENT;
    o[GL_OFS_SEQ] = (uint8_t)t->ev_no;
    icd_put_u16(&o[GL_EV_OFS_NO], t->ev_no);
    o[GL_EV_OFS_SOURCE] = src;
    o[GL_EV_OFS_TARGET] = tgt;
    o[GL_EV_OFS_CMD] = cmd;
    o[GL_EV_OFS_RESULT] = result;
    o[GL_EV_OFS_ECMD_SEQ] = eseq;
    icd_put_u16(&o[GL_EV_OFS_DETAIL], detail);
    icd_put_u32(&o[GL_EV_OFS_UPTIME_S], t->node.now_ms / 1000u);
    seal(o, GL_EV_LEN - 2);
    if (!q_push(&t->prq, o, GL_EV_LEN)) t->tx_dropped++; /* still in the log below */
    uint8_t *e = t->ev_log[t->ev_head];
    icd_put_u16(&e[0], t->ev_no);
    e[2] = tgt;
    e[3] = cmd;
    e[4] = result;
    t->ev_head = (uint8_t)((t->ev_head + 1) % GL_EVL_COUNT);
}

static void report_set(ttc_report_t *r, uint8_t src, uint8_t tgt, uint8_t cmd, uint8_t eseq)
{
    r->active = true;
    r->src = src;
    r->tgt = tgt;
    r->cmd = cmd;
    r->eseq = eseq;
}

static void report_done(ttc_t *t, ttc_report_t *r, uint8_t result, uint16_t detail)
{
    if (!r->active) return;
    r->active = false;
    emit_event(t, r->src, r->tgt, r->cmd, result, r->eseq, detail);
}

/* result: why it stopped (OK = deadline or antenna open, HW_FAULT = RBF back in,
 * INVALID_MODE = mode). Automatic and emergency burns report it to the ground. */
static void burn_stop(ttc_t *t, uint32_t now, uint8_t result)
{
    if (t->burning) {
        ttc_hw_burn(false);
        t->burning = false;
        t->burn_done = true;
        uint32_t d = now - t->burn_start;
        t->last_burn_ms = d > 65535u ? 65535u : (uint16_t)d;
        report_done(t, &t->burn_rep, result, t->last_burn_ms);
    }
}

static void burn_start(ttc_t *t, uint32_t now, uint16_t ms, uint8_t src)
{
    t->armed = false;
    t->burning = true;
    t->burn_done = false;
    t->burn_src = src;
    t->burn_start = now;
    t->burn_end = now + ms;
    if (t->burn_count < 255) t->burn_count++;
    t->burn_rep.active = false; /* a manual burn reports through the OBC's ACK */
    t->burn_sw_seen = false;
    ttc_hw_burn(true);
}

static bool rbf_removed(void) { return ttc_hw_rbf_in() == (bool)TTC_RBF_REMOVED_LEVEL; }
static bool deployed(void) { return ttc_hw_deploy_sw() == (bool)TTC_DEPLOYED_LEVEL; }

static bool auto_active(const ttc_t *t)
{
    return !ttc_nv_get(&t->nv, TTC_NV_DEPLOYED) &&
           ttc_nv_get(&t->nv, TTC_NV_AUTO_ATTEMPTS) < TTC_AUTODEPLOY_MAX_ATTEMPTS;
}

static uint8_t deploy_flags(const ttc_t *t)
{
    uint8_t df = 0;
    if (ttc_hw_deploy_sw()) df |= TTC_DF_DEPLOY_SW;
    if (ttc_hw_rbf_in()) df |= TTC_DF_RBF_IN;
    if (t->armed) df |= TTC_DF_ARMED;
    if (t->burning) df |= TTC_DF_BURNING;
    if (t->burn_done) df |= TTC_DF_BURN_DONE;
    if (ttc_nv_get(&t->nv, TTC_NV_DEPLOYED)) df |= TTC_DF_DEPLOYED_FLAG;
    if (auto_active(t)) df |= TTC_DF_AUTO_ACTIVE;
    return df;
}

static void nv_set(ttc_t *t, uint8_t key, uint32_t v)
{
    if (!ttc_nv_set(&t->nv, key, v)) icd_node_raise_fault(&t->node, TTC_FLT_NV);
}

static bool valid_ground_packet(const uint8_t *d, uint8_t n)
{
    if (n < GL_HDR_LEN + GL_CRC_LEN || n > GL_MAX_LEN) return false;
    if (d[GL_OFS_SAT] != GL_SAT_ID) return false;
    uint16_t crc = icd_crc16(d, (size_t)(n - 2));
    return (uint8_t)crc == d[n - 2] && (uint8_t)(crc >> 8) == d[n - 1];
}

static void seal(uint8_t *o, uint8_t n_body)
{
    uint16_t crc = icd_crc16(o, n_body);
    icd_put_u16(&o[n_body], crc);
}

static void default_radio(ttc_radio_cfg_t *r)
{
    r->freq_down_hz = TTC_FREQ_DOWN_DEFAULT_HZ;
    r->freq_up_hz = TTC_FREQ_UP_DEFAULT_HZ;
    r->sf = TTC_SF_DEFAULT;
    r->bw = TTC_BW_DEFAULT;
    r->cr = TTC_CR_DEFAULT;
    r->power = TTC_POWER_DEFAULT_DBM;
    r->sync = TTC_SYNC_DEFAULT;
}

uint32_t ttc_airtime_ms(const ttc_radio_cfg_t *c, uint8_t n)
{
    static const uint32_t bw_hz[4] = {62500, 125000, 250000, 500000};
    uint32_t sf = c->sf < 7 ? 7 : c->sf > 12 ? 12 : c->sf;
    uint32_t ts_us = (uint32_t)((1000000ull << sf) / bw_hz[c->bw > 3 ? 1 : c->bw]);
    uint32_t de = ts_us > 16000u ? 1u : 0u; /* low data rate optimisation, as sx1278.c */
    int32_t num = 8 * (int32_t)n - 4 * (int32_t)sf + 28 + 16; /* CRC on, explicit header */
    int32_t den = 4 * (int32_t)(sf - 2 * de);
    int32_t ceil_ = num > 0 ? (num + den - 1) / den : 0;
    uint32_t sym = 8u + (uint32_t)ceil_ * c->cr;
    uint64_t us = (uint64_t)ts_us * 49u / 4u + (uint64_t)ts_us * sym; /* 8 + 4.25 preamble */
    return (uint32_t)(us / 1000u) + 1u;
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
    seal(out, GL_BT_LEN - 2);
    return GL_BT_LEN;
}

/* ---------------- ICD callbacks ---------------- */

static uint16_t live_faults(void *ctx)
{
    ttc_t *t = (ttc_t *)ctx;
    uint16_t f = 0;
    if (!t->radio_ok) f |= TTC_FLT_RADIO_INIT;
    if (!t->temp_ok) f |= TTC_FLT_TEMP_SENSOR;
    if (!t->nv.ok) f |= TTC_FLT_NV;
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
        burn_stop(t, t->node.now_ms, ICD_ST_INVALID_MODE);
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
        o[TTC_TLM0_OFS_DEPLOY_FLAGS] = deploy_flags(t);
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
        uint32_t a = ttc_nv_get(&t->nv, TTC_NV_AUTO_ATTEMPTS);
        o[TTC_TLM1_OFS_AUTO_ATTEMPTS] = a > 255u ? 255u : (uint8_t)a;
        icd_put_u16(&o[TTC_TLM1_OFS_LAST_BURN_MS], t->last_burn_ms);
        *len = TTC_TLM1_LEN;
        return ICD_ST_OK;
    }
    return ICD_ST_BAD_PARAM;
}

static void put_radio(const ttc_radio_cfg_t *c, uint8_t *o)
{
    icd_put_u32(&o[TTC_RADIO_OFS_FREQ_DOWN_HZ], c->freq_down_hz);
    icd_put_u32(&o[TTC_RADIO_OFS_FREQ_UP_HZ], c->freq_up_hz);
    o[TTC_RADIO_OFS_SF] = c->sf;
    o[TTC_RADIO_OFS_BW] = c->bw;
    o[TTC_RADIO_OFS_CR] = c->cr;
    o[TTC_RADIO_OFS_POWER_DBM] = (uint8_t)c->power;
    o[TTC_RADIO_OFS_SYNC] = c->sync;
}

static uint8_t set_beacon_period(ttc_t *t, uint16_t p)
{
    if (p != 0 && p < TTC_BEACON_PERIOD_MIN_S) return ICD_ST_BAD_PARAM;
    t->beacon_period_s = p;
    return ICD_ST_OK;
}

static uint8_t queue_tx_part(ttc_t *t, const uint8_t *req, uint8_t n)
{
    if (n <= TTC_TXP_OFS_DATA) return ICD_ST_BAD_LENGTH;
    if (!tx_allowed_mode(t)) {
        t->txp_active = false;
        return ICD_ST_INVALID_MODE;
    }
    uint8_t off = req[TTC_TXP_OFS_OFFSET], dn = (uint8_t)(n - TTC_TXP_OFS_DATA);
    if (off == 0) {
        t->txp_active = true; /* a new packet: drop any half-built one */
        t->txp_len = 0;
    } else if (!t->txp_active || off != t->txp_len) {
        t->txp_active = false; /* a part went missing: the OBC starts again */
        return ICD_ST_BAD_PARAM;
    }
    if (t->txp_len + dn > TTC_TX_MAX_LEN) {
        t->txp_active = false;
        return ICD_ST_BAD_LENGTH;
    }
    memcpy(&t->txp[t->txp_len], &req[TTC_TXP_OFS_DATA], dn);
    t->txp_len = (uint16_t)(t->txp_len + dn);
    if (!(req[TTC_TXP_OFS_FLAGS] & TTC_TXP_F_LAST)) return ICD_ST_OK;
    t->txp_active = false;
    return q_push(&t->txq, t->txp, t->txp_len) ? ICD_ST_OK : ICD_ST_QUEUE_FULL;
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
        if (!tx_allowed_mode(t) && !is_cmd_answer(req, n)) return ICD_ST_INVALID_MODE;
        return q_push(&t->txq, req, n) ? ICD_ST_OK : ICD_ST_QUEUE_FULL;

    case TTC_CMD_QUEUE_TX_PART:
        return queue_tx_part(t, req, n);

    case TTC_CMD_SET_BEACON:
        memcpy(t->obc_beacon, req, n);
        t->obc_beacon_len = n;
        t->obc_beacon_time = now;
        return ICD_ST_OK;

    case TTC_CMD_SET_BEACON_PERIOD:
        if (n != 2) return ICD_ST_BAD_LENGTH;
        return set_beacon_period(t, icd_get_u16(req));

    case TTC_CMD_SET_RADIO: {
        if (n != TTC_RADIO_LEN) return ICD_ST_BAD_LENGTH;
        ttc_radio_cfg_t c;
        c.freq_down_hz = icd_get_u32(&req[TTC_RADIO_OFS_FREQ_DOWN_HZ]);
        c.freq_up_hz = icd_get_u32(&req[TTC_RADIO_OFS_FREQ_UP_HZ]);
        c.sf = req[TTC_RADIO_OFS_SF];
        c.bw = req[TTC_RADIO_OFS_BW];
        c.cr = req[TTC_RADIO_OFS_CR];
        c.power = (int8_t)req[TTC_RADIO_OFS_POWER_DBM];
        c.sync = req[TTC_RADIO_OFS_SYNC];
        /* Both frequencies in the band, and the uplink above the downlink. */
        if (c.freq_down_hz < TTC_FREQ_MIN_HZ || c.freq_up_hz > TTC_FREQ_MAX_HZ ||
            c.freq_up_hz <= c.freq_down_hz || c.sf < 7 || c.sf > 12 || c.bw > TTC_BW_500K ||
            c.cr < 5 || c.cr > 8 || c.power < 2 || c.power > 17)
            return ICD_ST_BAD_PARAM;
        t->radio_pending = c;
        t->radio_cfg_pending = true; /* applied when the radio is idle */
        t->radio_pending_needs_confirm = true;
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
        if (t->burning) return ICD_ST_INVALID_MODE; /* never extend a running (e.g. automatic) burn */
        burn_start(t, now, ms, TTC_BURN_MANUAL);
        return ICD_ST_OK;
    }

    case TTC_CMD_DEPLOY_RESET:
        /* Bench only: clearing the flag on a flying satellite would burn again. */
        if (n != 4) return ICD_ST_BAD_LENGTH;
        if (icd_get_u32(req) != TTC_DEPLOY_RESET_KEY) return ICD_ST_BAD_PARAM;
        if (rbf_removed()) return ICD_ST_INVALID_MODE;
        nv_set(t, TTC_NV_DEPLOYED, 0);
        nv_set(t, TTC_NV_AUTO_ATTEMPTS, 0);
        t->auto_wait = false;
        t->deployed_seen = false;
        return t->nv.ok ? ICD_ST_OK : ICD_ST_HW_FAULT;

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

/* ---------------- emergency commands (GL_T_ECMD) ---------------- */

static void line_pulse(ttc_t *t, uint8_t line, uint32_t now, uint32_t ms, uint8_t src, uint8_t tgt,
                       uint8_t cmd, uint8_t eseq)
{
    ttc_hw_line(line, true);
    t->line_on[line] = true;
    t->line_until[line] = now + ms;
    report_set(&t->line_rep[line], src, tgt, cmd, eseq);
}

/* *later = true: accepted, the outcome comes in an EVENT (GL_EA_REPORT_FOLLOWS). */
static uint8_t ecmd_exec(ttc_t *t, uint8_t tgt, uint8_t cmd, const uint8_t *a, uint8_t n, uint8_t *o,
                         uint8_t *on, uint32_t now, uint8_t eseq, bool *later)
{
    *on = 0;
    *later = false;
    bool keyed = n >= 4 && icd_get_u32(a) == GL_REBOOT_KEY;
    if (tgt == GL_TGT_TTC) {
        switch (cmd) {
        case GL_E_PING: {
            if (n != 0) return ICD_ST_BAD_LENGTH;
            uint8_t st[ICD_GET_STATUS_RESP_LEN];
            icd_node_peek_status(&t->node, st);
            icd_put_u32(&o[GL_EP_OFS_UPTIME_S], now / 1000u);
            o[GL_EP_OFS_MODE] = icd_node_mode(&t->node);
            memcpy(&o[GL_EP_OFS_FAULTS], &st[ICD_STATUS_OFS_FAULTS], 2);
            uint32_t silent = (now - t->node.last_contact_ms) / 1000u;
            icd_put_u16(&o[GL_EP_OFS_OBC_SILENT_S], silent > 65535u ? 65535u : (uint16_t)silent);
            o[GL_EP_OFS_DEPLOY_FLAGS] = deploy_flags(t);
            uint32_t at = ttc_nv_get(&t->nv, TTC_NV_AUTO_ATTEMPTS);
            o[GL_EP_OFS_AUTO_ATTEMPTS] = at > 255u ? 255u : (uint8_t)at;
            icd_put_u16(&o[GL_EP_OFS_WDT_MIN], (uint16_t)ttc_nv_get(&t->nv, TTC_NV_OBC_WDT_MIN));
            o[GL_EP_OFS_WDT_ACTIONS] = t->wdt_actions;
            uint32_t left = t->silence ? (t->silence_until - now + 59999u) / 60000u : 0;
            o[GL_EP_OFS_SILENCE_MIN] = left > 255u ? 255u : (uint8_t)left;
            *on = GL_EP_LEN;
            return ICD_ST_OK;
        }
        case GL_E_TTC_RESET:
            if (n != 4) return ICD_ST_BAD_LENGTH;
            if (!keyed) return ICD_ST_BAD_PARAM;
            t->reset_after_tx = true;
            report_set(&t->reset_rep, GL_EVS_ECMD, GL_TGT_TTC, GL_E_TTC_RESET, eseq);
            *later = true;
            return ICD_ST_OK;
        case GL_E_RADIO_DEFAULTS:
            if (n != 0) return ICD_ST_BAD_LENGTH;
            t->defaults_after_tx = true;
            report_set(&t->cfg_rep, GL_EVS_ECMD, GL_TGT_TTC, GL_E_RADIO_DEFAULTS, eseq);
            *later = true;
            return ICD_ST_OK;
        case GL_E_BEACON_PERIOD:
            if (n != 2) return ICD_ST_BAD_LENGTH;
            return set_beacon_period(t, icd_get_u16(a));
        case GL_E_TX_SILENCE: {
            if (n != 2) return ICD_ST_BAD_LENGTH;
            uint16_t m = icd_get_u16(a);
            if (m > GL_E_SILENCE_MAX_MIN) return ICD_ST_BAD_PARAM;
            t->silence = m != 0;
            t->silence_until = now + (uint32_t)m * 60000u;
            if (t->silence)
                report_set(&t->silence_rep, GL_EVS_ECMD, GL_TGT_TTC, GL_EV_SILENCE_END, eseq);
            else
                t->silence_rep.active = false;
            return ICD_ST_OK;
        }
        case GL_E_GET_EVENTS:
            if (n != 0) return ICD_ST_BAD_LENGTH;
            memset(o, 0, GL_EVL_COUNT * GL_EVL_ENTRY_LEN);
            for (uint8_t i = 0; i < GL_EVL_COUNT && i < t->ev_no; i++) /* newest first */
                memcpy(&o[i * GL_EVL_ENTRY_LEN],
                       t->ev_log[(t->ev_head + 2u * GL_EVL_COUNT - 1u - i) % GL_EVL_COUNT], GL_EVL_ENTRY_LEN);
            *on = GL_EVL_COUNT * GL_EVL_ENTRY_LEN;
            return ICD_ST_OK;
        case GL_E_BURN: {
            if (n != 2) return ICD_ST_BAD_LENGTH;
            uint16_t ms = icd_get_u16(a);
            if (ms == 0 || ms > TTC_BURN_MAX_MS) return ICD_ST_BAD_PARAM;
            if (!rbf_removed()) return ICD_ST_HW_FAULT;
            if (t->burning || commanded_safe(t)) return ICD_ST_INVALID_MODE;
            burn_start(t, now, ms, TTC_BURN_EMERGENCY);
            report_set(&t->burn_rep, GL_EVS_ECMD, GL_TGT_TTC, GL_E_BURN, eseq);
            *later = true;
            return ICD_ST_OK;
        }
        default:
            return ICD_ST_UNKNOWN_CMD;
        }
    }
    if (tgt == GL_TGT_OBC) {
        switch (cmd) {
        case GL_E_OBC_RESET:
            if (n != 4) return ICD_ST_BAD_LENGTH;
            if (!keyed) return ICD_ST_BAD_PARAM;
            line_pulse(t, TTC_LINE_OBC_RUN, now, GL_E_OBC_RESET_PULSE_MS, GL_EVS_ECMD, tgt, cmd, eseq);
            *later = true;
            return ICD_ST_OK;
        case GL_E_OBC_POWER_CYCLE:
            if (n != 5) return ICD_ST_BAD_LENGTH;
            if (!keyed || a[4] == 0 || a[4] > GL_E_POWER_OFF_MAX_S) return ICD_ST_BAD_PARAM;
            line_pulse(t, TTC_LINE_OBC_GLOBAL_EN, now, (uint32_t)a[4] * 1000u, GL_EVS_ECMD, tgt, cmd, eseq);
            *later = true;
            return ICD_ST_OK;
        case GL_E_OBC_WATCHDOG: {
            if (n != 2) return ICD_ST_BAD_LENGTH;
            uint16_t m = icd_get_u16(a);
            if (m != 0 && (m < GL_E_WDT_MIN_MIN || m > GL_E_WDT_MAX_MIN)) return ICD_ST_BAD_PARAM;
            nv_set(t, TTC_NV_OBC_WDT_MIN, m);
            t->wdt_actions = 0;
            return t->nv.ok ? ICD_ST_OK : ICD_ST_HW_FAULT;
        }
        default:
            return ICD_ST_UNKNOWN_CMD;
        }
    }
    if (tgt == GL_TGT_ADCS && cmd == GL_E_ADCS_RESET) {
        if (n != 4) return ICD_ST_BAD_LENGTH;
        if (!keyed) return ICD_ST_BAD_PARAM;
        line_pulse(t, TTC_LINE_ADCS_NRST, now, GL_E_ADCS_RESET_PULSE_MS, GL_EVS_ECMD, tgt, cmd, eseq);
        *later = true;
        return ICD_ST_OK;
    }
    return ICD_ST_UNKNOWN_CMD; /* EPS has no processor and no TT&C line */
}

/* Check, execute and acknowledge one emergency command. Bad or replayed
 * packets get no answer, as on the OBC. */
static void ecmd_rx(ttc_t *t, const uint8_t *d, uint8_t n, uint32_t now)
{
    const uint8_t trailer = GL_AUTH_CTR_LEN + GL_AUTH_MAC_LEN + GL_CRC_LEN;
    uint8_t an = n > GL_CMD_OFS_LEN ? d[GL_CMD_OFS_LEN] : 0xFF;
    if (ttc_ecmd_key_len == 0 || an > GL_CMD_MAX_ARGS || GL_CMD_OFS_ARGS + an + trailer != n) {
        t->ecmd_rejected++;
        return;
    }
    uint8_t mac_at = (uint8_t)(n - GL_CRC_LEN - GL_AUTH_MAC_LEN);
    uint8_t mac[SHA256_LEN], diff = 0;
    hmac_sha256(ttc_ecmd_key, ttc_ecmd_key_len, d, mac_at, mac);
    for (unsigned i = 0; i < GL_AUTH_MAC_LEN; i++) diff |= (uint8_t)(mac[i] ^ d[mac_at + i]);
    if (diff) {
        t->ecmd_rejected++;
        return;
    }
    uint32_t ctr = icd_get_u32(&d[GL_CMD_OFS_ARGS + an]);
    uint32_t last = ttc_nv_get(&t->nv, TTC_NV_ECMD_COUNTER);
    if (ctr == last && n == t->ecmd_last_len && memcmp(d, t->ecmd_last, n) == 0) {
        q_push(&t->prq, t->ecmd_ack, t->ecmd_ack_len); /* retransmission: same ACK, no re-run */
        return;
    }
    if (ctr <= last || ctr <= ttc_ecmd_ctr_floor) { /* replay; the floor holds even after a flash wipe */
        t->ecmd_rejected++;
        return;
    }
    nv_set(t, TTC_NV_ECMD_COUNTER, ctr); /* before acting, so a reset cannot reopen it */

    uint8_t data[GL_ACK_MAX_DATA], dn = 0;
    uint8_t tgt = d[GL_CMD_OFS_TARGET], cmd = d[GL_CMD_OFS_CMD];
    bool later = false;
    uint8_t st = ecmd_exec(t, tgt, cmd, &d[GL_CMD_OFS_ARGS], an, data, &dn, now, d[GL_OFS_SEQ], &later);
    if (st == ICD_ST_OK && later) {
        data[0] = GL_EA_REPORT_FOLLOWS;
        dn = 1;
    }
    t->ecmd_ok++;

    uint8_t *o = t->ecmd_ack;
    o[GL_OFS_SAT] = GL_SAT_ID;
    o[GL_OFS_TYPE] = GL_T_ACK;
    o[GL_OFS_SEQ] = d[GL_OFS_SEQ];
    o[GL_ACK_OFS_TARGET] = (uint8_t)(tgt | GL_TGT_EMERGENCY);
    o[GL_ACK_OFS_CMD] = cmd;
    o[GL_ACK_OFS_STATUS] = st;
    o[GL_ACK_OFS_LEN] = dn;
    memcpy(&o[GL_ACK_OFS_DATA], data, dn);
    seal(o, (uint8_t)(GL_ACK_OFS_DATA + dn));
    t->ecmd_ack_len = (uint8_t)(GL_ACK_OFS_DATA + dn + GL_CRC_LEN);
    memcpy(t->ecmd_last, d, n);
    t->ecmd_last_len = n;
    /* Ahead of any EVENT this command causes. If the queue is full, the
     * oldest waiting packet makes room: the newest answer matters most. */
    if (!q_push(&t->prq, o, t->ecmd_ack_len)) {
        uint8_t tmp[GL_MAX_LEN], tn;
        q_pop(&t->prq, tmp, &tn);
        t->tx_dropped++;
        q_push(&t->prq, o, t->ecmd_ack_len);
    }
}

/* ---------------- tasks ---------------- */

static void radio_start(ttc_t *t, uint32_t now)
{
    t->radio_ok = ttc_hw_radio_init(&t->radio);
    if (t->radio_ok) {
        ttc_hw_radio_rx();
        t->radio_state = TTC_RS_RX;
        t->rx_since = now;
    } else {
        t->radio_state = TTC_RS_OFF;
        t->radio_retry_at = now + TTC_RADIO_RETRY_MS;
    }
}

static void radio_rx_event(ttc_t *t, const uint8_t *buf, uint8_t len, uint32_t now)
{
    if (!valid_ground_packet(buf, len)) {
        t->rx_dropped++; /* not ours, or corrupted */
        return;
    }
    t->rx_count++;
    if (t->radio_unconfirmed) { /* the ground hears us and we hear it: keep these settings */
        t->radio_unconfirmed = false;
        t->radio_good = t->radio;
    }
    if (buf[GL_OFS_TYPE] == GL_T_ECMD) {
        ecmd_rx(t, buf, len, now);
        return;
    }
    if (!q_push(&t->rxq, buf, len)) {
        t->rx_dropped++;
        icd_node_raise_fault(&t->node, TTC_FLT_RX_OVERFLOW);
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
        radio_rx_event(t, buf, len, now);
    } else if (ev == TTC_EV_RX_CRC_ERR) {
        t->rx_crc_err++;
    } else if (ev == TTC_EV_TX_DONE && t->radio_state == TTC_RS_TX) {
        t->tx_count++;
        ttc_hw_radio_rx();
        t->radio_state = TTC_RS_RX;
        t->rx_since = now;
    }

    if (t->radio_state == TTC_RS_TX) {
        if ((uint32_t)(now - t->tx_start) >= t->tx_timeout_ms) {
            t->tx_timeouts++;
            icd_node_raise_fault(&t->node, TTC_FLT_RADIO_TX_TIMEOUT);
            radio_start(t, now);
        }
        return;
    }

    /* Never key up over a packet that is coming in (an emergency command may be
     * in it), but do not let noise block the transmitter for long. */
    /* The hold restarts with each detection. Noise cannot block the
     * transmitter: any quiet moment lets the next packet out, and a steady
     * "signal" is held for at most TTC_RX_HOLD_MAX_MS. (Not restarting it would
     * let a stale hold expire just as a real packet comes in.) */
    if (ttc_hw_radio_rx_active()) {
        if (!t->rx_hold) {
            t->rx_hold = true;
            t->rx_hold_since = now;
        }
        if ((uint32_t)(now - t->rx_hold_since) < TTC_RX_HOLD_MAX_MS) return;
    } else {
        t->rx_hold = false;
    }

    /* Radio is receiving and idle. Emergency ACKs and EVENTs go first, the
     * ACK on the settings the ground used; the actions it announced come after. */
    if (!t->prq.count) {
        if (t->reset_after_tx) {
            t->reset_after_tx = false;
            /* Reported after the reboot, from flash (TTC_NV_RESET_REPORT). */
            nv_set(t, TTC_NV_RESET_REPORT, 0x80000000u | t->reset_rep.eseq);
            ttc_hw_system_reset(); /* does not return on the STM32 */
            return;
        }
        if (t->defaults_after_tx) {
            t->defaults_after_tx = false;
            default_radio(&t->radio_pending);
            t->radio_cfg_pending = true;
            t->radio_pending_needs_confirm = false;
        }
        /* SET_RADIO settings nobody answered on: back to the last good ones. */
        if (t->radio_unconfirmed && !t->radio_cfg_pending &&
            (int32_t)(now - t->radio_confirm_until) >= 0) {
            t->radio_unconfirmed = false;
            t->radio_pending = t->radio_good;
            t->radio_cfg_pending = true;
            t->radio_pending_needs_confirm = false;
            t->radio_reverts++;
            icd_node_raise_fault(&t->node, TTC_FLT_RADIO_REVERTED);
            report_set(&t->cfg_rep, GL_EVS_AUTO, GL_TGT_TTC, GL_EV_RADIO_REVERTED, 0);
        }
        if (t->radio_cfg_pending) {
            t->radio_cfg_pending = false;
            t->radio = t->radio_pending;
            t->radio_unconfirmed = t->radio_pending_needs_confirm;
            if (t->radio_unconfirmed)
                t->radio_confirm_until = now + TTC_RADIO_CONFIRM_MS;
            else
                t->radio_good = t->radio;
            radio_start(t, now);
            /* Reported on the new settings: that is where the ground now listens. */
            report_done(t, &t->cfg_rep, t->radio_ok ? ICD_ST_OK : ICD_ST_HW_FAULT, t->radio_reverts);
            if (!t->radio_ok) return;
        }
    }

    uint8_t pkt[TTC_TX_MAX_LEN];
    uint8_t n = 0;
    if (q_pop(&t->prq, pkt, &n)) {
        /* priority: no listen gap */
    } else if ((uint32_t)(now - t->rx_since) < TTC_LISTEN_GAP_MS) {
        return; /* a window for the ground between our packets */
    } else if (t->silence) {
        t->beacon_due = false; /* radio silence: only emergency ACKs */
        return;
    } else if (t->beacon_due) {
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
        uint32_t to = 2u * ttc_airtime_ms(&t->radio, n) + 500u;
        t->tx_timeout_ms = to > TTC_TX_TIMEOUT_MS ? to : TTC_TX_TIMEOUT_MS;
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
    /* Stop at the deadline, as soon as the antenna reports deployed, if the
     * RBF plug goes back in, or if the mode no longer allows it. */
    bool mode_stop = t->burn_src == TTC_BURN_MANUAL ? !tx_allowed_mode(t) : commanded_safe(t);
    /* Debounced: the burn current switching next to the switch wiring must
     * not end a burn on one noisy reading. */
    bool sw_open = false;
    if (!deployed()) {
        t->burn_sw_seen = false;
    } else if (!t->burn_sw_seen) {
        t->burn_sw_seen = true;
        t->burn_sw_since = now;
    } else {
        sw_open = (uint32_t)(now - t->burn_sw_since) >= TTC_DEPLOY_DEBOUNCE_MS;
    }
    if ((int32_t)(now - t->burn_end) >= 0 || sw_open)
        burn_stop(t, now, ICD_ST_OK);
    else if (!rbf_removed())
        burn_stop(t, now, ICD_ST_HW_FAULT);
    else if (mode_stop)
        burn_stop(t, now, ICD_ST_INVALID_MODE);
}

/* Boot procedure, part 2: deploy the antenna once in the satellite's life. */
static void deploy_task(ttc_t *t, uint32_t now)
{
    if (!ttc_nv_get(&t->nv, TTC_NV_DEPLOYED)) {
        if (!deployed()) {
            t->deployed_seen = false;
        } else if (!t->deployed_seen) {
            t->deployed_seen = true;
            t->deployed_since = now;
        } else if ((uint32_t)(now - t->deployed_since) >= TTC_DEPLOYED_CONFIRM_MS) {
            nv_set(t, TTC_NV_DEPLOYED, 1); /* the check flag: never burn on our own again */
            emit_event(t, GL_EVS_AUTO, GL_TGT_TTC, GL_EV_DEPLOYED, ICD_ST_OK, 0, deploy_flags(t));
        }
    }
    if (!rbf_removed()) {
        t->rbf_out = false;
        return;
    }
    if (!t->rbf_out) {
        t->rbf_out = true;
        t->rbf_out_since = now;
    }
    if (!auto_active(t) || t->burning || t->deployed_seen || commanded_safe(t)) return;
    if ((uint32_t)(now - t->rbf_out_since) < TTC_AUTODEPLOY_DELAY_S * 1000u) return;
    if (t->auto_wait && (int32_t)(now - t->auto_next_at) < 0) return;
    /* Count the attempt before burning: a brown-out during the burn still counts. */
    nv_set(t, TTC_NV_AUTO_ATTEMPTS, ttc_nv_get(&t->nv, TTC_NV_AUTO_ATTEMPTS) + 1u);
    burn_start(t, now, TTC_AUTODEPLOY_BURN_MS, TTC_BURN_AUTO);
    report_set(&t->burn_rep, GL_EVS_AUTO, GL_TGT_TTC, GL_E_BURN, 0);
    t->auto_wait = true;
    t->auto_next_at = now + TTC_AUTODEPLOY_BURN_MS + TTC_AUTODEPLOY_RETRY_S * 1000u;
}

/* Emergency lines, radio silence, and the optional OBC watchdog. */
static void emergency_task(ttc_t *t, uint32_t now)
{
    for (uint8_t i = 0; i < TTC_LINES; i++) {
        if (t->line_on[i] && (int32_t)(now - t->line_until[i]) >= 0) {
            ttc_hw_line(i, false);
            t->line_on[i] = false;
            ttc_report_t *r = &t->line_rep[i];
            if (r->active && i != TTC_LINE_ADCS_NRST) { /* then wait for the Pi to talk again */
                t->obc_watch = true;
                t->obc_watch_t0 = now;
                t->obc_watch_src = r->src;
                t->obc_watch_eseq = r->eseq;
            }
            report_done(t, r, ICD_ST_OK, 0);
        }
    }
    if (t->obc_watch) {
        if ((int32_t)(t->node.last_contact_ms - t->obc_watch_t0) > 0) {
            uint32_t s_ = (t->node.last_contact_ms - t->obc_watch_t0) / 1000u;
            t->obc_watch = false;
            emit_event(t, t->obc_watch_src, GL_TGT_OBC, GL_EV_OBC_BACK, ICD_ST_OK, t->obc_watch_eseq,
                       s_ > 65535u ? 65535u : (uint16_t)s_);
        } else if ((uint32_t)(now - t->obc_watch_t0) >= GL_EV_OBC_WAIT_S * 1000u) {
            t->obc_watch = false;
            emit_event(t, t->obc_watch_src, GL_TGT_OBC, GL_EV_OBC_NOT_BACK, GL_ST_COMM_FAULT, t->obc_watch_eseq,
                       GL_EV_OBC_WAIT_S);
        }
    }
    if (t->silence && (int32_t)(now - t->silence_until) >= 0) {
        t->silence = false;
        report_done(t, &t->silence_rep, ICD_ST_OK, 0);
    }

    uint32_t wdt_min = ttc_nv_get(&t->nv, TTC_NV_OBC_WDT_MIN);
    if (wdt_min == 0 || !icd_node_master_lost(&t->node)) {
        t->wdt_actions = 0;
        return;
    }
    if (t->wdt_actions >= GL_E_WDT_MAX_ACTIONS) return; /* the OBC is beyond a reset: stop */
    if (t->line_on[TTC_LINE_OBC_RUN] || t->line_on[TTC_LINE_OBC_GLOBAL_EN]) return;
    uint32_t silent = now - t->node.last_contact_ms;
    if (silent < (uint32_t)(t->wdt_actions + 1u) * wdt_min * 60000u) return;
    if (t->wdt_actions == 0) /* first: a reset, then power cycles */
        line_pulse(t, TTC_LINE_OBC_RUN, now, GL_E_OBC_RESET_PULSE_MS, GL_EVS_AUTO, GL_TGT_OBC, GL_E_OBC_RESET, 0);
    else
        line_pulse(t, TTC_LINE_OBC_GLOBAL_EN, now, TTC_OBC_POWER_CYCLE_S * 1000u, GL_EVS_AUTO, GL_TGT_OBC,
                   GL_E_OBC_POWER_CYCLE, 0);
    t->wdt_actions++;
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
    q_setup(&t->rxq, t->rxq_data, t->rxq_lens, ICD_MAX_PAYLOAD, TTC_RX_QUEUE_DEPTH);
    q_setup(&t->txq, t->txq_data, t->txq_lens, TTC_TX_MAX_LEN, TTC_TX_QUEUE_DEPTH);
    q_setup(&t->prq, t->prq_data, t->prq_lens, GL_MAX_LEN, TTC_PRIO_DEPTH);
    ttc_nv_load(&t->nv); /* may erase (compact) the sector: before the I2C slave starts */
    if (!t->nv.ok) icd_node_raise_fault(&t->node, TTC_FLT_NV);
    default_radio(&t->radio);
    t->radio_good = t->radio;
    t->beacon_period_s = TTC_BEACON_PERIOD_DEFAULT_S;
    t->last_rssi = -200;

    ttc_hw_init();
    ttc_hw_burn(false);
    for (uint8_t i = 0; i < TTC_LINES; i++) ttc_hw_line(i, false);
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

    /* Tell the ground TT&C (re)started, and whether an E_TTC_RESET asked for it. */
    uint32_t rr = ttc_nv_get(&t->nv, TTC_NV_RESET_REPORT);
    if (rr) {
        nv_set(t, TTC_NV_RESET_REPORT, 0);
        emit_event(t, GL_EVS_ECMD, GL_TGT_TTC, GL_E_TTC_RESET, ICD_ST_OK, (uint8_t)rr, 0);
    } else {
        emit_event(t, GL_EVS_AUTO, GL_TGT_TTC, GL_EV_BOOT, ICD_ST_OK, 0, 0);
    }
    /* A blank flag store: first boot ever, or sector 7 was erased. Say so, so
     * the ground knows the burn count and the counter started again. A deployed
     * antenna is found again from DEPLOY_SW within 2 s, long before any burn. */
    if (!ttc_nv_get(&t->nv, TTC_NV_INIT)) {
        nv_set(t, TTC_NV_INIT, 1);
        emit_event(t, GL_EVS_AUTO, GL_TGT_TTC, GL_EV_NV_BLANK, t->nv.ok ? ICD_ST_OK : ICD_ST_HW_FAULT, 0, 0);
    }
}

void ttc_step(ttc_t *t, uint32_t now)
{
    icd_node_poll(&t->node, now);
    beacon_task(t, now);
    radio_task(t, now);
    burn_task(t, now);
    deploy_task(t, now);
    emergency_task(t, now);
    housekeeping_task(t, now);
}
