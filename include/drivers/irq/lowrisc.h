/*
 * Copyright 2021, Google LLC
 *
 * The reader may wish to skip to the comment "seL4 interface." The rest of the
 * file contains copies of functions from the OpenTitan DIF reference
 * implementation that are useful for implementing the functions declared under
 * "seL4 interface."
 *
 * Adapted from OpenTitan DIF implementation
 * https://docs.opentitan.org/sw/apis/dif__plic_8c_source.html
 * and its dependencies from the same source tree.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <kernel/gen_config.h>
#include <plat/machine/devices_gen.h>
#include <stdint.h>

#include "opentitan/plic.h"

#define RV_PLIC0_BASE_ADDR PLIC_PPTR

#define RV_PLIC_SEL4_TARGET CONFIG_FIRST_HART_ID

#define HAVE_SET_TRIGGER 1

typedef word_t ptrdiff_t;
typedef bool_t bool;

// From mmio.h

/**
 * An opaque handle to an MMIO region.
 */
typedef struct mmio_region {
  volatile void *base;
} mmio_region_t;

/**
 * Reads an aligned uint32_t from the MMIO region `base` at the given byte
 * offset.
 *
 * This function is guaranteed to commit a read to memory, and will not be
 * reordered with respect to other MMIO manipulations.
 *
 * @param base the region to read from.
 * @param offset the offset to read at, in bytes.
 * @return the read value.
 */
static inline uint32_t mmio_region_read32(mmio_region_t base,
                                          ptrdiff_t offset) {
  return ((volatile uint32_t *)base.base)[offset / sizeof(uint32_t)];
}

/**
 * Writes an aligned uint32_t to the MMIO region `base` at the given byte
 * offset.
 *
 * This function is guaranteed to commit a write to memory, and will not be
 * reordered with respect to other region manipulations.
 *
 * @param base the region to write to.
 * @param offset the offset to write at, in bytes.
 * @param value the value to write.
 */
static inline void mmio_region_write32(mmio_region_t base, ptrdiff_t offset,
                                       uint32_t value) {
  ((volatile uint32_t *)base.base)[offset / sizeof(uint32_t)] = value;
}

// From bitfield.h, for, e.g., the packed enable (IE) bits.

/**
 * A field of a 32-bit bitfield.
 *
 * The following field definition: `{ .mask = 0b11, .index = 12 }`
 *
 * Denotes the X-marked bits in the following 32-bit bitfield:
 *
 *     field:  0b--------'--------'--XX----'--------
 *     index:   31                                 0
 *
 * Restrictions: The index plus the width of the mask must not be greater than
 * 31.
 */
typedef struct bitfield_field32 {
  /** The field mask. Usually all ones. */
  uint32_t mask;
  /** The field position in the bitfield, counting from the zero-bit. */
  uint32_t index;
} bitfield_field32_t;

/**
 * A single bit in a 32-bit bitfield.
 *
 * This denotes the position of a single bit, counting from the zero-bit.
 *
 * For instance, `(bitfield_bit_index_t)4` denotes the X-marked bit in the
 * following 32-bit bitfield:
 *
 *     field:  0b--------'--------'--------'---X----
 *     index:   31                                 0
 *
 * Restrictions: The value must not be greater than 31.
 */
typedef uint32_t bitfield_bit32_index_t;

/**
 * Writes `value` to `field` in `bitfield`.
 *
 * This function uses the `field` parameter to set specific bits in `bitfield`.
 * The relevant portion of `bitfield` is zeroed before the bits are set to
 * `value`.
 *
 * @param bitfield Bitfield to set the field in.
 * @param field Field within bitfield to be set.
 * @param value Value for the new field.
 * @return `bitfield` with `field` set to `value`.
 */
static inline uint32_t bitfield_field32_write(uint32_t bitfield,
                                              bitfield_field32_t field,
                                              uint32_t value) {
  bitfield &= ~(field.mask << field.index);
  bitfield |= (value & field.mask) << field.index;
  return bitfield;
}

/**
 * Turns a `bitfield_bit32_index_t` into a `bitfield_field32_t` (which is more
 * general).
 *
 * @param bit_index The corresponding single bit to turn into a field.
 * @return A 1-bit field that corresponds to `bit_index`.
 */
static inline bitfield_field32_t bitfield_bit32_to_field32(
    bitfield_bit32_index_t bit_index) {
  return (bitfield_field32_t){
      .mask = 0x1,
      .index = bit_index,
  };
}

/**
 * Writes `value` to the `bit_index`th bit in `bitfield`.
 *
 * @param bitfield Bitfield to update the bit in.
 * @param bit_index Bit to update.
 * @param value Bit value to write to `bitfield`.
 * @return `bitfield` with the `bit_index`th bit set to `value`.
 */
static inline uint32_t bitfield_bit32_write(uint32_t bitfield,
                                            bitfield_bit32_index_t bit_index,
                                            bool value) {
  return bitfield_field32_write(bitfield, bitfield_bit32_to_field32(bit_index),
                                value ? 0x1u : 0x0u);
}

// From dif_plic.c

typedef uint32_t dif_plic_irq_id_t;
typedef uint32_t dif_plic_target_t;

/**
 * PLIC register info.
 *
 * This data type is used to store IRQ source bit offset within a register,
 * and the offset of this register inside the PLIC.
 */
typedef struct plic_reg_info {
  ptrdiff_t offset;
  bitfield_bit32_index_t bit_index;
} plic_reg_info_t;

