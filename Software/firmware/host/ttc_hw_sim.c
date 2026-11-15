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
    uint32_t ready_at; /* half-duplex mode: received once its airtime is over */
} pkt_t;

static pkt_t up[QN], down[QN];
static int up_h, up_n, down_h, down_n;
static int radio_ok = 1, rx_on, tx_busy;
static uint32_t tx_done_at;
static int deploy_pin = 0, rbf_pin = 0, burn;
static uint32_t burn_ms;
static uint32_t freq_up, freq_down, rx_freq, tx_freq; /* split-frequency checks */
static int line_on[TTC_LINES];
static uint32_t line_pulses[TTC_LINES], line_ms[TTC_LINES];
static int reset_requested, resets;
/* Half-duplex model (off by default, so older tests see instant delivery):
 * an uplink packet sent while TT&C transmits is lost, one sent while it
 * listens takes 60 + n ms to arrive, and starting a TX loses it. */
static int half_duplex;
static uint32_t up_lost;
static int rx_noise; /* the modem "detects a signal" all the time (interference) */

/* Flash sector for ttc_nv: small, so the tests can fill it. Survives a TT&C
 * reboot (sim_reset_slave, GL_E_TTC_RESET); sim_init starts with it erased. */
#define NV_WORDS 128
static uint32_t nv[NV_WORDS];
static int nv_fail;
static uint32_t nv_erases, nv_programs;

void ttc_sim_reset(void)
{
    up_h = up_n = down_h = down_n = 0;
    radio_ok = 1;
    rx_on = tx_busy = 0;
    deploy_pin = 0;
    rbf_pin = 0; /* a satellite on the bench: RBF plug in, nothing burns on its own */
    burn = 0;
    burn_ms = 0;
    memset(line_on, 0, sizeof line_on);
    memset(line_pulses, 0, sizeof line_pulses);
    memset(line_ms, 0, sizeof line_ms);
    reset_requested = resets = 0;
    half_duplex = 0;
    up_lost = 0;
    rx_noise = 0;
    memset(nv, 0xFF, sizeof nv);
    nv_fail = 0;
    nv_erases = nv_programs = 0;
}

void ttc_sim_step(void)
{
    if (burn) burn_ms++;
    for (int i = 0; i < TTC_LINES; i++)
        if (line_on[i]) line_ms[i]++;
}

int ttc_sim_take_reset(void)
{
    int r = reset_requested;
    reset_requested = 0;
    if (r) resets++;
    return r;
}

bool ttc_hw_init(void) { return true; }

bool ttc_hw_radio_init(const ttc_radio_cfg_t *c)
{
    freq_up = c->freq_up_hz;
    freq_down = c->freq_down_hz;
    tx_busy = 0;
    rx_on = 0;
    return radio_ok != 0;
}

void ttc_hw_radio_rx(void)
{
    rx_freq = freq_up;
    rx_on = 1;
    tx_busy = 0;
}

bool ttc_hw_radio_rx_active(void)
{
    if (rx_noise && rx_on && !tx_busy) return true;
    return half_duplex && rx_on && !tx_busy && up_n > 0 && sim_time_ms() < up[up_h].ready_at;
}

bool ttc_hw_radio_tx(const uint8_t *d, uint8_t n)
{
    if (!radio_ok) return false;
    if (half_duplex && up_n) { /* the transmitter drowns anything still coming in */
        up_lost += (uint32_t)up_n;
        up_h = up_n = 0;
    }
    tx_freq = freq_down;
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
    if (sim_time_ms() < p->ready_at) return TTC_EV_NONE;
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

void ttc_hw_line(uint8_t line, bool asserted)
{
    if (line >= TTC_LINES) return;
    if (asserted && !line_on[line]) line_pulses[line]++;
    line_on[line] = asserted;
}

void ttc_hw_system_reset(void) { reset_requested = 1; } /* sim_run_ms reboots TT&C after this step */

uint32_t ttc_hw_nv_size(void) { return NV_WORDS * 4u; }
uint32_t ttc_hw_nv_read(uint32_t ofs) { return ofs / 4u < NV_WORDS ? nv[ofs / 4u] : 0xFFFFFFFFu; }

bool ttc_hw_nv_program(uint32_t ofs, uint32_t w)
{
    if (nv_fail || ofs % 4u || ofs / 4u >= NV_WORDS || nv[ofs / 4u] != 0xFFFFFFFFu) return false;
    nv[ofs / 4u] = w;
    nv_programs++;
    return true;
}

bool ttc_hw_nv_erase(void)
{
    if (nv_fail) return false;
    memset(nv, 0xFF, sizeof nv);
    nv_erases++;
    return true;
}

SIM_API void sim_ttc_uplink(const uint8_t *d, int n, int rssi)
{
    if (up_n >= QN || n <= 0 || n > TTC_RADIO_BUF) return;
    if (half_duplex && (tx_busy || !rx_on || up_n)) { /* deaf while sending; one packet at a time */
        up_lost++;
        return;
    }
    pkt_t *p = &up[(up_h + up_n) % QN];
    p->ready_at = half_duplex ? sim_time_ms() + 60u + (uint32_t)n : 0;
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
/* Frequency the simulated radio last received / transmitted on (ICD v0.8). */
SIM_API uint32_t sim_ttc_rx_freq(void) { return rx_freq; }
SIM_API uint32_t sim_ttc_tx_freq(void) { return tx_freq; }

SIM_API int sim_ttc_line(int line) { return line >= 0 && line < TTC_LINES ? line_on[line] : 0; }
SIM_API uint32_t sim_ttc_line_pulses(int line) { return line >= 0 && line < TTC_LINES ? line_pulses[line] : 0; }
SIM_API uint32_t sim_ttc_line_ms(int line) { return line >= 0 && line < TTC_LINES ? line_ms[line] : 0; }
SIM_API int sim_ttc_resets(void) { return resets; }
SIM_API void sim_ttc_nv_fail(int fail) { nv_fail = fail; }
SIM_API uint32_t sim_ttc_nv_erases(void) { return nv_erases; }
SIM_API uint32_t sim_ttc_nv_programs(void) { return nv_programs; }
/* Write a raw word, e.g. to fake a record cut short by a power loss. */
SIM_API void sim_ttc_nv_wipe(void) { memset(nv, 0xFF, sizeof nv); } /* a full-chip erase */
SIM_API void sim_ttc_nv_poke(uint32_t ofs, uint32_t w) { if (ofs / 4u < NV_WORDS) nv[ofs / 4u] = w; }
SIM_API void sim_ttc_set_half_duplex(int on) { half_duplex = on; }
SIM_API void sim_ttc_set_rx_noise(int on) { rx_noise = on; }
SIM_API uint32_t sim_ttc_uplink_lost(void) { return up_lost; }
