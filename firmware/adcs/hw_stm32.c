/*
 * ADCS hardware on the real board (pin map from ADCS.kicad_pcb, U1 STM32F446RET6):
 *
 *   OBC link   I2C3  PA8 SCL, PC9 SDA            (i2c_slave.c)
 *   Sensors    I2C1  PB6 SCL, PB7 SDA            MPU-6050 0x68 (J12), MMC5983MA 0x30 (J10)
 *   MPU_INT    PA15  (not used: polled)
 *   Coil X     DRV8833 U3: AIN1 PC6 TIM8_CH1, AIN2 PC7 TIM8_CH2, nSLEEP PB2,  nFAULT PB13
 *   Coil Y     DRV8833 U4: AIN1 PA6 TIM3_CH1, AIN2 PA7 TIM3_CH2, nSLEEP PC13, nFAULT PB14
 *   Coil Z     DRV8833 U5: AIN1 PA9 TIM1_CH2, AIN2 PA10 TIM1_CH3, nSLEEP PC14, nFAULT PB15
 *   Sun        SS1..SS6 = PA0..PA5 = ADC1_IN0..IN5 (MCP6001 buffers)
 *   Debug      SWD + SWO (PB3) on J16 Tag-Connect. Log goes out on SWO (ITM).
 *
 * Each DRV8833 AISEN goes to ground through a 1 ohm sense resistor (the
 * driver's own current limit, about 200 mV / 1 ohm = 200 mA). It is not
 * connected to the STM32, so coil current cannot be measured.
 */
#include "adcs_hw.h"

#include "board_config.h"
#include "i2c_master.h"
#include "platform.h"

static const int s_mag_map[3] = MAG_MAP, s_mag_sign[3] = MAG_SIGN;
static const int s_gyro_map[3] = GYRO_MAP, s_gyro_sign[3] = GYRO_SIGN;
static const int s_mtq_sign[3] = MTQ_SIGN;

/* ---------------- PWM ---------------- */

