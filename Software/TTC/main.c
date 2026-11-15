/* TT&C firmware entry point (STM32F446 on the TT&C board).
 * Log: USART1 on J6 (PA9 TX), 115200 8N1. Connect a 3.3 V USB-serial adapter. */
#include "cubesat_icd.h"
#include "i2c_slave.h"
#include "log.h"
#include "platform.h"
#include "ttc_app.h"

static ttc_t g_ttc;

int main(void)
{
    platform_init();
    log_init(LOG_UART_USART1);
    /* Watchdog first: ttc_init may compact the flag store (sector erase, at
     * most 2 s with the CPU stalled) and initialises the radio (~15 ms); a
     * hang in either must still reset the board. */
    iwdg_start(4000);
    ttc_init(&g_ttc, millis());
    iwdg_kick();
    i2c_slave_init(&g_ttc.node.link, ICD_ADDR_TTC);

    LOGI("TTC fw %d.%d.%d ICD %d.%d reset=0x%02X radio=%d mode=%d", TTC_FW_MAJOR, TTC_FW_MINOR,
         TTC_FW_PATCH, ICD_VERSION_MAJOR, ICD_VERSION_MINOR, platform_reset_cause(),
         g_ttc.radio_ok, icd_node_mode(&g_ttc.node));

#ifdef TTC_TX_INHIBIT
    LOGI("*** TX INHIBITED build: receive only (no antenna) ***");
#endif
    uint8_t last_mode = icd_node_mode(&g_ttc.node);
    uint16_t last_rx = 0, last_tx = 0;
    for (;;) {
        uint32_t now = millis();
        ttc_step(&g_ttc, now);
        i2c_slave_watchdog(now);
        if (icd_node_mode(&g_ttc.node) != last_mode) {
            last_mode = icd_node_mode(&g_ttc.node);
            LOGI("mode -> %d (reason %d)", last_mode, g_ttc.node.mode_reason);
        }
        if (g_ttc.rx_count != last_rx) {
            last_rx = g_ttc.rx_count;
            LOGI("rx #%u rssi %d snr %d q=%u", last_rx, g_ttc.last_rssi, g_ttc.last_snr / 4,
                 g_ttc.rxq.count);
        }
        if (g_ttc.tx_count != last_tx) {
            last_tx = g_ttc.tx_count;
            LOGI("tx #%u", last_tx);
        }
        log_poll();
        iwdg_kick();
    }
}
