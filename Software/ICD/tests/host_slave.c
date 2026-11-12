/* Host build of the reference slave (slave/icd_node.c + icd_link.c) for the
 * tests. The I2C "bus" calls the same icd_link_isr_* hooks the STM32 driver
 * calls from its interrupt; time only moves in hs_run_ms(). */
#include <stdint.h>

#include "icd_node.h"
#include "icd_port.h"

uint32_t icd_port_lock(void) { return 0; }   /* single-threaded on the PC */
void icd_port_unlock(uint32_t s) { (void)s; }

static icd_node_t node;
static uint32_t now_ms;
static uint16_t live;

static uint16_t live_faults(void *ctx) { (void)ctx; return live; }

static uint8_t telemetry(void *ctx, uint8_t page, uint8_t *out, uint8_t *len)
{
    (void)ctx;
    if (page != 0) return ICD_ST_BAD_PARAM;
    for (uint8_t i = 0; i < 4; i++) out[i] = (uint8_t)(0xA0 + i);
    *len = 4;
    return ICD_ST_OK;
}

static const icd_node_app_t app = {
    .fw_major = 9, .fw_minor = 8, .fw_patch = 7,
    .live_faults = live_faults,
    .get_telemetry = telemetry,
};

void hs_init(void)   /* power-on or watchdog reset: the ms clock restarts at 0 */
{
    now_ms = 0;
    live = 0;
    icd_node_init(&node, &app, now_ms);
}

void hs_run_ms(uint32_t ms)
{
    while (ms--) icd_node_poll(&node, ++now_ms);   /* one main-loop pass per ms */
}

void hs_write(const uint8_t *d, int n)
{
    icd_link_isr_write_begin(&node.link);
    for (int i = 0; i < n; i++) icd_link_isr_rx_byte(&node.link, d[i]);
    icd_link_isr_write_end(&node.link);
}

void hs_read(uint8_t *d, int n)
{
    icd_link_isr_read_begin(&node.link);
    for (int i = 0; i < n; i++) d[i] = icd_link_isr_tx_byte(&node.link);
    icd_link_isr_read_end(&node.link);
}

int hs_mode(void) { return node.mode; }
void hs_set_live_faults(uint16_t f) { live = f; }
uint32_t hs_overwritten(void) { return node.link.stats.overwritten; }
