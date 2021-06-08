/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2015, 2016 Hesham Almatary <heshamelmatary@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <config.h>
#include <util.h>

.section .boot.text, "ax"
.global _start
.extern __global_pointer$
.extern machine_stack_alloc
.extern preinit_kernel

/*
 * seL4 elf entry point
 */
_start:
  fence.i
.option push
.option norelax
  la gp, __global_pointer$
  // sp = stack_top - 4 for the machine stack sentinel
  la sp, machine_stack_alloc + BIT(CONFIG_KERNEL_STACK_BITS) - 4
  j preinit_kernel
.option pop
