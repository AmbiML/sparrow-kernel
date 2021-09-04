/*
 * Copyright 2021, Google LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

static void plic_clear_target_registers(uint32_t target) {
  mmio_region_t base_addr = plic_base_addr();

  // Clears interrupt enable registers.
  ptrdiff_t offset = plic_irq_enable_base_for_target(target);
  for (int i = 0; i < RV_PLIC_IE0_MULTIREG_COUNT; ++i) {
    ptrdiff_t multireg_offset = offset + (i * sizeof(uint32_t));
    mmio_region_write32(base_addr, multireg_offset, 0);
  }

  // Clears threshold registers.
  offset = plic_threshold_base_for_target(target);
  mmio_region_write32(base_addr, offset, 0);

  // Clears software interrupt registers.
  offset = plic_software_irq_base_for_target(target);
  mmio_region_write32(base_addr, offset, 0);
}

static void plic_reset(void) {
  // Do not touch any of the source registers; we assume that the secure core
  // has set these up.

  // Clears all of the target related registers.
  plic_clear_target_registers(RV_PLIC_SEL4_TARGET);
}
