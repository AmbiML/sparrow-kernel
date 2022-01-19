/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <arch/kernel/machine_uart.h>
#include <stdarg.h>
#include <stdint.h>

// Machine-mode uart putchar + trivial printf for debugging.

#define UART0_BASE_ADDR 0x50000000

// Register definitions copied from opentitan/uart.h
//
// Copy/paste instead of include is to avoid adding includes to seL4 kernel
// build rules.
#define UART_FIFO_CTRL(id) (UART##id##_BASE_ADDR + 0x1c)
#define UART_FIFO_CTRL_RXRST 0
#define UART_FIFO_CTRL_TXRST 1

#define UART_CTRL(id) (UART##id##_BASE_ADDR + 0xc)
#define UART_CTRL_TX 0
#define UART_CTRL_RX 1
#define UART_CTRL_NCO_MASK 0xffff
#define UART_CTRL_NCO_OFFSET 16

#define UART_STATUS(id) (UART##id##_BASE_ADDR + 0x10)
#define UART_STATUS_TXFULL 0

#define UART_WDATA(id) (UART##id##_BASE_ADDR + 0x18)
#define UART_WDATA_REG_OFFSET 0x18
#define UART_WDATA_WDATA_MASK 0xff
#define UART_WDATA_WDATA_OFFSET 0

// Accessor for 32-bit hardware registers
#define REG32(name) *((volatile uint32_t *)(name))

#define MASK_AND_OFFSET(value, regname) \
  ((value & UART_##regname##_MASK) << UART_##regname##_OFFSET)

#define MACHINE_UART_BAUD 115200ull
#define MACHINE_UART_CLK_HZ (48ull * 1000 * 1000)

void machine_init_uart(void) {
  uint64_t ctrl_nco = ((uint64_t)MACHINE_UART_BAUD << 20) / MACHINE_UART_CLK_HZ;

  // Enables TX and RX and sets baud.
  REG32(UART_CTRL(0)) = (1 << UART_CTRL_TX) | (1 << UART_CTRL_RX) |
                        MASK_AND_OFFSET(ctrl_nco, CTRL_NCO);

  // Resets TX and RX FIFOs.
  REG32(UART_FIFO_CTRL(0)) =
      (1 << UART_FIFO_CTRL_RXRST) | (1 << UART_FIFO_CTRL_TXRST);
}

void machine_putchar(char c) {
  while ((REG32(UART_STATUS(0)) & (1 << UART_STATUS_TXFULL)) != 0) {
  }
  REG32(UART_WDATA(0)) = MASK_AND_OFFSET(c, WDATA_WDATA);
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
