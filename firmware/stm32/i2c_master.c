#include "i2c_master.h"

#include "platform.h"

#define I2CM_TIMEOUT_MS 5u

static uint16_t s_errors;

static void pins_af(void)
{
    gpio_config(GPIOB, 6, GPIO_MODE_AF, GPIO_OD, GPIO_NOPULL, 4);
    gpio_config(GPIOB, 7, GPIO_MODE_AF, GPIO_OD, GPIO_NOPULL, 4);
}

/* Clock out a slave that is holding SDA low, then send STOP. */
static void bus_recover(void)
{
    gpio_config(GPIOB, 6, GPIO_MODE_OUT, GPIO_OD, GPIO_NOPULL, 0);
    gpio_config(GPIOB, 7, GPIO_MODE_OUT, GPIO_OD, GPIO_NOPULL, 0);
    gpio_write(GPIOB, 7, true);
    for (int i = 0; i < 9 && !gpio_read(GPIOB, 7); i++) {
        gpio_write(GPIOB, 6, false);
        delay_us(5);
        gpio_write(GPIOB, 6, true);
        delay_us(5);
    }
    gpio_write(GPIOB, 7, false); /* STOP: SDA low -> high while SCL high */
    delay_us(5);
    gpio_write(GPIOB, 6, true);
    delay_us(5);
    gpio_write(GPIOB, 7, true);
    delay_us(5);
}

static void hw_setup(void)
{
    RCC->APB1RSTR |= RCC_APB1RSTR_I2C1RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_I2C1RST;
    pins_af();
    I2C1->CR1 = 0;
    I2C1->CR2 = PLATFORM_PCLK1_HZ / 1000000u;
    I2C1->CCR = PLATFORM_PCLK1_HZ / (2u * 100000u); /* 100 kHz standard mode */
    I2C1->TRISE = PLATFORM_PCLK1_HZ / 1000000u + 1u;
    I2C1->CR1 = I2C_CR1_PE;
}

void i2cm_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    (void)RCC->APB1ENR;
    bus_recover();
    hw_setup();
}

static void fail(void)
{
    s_errors++;
    I2C1->CR1 |= I2C_CR1_STOP;
    bus_recover();
    hw_setup();
}

/* Wait for an SR1 flag. Fails on timeout or NACK. */
static bool wait_sr1(uint32_t flag)
{
    uint32_t t0 = millis();
    while (!(I2C1->SR1 & flag)) {
        if (I2C1->SR1 & I2C_SR1_AF) {
            I2C1->SR1 = (uint32_t)~I2C_SR1_AF;
            return false;
        }
        if ((uint32_t)(millis() - t0) > I2CM_TIMEOUT_MS) return false;
    }
    return true;
}

static bool wait_not_busy(void)
{
    uint32_t t0 = millis();
    while (I2C1->SR2 & I2C_SR2_BUSY) {
        if ((uint32_t)(millis() - t0) > I2CM_TIMEOUT_MS) return false;
    }
    return true;
}

static bool start_addr(uint8_t addr7, bool read)
{
    I2C1->CR1 |= I2C_CR1_START;
    if (!wait_sr1(I2C_SR1_SB)) return false;
    I2C1->DR = (uint8_t)((addr7 << 1) | (read ? 1u : 0u));
    return wait_sr1(I2C_SR1_ADDR);
}

static void clear_addr(void)
{
    (void)I2C1->SR1;
    (void)I2C1->SR2;
}

static bool write_bytes(const uint8_t *data, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++) {
        if (!wait_sr1(I2C_SR1_TXE)) return false;
        I2C1->DR = data[i];
    }
    return wait_sr1(I2C_SR1_BTF);
}

bool i2cm_write(uint8_t addr7, const uint8_t *data, uint8_t len)
{
    if (!wait_not_busy()) { fail(); return false; }
    if (!start_addr(addr7, false)) { fail(); return false; }
    clear_addr();
    if (!write_bytes(data, len)) { fail(); return false; }
    I2C1->CR1 |= I2C_CR1_STOP;
    return true;
}

bool i2cm_write_reg(uint8_t addr7, uint8_t reg, uint8_t value)
{
    uint8_t b[2] = {reg, value};
    return i2cm_write(addr7, b, 2);
}

/* Reference-manual (RM0390) read sequences for N = 1, 2 and > 2. */
static bool read_bytes(uint8_t *buf, uint8_t len)
{
    if (len == 1) {
        I2C1->CR1 &= ~I2C_CR1_ACK;
        __disable_irq();
        clear_addr();
        I2C1->CR1 |= I2C_CR1_STOP;
        __enable_irq();
        if (!wait_sr1(I2C_SR1_RXNE)) return false;
        buf[0] = (uint8_t)I2C1->DR;
        return true;
    }
    if (len == 2) {
        I2C1->CR1 |= I2C_CR1_POS;
        I2C1->CR1 &= ~I2C_CR1_ACK;
        clear_addr();
        if (!wait_sr1(I2C_SR1_BTF)) return false;
        I2C1->CR1 |= I2C_CR1_STOP;
        buf[0] = (uint8_t)I2C1->DR;
        buf[1] = (uint8_t)I2C1->DR;
        I2C1->CR1 &= ~I2C_CR1_POS;
        return true;
    }
    I2C1->CR1 |= I2C_CR1_ACK;
    clear_addr();
    uint8_t i = 0;
    while ((uint8_t)(len - i) > 3u) {
        if (!wait_sr1(I2C_SR1_RXNE)) return false;
        buf[i++] = (uint8_t)I2C1->DR;
    }
    /* Three bytes left: N-2 in DR, wait until N-1 is in the shift register. */
    if (!wait_sr1(I2C_SR1_BTF)) return false;
    I2C1->CR1 &= ~I2C_CR1_ACK;
    buf[i++] = (uint8_t)I2C1->DR;
    if (!wait_sr1(I2C_SR1_BTF)) return false;
    I2C1->CR1 |= I2C_CR1_STOP;
    buf[i++] = (uint8_t)I2C1->DR;
    buf[i++] = (uint8_t)I2C1->DR;
    return true;
}

bool i2cm_read_regs(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t len)
{
    if (len == 0) return true;
    if (!wait_not_busy()) { fail(); return false; }
    if (!start_addr(addr7, false)) { fail(); return false; }
    clear_addr();
    if (!write_bytes(&reg, 1)) { fail(); return false; }
    if (!start_addr(addr7, true)) { fail(); return false; } /* repeated START */
    if (!read_bytes(buf, len)) { fail(); return false; }
    return true;
}

uint16_t i2cm_error_count(void) { return s_errors; }
