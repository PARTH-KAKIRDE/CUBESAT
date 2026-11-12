#include "icd_node.h"

#include <string.h>

#include "icd_pack.h"

static uint16_t live_faults(icd_node_t *n)
{
    return n->app->live_faults ? n->app->live_faults(n->app->ctx) : 0;
}

static void fill_status(icd_node_t *n, uint8_t *out)
{
    uint16_t live = live_faults(n);
    uint8_t flags = 0;
    if (live) flags |= ICD_SF_FAULT_ACTIVE;
    if (n->no_master_latched || n->master_lost) flags |= ICD_SF_NO_MASTER_CONTACT;
    if (n->reset_flag) flags |= ICD_SF_RESET_OCCURRED;
    if (n->app->extra_flags) flags |= n->app->extra_flags(n->app->ctx);
    out[ICD_STATUS_OFS_MODE] = n->mode;
    out[ICD_STATUS_OFS_FLAGS] = flags;
    icd_put_u16(&out[ICD_STATUS_OFS_FAULTS], (uint16_t)(n->sticky_faults | live));
    icd_put_u32(&out[ICD_STATUS_OFS_UPTIME_S], n->now_ms / 1000u);
}

void icd_node_peek_status(icd_node_t *n, uint8_t *out) { fill_status(n, out); }

static uint8_t handle_set_mode(icd_node_t *n, uint8_t to)
{
    if (to == ICD_MODE_BOOT || to >= ICD_MODE_COUNT) return ICD_ST_INVALID_MODE;
    if (to == n->mode) return ICD_ST_OK;
    /* From BOOT only SAFE may be requested. */
    if (n->mode == ICD_MODE_BOOT && to != ICD_MODE_SAFE) return ICD_ST_INVALID_MODE;
    if (n->app->on_set_mode) {
        uint8_t st = n->app->on_set_mode(n->app->ctx, n->mode, to);
        if (st != ICD_ST_OK && to != ICD_MODE_SAFE) return st;
    }
    n->mode = to;
    n->mode_reason = ICD_MR_COMMAND;
    return ICD_ST_OK;
}

static uint8_t dispatch(void *ctx, uint8_t cmd, const uint8_t *req, uint8_t req_len,
                        uint8_t *resp, uint8_t *resp_len)
{
    icd_node_t *n = (icd_node_t *)ctx;
    *resp_len = 0;

    /* Any valid frame counts as master contact. */
    n->last_contact_ms = n->now_ms;
    n->master_lost = false;

    switch (cmd) {
    case ICD_CMD_PING:
        return req_len == 0 ? ICD_ST_OK : ICD_ST_BAD_LENGTH;

    case ICD_CMD_GET_VERSION:
        if (req_len != 0) return ICD_ST_BAD_LENGTH;
        resp[0] = n->app->fw_major;
        resp[1] = n->app->fw_minor;
        resp[2] = n->app->fw_patch;
        resp[3] = ICD_VERSION_MAJOR;
        resp[4] = ICD_VERSION_MINOR;
        *resp_len = ICD_GET_VERSION_RESP_LEN;
        return ICD_ST_OK;

    case ICD_CMD_GET_STATUS: {
        if (req_len != 0) return ICD_ST_BAD_LENGTH;
        fill_status(n, resp);
        *resp_len = ICD_GET_STATUS_RESP_LEN;
        /* Reported: clear one-shot flags and faults that have gone away. */
        n->reset_flag = false;
        n->no_master_latched = false;
        n->sticky_faults &= live_faults(n);
        return ICD_ST_OK;
    }

    case ICD_CMD_GET_TELEMETRY:
        if (req_len != 1) return ICD_ST_BAD_LENGTH;
        if (!n->app->get_telemetry) return ICD_ST_BAD_PARAM;
        return n->app->get_telemetry(n->app->ctx, req[0], resp, resp_len);

    case ICD_CMD_SET_MODE:
        if (req_len != 1) return ICD_ST_BAD_LENGTH;
        return handle_set_mode(n, req[0]);

    default:
        if (!n->app->app_command) return ICD_ST_UNKNOWN_CMD;
        return n->app->app_command(n->app->ctx, cmd, req, req_len, resp, resp_len);
    }
}

void icd_node_init(icd_node_t *n, const icd_node_app_t *app, uint32_t now_ms)
{
    memset(n, 0, sizeof(*n));
    n->app = app;
    n->mode = ICD_MODE_BOOT;
    n->mode_reason = ICD_MR_BOOT;
    n->reset_flag = true; /* every boot (power-on or watchdog) */
    n->now_ms = now_ms;
    n->last_contact_ms = now_ms;
    icd_link_init(&n->link, dispatch, n);
}

void icd_node_poll(icd_node_t *n, uint32_t now_ms)
{
    n->now_ms = now_ms;
    /* Latch every fault seen, even for one pass, until GET_STATUS reports it. */
    n->sticky_faults |= live_faults(n);
    icd_link_poll(&n->link);
    if (!n->master_lost && (uint32_t)(now_ms - n->last_contact_ms) >= ICD_NO_MASTER_TIMEOUT_MS) {
        n->master_lost = true;
        n->no_master_latched = true;
    }
}

void icd_node_set_mode(icd_node_t *n, uint8_t mode, uint8_t reason)
{
    if (mode >= ICD_MODE_COUNT) return;
    n->mode = mode;
    n->mode_reason = reason;
}

void icd_node_raise_fault(icd_node_t *n, uint16_t bits) { n->sticky_faults |= bits; }
