cmake_minimum_required(VERSION 3.7.2)

declare_platform(sparrow KernelPlatformSparrow PLAT_SHODAN KernelArchRiscV)

if(KernelPlatformSparrow)
    declare_seL4_arch(riscv32)
    config_set(KernelRiscVPlatform RISCV_PLAT "sparrow")
    config_set(KernelPlatformFirstHartID FIRST_HART_ID 0)
    list(APPEND KernelDTSList "tools/dts/sparrow.dts")
    declare_default_headers(
        TIMER_FREQUENCY 10000000llu PLIC_MAX_NUM_INT 0
        INTERRUPT_CONTROLLER arch/machine/plic.h
    )
else()
    unset(KernelPlatformFirstHartID CACHE)
endif()
