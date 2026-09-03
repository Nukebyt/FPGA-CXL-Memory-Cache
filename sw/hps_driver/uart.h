/* Minimal polled driver for the HPS's hard UART0 (DesignWare 8250/16550-
 * compatible), physical base 0xFFC02000 on Cyclone V. Baud rate etc. are
 * assumed already configured by the preloader/U-Boot that ran before this
 * bare-metal app was loaded -- this driver only transmits, at whatever
 * rate is already set up, matching the "load via U-Boot after preloader
 * init" bring-up plan in FPGA_Implementation_Roadmap.md Phase 1H. */
#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_hex32(uint32_t v);

#endif /* UART_H */
