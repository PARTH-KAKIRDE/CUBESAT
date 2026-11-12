#include "platform.h"
#include "icd_port.h"

uint32_t SystemCoreClock = PLATFORM_HCLK_HZ;

static volatile uint32_t s_ticks;
static uint8_t s_reset_cause;

/* Called from Reset_Handler before .data users run. */
void SystemInit(void)
{
    /* Enable the FPU (CP10/CP11 full access) before any float code runs. */
    SCB->CPACR |= (3uL << 20) | (3uL << 22);
    __DSB();
    __ISB();
    /* After reset the MCU already runs from HSI 16 MHz with all bus
     * prescalers at 1. Make sure that is true even after a soft reset. */
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) {
    }
    RCC->CFGR = 0; /* SW = HSI, HPRE/PPRE1/PPRE2 = /1 */
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) {
    }
    /* 0 wait states are valid up to 30 MHz at 3.3 V. */
    FLASH->ACR = FLASH_ACR_LATENCY_0WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    SCB->VTOR = FLASH_BASE;
}

void SystemCoreClockUpdate(void) { SystemCoreClock = PLATFORM_HCLK_HZ; }

void SysTick_Handler(void) { s_ticks++; }

void platform_init(void)
{
    uint32_t csr = RCC->CSR;
    s_reset_cause = 0;
    if (csr & RCC_CSR_PORRSTF) s_reset_cause |= RESET_CAUSE_POWER;
    if (csr & RCC_CSR_PINRSTF) s_reset_cause |= RESET_CAUSE_PIN;
    if (csr & RCC_CSR_IWDGRSTF) s_reset_cause |= RESET_CAUSE_IWDG;
    if (csr & RCC_CSR_SFTRSTF) s_reset_cause |= RESET_CAUSE_SW;
    if (csr & RCC_CSR_BORRSTF) s_reset_cause |= RESET_CAUSE_BROWN;
    RCC->CSR |= RCC_CSR_RMVF;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN |
                    RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_GPIOHEN;
    (void)RCC->AHB1ENR;

    NVIC_SetPriorityGrouping(3); /* 4 bits preemption, 0 bits sub */
    SysTick_Config(PLATFORM_HCLK_HZ / 1000u);
    NVIC_SetPriority(SysTick_IRQn, 3);
}

uint8_t platform_reset_cause(void) { return s_reset_cause; }

uint32_t millis(void) { return s_ticks; }

void delay_ms(uint32_t ms)
{
    uint32_t start = s_ticks;
    while ((uint32_t)(s_ticks - start) < ms) {
    }
}

void delay_us(uint32_t us)
{
    /* About 4 cycles per iteration at 16 MHz -> 4 iterations per us. */
    volatile uint32_t n = us * 4u;
    while (n--) {
    }
}

void iwdg_start(uint32_t timeout_ms)
{
    /* LSI ~32 kHz, prescaler /64 -> 500 Hz -> 2 ms per count. */
    uint32_t reload = timeout_ms / 2u;
    if (reload > 0xFFFu) reload = 0xFFFu;
    if (reload == 0) reload = 1;
    IWDG->KR = 0x5555;
    IWDG->PR = 4; /* /64 */
    IWDG->RLR = reload;
    while (IWDG->SR) {
    }
    IWDG->KR = 0xAAAA;
    IWDG->KR = 0xCCCC;
}

void iwdg_kick(void) { IWDG->KR = 0xAAAA; }

void gpio_config(GPIO_TypeDef *g, uint32_t pin, uint32_t mode, uint32_t otype,
                 uint32_t pull, uint32_t af)
{
    uint32_t p2 = pin * 2u;
    g->MODER = (g->MODER & ~(3u << p2)) | (mode << p2);
    g->OTYPER = (g->OTYPER & ~(1u << pin)) | (otype << pin);
    g->OSPEEDR = (g->OSPEEDR & ~(3u << p2)) | (2u << p2); /* fast */
    g->PUPDR = (g->PUPDR & ~(3u << p2)) | (pull << p2);
    if (pin < 8u) {
        uint32_t s = pin * 4u;
        g->AFR[0] = (g->AFR[0] & ~(0xFu << s)) | (af << s);
    } else {
        uint32_t s = (pin - 8u) * 4u;
        g->AFR[1] = (g->AFR[1] & ~(0xFu << s)) | (af << s);
    }
}

/* ICD link layer lock: save PRIMASK, disable IRQs, restore later. */
uint32_t icd_port_lock(void)
{
    uint32_t s = __get_PRIMASK();
    __disable_irq();
    return s;
}

void icd_port_unlock(uint32_t state) { __set_PRIMASK(state); }
