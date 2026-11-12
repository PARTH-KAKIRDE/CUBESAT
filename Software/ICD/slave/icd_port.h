/* Platform hooks the ICD link layer needs. Implemented once per platform:
 *   firmware/stm32/icd_port_stm32.c  (PRIMASK save/restore)
 *   firmware/host/icd_port_host.c    (no-op, single-threaded simulation) */
#ifndef ICD_PORT_H
#define ICD_PORT_H

#include <stdint.h>

/* Disable interrupts, return previous state. Must nest correctly. */
uint32_t icd_port_lock(void);
void icd_port_unlock(uint32_t state);

#endif
