/* ADCS firmware entry point (STM32F446 on the ADCS board). */
#include "adcs_app.h"
#include "cubesat_icd.h"
#include "i2c_slave.h"
#include "log.h"
#include "platform.h"

static adcs_t g_adcs;

int main(void)
{
    platform_init();
    log_init(LOG_ITM);
    adcs_init(&g_adcs, millis());
    i2c_slave_init(&g_adcs.node.link, ICD_ADDR_ADCS);
    iwdg_start(2000);

    LOGI("ADCS fw %d.%d.%d ICD %d.%d reset=0x%02X mode=%d mag=%d imu=%d", ADCS_FW_MAJOR,
         ADCS_FW_MINOR, ADCS_FW_PATCH, ICD_VERSION_MAJOR, ICD_VERSION_MINOR,
         platform_reset_cause(), icd_node_mode(&g_adcs.node), g_adcs.mag_ok, g_adcs.imu_ok);

    uint8_t last_mode = icd_node_mode(&g_adcs.node);
    for (;;) {
        uint32_t now = millis();
        adcs_step(&g_adcs, now);
        i2c_slave_watchdog(now);
        if (icd_node_mode(&g_adcs.node) != last_mode) {
            last_mode = icd_node_mode(&g_adcs.node);
            LOGI("mode -> %d (reason %d)", last_mode, g_adcs.node.mode_reason);
        }
        log_poll();
        iwdg_kick();
    }
}
