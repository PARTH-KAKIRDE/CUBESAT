#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "platform.h"

#define LOG_BUF 1024u

static char s_buf[LOG_BUF];
static volatile uint32_t s_head, s_tail;
static uint32_t s_dropped;
static int s_backend;
static USART_TypeDef *s_uart;

void log_init(int backend)
{
    s_backend = backend;
    if (backend == LOG_UART_USART1) {
        RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
        (void)RCC->APB2ENR;
        gpio_config(GPIOA, 9, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 7);
        gpio_config(GPIOA, 10, GPIO_MODE_AF, GPIO_PP, GPIO_PULLUP, 7);
        s_uart = USART1;
        s_uart->BRR = (PLATFORM_PCLK2_HZ + 115200u / 2u) / 115200u;
    } else if (backend == LOG_UART_USART2) {
        RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
        (void)RCC->APB1ENR;
        gpio_config(GPIOA, 2, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 7);
        gpio_config(GPIOA, 3, GPIO_MODE_AF, GPIO_PP, GPIO_PULLUP, 7);
        s_uart = USART2;
        s_uart->BRR = (PLATFORM_PCLK1_HZ + 115200u / 2u) / 115200u;
    } else {
        /* ITM over SWO (PB3), set up by the firmware itself as plain NRZ/UART
         * at 115200 baud, so a USB-serial adapter or the Raspberry Pi UART RX
         * can read it without a debug probe (see obc/scripts/swo_log.py).
         * Each log byte appears on the wire as 0x01 <byte> (ITM port 0). */
        s_uart = 0;
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DBGMCU->CR |= DBGMCU_CR_TRACE_IOEN;             /* TRACE_MODE = 00: async SWO */
        TPI->CSPSR = 1u;                                /* 1-bit port */
        TPI->ACPR = PLATFORM_HCLK_HZ / 115200u - 1u;    /* SWO clock = HCLK / (ACPR+1) */
        TPI->SPPR = 2u;                                 /* NRZ (UART) encoding */
        TPI->FFCR = 0x100u;                             /* formatter off */
        ITM->LAR = 0xC5ACCE55u;                         /* unlock */
        ITM->TCR = ITM_TCR_ITMENA_Msk | (1u << ITM_TCR_TraceBusID_Pos);
        ITM->TPR = 0;
        ITM->TER = 1u;                                  /* stimulus port 0 */
    }
    if (s_uart) s_uart->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

static void push(const char *s, uint32_t n)
{
    uint32_t used = s_head - s_tail;
    if (used + n > LOG_BUF) {
        s_dropped++;
        return;
    }
    for (uint32_t i = 0; i < n; i++) s_buf[(s_head + i) % LOG_BUF] = s[i];
    s_head += n;
}

void log_printf(const char *fmt, ...)
{
    char line[160];
    int n = snprintf(line, sizeof line, "[%lu] ", (unsigned long)millis());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 2u, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    n += m;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\r';
    line[n++] = '\n';
    uint32_t st = __get_PRIMASK();
    __disable_irq();
    push(line, (uint32_t)n);
    __set_PRIMASK(st);
}

void log_poll(void)
{
    /* Drain at most 64 bytes per pass so the main loop stays responsive. */
    for (int i = 0; i < 64 && s_tail != s_head; i++) {
        char c = s_buf[s_tail % LOG_BUF];
        if (s_backend == LOG_ITM) {
            if (!(ITM->TCR & ITM_TCR_ITMENA_Msk) || !(ITM->TER & 1u)) {
                s_tail = s_head; /* no debugger attached: discard */
                return;
            }
            if (ITM->PORT[0].u32 == 0) return; /* port busy, try next pass */
            ITM->PORT[0].u8 = (uint8_t)c;
        } else if (s_uart) {
            if (!(s_uart->SR & USART_SR_TXE)) return;
            s_uart->DR = (uint8_t)c;
        }
        s_tail++;
    }
}

uint32_t log_dropped(void) { return s_dropped; }