/**
 * Get an IE, IP or LE register offset (IE0_0, IE01, ...) from an IRQ source ID.
 *
 * With more than 32 IRQ sources, there is a multiple of these registers to
 * accommodate all the bits (1 bit per IRQ source). This function calculates
 * the offset for a specific IRQ source ID (ID 32 would be IE01, ...).
 */
static ptrdiff_t plic_offset_from_reg0(dif_plic_irq_id_t irq) {
  uint8_t register_index = irq / RV_PLIC_PARAM_REG_WIDTH;
  return register_index * sizeof(uint32_t);
}

/**
 * Get an IE, IP, LE register bit index from an IRQ source ID.
 *
 * With more than 32 IRQ sources, there is a multiple of these registers to
 * accommodate all the bits (1 bit per IRQ source). This function calculates
 * the bit position within a register for a specific IRQ source ID (ID 32 would
 * be bit 0).
 */
static uint8_t plic_irq_bit_index(dif_plic_irq_id_t irq) {
  return irq % RV_PLIC_PARAM_REG_WIDTH;
}

/**
 * Get the address of the first target N interrupt enable register (IEN0).
 */
static ptrdiff_t plic_irq_enable_base_for_target(dif_plic_target_t target) {
  assert(target < 2);
  if (target == 0) {
    return RV_PLIC_IE0_0_REG_OFFSET;
  } else {
    return RV_PLIC_IE1_0_REG_OFFSET;
  }
}

/**
 * Get a target and an IRQ source specific Interrupt Enable register info.
 */
static plic_reg_info_t plic_irq_enable_reg_info(dif_plic_irq_id_t irq,
                                                dif_plic_target_t target) {
  ptrdiff_t offset = plic_offset_from_reg0(irq);
  return (plic_reg_info_t){
      .offset = plic_irq_enable_base_for_target(target) + offset,
      .bit_index = plic_irq_bit_index(irq),
  };
}

/**
 * Get an IRQ source specific Level/Edge register info.
 */
static plic_reg_info_t plic_irq_trigger_type_reg_info(dif_plic_irq_id_t irq) {
  ptrdiff_t offset = plic_offset_from_reg0(irq);
  return (plic_reg_info_t){
      .offset = RV_PLIC_LE_0_REG_OFFSET + offset,
      .bit_index = plic_irq_bit_index(irq),
  };
}

/**
 * Get the address of the first target N software interrupt register (MSIPN).
 */
static ptrdiff_t plic_software_irq_base_for_target(dif_plic_target_t target) {
  assert(target < 2);
  if (target == 0) {
    return RV_PLIC_MSIP0_REG_OFFSET;
  } else {
    return RV_PLIC_MSIP1_REG_OFFSET;
  }
}

/**
 * Get the address of the first target N threshold register (THRESHOLDN).
 */
static ptrdiff_t plic_threshold_base_for_target(dif_plic_target_t target) {
  assert(target < 2);
  if (target == 0) {
    return RV_PLIC_THRESHOLD0_REG_OFFSET;
  } else {
    return RV_PLIC_THRESHOLD1_REG_OFFSET;
  }
}

/**
 * Get the address of the first target N claim complete register (CCN).
 */
static ptrdiff_t plic_claim_complete_base_for_target(dif_plic_target_t target) {
  assert(target < 2);
  if (target == 0) {
    return RV_PLIC_CC0_REG_OFFSET;
  } else {
    return RV_PLIC_CC1_REG_OFFSET;
  }
}

static inline mmio_region_t plic_base_addr(void) {
  return (mmio_region_t){.base = (void *)RV_PLIC0_BASE_ADDR};
}

static void plic_reset(void);

static inline irq_t plic_get_claim(void) {
  ptrdiff_t claim_complete_reg =
      plic_claim_complete_base_for_target(RV_PLIC_SEL4_TARGET);
  return mmio_region_read32(plic_base_addr(), claim_complete_reg);
}

static inline void plic_complete_claim(irq_t irq) {
  ptrdiff_t claim_complete_reg =
      plic_claim_complete_base_for_target(RV_PLIC_SEL4_TARGET);
  mmio_region_write32(plic_base_addr(), claim_complete_reg, irq);
}

static inline void plic_mask_irq(bool_t disable, irq_t irq) {
  mmio_region_t base_addr = plic_base_addr();
  for (uint32_t target = 0; target < RV_PLIC_PARAM_NUM_TARGET; ++target) {
    bool new_bit = !disable;
    // When an IRQ is enabled for seL4, disable it for all other targets to
    // prevent stolen claims. If we are disabling for seL4, do nothing for other
    // targets.
    //
    // TODO(b/199195540): Adjust this in accord with the strategy that is
    // ultimately agreed upon in scope of the bug.
    if (target != RV_PLIC_SEL4_TARGET) {
      if (disable) {
        continue;
      } else {
        new_bit = false;
      }
    }

    plic_reg_info_t reg_info = plic_irq_enable_reg_info(irq, target);
    uint32_t reg = mmio_region_read32(base_addr, reg_info.offset);
    reg = bitfield_bit32_write(reg, reg_info.bit_index, new_bit);
    mmio_region_write32(base_addr, reg_info.offset, reg);
  }
}

#ifdef HAVE_SET_TRIGGER
static inline void plic_irq_set_trigger(irq_t irq, bool_t edge_triggered) {
  mmio_region_t base_addr = plic_base_addr();
  plic_reg_info_t reg_info = plic_irq_trigger_type_reg_info(irq);

  uint32_t reg = mmio_region_read32(base_addr, reg_info.offset);
  reg = bitfield_bit32_write(reg, reg_info.bit_index, edge_triggered);
  mmio_region_write32(base_addr, reg_info.offset, reg);
}
#endif

static void plic_init_controller(void) { plic_reset(); }
