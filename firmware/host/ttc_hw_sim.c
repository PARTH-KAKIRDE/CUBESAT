/* Simulated TT&C hardware: the radio is two in-memory packet queues. */
#include <string.h>

#include "sim_api.h"
#include "sim_internal.h"
#include "ttc_hw.h"

#define QN 32

typedef struct {
    uint8_t d[TTC_RADIO_BUF];
    uint8_t n;
    int16_t rssi;
} pkt_t;

static pkt_t up[QN], down[QN];
static int up_h, up_n, down_h, down_n;
static int radio_ok = 1, rx_on, tx_busy;
static uint32_t tx_done_at;
static int deploy_pin = 0, rbf_pin = 1, burn;
static uint32_t burn_ms;

void ttc_sim_reset(void)
{
    up_h = up_n = down_h = down_n = 0;
    radio_ok = 1;
    rx_on = tx_busy = 0;
    deploy_pin = 0;
    rbf_pin = 1;
    burn = 0;
    burn_ms = 0;
}

void ttc_sim_step(void)
{
    if (burn) burn_ms++;
}

bool ttc_hw_init(void) { return true; }

bool ttc_hw_radio_init(const ttc_radio_cfg_t *c)
{
    (void)c;
    tx_busy = 0;
    rx_on = 0;
    return radio_ok != 0;
}

void ttc_hw_radio_rx(void)
{
    rx_on = 1;
    tx_busy = 0;
}

bool ttc_hw_radio_tx(const uint8_t *d, uint8_t n)
{
    if (!radio_ok) return false;
    if (down_n < QN) {
        pkt_t *p = &down[(down_h + down_n) % QN];
        memcpy(p->d, d, n);
        p->n = n;
        down_n++;
    }
    rx_on = 0;
    tx_busy = 1;
    tx_done_at = sim_time_ms() + 60u + n; /* rough LoRa airtime */
    return true;
}

int ttc_hw_radio_poll(uint8_t *buf, uint8_t *len, int16_t *rssi, int8_t *snr)
{
    if (!radio_ok) return TTC_EV_NONE;
    if (tx_busy) {
        if (sim_time_ms() >= tx_done_at) {
            tx_busy = 0;
            return TTC_EV_TX_DONE;
        }
        return TTC_EV_NONE;
    }
    if (!rx_on || up_n == 0) return TTC_EV_NONE;
    pkt_t *p = &up[up_h];
    up_h = (up_h + 1) % QN;
    up_n--;
    memcpy(buf, p->d, p->n);
    *len = p->n;
    *rssi = p->rssi;
    *snr = 32; /* +8 dB */
    return TTC_EV_RX;
}

bool ttc_hw_temp_read(int16_t *c)
{
    *c = 2437;
    return true;
}

bool ttc_hw_deploy_sw(void) { return deploy_pin != 0; }
bool ttc_hw_rbf_in(void) { return rbf_pin != 0; }
void ttc_hw_burn(bool on) { burn = on ? 1 : 0; }
void ttc_hw_led(bool on) { (void)on; }
uint16_t ttc_hw_local_i2c_errors(void) { return 0; }

SIM_API void sim_ttc_uplink(const uint8_t *d, int n, int rssi)
{
    if (up_n >= QN || n <= 0 || n > TTC_RADIO_BUF) return;
    pkt_t *p = &up[(up_h + up_n) % QN];
    memcpy(p->d, d, (size_t)n);
    p->n = (uint8_t)n;
    p->rssi = (int16_t)rssi;
    up_n++;
}

SIM_API int sim_ttc_downlink(uint8_t *buf)
{
    if (!down_n) return 0;
    pkt_t *p = &down[down_h];
    down_h = (down_h + 1) % QN;
    down_n--;
    memcpy(buf, p->d, p->n);
    return p->n;
}

SIM_API void sim_ttc_set_pins(int d, int r)
{
    deploy_pin = d;
    rbf_pin = r;
}

SIM_API int sim_ttc_burn_on(void) { return burn; }
SIM_API uint32_t sim_ttc_burn_total_ms(void) { return burn_ms; }
SIM_API void sim_ttc_set_radio_ok(int ok) { radio_ok = ok; }
