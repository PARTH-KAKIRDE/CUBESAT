/*
 * Dummy ICD slave for a Nucleo-F446RE (deliverable 4).
 *
 * Wiring to the Raspberry Pi 4B (see docs/WIRING.md):
 *   Nucleo D7  (PA8, I2C3 SCL)  -> Pi GPIO3 (pin 5)
 *   Nucleo CN10-1 (PC9, I2C3 SDA) -> Pi GPIO2 (pin 3)
 *   GND -> GND. The Pi has 1.8k pull-ups on GPIO2/3, so none are needed.
 *
 * Answers every common command (PING, GET_VERSION, GET_STATUS, SET_MODE,
 * GET_TELEMETRY page 0 = link counters). Logs to the ST-LINK virtual COM
 * port at 115200. LD2 (PA5) blinks at 1 Hz; fast blink = OBC talking.
 *
 * Build with -DDUMMY_ADDR=0x33 to impersonate TT&C instead of ADCS.
 */
#include "cubesat_icd.h"
#include "i2c_slave.h"
#include "icd_node.h"
#include "icd_pack.h"
#include "log.h"
#include "platform.h"

#ifndef DUMMY_ADDR
#define DUMMY_ADDR ICD_ADDR_ADCS
#endif

static icd_node_t s_node;

static uint8_t dummy_telemetry(void *ctx, uint8_t page, uint8_t *out, uint8_t *len)
{
    icd_node_t *n = (icd_node_t *)ctx;
    if (page != 0) return ICD_ST_BAD_PARAM;
    const icd_link_stats_t *st = &n->link.stats;
    icd_put_u32(&out[DUMMY_TLM0_OFS_UPTIME_MS], millis());
    icd_put_u32(&out[DUMMY_TLM0_OFS_FRAMES_OK], st->frames_ok);
    icd_put_u32(&out[DUMMY_TLM0_OFS_CRC_ERRORS], st->crc_errors);
    icd_put_u32(&out[DUMMY_TLM0_OFS_LEN_ERRORS], st->length_errors);
    icd_put_u32(&out[DUMMY_TLM0_OFS_OVERWRITTEN], st->overwritten);
    icd_put_u16(&out[DUMMY_TLM0_OFS_BUS_RESETS], (uint16_t)i2c_slave_reset_count());
    icd_put_u16(&out[DUMMY_TLM0_OFS_READS_LO], (uint16_t)st->reads);
    *len = DUMMY_TLM0_LEN;
    return ICD_ST_OK;
}

static uint16_t dummy_faults(void *ctx)
{
    (void)ctx;
    return 0;
}

static const icd_node_app_t s_app = {
    .fw_major = DUMMY_FW_MAJOR,
    .fw_minor = DUMMY_FW_MINOR,
    .fw_patch = DUMMY_FW_PATCH,
    .ctx = &s_node,
    .live_faults = dummy_faults,
    .get_telemetry = dummy_telemetry,
};

int main(void)
{
    platform_init();
    log_init(LOG_UART_USART2);
    gpio_config(GPIOA, 5, GPIO_MODE_OUT, GPIO_PP, GPIO_NOPULL, 0);

    icd_node_init(&s_node, &s_app, millis());
    icd_node_set_mode(&s_node, ICD_MODE_SAFE, ICD_MR_AUTO);
    i2c_slave_init(&s_node.link, DUMMY_ADDR);
    iwdg_start(2000);

    LOGI("dummy slave up, addr 0x%02X, ICD v%d.%d, reset cause 0x%02X", DUMMY_ADDR,
         ICD_VERSION_MAJOR, ICD_VERSION_MINOR, platform_reset_cause());

    uint32_t last_blink = 0, last_stats = 0, last_ok = 0;
    for (;;) {
        uint32_t now = millis();
        icd_node_poll(&s_node, now);
        i2c_slave_watchdog(now);
        log_poll();

        /* 1 Hz blink, 10 Hz while frames are arriving. */
        uint32_t ok = s_node.link.stats.frames_ok;
        uint32_t period = (ok != last_ok) ? 50u : 500u;
        if ((uint32_t)(now - last_blink) >= period) {
            last_blink = now;
            last_ok = ok;
            GPIOA->ODR ^= (1u << 5);
        }
        if ((uint32_t)(now - last_stats) >= 10000u) {
            last_stats = now;
            const icd_link_stats_t *st = &s_node.link.stats;
            LOGI("ok=%lu crc=%lu len=%lu ovw=%lu reads=%lu resets=%lu",
                 (unsigned long)st->frames_ok, (unsigned long)st->crc_errors,
                 (unsigned long)st->length_errors, (unsigned long)st->overwritten,
                 (unsigned long)st->reads, (unsigned long)i2c_slave_reset_count());
        }
        iwdg_kick();
    }
}
