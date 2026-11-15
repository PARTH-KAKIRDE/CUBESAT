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
 *   Emergency  (v0.9, on the spare-GPIO header J8; each through a 2N7002 to its target)
 *              PC6 -> Pi 4B RUN, PC7 -> ADCS NRST, PC8 -> Pi 4B GLOBAL_EN.
 *              High = asserted (target held in reset / off). 100k pull-downs.
 *   Flash      sector 7 (0x08060000, 128 KB) = ttc_nv journal; the linker
 *              script stops code at 384 KB so it never lands there.
 *   Debug      SWD on J4 Tag-Connect TC2030
 */
#include "i2c_master.h"
#include "platform.h"
#include "spi1.h"
#include "ttc_hw.h"

#define TMP102_ADDR 0x48

static const uint8_t s_line_pin[TTC_LINES] = {6, 8, 7}; /* OBC_RUN, OBC_GLOBAL_EN, ADCS_NRST on GPIOC */

#define NV_BASE   0x08060000u
#define NV_SIZE   (128u * 1024u)
#define NV_SECTOR 7u

bool ttc_hw_init(void)
{
    gpio_write(GPIOC, 2, false);
    gpio_config(GPIOC, 2, GPIO_MODE_OUT, GPIO_PP, GPIO_PULLDOWN, 0); /* BURN_EN off */
    gpio_config(GPIOC, 0, GPIO_MODE_IN, GPIO_PP, GPIO_NOPULL, 0);    /* DEPLOY_SW */
    gpio_config(GPIOC, 1, GPIO_MODE_IN, GPIO_PP, GPIO_NOPULL, 0);    /* RBF_IN */
    gpio_config(GPIOC, 11, GPIO_MODE_OUT, GPIO_PP, GPIO_NOPULL, 0);  /* LED */
    for (unsigned i = 0; i < TTC_LINES; i++) {                       /* emergency lines off */
        gpio_write(GPIOC, s_line_pin[i], false);
        gpio_config(GPIOC, s_line_pin[i], GPIO_MODE_OUT, GPIO_PP, GPIO_PULLDOWN, 0);
    }
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

void ttc_hw_line(uint8_t line, bool asserted)
{
    if (line < TTC_LINES) gpio_write(GPIOC, s_line_pin[line], asserted);
}

void ttc_hw_system_reset(void) { NVIC_SystemReset(); }

/* ---------------- flash sector 7 (ttc_nv) ---------------- */

#define FLASH_ERRORS (FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR | FLASH_SR_SOP) /* SOP = operation error (RM0390 OPERR) */

static bool flash_wait(void)
{
    while (FLASH->SR & FLASH_SR_BSY) {
    }
    bool ok = !(FLASH->SR & FLASH_ERRORS);
    FLASH->SR = FLASH_ERRORS | FLASH_SR_EOP; /* write 1 to clear */
    return ok;
}

static void flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

/* The ART data cache may still hold the old words. */
static void flash_done(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
    uint32_t acr = FLASH->ACR;
    FLASH->ACR = acr & ~FLASH_ACR_DCEN;
    FLASH->ACR = (acr & ~FLASH_ACR_DCEN) | FLASH_ACR_DCRST;
    FLASH->ACR = acr & ~FLASH_ACR_DCRST;
}

uint32_t ttc_hw_nv_size(void) { return NV_SIZE; }

uint32_t ttc_hw_nv_read(uint32_t ofs)
{
    if (ofs + 4u > NV_SIZE) return 0xFFFFFFFFu;
    return *(volatile const uint32_t *)(uintptr_t)(NV_BASE + ofs);
}

bool ttc_hw_nv_program(uint32_t ofs, uint32_t word)
{
    if (ofs + 4u > NV_SIZE || (ofs & 3u) || ttc_hw_nv_read(ofs) != 0xFFFFFFFFu) return false;
    flash_unlock();
    (void)flash_wait();
    FLASH->CR = (FLASH->CR & ~FLASH_CR_PSIZE) | FLASH_CR_PSIZE_1 | FLASH_CR_PG; /* x32, 2.7-3.6 V */
    *(volatile uint32_t *)(uintptr_t)(NV_BASE + ofs) = word;
    __DSB();
    bool ok = flash_wait();
    FLASH->CR &= ~FLASH_CR_PG;
    flash_done();
    return ok && ttc_hw_nv_read(ofs) == word;
}

bool ttc_hw_nv_erase(void)
{
    flash_unlock();
    (void)flash_wait();
    FLASH->CR = (FLASH->CR & ~(FLASH_CR_PSIZE | FLASH_CR_SNB)) | FLASH_CR_PSIZE_1 | FLASH_CR_SER |
                (NV_SECTOR << FLASH_CR_SNB_Pos);
    FLASH->CR |= FLASH_CR_STRT;
    bool ok = flash_wait();              /* the CPU stalls here: about 1 s, at most 2 s */
    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB);
    flash_done();
    return ok;
}
