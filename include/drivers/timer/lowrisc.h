/*
 * Copyright 2021, Google LLC
 *
 * seL4 timer implemented using the OpenTitan Timer HWIP
 *
 * https://docs.opentitan.org/hw/ip/rv_timer/doc/
 *
 * This implementation assumes we use only the compatator at index 0 on a single
 * hart, the one with hartid == CONFIG_FIRST_HART_ID.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <config.h>
#include <machine/timer.h>
#include <stdint.h>

#include "opentitan/timer.h"

// Base address for the entire timer, defined in devices_gen.h based on the
// device tree used at build time.
#define RV_TIMER_BASE ((uint32_t)TIMER_PPTR)

// The prescaler and step will be set so that, in each second, the counter
// increases by this number.
#define RV_TIMER_COUNTER_FREQUENCY 1000

// Offset between hart-specific registers of consecutive
#define RV_TIMER_HART_SPACING 0x100

// Base address of hart-specific registers for hartid == CONFIG_FIRST_HART_ID.
// These can be accessed by adding this to one of the _0_REG_OFFSET macros from
// opentitan/timer.h.
#define RV_TIMER_HART_BASE \
  (RV_TIMER_BASE + (RV_TIMER_HART_SPACING * CONFIG_FIRST_HART_ID))

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

// All definitions below are for signatures that make up the general seL4 timer
// interface.

static inline ticks_t getCurrentTime(void) {
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

static inline void setDeadline(ticks_t deadline) {
  const uint32_t lower = deadline;
  const uint32_t upper = deadline >> 32;
  // First, set the upper register to the largest value possible so that an
  // interrupt will not fire when we set the lower register.
  RV_TIMER_HART_REG(RV_TIMERCMP_UPPER_OFFSET) = UINT32_MAX;
  RV_TIMER_HART_REG(RV_TIMERCMP_LOWER_OFFSET) = lower;
  // Now set the upper register to the value from the function argument, which
  // might cause an interrupt.
  RV_TIMER_HART_REG(RV_TIMERCMP_UPPER_OFFSET) = upper;
}

static inline void ackDeadlineIRQ(void) {
  RV_TIMER_HART_REG(RV_TIMER_INTR_STATE0_REG_OFFSET) = 1;
}

#ifndef CONFIG_KERNEL_MCS
static inline void resetTimer(void) {
  uint64_t target;
  do {
    target = getCurrentTime() +
             CONFIG_TIMER_TICK_MS * RV_TIMER_COUNTER_FREQUENCY / 1000;
    setDeadline(target);
  } while (getCurrentTime() > target);
}
#endif /* !CONFIG_KERNEL_MCS */
