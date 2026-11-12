#include "i2c_slave.h"

#include "platform.h"

#define STUCK_TIMEOUT_MS 25u

static icd_link_t *s_link;
static uint8_t s_addr;
static volatile uint8_t s_reading;
static volatile uint8_t s_reset_request;
static uint32_t s_resets;
static uint32_t s_last_activity;
static uint32_t s_last_change_ms;

static void hw_setup(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_I2C3EN;
    (void)RCC->APB1ENR;

    /* Pins: open-drain AF4. Pull-ups are on the boards (4.7k). */
    gpio_config(GPIOA, 8, GPIO_MODE_AF, GPIO_OD, GPIO_NOPULL, 4);
    gpio_config(GPIOC, 9, GPIO_MODE_AF, GPIO_OD, GPIO_NOPULL, 4);

    I2C3->CR1 = I2C_CR1_SWRST;
    I2C3->CR1 = 0;
    I2C3->CR2 = (PLATFORM_PCLK1_HZ / 1000000u) & I2C_CR2_FREQ;
    I2C3->CCR = PLATFORM_PCLK1_HZ / (2u * 100000u); /* only used as master */
    I2C3->TRISE = PLATFORM_PCLK1_HZ / 1000000u + 1u;
    I2C3->OAR1 = (1u << 14) | ((uint32_t)s_addr << 1); /* bit 14 must be 1 */
    I2C3->OAR2 = 0;
    I2C3->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN;
    I2C3->CR1 = I2C_CR1_PE; /* clock stretching enabled (NOSTRETCH = 0) */
    I2C3->CR1 |= I2C_CR1_ACK;  /* ACK can only be set once PE = 1 */
    s_reading = 0;
}

static void peripheral_reset(void)
{
    NVIC_DisableIRQ(I2C3_EV_IRQn);
    NVIC_DisableIRQ(I2C3_ER_IRQn);
    RCC->APB1RSTR |= RCC_APB1RSTR_I2C3RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_I2C3RST;
    hw_setup();
    if (s_link) icd_link_isr_bus_reset(s_link);
    s_resets++;
    NVIC_ClearPendingIRQ(I2C3_EV_IRQn);
    NVIC_ClearPendingIRQ(I2C3_ER_IRQn);
    NVIC_EnableIRQ(I2C3_EV_IRQn);
    NVIC_EnableIRQ(I2C3_ER_IRQn);
}

void i2c_slave_init(icd_link_t *link, uint8_t addr7)
{
    s_link = link;
    s_addr = addr7;
    hw_setup();
    /* Highest priority: the master is waiting on a stretched clock. */
    NVIC_SetPriority(I2C3_EV_IRQn, 0);
    NVIC_SetPriority(I2C3_ER_IRQn, 0);
    NVIC_EnableIRQ(I2C3_EV_IRQn);
    NVIC_EnableIRQ(I2C3_ER_IRQn);
}

void I2C3_EV_IRQHandler(void)
{
    uint32_t sr1 = I2C3->SR1;

    if (sr1 & I2C_SR1_ADDR) {
        uint32_t sr2 = I2C3->SR2; /* reading SR1 then SR2 clears ADDR */
        if (sr2 & I2C_SR2_TRA) {
            s_reading = 1;
            icd_link_isr_read_begin(s_link);
            I2C3->DR = icd_link_isr_tx_byte(s_link);
        } else {
            s_reading = 0;
            icd_link_isr_write_begin(s_link);
        }
        return;
    }

    if (sr1 & I2C_SR1_RXNE) {
        icd_link_isr_rx_byte(s_link, (uint8_t)I2C3->DR);
        sr1 = I2C3->SR1;
    }

    if (s_reading && (sr1 & I2C_SR1_TXE)) {
        I2C3->DR = icd_link_isr_tx_byte(s_link);
    }

    if (sr1 & I2C_SR1_STOPF) {
        /* Clear STOPF: SR1 already read, now write CR1. */
        I2C3->CR1 |= I2C_CR1_PE;
        if (!s_reading) icd_link_isr_write_end(s_link);
        else icd_link_isr_read_end(s_link);
        s_reading = 0;
    }
}

void I2C3_ER_IRQHandler(void)
{
    uint32_t sr1 = I2C3->SR1;

    if (sr1 & I2C_SR1_AF) {
        /* Master NACKed the last byte it wanted: normal end of a read. */
        I2C3->SR1 = (uint32_t)~I2C_SR1_AF;
        icd_link_isr_read_end(s_link);
        s_reading = 0;
    }
    if (sr1 & (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR | I2C_SR1_TIMEOUT)) {
        I2C3->SR1 = (uint32_t)~(I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR | I2C_SR1_TIMEOUT);
        /* ICD: on any bus error reset the peripheral and re-arm. */
        peripheral_reset();
    }
}

void i2c_slave_watchdog(uint32_t now_ms)
{
    uint32_t act = s_link ? s_link->isr_activity : 0;
    if (act != s_last_activity || !(I2C3->SR2 & I2C_SR2_BUSY)) {
        s_last_activity = act;
        s_last_change_ms = now_ms;
        return;
    }
    if ((uint32_t)(now_ms - s_last_change_ms) >= STUCK_TIMEOUT_MS) {
        peripheral_reset();
        s_last_change_ms = now_ms;
    }
}

uint32_t i2c_slave_reset_count(void) { return s_resets; }
