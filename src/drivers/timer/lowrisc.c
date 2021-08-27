/*
 * Copyright 2021, Google LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <drivers/timer/lowrisc.h>

// Nonstandard offset of TIMER_CTRL register.
//
// The Renode emulation has TIMER_CTRL at 0x0, which does not agree with the
// OpenTitan documentation and opentitan/timer.h. These have
// RV_TIMER_CTRL_REG_OFFSET == 0x4.
#define RV_TIMER_CUSTOM_CTRL_REG_OFFSET 0x0

static uint64_t euclidean_gcd(uint64_t a, uint64_t b) {
  while (b != 0) {
    uint64_t old_b = b;
    b = a % b;
    a = old_b;
  }
  return a;
}

BOOT_CODE void initTimer(void) {
  // Disable counters.
  const uint32_t hart_bit = (1ul << CONFIG_FIRST_HART_ID);
  uint32_t timer_ctrl = RV_TIMER_REG(RV_TIMER_CUSTOM_CTRL_REG_OFFSET);
  timer_ctrl &= ~hart_bit;
  RV_TIMER_REG(RV_TIMER_CUSTOM_CTRL_REG_OFFSET) = timer_ctrl;

  // Claim any pending interrupts.
  RV_TIMER_HART_REG(RV_TIMER_INTR_STATE0_REG_OFFSET) = 1;
  // Disable all interrupts.
  RV_TIMER_HART_REG(RV_TIMER_INTR_ENABLE0_REG_OFFSET) = 0;

  // Reset the comparator to its default all-ones state.
  RV_TIMER_HART_REG(RV_TIMERCMP_LOWER_OFFSET) = UINT32_MAX;
  RV_TIMER_HART_REG(RV_TIMERCMP_UPPER_OFFSET) = UINT32_MAX;

  // Reset the counter to zero.
  RV_TIMER_HART_REG(RV_TIMER_TIMER_V_LOWER0_REG_OFFSET) = 0;
  RV_TIMER_HART_REG(RV_TIMER_TIMER_V_UPPER0_REG_OFFSET) = 0;

  // Set the prescale and step.
  const uint64_t clock_freq = TIMER_CLOCK_HZ;
  const uint64_t counter_freq =
      (RV_TIMER_TICKS_PER_US * RV_TIMER_US_PER_MS * RV_TIMER_MS_PER_S);
  const uint64_t gcd = euclidean_gcd(clock_freq, counter_freq);
  const uint64_t prescale = div64(clock_freq, gcd) - 1;
  const uint64_t step = div64(counter_freq, gcd);
  RV_TIMER_HART_REG(RV_TIMER_CFG0_REG_OFFSET) =
      (((prescale & RV_TIMER_CFG0_PRESCALE_MASK)
        << RV_TIMER_CFG0_PRESCALE_OFFSET) |
       ((step & RV_TIMER_CFG0_STEP_MASK) << RV_TIMER_CFG0_STEP_OFFSET));

  // Enable interrupts.
  RV_TIMER_HART_REG(RV_TIMER_INTR_ENABLE0_REG_OFFSET) = 1;

  // Set enabled for seL4's hart.
  RV_TIMER_REG(RV_TIMER_CUSTOM_CTRL_REG_OFFSET) = hart_bit;

  // CONFIG_TIMER_TICK_MS is only defined in non-MCS.
#ifndef CONFIG_KERNEL_MCS
  resetTimer();
#endif /* !CONFIG_KERNEL_MCS */
}
