/*
 * Copyright 2021, Google LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

static void plic_reset(void) {
  mmio_region_t base_addr = plic_base_addr();

  // Clears all of the Level/Edge registers.
  for (int i = 0; i < RV_PLIC_LE_MULTIREG_COUNT; ++i) {
    ptrdiff_t offset = RV_PLIC_LE_0_REG_OFFSET + i * sizeof(uint32_t);
    mmio_region_write32(base_addr, offset, 0);
  }

  // Clears all of the priority registers.
  for (int i = 0; i < RV_PLIC_PARAM_NUM_SRC; ++i) {
    ptrdiff_t offset = RV_PLIC_PRIO0_REG_OFFSET + i * sizeof(uint32_t);
    mmio_region_write32(base_addr, offset, 0);
  }

  // Clears all of the target related registers.
  for (dif_plic_target_t target = 0; target < RV_PLIC_PARAM_NUM_TARGET;
       ++target) {
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
}
