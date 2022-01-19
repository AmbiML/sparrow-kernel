/*
 * Copyright 2021, Google LLC
 *
 * Simple API to the OpenTitan Timer HWIP
 *
 * https://docs.opentitan.org/hw/ip/rv_timer/doc/
 *
 * This implementation assumes we use only the compatator at index 0 on a single
 * hart at index 0.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "opentitan/timer.h"

// Resets and initializes the timer.
//
// In particular this:
//   1. Disables interrupts and clears the hart enabled bit.
//   2. Sets the counter to zero and the deadline to the maximum possible.
//   3. Sets the prescaler and step so that the counter will increase at the
//      given counter_frequency_hz.
//   4. Re-enables interrupts and sets the hart enabled bit.
//
// Returns whether the timer was initialized successfully.
bool_t opentitan_timer_init(uint32_t counter_frequency_hz);

// Gets the current value of the counter.
uint64_t opentitan_timer_get_count(void);

// Schedules an interrupt at the given (absolute) counter value.
void opentitan_timer_set_deadline(uint64_t count);

// Clears a pending interrupt, if any.
void opentitan_timer_ack(void);
