/*
 * Copyright 2021, Google LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <arch/kernel/machine_timer.h>

// Base address for the entire timer, defined in devices_gen.h based on the
// device tree used at build time.
#define RV_TIMER_BASE 0x40104000

// The clock rate of the timer. Used for setting the prescaler and step.
#define RV_TIMER_CLOCK_HZ 24000000

// Identifies which hart timer interrupts should be sent to, among the harts
// connected to that timer. This may not be identical with the hartid that hart
// will report.
#define RV_TIMER_HART_INDEX 0

// Nonstandard offset of TIMER_CTRL register.
//
// The Renode emulation has TIMER_CTRL at 0x0, which does not agree with the
// OpenTitan documentation and opentitan/timer.h. These have
// RV_TIMER_CTRL_REG_OFFSET == 0x4.
#define RV_TIMER_CUSTOM_CTRL_REG_OFFSET 0x0

// Offset between hart-specific registers of consecutive
#define RV_TIMER_HART_SPACING 0x100

// Base address of hart-specific registers for RV_TIMER_HART_INDEX.  These can
// be accessed by adding this to one of the _0_REG_OFFSET macros from
// opentitan/timer.h.
#define RV_TIMER_HART_BASE \
  (RV_TIMER_BASE + (RV_TIMER_HART_SPACING * RV_TIMER_HART_INDEX))

// We assume a unique comparator, since seL4 needs only one.
#define RV_TIMER_COMPARATOR_INDEX 0

// Offsets of our preferred comparator defined in terms of its index.
#define RV_TIMERCMP_LOWER_OFFSET          \
  (RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET + \
   (sizeof(uint64_t) * RV_TIMER_COMPARATOR_INDEX))
#define RV_TIMERCMP_UPPER_OFFSET          \
  (RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET + \
   (sizeof(uint64_t) * RV_TIMER_COMPARATOR_INDEX))

// Read/write access for a hart-independent register.
#define RV_TIMER_REG(name) *((volatile uint32_t *)(RV_TIMER_BASE + name))

// Read/write access for a register pertaining to the seL4 "first hart."
#define RV_TIMER_HART_REG(name) \
  *((volatile uint32_t *)(RV_TIMER_HART_BASE + name))

static uint32_t euclidean_gcd(uint32_t a, uint32_t b) {
  while (b != 0) {
    uint32_t old_b = b;
    b = a % b;
    a = old_b;
  }
  return a;
}

static void opentitan_timer_set_count(uint64_t count) {
  RV_TIMER_HART_REG(RV_TIMER_TIMER_V_LOWER0_REG_OFFSET) = (uint32_t)count;
  RV_TIMER_HART_REG(RV_TIMER_TIMER_V_UPPER0_REG_OFFSET) = count >> 32;
}

bool_t opentitan_timer_init(uint32_t counter_frequency_hz) {
  // Disable counters.
  const uint32_t hart_bit = (1ul << RV_TIMER_HART_INDEX);
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

  // Set the prescale and step.
  const uint32_t gcd = euclidean_gcd(RV_TIMER_CLOCK_HZ, counter_frequency_hz);
  const uint32_t prescale = RV_TIMER_CLOCK_HZ / gcd - 1;
  const uint32_t step = counter_frequency_hz / gcd;
  const uint32_t prescale_masked = prescale & RV_TIMER_CFG0_PRESCALE_MASK;
  const uint32_t step_masked = step & RV_TIMER_CFG0_STEP_MASK;
  if (!((prescale == prescale_masked) && (step == step_masked))) {
    return false;
  }
  RV_TIMER_HART_REG(RV_TIMER_CFG0_REG_OFFSET) =
      ((prescale_masked << RV_TIMER_CFG0_PRESCALE_OFFSET) |
       (step_masked << RV_TIMER_CFG0_STEP_OFFSET));

  // Set the initial count to zero.
  opentitan_timer_set_count(0);

  // Enable interrupts.
  RV_TIMER_HART_REG(RV_TIMER_INTR_ENABLE0_REG_OFFSET) = 1;

  // Set enabled for seL4's hart.
  RV_TIMER_REG(RV_TIMER_CUSTOM_CTRL_REG_OFFSET) = hart_bit;

  return true;
}

uint64_t opentitan_timer_get_count(void) {
  while (true) {
    const uint32_t upper =
        RV_TIMER_HART_REG(RV_TIMER_TIMER_V_UPPER0_REG_OFFSET);
    const uint32_t lower =
        RV_TIMER_HART_REG(RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
    const uint32_t overflow_check =
        RV_TIMER_HART_REG(RV_TIMER_TIMER_V_UPPER0_REG_OFFSET);
    if (upper == overflow_check) {
      return (((uint64_t)upper) << 32) | lower;
    }
  }
}

void opentitan_timer_set_deadline(uint64_t count) {
  const uint32_t lower = count;
  const uint32_t upper = count >> 32;
  // First, set the upper register to the largest value possible so that an
  // interrupt will not fire when we set the lower register.
  RV_TIMER_HART_REG(RV_TIMERCMP_UPPER_OFFSET) = UINT32_MAX;
  RV_TIMER_HART_REG(RV_TIMERCMP_LOWER_OFFSET) = lower;
  // Now set the upper register to the value from the function argument, which
  // might cause an interrupt.
  RV_TIMER_HART_REG(RV_TIMERCMP_UPPER_OFFSET) = upper;
}

void opentitan_timer_ack(void) {
  RV_TIMER_HART_REG(RV_TIMER_INTR_STATE0_REG_OFFSET) = 1;
}
