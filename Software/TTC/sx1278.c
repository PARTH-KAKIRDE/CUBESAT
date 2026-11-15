/*
 * SX1278 (RFM98W) LoRa driver for TT&C, register level over SPI1.
 * RESET = PC4. DIO0..5 are wired (PB0, PB1, PB12-15) but this driver polls
 * RegIrqFlags over SPI instead, which needs no interrupt setup.
 * Output on PA_BOOST (the RFM98W only brings out PA_BOOST).
 * Split frequency (ICD v0.8): receive on freq_up_hz, transmit on freq_down_hz.
 * The chip is half duplex, so it is retuned in standby before every TX and
 * again before going back to RX (three register writes; the synthesizer
 * settles in tens of microseconds).
 */
#include <stddef.h>

#include "cubesat_icd.h"
#include "platform.h"
#include "spi1.h"
#include "ttc_hw.h"

#define REG_FIFO           0x00
#define REG_OP_MODE        0x01
#define REG_FRF_MSB        0x06
#define REG_PA_CONFIG      0x09
#define REG_OCP            0x0B
#define REG_LNA            0x0C
#define REG_FIFO_ADDR_PTR  0x0D
#define REG_FIFO_TX_BASE   0x0E
#define REG_FIFO_RX_BASE   0x0F
#define REG_FIFO_RX_CUR    0x10
#define REG_IRQ_FLAGS      0x12
#define REG_RX_NB_BYTES    0x13
#define REG_MODEM_STAT     0x18
#define REG_PKT_SNR        0x19
#define REG_PKT_RSSI       0x1A
#define REG_MODEM_CONFIG1  0x1D
#define REG_MODEM_CONFIG2  0x1E
#define REG_PREAMBLE_MSB   0x20
#define REG_PREAMBLE_LSB   0x21
#define REG_PAYLOAD_LEN    0x22
#define REG_MODEM_CONFIG3  0x26
#define REG_SYNC_WORD      0x39
#define REG_DIO_MAPPING1   0x40
#define REG_VERSION        0x42
#define REG_PA_DAC         0x4D

#define MODE_LORA          0x80
#define MODE_SLEEP         0x00
#define MODE_STDBY         0x01
#define MODE_TX            0x03
#define MODE_RX_CONT       0x05

#define IRQ_RX_DONE        0x40
#define IRQ_CRC_ERR        0x20
#define IRQ_TX_DONE        0x08

static bool s_tx_mode;
static uint32_t s_freq_up_hz, s_freq_down_hz;

static void wr(uint8_t reg, uint8_t v)
{
    spi1_select();
    spi1_xfer((uint8_t)(reg | 0x80u));
    spi1_xfer(v);
    spi1_deselect();
}

static uint8_t rd(uint8_t reg)
{
    spi1_select();
    spi1_xfer((uint8_t)(reg & 0x7Fu));
    uint8_t v = spi1_xfer(0);
    spi1_deselect();
    return v;
}

static void set_mode(uint8_t m) { wr(REG_OP_MODE, (uint8_t)(MODE_LORA | m)); }

/* Frf = f * 2^19 / 32 MHz. Write in SLEEP or STDBY only; the new value takes
 * effect when the LSB is written. */
static void set_freq(uint32_t hz)
{
    uint64_t frf = ((uint64_t)hz << 19) / 32000000u;
    wr(REG_FRF_MSB, (uint8_t)(frf >> 16));
    wr(REG_FRF_MSB + 1, (uint8_t)(frf >> 8));
    wr(REG_FRF_MSB + 2, (uint8_t)frf);
}

