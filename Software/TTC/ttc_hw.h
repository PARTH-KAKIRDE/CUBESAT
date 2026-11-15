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
    uint32_t freq_down_hz; /* satellite TX (downlink) */
    uint32_t freq_up_hz;   /* satellite RX (uplink), higher than freq_down_hz */
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
void ttc_hw_radio_rx(void);                         /* continuous receive on freq_up_hz */
bool ttc_hw_radio_tx(const uint8_t *data, uint8_t len); /* one packet (1..255 B) on freq_down_hz */
/* True while the radio is picking up a packet (preamble/header seen, not yet
 * done). TT&C does not start a transmission then (ICD v0.9 emergency priority). */
bool ttc_hw_radio_rx_active(void);
/* Poll the radio. buf must hold TTC_RADIO_BUF bytes. */
int ttc_hw_radio_poll(uint8_t *buf, uint8_t *len, int16_t *rssi_dbm, int8_t *snr_q);

bool ttc_hw_temp_read(int16_t *centi_c);            /* TMP102 */
bool ttc_hw_deploy_sw(void);                        /* raw pin level */
bool ttc_hw_rbf_in(void);                           /* raw pin level */
void ttc_hw_burn(bool on);                          /* BURN_EN */
void ttc_hw_led(bool on);
uint16_t ttc_hw_local_i2c_errors(void);

/* Emergency lines (ICD v0.9): TT&C drives them without the OBC. "Asserted"
 * means the target is held in reset / off. On the board each goes through an
 * N-MOSFET (2N7002) to the target pin, so a high STM32 pin asserts it and the
 * reset-state input (pull-down) leaves every target running. */
#define TTC_LINE_OBC_RUN       0   /* Pi 4B RUN: low = CPU reset          */
#define TTC_LINE_OBC_GLOBAL_EN 1   /* Pi 4B GLOBAL_EN: low = PMIC off      */
#define TTC_LINE_ADCS_NRST     2   /* ADCS STM32 NRST: low = reset         */
#define TTC_LINES              3
void ttc_hw_line(uint8_t line, bool asserted);

void ttc_hw_system_reset(void);                     /* reboot TT&C (NVIC) */

/* One flash sector for ttc_nv.c. Erased words read 0xFFFFFFFF; a word is
 * programmed once (only 1 -> 0 bits). Offsets are bytes, multiples of 4. */
uint32_t ttc_hw_nv_size(void);
uint32_t ttc_hw_nv_read(uint32_t ofs);
bool ttc_hw_nv_program(uint32_t ofs, uint32_t word);
bool ttc_hw_nv_erase(void);                         /* blocks ~1-2 s on the STM32 */

#endif
