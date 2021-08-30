/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <arch/kernel/machine_isr.h>
#include <arch/kernel/machine_timer.h>
#include <arch/kernel/machine_uart.h>
#include <arch/kernel/preboot.h>
#include <stdint.h>

#define PAGE_SIZE 0x00001000
#define MEGAPAGE_SIZE 0x00400000
#define PERIPHERAL_START 0x40000000
#define EXTFLASH_START 0x44000000

// bigger than we need, but we have plenty of machine stack
#define REGISTER_FILE_SIZE 256

#define IRQ_S_SOFT 1
#define IRQ_M_SOFT 3
#define IRQ_S_TIMER 5
#define IRQ_S_EXT 9

#define MSTATUS_MPIE 0x00000080
#define MSTATUS_MPP 0x00001800
#define PRV_S 1

#define CAUSE_MISALIGNED_FETCH 0x0
#define CAUSE_BREAKPOINT 0x3
#define CAUSE_USER_ECALL 0x8
#define CAUSE_FETCH_PAGE_FAULT 0xc
#define CAUSE_LOAD_PAGE_FAULT 0xd
#define CAUSE_STORE_PAGE_FAULT 0xf

#define MIP_MSIP (1 << IRQ_M_SOFT)
#define MIP_SSIP (1 << IRQ_S_SOFT)
#define MIP_STIP (1 << IRQ_S_TIMER)
#define MIP_SEIP (1 << IRQ_S_EXT)

// The machine-mode stack. Register files are stashed in the top 256 bytes
// during interrupts/traps. Both ends of the machine stack have sentinel values.
char machine_stack_alloc[PAGE_SIZE];

// The initial supervisor-mode page table, only used to get us into virtual mode
// when we call init_kernel(). This corresponds to "l1pt" in elfloader's boot.c.
uint32_t supervisor_l1pt[PAGE_SIZE / sizeof(uint32_t)]
    __attribute__((aligned(PAGE_SIZE)));

extern char kernel_stack_alloc[CONFIG_MAX_NUM_NODES]
                              [BIT(CONFIG_KERNEL_STACK_BITS)];

BOOT_CODE void machine_assert(int x, const char *message) {
  if (!x) {
    machine_printf("assertion failed: ");
    machine_printf(message);
    while (1) {
    }
  }
}

BOOT_CODE static void machine_map_megapage(void *paddr, void *vaddr,
                                           uint32_t *pt) {
  machine_assert(((uint32_t)vaddr & (MEGAPAGE_SIZE - 1)) == 0,
                 "Misaligned virtual megapage address");
  machine_assert(((uint32_t)paddr & (MEGAPAGE_SIZE - 1)) == 0,
                 "Misaligned physical megapage address");

  uint32_t page = (uint32_t)vaddr >> 22;
  uint32_t leaf = (((uint32_t)paddr & 0xFFFFFC00) >> 2) | 0xCF;

  pt[page] = leaf;
}

BOOT_CODE static __attribute__((naked)) void set_mscratch(uint32_t a0) {
  asm volatile("csrw mscratch, a0; ret;");
}

BOOT_CODE static __attribute__((naked)) void set_sscratch(uint32_t a0) {
  asm volatile("csrw sscratch, a0; ret;");
}

// Resets GP+SP, enables virtual memory, and then "mret"s into the target
// function so we can run in supervisor mode using the kernel stack with virtual
// addresses. Arguments a0-a4 are passed along to the target function.

BOOT_CODE static __attribute__((naked)) void
machine_to_supervisor_trampoline(uint32_t a0, uint32_t a1, uint32_t a2,
                                 uint32_t a3, uint32_t a4, uint32_t a5_satp,
                                 uint32_t a6_target, uint32_t a7_offset) {
  asm volatile(
      ".option push\n"
      ".option norelax\n"

      // Set the global pointer to the linker-provided base address.
      "la gp, __global_pointer$\n"

      // Set the stack pointer to the kernel stack end.
      "la sp, (kernel_stack_alloc + 4096)\n"

      // Clear supervisor-mode scratch register
      "csrw sscratch, x0\n"

      // Enable supervisor-mode virtual memory
      "csrw satp, a5\n"

      // Shift our target address, sp, and gp up to virtual space.
      "add a6, a6, a7\n"
      "add gp, gp, a7\n"
      "add sp, sp, a7\n"

      // Leave machine-mode and resume at init_kernel in supervisor mode.
      "csrw mepc, a6\n"
      "mret\n"

      ".option pop\n");
}