bool ttc_hw_radio_init(const ttc_radio_cfg_t *c)
{
    /* Hardware reset: hold RESET low 1 ms, release (input), wait 10 ms. */
    gpio_config(GPIOC, 4, GPIO_MODE_OUT, GPIO_PP, GPIO_NOPULL, 0);
    gpio_write(GPIOC, 4, false);
    delay_ms(1);
    gpio_config(GPIOC, 4, GPIO_MODE_IN, GPIO_PP, GPIO_NOPULL, 0);
    delay_ms(10);

    if (rd(REG_VERSION) != 0x12) return false;

    wr(REG_OP_MODE, MODE_SLEEP); /* LoRa bit can only change in sleep */
    wr(REG_OP_MODE, MODE_LORA | MODE_SLEEP);

    s_freq_up_hz = c->freq_up_hz;
    s_freq_down_hz = c->freq_down_hz;
    set_freq(s_freq_up_hz);

    wr(REG_FIFO_TX_BASE, 0);
    wr(REG_FIFO_RX_BASE, 0);
    wr(REG_LNA, 0x23);                     /* max gain, LNA boost */
    int p = c->power;
    if (p < 2) p = 2;
    if (p > 17) p = 17;
    wr(REG_PA_DAC, 0x84);                  /* default (no +20 dBm mode) */
    wr(REG_OCP, 0x20 | 0x0B);              /* over-current trip 100 mA */
    wr(REG_PA_CONFIG, (uint8_t)(0x80 | (p - 2))); /* PA_BOOST, Pout = 2 + n dBm */

    static const uint8_t bw_code[4] = {6, 7, 8, 9}; /* 62.5, 125, 250, 500 kHz */
    uint8_t bw = c->bw > 3 ? 1 : c->bw;
    wr(REG_MODEM_CONFIG1, (uint8_t)((bw_code[bw] << 4) | ((c->cr - 4) << 1))); /* explicit header */
    wr(REG_MODEM_CONFIG2, (uint8_t)((c->sf << 4) | 0x04));                    /* CRC on */
    /* Low data rate optimisation when a symbol is longer than 16 ms. */
    static const uint32_t bw_hz[4] = {62500, 125000, 250000, 500000};
    uint32_t sym_us = (uint32_t)(((uint64_t)1000000u << c->sf) / bw_hz[bw]);
    wr(REG_MODEM_CONFIG3, (uint8_t)(0x04 | (sym_us > 16000u ? 0x08 : 0x00))); /* AGC on */
    wr(REG_PREAMBLE_MSB, 0);
    wr(REG_PREAMBLE_LSB, 8);
    wr(REG_SYNC_WORD, c->sync);
    set_mode(MODE_STDBY);
    s_tx_mode = false;
    return rd(REG_VERSION) == 0x12;
}

void ttc_hw_radio_rx(void)
{
    set_mode(MODE_STDBY);       /* after TX_DONE the chip is already in STDBY */
    set_freq(s_freq_up_hz);
    wr(REG_DIO_MAPPING1, 0x00); /* DIO0 = RxDone */
    wr(REG_IRQ_FLAGS, 0xFF);
    wr(REG_FIFO_ADDR_PTR, 0);
    set_mode(MODE_RX_CONT);
    s_tx_mode = false;
}

bool ttc_hw_radio_tx(const uint8_t *d, uint8_t n)
{
    if (n == 0) return false;
    set_mode(MODE_STDBY);
    set_freq(s_freq_down_hz);
    wr(REG_DIO_MAPPING1, 0x40); /* DIO0 = TxDone */
    wr(REG_IRQ_FLAGS, 0xFF);
    wr(REG_FIFO_ADDR_PTR, 0);
    spi1_select();
    spi1_xfer(REG_FIFO | 0x80u);
    for (uint8_t i = 0; i < n; i++) spi1_xfer(d[i]);
    spi1_deselect();
    wr(REG_PAYLOAD_LEN, n);
    set_mode(MODE_TX);
    s_tx_mode = true;
    return true;
}

bool ttc_hw_radio_rx_active(void)
{
    if (s_tx_mode) return false;
    /* RegModemStat: bit 0 signal detected, 1 synchronised, 3 header valid. */
    return (rd(REG_MODEM_STAT) & 0x0Bu) != 0;
}

int ttc_hw_radio_poll(uint8_t *buf, uint8_t *len, int16_t *rssi, int8_t *snr)
{
    uint8_t f = rd(REG_IRQ_FLAGS);
    if (s_tx_mode) {
        if (f & IRQ_TX_DONE) {
            wr(REG_IRQ_FLAGS, 0xFF);
            return TTC_EV_TX_DONE;
        }
        return TTC_EV_NONE;
    }
    if (!(f & IRQ_RX_DONE)) return TTC_EV_NONE;
    wr(REG_IRQ_FLAGS, 0xFF);
    if (f & IRQ_CRC_ERR) return TTC_EV_RX_CRC_ERR;

    uint8_t n = rd(REG_RX_NB_BYTES);
    wr(REG_FIFO_ADDR_PTR, rd(REG_FIFO_RX_CUR));
    spi1_select();
    spi1_xfer(REG_FIFO & 0x7Fu);
    for (uint8_t i = 0; i < n; i++) buf[i] = spi1_xfer(0);
    spi1_deselect();
    *len = n;
    int8_t s = (int8_t)rd(REG_PKT_SNR);
    int16_t r = (int16_t)(-164 + rd(REG_PKT_RSSI)); /* low-frequency port */
    if (s < 0) r = (int16_t)(r + s / 4);
    *snr = s;
    *rssi = r;
    wr(REG_FIFO_ADDR_PTR, 0);
    return TTC_EV_RX;
}
