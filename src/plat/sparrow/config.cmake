cmake_minimum_required(VERSION 3.7.2)

declare_platform(sparrow KernelPlatformSparrow PLAT_SHODAN KernelArchRiscV)

if(KernelPlatformSparrow)
    declare_seL4_arch(riscv32)
    config_set(KernelRiscVPlatform RISCV_PLAT "sparrow")
    config_set(KernelPlatformFirstHartID FIRST_HART_ID 1)
    list(APPEND KernelDTSList "tools/dts/sparrow.dts")
    include_directories("${CMAKE_BINARY_DIR}/opentitan-gen/include")
    declare_default_headers(
        TIMER_FREQUENCY 10000000llu PLIC_MAX_NUM_INT 96
        INTERRUPT_CONTROLLER drivers/irq/lowrisc.h
    )
else()
    unset(KernelPlatformFirstHartID CACHE)
endif()
