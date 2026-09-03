#include "uart.h"

#define UART0_BASE  ((volatile uint32_t *)0xFFC02000u)
#define UART_THR    (UART0_BASE[0]) /* transmit holding register, offset 0x00 */
#define UART_LSR    (UART0_BASE[5]) /* line status register, offset 0x14 (word 5) */
#define UART_LSR_THRE 0x20u          /* THR empty */

void uart_putc(char c) {
    if (c == '\n') uart_putc('\r');
    while (!(UART_LSR & UART_LSR_THRE)) {
        /* wait for transmit holding register to empty */
    }
    UART_THR = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

void uart_put_hex32(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(digits[(v >> shift) & 0xF]);
    }
}
