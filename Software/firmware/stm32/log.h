/* Lean logger. Messages go into a RAM ring buffer and are drained from the
 * main loop with log_poll(), so logging never blocks an interrupt or the
 * I2C link. If the buffer is full, the message is dropped (counted).
 *
 * Backends:
 *   LOG_UART_USART1  TT&C: PA9 TX / PA10 RX (header J6 "OBC UART")
 *   LOG_UART_USART2  Nucleo: PA2/PA3 -> ST-LINK virtual COM port
 *   LOG_ITM          ADCS: SWO pin PB3 on the Tag-Connect (ADCS has no free UART)
 * All UARTs run 115200 8N1. */
#ifndef LOG_H
#define LOG_H

#include <stdint.h>

#define LOG_UART_USART1 1
#define LOG_UART_USART2 2
#define LOG_ITM         3

void log_init(int backend);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_poll(void);      /* call every main loop pass */
uint32_t log_dropped(void);

#define LOGI(...) log_printf(__VA_ARGS__)

#endif
