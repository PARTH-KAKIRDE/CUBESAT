/* Startup code and vector table for STM32F446 (written in C, no assembly). */
#include <stdint.h>
#include "stm32f4xx.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);
extern void __libc_init_array(void);

void Reset_Handler(void);
void Default_Handler(void);

/* Every handler is weak and points at Default_Handler unless defined. */
#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) WEAK_ALIAS;
void HardFault_Handler(void) WEAK_ALIAS;
void MemManage_Handler(void) WEAK_ALIAS;
void BusFault_Handler(void) WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void) WEAK_ALIAS;
void DebugMon_Handler(void) WEAK_ALIAS;
void PendSV_Handler(void) WEAK_ALIAS;
void SysTick_Handler(void) WEAK_ALIAS;
void I2C1_EV_IRQHandler(void) WEAK_ALIAS;
void I2C1_ER_IRQHandler(void) WEAK_ALIAS;
void I2C3_EV_IRQHandler(void) WEAK_ALIAS;
void I2C3_ER_IRQHandler(void) WEAK_ALIAS;
void USART1_IRQHandler(void) WEAK_ALIAS;
void USART2_IRQHandler(void) WEAK_ALIAS;

typedef void (*vector_t)(void);

/* 16 core vectors + 97 device IRQs (FMPI2C1_ER is IRQ 96 on the F446). */
#define N_IRQ 97
/* Every device IRQ defaults to Default_Handler; the used ones are then
 * overridden below (GCC range initialiser, so silence -Woverride-init). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".isr_vector"), used))
const vector_t g_vectors[16 + N_IRQ] = {
    [16 ... 16 + N_IRQ - 1] = Default_Handler,
    [0] = (vector_t)(uintptr_t)&_estack,
    [1] = Reset_Handler,
    [2] = NMI_Handler,
    [3] = HardFault_Handler,
    [4] = MemManage_Handler,
    [5] = BusFault_Handler,
    [6] = UsageFault_Handler,
    [11] = SVC_Handler,
    [12] = DebugMon_Handler,
    [14] = PendSV_Handler,
    [15] = SysTick_Handler,
    [16 + I2C1_EV_IRQn] = I2C1_EV_IRQHandler,
    [16 + I2C1_ER_IRQn] = I2C1_ER_IRQHandler,
    [16 + USART1_IRQn] = USART1_IRQHandler,
    [16 + USART2_IRQn] = USART2_IRQHandler,
    [16 + I2C3_EV_IRQn] = I2C3_EV_IRQHandler,
    [16 + I2C3_ER_IRQn] = I2C3_ER_IRQHandler,
};
#pragma GCC diagnostic pop

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;) *dst++ = *src++;
    for (uint32_t *dst = &_sbss; dst < &_ebss;) *dst++ = 0;
    SystemInit();
    __libc_init_array();
    main();
    for (;;) {
    }
}

/* Unused interrupts land here. The watchdog will reset the MCU. */
void Default_Handler(void)
{
    for (;;) {
    }
}

/* newlib calls _init/_fini from __libc_init_array; nothing to do here. */
void _init(void) {}
void _fini(void) {}
