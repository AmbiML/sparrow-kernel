/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <arch/kernel/machine_uart.h>
#include <stdarg.h>
#include <stdint.h>

// Machine-mode uart putchar + trivial printf for debugging.

void machine_init_uart(void) {
  volatile uint8_t *const uart16550 = (uint8_t *)0x40030000;
  uart16550[1] = 0x00; // Disable all interrupts
  uart16550[3] = 0x80; // Enable DLAB (set baud rate divisor)
  uart16550[0] = 0x03; // Set divisor to 3 (lo byte) 38400 baud
  uart16550[1] = 0x00; //                  (hi byte)
  uart16550[3] = 0x03; // 8 bits, no parity, one stop bit
  uart16550[2] = 0xC7; // Enable FIFO, clear them, with 14-byte threshold
}

void machine_putchar(char c) {
  volatile uint8_t *uart16550 = (uint8_t *)0x40030000;
  const int UART_REG_QUEUE = 0;
  const int UART_REG_LINESTAT = 5;
  const int UART_REG_STATUS_TX = 0x20;
  while ((uart16550[UART_REG_LINESTAT] & UART_REG_STATUS_TX) == 0) {
  };
  uart16550[UART_REG_QUEUE] = c;
}

// only prints 32-bit "%x" hex values
void machine_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  for (; *fmt; fmt++) {
    if (*fmt == '%') {
      fmt++;
      if (*fmt == 'x') {
        uint32_t arg = va_arg(args, uint32_t);
        for (int i = 7; i >= 0; i--) {
          int n = (arg >> (4 * i)) & 0xF;
          machine_putchar(n > 9 ? 'A' + n - 10 : '0' + n);
        }
      }
    } else {
      machine_putchar(*fmt);
    }
  }
  va_end(args);
}
