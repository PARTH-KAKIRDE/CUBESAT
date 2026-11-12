/* TT&C application: radio scheduling, packet queues, beacons, burn wire,
 * ICD commands and telemetry. Portable: hardware only through ttc_hw.h. */
#ifndef TTC_APP_H
#define TTC_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "cubesat_icd.h"
#include "icd_node.h"
#include "ttc_hw.h"

#define TTC_FW_MAJOR 0
#define TTC_FW_MINOR 2
#define TTC_FW_PATCH 0

#define TTC_TX_TIMEOUT_MS       3000u
#define TTC_RADIO_RETRY_MS      10000u
#define TTC_TEMP_PERIOD_MS      1000u
#define TTC_OBC_BEACON_MAX_AGE  3u   /* use OBC beacon if updated within 3 periods */

/* Deploy pin polarity (TT&C header J2 "MISSION"). Both inputs have 10k
 * pull-ups, so an open switch reads 1. Check against the deploy hardware. */
#define TTC_RBF_REMOVED_LEVEL   1    /* RBF plug out (flight) -> pin high */
#define TTC_DEPLOYED_LEVEL      1    /* antenna released -> switch open -> high */

typedef struct {
    uint8_t data[ICD_MAX_PAYLOAD];
    uint8_t len;
} ttc_pkt_t;

typedef struct {
    ttc_pkt_t q[TTC_RX_QUEUE_DEPTH];
    uint8_t head, count;
} ttc_queue_t;

typedef struct {
    icd_node_t node;

    ttc_radio_cfg_t radio, radio_pending;
    bool radio_ok, radio_cfg_pending;
    uint8_t radio_state;
    uint32_t tx_start, radio_retry_at;

    ttc_queue_t rxq, txq;

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

    uint32_t armed_until;
    bool armed, burning, burn_done;
    uint32_t burn_start, burn_end;
    uint8_t burn_count;
    uint16_t last_burn_ms;

    uint16_t local_i2c_seen;
    uint32_t obc_resets_seen;
    uint32_t led_until;
} ttc_t;

void ttc_init(ttc_t *t, uint32_t now_ms);
void ttc_step(ttc_t *t, uint32_t now_ms);

/* Build TT&C's own beacon (GL_T_BEACON_TTC). Returns length. */
uint8_t ttc_build_beacon(ttc_t *t, uint8_t *out);

#endif
