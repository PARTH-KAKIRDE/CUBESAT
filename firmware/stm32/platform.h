/* STM32F446 platform helpers shared by the dummy slave, ADCS and TT&C.
 * Clock: HSI 16 MHz, no PLL. SYSCLK = HCLK = PCLK1 = PCLK2 = 16 MHz.
 * Keeping the clock simple avoids PLL/flash-latency mistakes; every
 * peripheral used here (I2C 100 kHz, SPI 4 MHz, UART 115200, 20 kHz PWM)
 * works at 16 MHz. */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32f4xx.h"

#define PLATFORM_HCLK_HZ  16000000u
#define PLATFORM_PCLK1_HZ 16000000u
#define PLATFORM_PCLK2_HZ 16000000u

/* --- clock, tick, watchdog --- */
void platform_init(void);          /* caches, SysTick 1 kHz, GPIO clocks */
uint32_t millis(void);
void delay_ms(uint32_t ms);
void delay_us(uint32_t us);        /* busy wait, approximate */

void iwdg_start(uint32_t timeout_ms); /* cannot be stopped once started */
void iwdg_kick(void);

/* Reset cause, read once at boot (RCC->CSR), flags cleared afterwards. */
#define RESET_CAUSE_POWER  0x01
#define RESET_CAUSE_PIN    0x02
#define RESET_CAUSE_IWDG   0x04
#define RESET_CAUSE_SW     0x08
#define RESET_CAUSE_BROWN  0x10
uint8_t platform_reset_cause(void);

/* --- GPIO --- */
#define GPIO_MODE_IN   0u
#define GPIO_MODE_OUT  1u
#define GPIO_MODE_AF   2u
#define GPIO_MODE_AN   3u
#define GPIO_PP        0u
#define GPIO_OD        1u
#define GPIO_NOPULL    0u
#define GPIO_PULLUP    1u
#define GPIO_PULLDOWN  2u

void gpio_config(GPIO_TypeDef *g, uint32_t pin, uint32_t mode, uint32_t otype,
                 uint32_t pull, uint32_t af);
static inline void gpio_write(GPIO_TypeDef *g, uint32_t pin, bool on)
{
    g->BSRR = on ? (1u << pin) : (1u << (pin + 16u));
}
static inline bool gpio_read(GPIO_TypeDef *g, uint32_t pin)
{
    return (g->IDR >> pin) & 1u;
}

#endif