static void pwm_timer(TIM_TypeDef *t, bool advanced, int ch_a, int ch_b)
{
    t->CR1 = 0;
    t->PSC = 0;
    t->ARR = MTQ_PWM_ARR;
    int chs[2] = {ch_a, ch_b};
    for (int k = 0; k < 2; k++) {
        int ch = chs[k];
        uint32_t mode = (6u << 4) | (1u << 3); /* PWM mode 1, preload */
        if (ch == 1) t->CCMR1 = (t->CCMR1 & ~0xFFu) | mode;
        if (ch == 2) t->CCMR1 = (t->CCMR1 & ~0xFF00u) | (mode << 8);
        if (ch == 3) t->CCMR2 = (t->CCMR2 & ~0xFFu) | mode;
        t->CCER |= 1u << ((ch - 1) * 4);
    }
    if (advanced) t->BDTR = TIM_BDTR_MOE;
    t->EGR = TIM_EGR_UG;
    t->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

static void set_ccr(TIM_TypeDef *t, int ch, uint32_t v)
{
    if (ch == 1) t->CCR1 = v;
    else if (ch == 2) t->CCR2 = v;
    else t->CCR3 = v;
}

/* DRV8833: IN1 = PWM, IN2 = 0 -> forward / fast decay; swap for reverse. */
static void coil(TIM_TypeDef *t, int ch1, int ch2, int16_t duty)
{
    uint32_t mag = (uint32_t)(duty < 0 ? -duty : duty);
    if (mag > 1000u) mag = 1000u;
    uint32_t ccr = mag * (MTQ_PWM_ARR + 1u) / 1000u;
    set_ccr(t, ch1, duty > 0 ? ccr : 0);
    set_ccr(t, ch2, duty < 0 ? ccr : 0);
}

/* ---------------- ADC ---------------- */

static void adc_setup(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    (void)RCC->APB2ENR;
    for (uint32_t p = 0; p < 6; p++) gpio_config(GPIOA, p, GPIO_MODE_AN, GPIO_PP, GPIO_NOPULL, 0);
    ADC->CCR = 0; /* ADCCLK = PCLK2 / 2 = 8 MHz */
    ADC1->CR1 = 0; /* 12-bit */
    ADC1->CR2 = ADC_CR2_ADON;
    ADC1->SMPR2 = 0;
    for (uint32_t ch = 0; ch < 6; ch++) ADC1->SMPR2 |= 6u << (ch * 3u); /* 144 cycles */
    ADC1->SQR1 = 0; /* one conversion */
}

bool adcs_hw_sun_read(uint16_t out[6])
{
    for (uint32_t ch = 0; ch < 6; ch++) {
        ADC1->SQR3 = ch;
        ADC1->SR = 0;
        ADC1->CR2 |= ADC_CR2_SWSTART;
        uint32_t guard = 20000;
        while (!(ADC1->SR & ADC_SR_EOC)) {
            if (--guard == 0) return false;
        }
        out[ch] = (uint16_t)(ADC1->DR & 0x0FFFu);
    }
    return true;
}

/* ---------------- init ---------------- */

bool adcs_hw_init(void)
{
    /* nSLEEP outputs low first so the drivers stay off during init. */
    gpio_write(GPIOB, 2, false);
    gpio_write(GPIOC, 13, false);
    gpio_write(GPIOC, 14, false);
    gpio_config(GPIOB, 2, GPIO_MODE_OUT, GPIO_PP, GPIO_NOPULL, 0);
    gpio_config(GPIOC, 13, GPIO_MODE_OUT, GPIO_PP, GPIO_NOPULL, 0);
    gpio_config(GPIOC, 14, GPIO_MODE_OUT, GPIO_PP, GPIO_NOPULL, 0);
    /* nFAULT inputs (open drain, 10k pull-ups on the board). */
    gpio_config(GPIOB, 13, GPIO_MODE_IN, GPIO_PP, GPIO_PULLUP, 0);
    gpio_config(GPIOB, 14, GPIO_MODE_IN, GPIO_PP, GPIO_PULLUP, 0);
    gpio_config(GPIOB, 15, GPIO_MODE_IN, GPIO_PP, GPIO_PULLUP, 0);
    gpio_config(GPIOA, 15, GPIO_MODE_IN, GPIO_PP, GPIO_NOPULL, 0); /* MPU_INT */

    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN | RCC_APB2ENR_TIM8EN;
    (void)RCC->APB2ENR;
    gpio_config(GPIOC, 6, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 3);  /* TIM8_CH1 */
    gpio_config(GPIOC, 7, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 3);  /* TIM8_CH2 */
    gpio_config(GPIOA, 6, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 2);  /* TIM3_CH1 */
    gpio_config(GPIOA, 7, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 2);  /* TIM3_CH2 */
    gpio_config(GPIOA, 9, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 1);  /* TIM1_CH2 */
    gpio_config(GPIOA, 10, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 1); /* TIM1_CH3 */
    pwm_timer(TIM8, true, 1, 2);
    pwm_timer(TIM3, false, 1, 2);
    pwm_timer(TIM1, true, 2, 3);

    adc_setup();
    i2cm_init();
    return true;
}

void adcs_hw_mtq_enable(bool on)
{
    gpio_write(GPIOB, 2, on);
    gpio_write(GPIOC, 13, on);
    gpio_write(GPIOC, 14, on);
}

void adcs_hw_mtq_set(const int16_t duty[3])
{
    coil(TIM8, 1, 2, (int16_t)(duty[0] * s_mtq_sign[0]));
    coil(TIM3, 1, 2, (int16_t)(duty[1] * s_mtq_sign[1]));
    coil(TIM1, 2, 3, (int16_t)(duty[2] * s_mtq_sign[2]));
}

uint8_t adcs_hw_drv_faults(void)
{
    uint8_t f = 0;
    if (!gpio_read(GPIOB, 13)) f |= 1u;
    if (!gpio_read(GPIOB, 14)) f |= 2u;
    if (!gpio_read(GPIOB, 15)) f |= 4u;
    return f;
}

uint16_t adcs_hw_local_i2c_errors(void) { return i2cm_error_count(); }

/* ---------------- MPU-6050 ---------------- */

bool adcs_hw_imu_init(void)
{
    uint8_t who = 0;
    if (!i2cm_read_regs(MPU6050_ADDR, 0x75, &who, 1) || who != 0x68) return false;
    /* A full device reset takes 100 ms: only at boot, never during a retry,
     * so the main loop keeps answering the OBC within the ICD turnaround. */
    static bool s_first = true;
    if (s_first) {
        s_first = false;
        if (!i2cm_write_reg(MPU6050_ADDR, 0x6B, 0x80)) return false;
        delay_ms(100);
    }
    bool ok = i2cm_write_reg(MPU6050_ADDR, 0x6B, 0x01)    /* wake, PLL on gyro X */
              && i2cm_write_reg(MPU6050_ADDR, 0x19, 9)    /* 100 Hz sample rate */
              && i2cm_write_reg(MPU6050_ADDR, 0x1A, 3)    /* DLPF 44 Hz */
              && i2cm_write_reg(MPU6050_ADDR, 0x1B, 0x08) /* gyro +-500 dps */
              && i2cm_write_reg(MPU6050_ADDR, 0x1C, 0x00);/* accel +-2 g */
    return ok;
}

bool adcs_hw_imu_read(int16_t gyro[3], int16_t *temp_centi)
{
    uint8_t b[14];
    if (!i2cm_read_regs(MPU6050_ADDR, 0x3B, b, sizeof b)) return false;
    int16_t t = (int16_t)((b[6] << 8) | b[7]);
    int16_t g[3];
    for (int i = 0; i < 3; i++) g[i] = (int16_t)((b[8 + 2 * i] << 8) | b[9 + 2 * i]);
    for (int i = 0; i < 3; i++) gyro[i] = (int16_t)(s_gyro_sign[i] * g[s_gyro_map[i]]);
    /* T[degC] = raw / 340 + 36.53 */
    *temp_centi = (int16_t)((int32_t)t * 100 / 340 + 3653);
    return true;
}

/* ---------------- MMC5983MA ---------------- */

bool adcs_hw_mag_init(void)
{
    uint8_t id = 0;
    static bool s_first = true;
    if (s_first) { /* software reset (15 ms) only at boot */
        s_first = false;
        if (!i2cm_write_reg(MMC5983_ADDR, 0x0A, 0x80)) return false;
        delay_ms(15);
    }
    if (!i2cm_read_regs(MMC5983_ADDR, 0x2F, &id, 1) || id != 0x30) return false;
    /* Control 1: bandwidth 100 Hz (8 ms measurement). Single-shot mode. */
    return i2cm_write_reg(MMC5983_ADDR, 0x0A, 0x00) && i2cm_write_reg(MMC5983_ADDR, 0x0B, 0x00);
}

bool adcs_hw_mag_start(void)
{
    /* SET pulse (re-magnetise the sensor), then start one measurement. */
    if (!i2cm_write_reg(MMC5983_ADDR, 0x09, 0x08)) return false;
    delay_us(2);
    return i2cm_write_reg(MMC5983_ADDR, 0x09, 0x01);
}

int adcs_hw_mag_poll(int16_t mag[3])
{
    uint8_t st = 0;
    if (!i2cm_read_regs(MMC5983_ADDR, 0x08, &st, 1)) return -1;
    if (!(st & 0x01)) return 0;
    uint8_t b[7];
    if (!i2cm_read_regs(MMC5983_ADDR, 0x00, b, sizeof b)) return -1;
    /* 18-bit unsigned, offset 131072, 16384 counts per gauss. */
    int32_t raw[3];
    raw[0] = (int32_t)(((uint32_t)b[0] << 10) | ((uint32_t)b[1] << 2) | ((b[6] >> 6) & 3u));
    raw[1] = (int32_t)(((uint32_t)b[2] << 10) | ((uint32_t)b[3] << 2) | ((b[6] >> 4) & 3u));
    raw[2] = (int32_t)(((uint32_t)b[4] << 10) | ((uint32_t)b[5] << 2) | ((b[6] >> 2) & 3u));
    int16_t s[3];
    for (int i = 0; i < 3; i++) {
        /* 1 G = 100 uT = 4000 LSB of 0.025 uT -> LSB = counts * 4000 / 16384 */
        s[i] = (int16_t)(((raw[i] - 131072) * 125) / 512);
    }
    for (int i = 0; i < 3; i++) mag[i] = (int16_t)(s_mag_sign[i] * s[s_mag_map[i]]);
    return 1;
}
