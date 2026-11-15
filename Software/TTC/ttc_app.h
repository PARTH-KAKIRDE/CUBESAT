/* TT&C application: radio scheduling, packet queues, beacons, burn wire,
 * automatic antenna deployment, emergency commands, ICD commands and
 * telemetry. Portable: hardware only through ttc_hw.h. */
#ifndef TTC_APP_H
#define TTC_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "cubesat_icd.h"
#include "ground_link.h"
#include "icd_node.h"
#include "ttc_hw.h"
#include "ttc_nv.h"

#define TTC_FW_MAJOR 0
#define TTC_FW_MINOR 4
#define TTC_FW_PATCH 2   /* 0.4.2: TT&C review fixes (burn debounce, no overlapping burns, IWDG at boot) */

#define TTC_TX_TIMEOUT_MS       3000u   /* minimum; long packets get 2 x airtime + 500 ms */
#define TTC_RADIO_RETRY_MS      10000u
#define TTC_TEMP_PERIOD_MS      1000u
#define TTC_OBC_BEACON_MAX_AGE  3u   /* use OBC beacon if updated within 3 periods */
#define TTC_OBC_POWER_CYCLE_S   10u  /* watchdog power cycle: GLOBAL_EN low this long */

/* Deploy pin polarity (TT&C header J2 "MISSION"). Both inputs have 10k
 * pull-ups, so an open switch reads 1. Check against the deploy hardware. */
#define TTC_RBF_REMOVED_LEVEL   1    /* RBF plug out (flight) -> pin high */
#define TTC_DEPLOYED_LEVEL      1    /* antenna released -> switch open -> high */

/* Emergency-command key, from the build (firmware/build.py writes it from
 * TTC_KEY_FILE; the host tests use a fixed test key). Length 0 = disabled. */
extern const uint8_t ttc_ecmd_key[];
extern const uint8_t ttc_ecmd_key_len;
/* Lowest emergency counter accepted, whatever flash says: the build time. */
extern const uint32_t ttc_ecmd_ctr_floor;

/* Fixed-slot ring buffer. Each queue has its own depth and slot size. */
typedef struct {
    uint8_t *data;      /* depth * slot bytes */
    uint8_t *lens;      /* depth entries */
    uint16_t slot;
    uint8_t depth, head, count;
} ttc_queue_t;

#define TTC_BURN_MANUAL    0   /* OBC BURN_ARM + BURN_FIRE */
#define TTC_BURN_AUTO      1   /* automatic deployment */
#define TTC_BURN_EMERGENCY 2   /* GL_E_BURN */

/* An action that sends a GL_T_EVENT when it finishes. */
typedef struct {
    bool active;
    uint8_t src, tgt, cmd, eseq;
} ttc_report_t;

typedef struct {
    icd_node_t node;
    ttc_nv_t nv;

    ttc_radio_cfg_t radio, radio_pending, radio_good;
    bool radio_ok, radio_cfg_pending, radio_pending_needs_confirm;
    bool radio_unconfirmed;          /* running on SET_RADIO settings not yet heard on */
    uint32_t radio_confirm_until;
    uint16_t radio_reverts;
    uint8_t radio_state;
    uint32_t tx_start, tx_timeout_ms, radio_retry_at;

    ttc_queue_t rxq, txq;
    uint8_t rxq_data[TTC_RX_QUEUE_DEPTH * ICD_MAX_PAYLOAD], rxq_lens[TTC_RX_QUEUE_DEPTH];
    uint8_t txq_data[TTC_TX_QUEUE_DEPTH * TTC_TX_MAX_LEN], txq_lens[TTC_TX_QUEUE_DEPTH];
    uint8_t txp[TTC_TX_MAX_LEN];     /* QUEUE_TX_PART packet being built */
    uint16_t txp_len;
    bool txp_active;

    uint8_t obc_beacon[ICD_MAX_PAYLOAD];
    uint8_t obc_beacon_len;
    uint32_t obc_beacon_time;
    uint16_t beacon_period_s;
    uint32_t last_beacon;
    bool beacon_due;
    uint8_t beacon_seq;

    int16_t last_rssi;
    int8_t last_snr;
    uint16_t rx_count, tx_count, rx_crc_err, tx_timeouts, rx_dropped, tx_dropped, beacon_count;

    int16_t temp;
    bool temp_ok;
    uint32_t last_temp;

    /* burn wire */
    uint32_t armed_until;
    bool armed, burning, burn_done;
    uint8_t burn_src;
    uint32_t burn_start, burn_end;
    uint8_t burn_count;
    uint16_t last_burn_ms;

    /* automatic deployment (boot procedure) */
    bool rbf_out;
    uint32_t rbf_out_since;
    bool auto_wait;                  /* waiting RETRY_S after a burn */
    uint32_t auto_next_at;
    bool deployed_seen;
    uint32_t deployed_since;
    bool burn_sw_seen;               /* deploy switch debounce while burning */
    uint32_t burn_sw_since;

    /* emergency commands (GL_T_ECMD) */
    ttc_queue_t prq;                 /* ECMD ACKs and EVENTs: go out before anything else */
    uint8_t prq_data[TTC_PRIO_DEPTH * GL_MAX_LEN], prq_lens[TTC_PRIO_DEPTH];
    uint32_t rx_since;               /* radio back in receive at */
    bool rx_hold;                    /* a packet is coming in: wait before sending (1 s at most) */
    uint32_t rx_hold_since;
    uint16_t ev_no;
    uint8_t ev_log[GL_EVL_COUNT][GL_EVL_ENTRY_LEN], ev_head;
    ttc_report_t line_rep[TTC_LINES], burn_rep, reset_rep, cfg_rep, silence_rep;
    bool obc_watch;                  /* waiting for the OBC to talk after a reset */
    uint32_t obc_watch_t0;
    uint8_t obc_watch_src, obc_watch_eseq;
    uint8_t ecmd_last[GL_MAX_LEN], ecmd_last_len, ecmd_ack[GL_MAX_LEN], ecmd_ack_len;
    uint16_t ecmd_ok, ecmd_rejected;
    bool reset_after_tx, defaults_after_tx;
    bool silence;
    uint32_t silence_until;
    bool line_on[TTC_LINES];
    uint32_t line_until[TTC_LINES];
    uint8_t wdt_actions;             /* OBC watchdog actions in this outage */

    uint16_t local_i2c_seen;
    uint32_t obc_resets_seen;
    uint32_t led_until;
} ttc_t;

void ttc_init(ttc_t *t, uint32_t now_ms);
void ttc_step(ttc_t *t, uint32_t now_ms);

/* Build TT&C's own beacon (GL_T_BEACON_TTC). Returns length. */
uint8_t ttc_build_beacon(ttc_t *t, uint8_t *out);

/* LoRa time on air of an n-byte packet with these settings, in ms. */
uint32_t ttc_airtime_ms(const ttc_radio_cfg_t *c, uint8_t n);

#endif
