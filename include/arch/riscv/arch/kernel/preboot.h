/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

BOOT_CODE void machine_assert(int x, const char *message);

BOOT_CODE void preinit_kernel(paddr_t ui_p_reg_start, paddr_t ui_p_reg_end,
                              sword_t pv_offset, vptr_t v_entry);
