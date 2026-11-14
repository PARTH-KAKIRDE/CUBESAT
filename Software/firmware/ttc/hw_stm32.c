/*
 * TT&C hardware on the real board (pin map from TTC_LoRa.kicad_pcb, U3 STM32F446RET6):
 *
 *   OBC link   I2C3   PA8 SCL, PC9 SDA (J5 "OBC I2C")             (i2c_slave.c)
 *   OBC UART   USART1 PA9 TX, PA10 RX (J6) -> lean log at 115200   (log.c)
 *   Radio      SPI1   PA5 SCK, PA6 MISO, PA7 MOSI, PA4 NSS, PC4 RESET (sx1278.c)
 *              DIO0 PB0, DIO1 PB1, DIO2..5 PB12..PB15 (inputs, unused)
 *   Temp       I2C1   PB6 SCL, PB7 SDA, TMP102 at 0x48 (ADD0 = GND)
 *   Mission    J2:    DEPLOY_SW PC0 (in, 10k pull-up), RBF_IN PC1 (in, 10k pull-up),
 *                     BURN_EN PC2 (out, 100k pull-down) -> external burn-wire switch
 *   LED        PC11 (D7 green, via 1k)
 *   Debug      SWD on J4 Tag-Connect TC2030
 */
#include "i2c_master.h"
#include "platform.h"
#include "spi1.h"
#include "ttc_hw.h"

#define TMP102_ADDR 0x48

bool ttc_hw_init(void)
{
    gpio_write(GPIOC, 2, false);
    gpio_config(GPIOC, 2, GPIO_MODE_OUT, GPIO_PP, GPIO_PULLDOWN, 0); /* BURN_EN off */
    gpio_config(GPIOC, 0, GPIO_MODE_IN, GPIO_PP, GPIO_NOPULL, 0);    /* DEPLOY_SW */
    gpio_config(GPIOC, 1, GPIO_MODE_IN, GPIO_PP, GPIO_NOPULL, 0);    /* RBF_IN */
    gpio_config(GPIOC, 11, GPIO_MODE_OUT, GPIO_PP, GPIO_NOPULL, 0);  /* LED */
    static const uint32_t dio_b[] = {0, 1, 12, 13, 14, 15};
    for (unsigned i = 0; i < sizeof dio_b / sizeof dio_b[0]; i++)
        gpio_config(GPIOB, dio_b[i], GPIO_MODE_IN, GPIO_PP, GPIO_PULLDOWN, 0);
    spi1_init();
    i2cm_init();
    return true;
}

bool ttc_hw_temp_read(int16_t *centi)
{
    uint8_t b[2];
    if (!i2cm_read_regs(TMP102_ADDR, 0x00, b, 2)) return false;
    int16_t raw = (int16_t)(((int16_t)b[0] << 8) | b[1]) >> 4; /* 12-bit, 0.0625 C */
    *centi = (int16_t)(raw * 625 / 100);
    return true;
}

bool ttc_hw_deploy_sw(void) { return gpio_read(GPIOC, 0); }
bool ttc_hw_rbf_in(void) { return gpio_read(GPIOC, 1); }
void ttc_hw_burn(bool on) { gpio_write(GPIOC, 2, on); }
void ttc_hw_led(bool on) { gpio_write(GPIOC, 11, on); }
uint16_t ttc_hw_local_i2c_errors(void) { return i2cm_error_count(); }
