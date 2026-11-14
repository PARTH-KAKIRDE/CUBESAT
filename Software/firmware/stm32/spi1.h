/* SPI1 master for the RFM98W (SX1278) on TT&C.
 * SCK = PA5, MISO = PA6, MOSI = PA7 (AF5), NSS = PA4 (GPIO, active low).
 * Mode 0, MSB first, 4 MHz (16 MHz / 4). */
#ifndef SPI1_H
#define SPI1_H

#include <stdint.h>

void spi1_init(void);
void spi1_select(void);
void spi1_deselect(void);
uint8_t spi1_xfer(uint8_t b);

#endif
