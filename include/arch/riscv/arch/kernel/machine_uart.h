/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

void machine_init_uart(void);
void machine_putchar(char c);
void machine_printf(const char *fmt, ...); // only prints 32-bit "%x" hex values
