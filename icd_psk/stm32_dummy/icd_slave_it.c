/*
 * I2C slave glue for STM32F4 HAL (interrupt mode, listen mode).
 *
 * ICD rule: the interrupt only moves bytes; parsing happens in icd_slave_poll().
 * A complete response frame is always staged before any read arrives, so the
 * slave never holds SCL low waiting for the main loop.
 *
 *  - Command bytes are received one at a time, so any length up to
 *    ICD_CMD_MAX_LEN works; extra bytes go to a sink so the bus never stalls.
 *  - When a command ends (STOP), the ISR stages a BUSY frame for it. The main
 *    loop then builds the real response in the back buffer and publishes it.
 *  - Reads always send 32 bytes from the published buffer. If the master reads
 *    more, it gets 0x00 padding instead of a stretched clock.
 *  - On bus errors (BERR, ARLO, OVR) the main loop resets the peripheral.
 */
#include <string.h>

#include "main.h"          /* CubeMX: pulls in stm32f4xx_hal.h */
#include "icd.h"
#include "icd_slave.h"

static I2C_HandleTypeDef *bus;

/* receive side: ISR writes rx_isr, copies it to rx_frame at end of frame */
static uint8_t rx_isr[ICD_CMD_MAX_LEN + 1];   /* +1 so a too-long frame is detectable */
static uint8_t rx_sink;
static volatile uint8_t rx_n;
static volatile uint8_t receiving;
static uint8_t rx_frame[ICD_CMD_MAX_LEN + 1];
static volatile uint8_t rx_frame_n;
static volatile uint8_t rx_ready;
static volatile uint32_t rx_seq;              /* bumps on every received command */

/* transmit side */
static uint8_t tx_buf[2][ICD_RESP_LEN];
static uint8_t tx_busy[ICD_RESP_LEN];
static const uint8_t tx_pad[ICD_RESP_LEN];    /* zeros for over-long reads */
static uint8_t * volatile tx_front;           /* what the next read sends */
static uint8_t tx_pub;                        /* index of tx_buf last published */
static volatile uint8_t tx_active;

static volatile uint8_t reset_pending;
static uint32_t last_contact_ms;

/* ---------------- ISR side ---------------- */

static void arm_rx_byte(void)
{
    uint8_t *dst = (rx_n < sizeof rx_isr) ? &rx_isr[rx_n] : &rx_sink;
    HAL_I2C_Slave_Seq_Receive_IT(bus, dst, 1, I2C_NEXT_FRAME);
}

static void end_of_command(void)
{
    if (!receiving)
        return;
    receiving = 0;
    if (rx_n == 0)
        return;
    memcpy(rx_frame, rx_isr, rx_n);
    rx_frame_n = rx_n;
    rx_ready = 1;
    rx_seq++;
    /* Small CRC over 3 bytes; keeps the "always a valid frame" rule. */
    icd_build_resp(tx_busy, rx_isr[0], ICD_ST_BUSY, NULL, 0);
    tx_front = tx_busy;
}

void HAL_I2C_AddrCallback(I2C_HandleTypeDef *h, uint8_t dir, uint16_t addr)
{
    (void)addr;
    if (h != bus)
        return;
    end_of_command();                 /* repeated START without STOP */
    if (dir == I2C_DIRECTION_TRANSMIT) {          /* master writes: we receive */
        receiving = 1;
        rx_n = 0;
        arm_rx_byte();
    } else {                                      /* master reads: we send */
        tx_active = 1;
        HAL_I2C_Slave_Seq_Transmit_IT(h, tx_front, ICD_RESP_LEN, I2C_NEXT_FRAME);
    }
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *h)
{
    if (h != bus)
        return;
    if (rx_n < sizeof rx_isr)
        rx_n++;
    arm_rx_byte();
}

void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *h)
{
    if (h != bus)
        return;
    /* 32 bytes loaded; if the master keeps clocking, feed padding */
    HAL_I2C_Slave_Seq_Transmit_IT(h, (uint8_t *)tx_pad, ICD_RESP_LEN, I2C_NEXT_FRAME);
}

void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *h)
{
    if (h != bus)
        return;
    end_of_command();
    tx_active = 0;
    HAL_I2C_EnableListen_IT(h);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *h)
{
    if (h != bus)
        return;
    uint32_t err = HAL_I2C_GetError(h);
    /* AF is the normal end of a transfer: master NACKs the last byte it reads,
     * or sends STOP while a receive byte is still armed. */
    end_of_command();
    tx_active = 0;
    if (err & ~HAL_I2C_ERROR_AF)
        reset_pending = 1;            /* BERR / ARLO / OVR: reset in main loop */
    else
        HAL_I2C_EnableListen_IT(h);   /* returns HAL_BUSY if already listening */
}

/* ---------------- main-loop side ---------------- */

void icd_slave_init(I2C_HandleTypeDef *hi2c)
{
    bus = hi2c;
    icd_build_resp(tx_buf[0], ICD_CMD_NONE, ICD_ST_BUSY, NULL, 0);
    tx_pub = 0;
    tx_front = tx_buf[0];
    last_contact_ms = HAL_GetTick();
    HAL_I2C_EnableListen_IT(bus);
}

int icd_slave_no_master_contact(void)
{
    return (HAL_GetTick() - last_contact_ms) > ICD_NO_MASTER_TIMEOUT_S * 1000u;
}

void icd_slave_poll(void)
{
    if (reset_pending) {
        reset_pending = 0;
        HAL_I2C_DeInit(bus);
        HAL_I2C_Init(bus);
        receiving = 0;
        tx_active = 0;
        HAL_I2C_EnableListen_IT(bus);
    }
    if (!rx_ready)
        return;

    uint8_t cmd[sizeof rx_frame];
    uint8_t n;
    uint32_t seq;
    __disable_irq();
    n = rx_frame_n;
    memcpy(cmd, rx_frame, n);
    seq = rx_seq;
    rx_ready = 0;
    __enable_irq();

    uint8_t back = (uint8_t)(1u - tx_pub);   /* never the buffer being sent */
    if (icd_slave_handle(cmd, n, tx_buf[back]))
        last_contact_ms = HAL_GetTick();

    /* Publish only between reads, and only if no newer command arrived. */
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        __disable_irq();
        if (!tx_active || HAL_GetTick() - t0 > 10) {
            if (rx_seq == seq) {
                tx_front = tx_buf[back];
                tx_pub = back;
            }
            __enable_irq();
            break;
        }
        __enable_irq();
    }
}
