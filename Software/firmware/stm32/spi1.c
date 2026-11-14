#include "spi1.h"

#include "platform.h"

void spi1_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;
    gpio_config(GPIOA, 4, GPIO_MODE_OUT, GPIO_PP, GPIO_NOPULL, 0);
    gpio_write(GPIOA, 4, true);
    gpio_config(GPIOA, 5, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 5);
    gpio_config(GPIOA, 6, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 5);
    gpio_config(GPIOA, 7, GPIO_MODE_AF, GPIO_PP, GPIO_NOPULL, 5);
    SPI1->CR1 = 0;
    /* Master, BR = fPCLK/4, CPOL = 0, CPHA = 0, software NSS. */
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_BR_0 | SPI_CR1_SSM | SPI_CR1_SSI;
    SPI1->CR1 |= SPI_CR1_SPE;
}

void spi1_select(void) { gpio_write(GPIOA, 4, false); }

void spi1_deselect(void)
{
    while (SPI1->SR & SPI_SR_BSY) {
    }
    gpio_write(GPIOA, 4, true);
}

uint8_t spi1_xfer(uint8_t b)
{
    uint32_t guard = 10000;
    while (!(SPI1->SR & SPI_SR_TXE) && --guard) {
    }
    *(volatile uint8_t *)&SPI1->DR = b;
    guard = 10000;
    while (!(SPI1->SR & SPI_SR_RXNE) && --guard) {
    }
    return *(volatile uint8_t *)&SPI1->DR;
}