// This is the first method in the seL4 kernel.elf that runs after _start.
// This function executes in machine-mode and uses the machine-mode stack.

BOOT_CODE void preinit_kernel(paddr_t ui_p_reg_start, // user app paddr
                              paddr_t ui_p_reg_end,   // user app paddr
                              sword_t pv_offset, // user app virt-to-phys offset
                              vptr_t v_entry)    // user app vaddr entry point
{

  machine_init_uart();
  machine_printf("\n");
  machine_printf("----------\n");
  machine_printf("preinit_kernel()\n");

  opentitan_timer_init(TIMER_CLOCK_HZ);
  // TODO(mattharvey): [rdtime_sync] The seL4 scheduler sets deadlines at
  // (rdtime + RESET_CYCLES). The implicit assumption is that the timer and
  // rdtime are coming from the same source, so we have to start the timer at
  // rdtime. When switching to Ibex, rdtime will have to be implemented in terms
  // of the timer, and this can be removed.
  opentitan_timer_set_count(riscv_read_time());

  uint32_t *sentinels = (uint32_t *)machine_stack_alloc;
  sentinels[0] = 0xDEADBEEF;
  sentinels[1023] = 0xF00DCAFE;

  // Machine-mode scratch register points to the machine-mode stack, scooted
  // down by 256 bytes to make room for the saved kernel/user register file.
  set_mscratch(
      (uint32_t)(machine_stack_alloc + PAGE_SIZE - REGISTER_FILE_SIZE));
  set_sscratch(0);

  // Enable user/supervisor use of perf counters.
  asm volatile("csrw scounteren, %0" ::"rK"(-1));
  asm volatile("csrw mcounteren, %0" ::"rK"(-1));

  // Set machine-mode vector table and enable software interrupts.
  asm volatile("csrw mtvec, %0" ::"rK"((uint32_t)(&machine_vector_table) | 1));
  asm volatile("csrw mie, %0" ::"rK"(MIP_MSIP));

  // Disable paging
  asm volatile("csrw sptbr, %0" ::"rK"(0));

  // Send S-mode interrupts and most exceptions straight to S-mode.
  uint32_t interrupts = MIP_SSIP | MIP_STIP | MIP_SEIP;
  uint32_t exceptions =
      (1U << CAUSE_MISALIGNED_FETCH) | (1U << CAUSE_FETCH_PAGE_FAULT) |
      (1U << CAUSE_BREAKPOINT) | (1U << CAUSE_LOAD_PAGE_FAULT) |
      (1U << CAUSE_STORE_PAGE_FAULT) | (1U << CAUSE_USER_ECALL);
  asm volatile("csrw mideleg, %0" ::"rK"(interrupts));
  asm volatile("csrw medeleg, %0" ::"rK"(exceptions));

  // Enable machine-mode interrupts.
  uint32_t mstatus;
  asm volatile("csrr %0, mstatus" : "=r"(mstatus));
  mstatus &= ~MSTATUS_MPP;
  mstatus |= PRV_S << 11;
  mstatus &= ~MSTATUS_MPIE;
  asm volatile("csrw mstatus, %0" ::"r"(mstatus));

  // Map the kernel physical megapage to virtual megapage.
  machine_map_megapage((void *)KERNEL_ELF_PADDR_BASE, (void *)KERNEL_ELF_BASE,
                       (uint32_t *)supervisor_l1pt);

  // Switch to supervisor mode & enter init_kernel.
  uint32_t new_satp =
      (uint32_t)(0x1llu << 31) | ((uint32_t)supervisor_l1pt >> 12);
  machine_to_supervisor_trampoline(ui_p_reg_start, ui_p_reg_end, pv_offset,
                                   v_entry, 0, new_satp, (uint32_t)init_kernel,
                                   KERNEL_ELF_BASE - KERNEL_ELF_PADDR_BASE);
}
