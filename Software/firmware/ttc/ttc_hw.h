/*
 * TT&C hardware interface. Everything above it is portable.
 *   hw_stm32.c + sx1278.c - real board
 *   firmware/host/ttc_hw_sim.c - PC simulation (radio = in-memory queues)
 */
#ifndef TTC_HW_H
#define TTC_HW_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t freq_hz;
    uint8_t sf;     /* 7..12 */
    uint8_t bw;     /* TTC_BW_* */
    uint8_t cr;     /* 5..8 = 4/5..4/8 */
    int8_t power;   /* dBm 2..17 (PA_BOOST) */
    uint8_t sync;
} ttc_radio_cfg_t;

#define TTC_EV_NONE       0
#define TTC_EV_RX         1   /* packet in buf */
#define TTC_EV_RX_CRC_ERR 2
#define TTC_EV_TX_DONE    3

#define TTC_RADIO_BUF     255

bool ttc_hw_init(void);
bool ttc_hw_radio_init(const ttc_radio_cfg_t *cfg); /* false if the chip does not answer */
void ttc_hw_radio_rx(void);                         /* continuous receive */
bool ttc_hw_radio_tx(const uint8_t *data, uint8_t len);
/* Poll the radio. buf must hold TTC_RADIO_BUF bytes. */
int ttc_hw_radio_poll(uint8_t *buf, uint8_t *len, int16_t *rssi_dbm, int8_t *snr_q);

bool ttc_hw_temp_read(int16_t *centi_c);            /* TMP102 */
bool ttc_hw_deploy_sw(void);                        /* raw pin level */
bool ttc_hw_rbf_in(void);                           /* raw pin level */
void ttc_hw_burn(bool on);                          /* BURN_EN */
void ttc_hw_led(bool on);
uint16_t ttc_hw_local_i2c_errors(void);

#endif
