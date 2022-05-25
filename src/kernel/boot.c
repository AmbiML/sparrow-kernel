/*
 * Copyright 2014, General Dynamics C4 Systems
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <assert.h>
#include <kernel/boot.h>
#include <kernel/thread.h>
#include <machine/io.h>
#include <machine/registerset.h>
#include <model/statedata.h>
#include <arch/machine.h>
#include <arch/kernel/boot.h>
#include <arch/kernel/vspace.h>
#include <linker.h>
#include <hardware.h>
#include <util.h>

/* (node-local) state accessed only during bootstrapping */
BOOT_BSS ndks_boot_t ndks_boot;

BOOT_BSS rootserver_mem_t rootserver;
BOOT_BSS static region_t rootserver_mem;

BOOT_BSS word_t current_untyped;
/*
 * To boostrap the system a 2-slot CNode is manually constructed in a static memory
 * block. The CNode holds an untyped memory object from which all rootserver objects
 * are derived and the CNode for the (1-level) rootserver CSpace.
 */
BOOT_BSS char dummy_cnode_mem[DUMMY_CNODE_SIZE] __attribute__((aligned (DUMMY_CNODE_SIZE)));
#define DUMMY_CNODE_SLOT      0 /* Bootstrap 2-slot CNode */
#define DUMMY_UNTYPED_SLOT    1 /* untyped root object */
#define DUMMY_ROOT_CNODE_SLOT 2 /* Rootesrver CNode */

BOOT_CODE static void merge_regions(void)
{
    /* Walk through reserved regions and see if any can be merged */
    for (word_t i = 1; i < ndks_boot.resv_count;) {
        if (ndks_boot.reserved[i - 1].end == ndks_boot.reserved[i].start) {
            /* extend earlier region */
            ndks_boot.reserved[i - 1].end = ndks_boot.reserved[i].end;
            /* move everything else down */
            for (word_t j = i + 1; j < ndks_boot.resv_count; j++) {
                ndks_boot.reserved[j - 1] = ndks_boot.reserved[j];
            }

            ndks_boot.resv_count--;
            /* don't increment i in case there are multiple adjacent regions */
        } else {
            i++;
        }
    }
}

BOOT_CODE bool_t reserve_region(p_region_t reg)
{
    word_t i;
    assert(reg.start <= reg.end);
    if (reg.start == reg.end) {
        return true;
    }

    /* keep the regions in order */
    for (i = 0; i < ndks_boot.resv_count; i++) {
        /* Try and merge the region to an existing one, if possible */
        if (ndks_boot.reserved[i].start == reg.end) {
            ndks_boot.reserved[i].start = reg.start;
            merge_regions();
            return true;
        }
        if (ndks_boot.reserved[i].end == reg.start) {
            ndks_boot.reserved[i].end = reg.end;
            merge_regions();
            return true;
        }
        /* Otherwise figure out where it should go. */
        if (ndks_boot.reserved[i].start > reg.end) {
            /* move regions down, making sure there's enough room */
            if (ndks_boot.resv_count + 1 >= MAX_NUM_RESV_REG) {
                printf("Can't mark region 0x%lx-0x%lx as reserved, try increasing MAX_NUM_RESV_REG (currently %d)\n",
                       reg.start, reg.end, (int)MAX_NUM_RESV_REG);
                return false;
            }
            for (word_t j = ndks_boot.resv_count; j > i; j--) {
                ndks_boot.reserved[j] = ndks_boot.reserved[j - 1];
            }
            /* insert the new region */
            ndks_boot.reserved[i] = reg;
            ndks_boot.resv_count++;
            return true;
        }
    }

    if (i + 1 == MAX_NUM_RESV_REG) {
        printf("Can't mark region 0x%lx-0x%lx as reserved, try increasing MAX_NUM_RESV_REG (currently %d)\n",
               reg.start, reg.end, (int)MAX_NUM_RESV_REG);
        return false;
    }

    ndks_boot.reserved[i] = reg;
    ndks_boot.resv_count++;

    return true;
}

BOOT_CODE bool_t insert_region(region_t reg)
{
    word_t i;

    assert(reg.start <= reg.end);
    if (is_reg_empty(reg)) {
        return true;
    }
    for (i = 0; i < MAX_NUM_FREEMEM_REG; i++) {
        if (is_reg_empty(ndks_boot.freemem[i])) {
            reserve_region(pptr_to_paddr_reg(reg));
            ndks_boot.freemem[i] = reg;
            return true;
        }
    }
#ifdef CONFIG_ARCH_ARM
    /* boot.h should have calculated MAX_NUM_FREEMEM_REG correctly.
     * If we've run out, then something is wrong.
     * Note that the capDL allocation toolchain does not know about
     * MAX_NUM_FREEMEM_REG, so throwing away regions may prevent
     * capDL applications from being loaded! */
    printf("Can't fit memory region 0x%lx-0x%lx, try increasing MAX_NUM_FREEMEM_REG (currently %d)\n",
           reg.start, reg.end, (int)MAX_NUM_FREEMEM_REG);
    assert(!"Ran out of freemem slots");
#else
    printf("Dropping memory region 0x%lx-0x%lx, try increasing MAX_NUM_FREEMEM_REG (currently %d)\n",
           reg.start, reg.end, (int)MAX_NUM_FREEMEM_REG);
#endif
    return false;
}

BOOT_CODE static pptr_t alloc_rootserver_obj(word_t size_bits, word_t n)
{
    pptr_t allocated = rootserver_mem.start;
    /* allocated memory must be aligned */
    assert(allocated % BIT(size_bits) == 0);
    rootserver_mem.start += (n * BIT(size_bits));
    /* we must not have run out of memory */
    assert(rootserver_mem.start <= rootserver_mem.end);
    memzero((void *) allocated, n * BIT(size_bits));
    return allocated;
}

BOOT_CODE static word_t alignUp_(word_t baseValue, word_t alignment)
{
    return (baseValue + (BIT(alignment) - 1)) & ~MASK(alignment);
}

BOOT_CODE cap_t create_rootserver_obj(object_t objectType, word_t slot, word_t userSize)
{
    assert(current_untyped > 0);
    word_t freeIndex, freeRef;
    word_t objectSize = getObjectSize(objectType, userSize);
    word_t untypedFreeBytes;
    cte_t *untyped_cte;
    cte_t *cnode_cte = (cte_t*)rootserver.cnode;
    cap_t untyped;

    do
    {
        untyped_cte = SLOT_PTR(rootserver.cnode, current_untyped);
        untyped = untyped_cte->cap;
        assert(cap_get_capType(untyped) == cap_untyped_cap);
        exception_t status = ensureNoChildren(untyped_cte);
        if (status != EXCEPTION_NONE) {
            freeIndex = cap_untyped_cap_get_capFreeIndex(untyped);
        } else {
            freeIndex = 0;
        }
        freeRef = GET_FREE_REF(cap_untyped_cap_get_capPtr(untyped), freeIndex);
        untypedFreeBytes = BIT(cap_untyped_cap_get_capBlockSize(untyped)) -
                           FREE_INDEX_TO_OFFSET(freeIndex);
    }
    while ((untypedFreeBytes >> objectSize) < 1 && ++current_untyped);

    // Mark untyped object as tainted by the kernel
    word_t untyped_index = current_untyped - ndks_boot.bi_frame->untyped.start;
    ndks_boot.bi_frame->untypedList[untyped_index].isTainted = true;

    word_t alignedFreeRef = alignUp_(freeRef, objectSize);

    createNewObjects(
        objectType,                            /* objectType   */
        untyped_cte,                           /* parent       */
        cnode_cte,                             /* destCNode    */
        slot,                                  /* destOffset   */
        1,                                     /* destLength   */
        (void *)alignedFreeRef,                /* retypeBase   */
        userSize,                              /* userSize     */
        false                                  /* deviceMemory */
    );

    freeRef += 1 << objectSize;
    void* regionBase = WORD_PTR(cap_untyped_cap_get_capPtr(untyped));
    untyped_cte->cap = cap_untyped_cap_set_capFreeIndex(untyped,
            GET_FREE_INDEX(regionBase, freeRef));
    return SLOT_PTR(rootserver.cnode, slot)->cap;
}

BOOT_CODE static word_t rootserver_max_size_bits(word_t extra_bi_size_bits)
{
    word_t cnode_size_bits = CONFIG_ROOT_CNODE_SIZE_BITS + seL4_SlotBits;
    word_t max = MAX(cnode_size_bits, seL4_VSpaceBits);
    return MAX(max, extra_bi_size_bits);
}

BOOT_CODE static word_t calculate_necessary_size(v_region_t v_reg, word_t extra_bi_size_bits)
{
    /* work out how much memory we need for root server objects */
    word_t size = BIT(seL4_ASIDPoolBits);
    size += BIT(seL4_PageBits); // boot info
    size += extra_bi_size_bits > 0 ? BIT(extra_bi_size_bits) : 0;
#ifdef CONFIG_KERNEL_MCS
    size += BIT(seL4_MinSchedContextBits); // root sched context
#endif
    return size;
}

BOOT_CODE static void maybe_alloc_extra_bi(word_t cmp_size_bits, word_t extra_bi_size_bits)
{
    if (extra_bi_size_bits >= cmp_size_bits && rootserver.extra_bi == 0) {
        rootserver.extra_bi = alloc_rootserver_obj(extra_bi_size_bits, 1);
    }
}

BOOT_CODE void create_rootserver_objects(pptr_t start, v_region_t v_reg, word_t extra_bi_size_bits)
{
    /* the largest object the PD, the root cnode, or the extra boot info */
    word_t cnode_size_bits = CONFIG_ROOT_CNODE_SIZE_BITS + seL4_SlotBits;
    word_t max = rootserver_max_size_bits(extra_bi_size_bits);

    word_t size = calculate_necessary_size(v_reg, extra_bi_size_bits)
        + BIT(cnode_size_bits);
    rootserver_mem.start = start;
    rootserver_mem.end = start + size;

    maybe_alloc_extra_bi(max, extra_bi_size_bits);
    word_t untyped_memory = alloc_rootserver_obj(cnode_size_bits, 1);

    /* at this point we are up to creating 4k objects - which is the min size of
     * extra_bi so this is the last chance to allocate it */
    maybe_alloc_extra_bi(seL4_PageBits, extra_bi_size_bits);
    rootserver.asid_pool = alloc_rootserver_obj(seL4_ASIDPoolBits, 1);
    rootserver.boot_info = alloc_rootserver_obj(seL4_PageBits, 1);

#ifdef CONFIG_KERNEL_MCS
    rootserver.sc = alloc_rootserver_obj(seL4_MinSchedContextBits, 1);
#endif

    cap_t cap =
        cap_cnode_cap_new(
            2,                             /* radix      */
            wordBits - 2,                  /* guard size */
            0,                             /* guard      */
            (word_t)dummy_cnode_mem        /* pptr       */
        );
    write_slot(SLOT_PTR(dummy_cnode_mem, DUMMY_CNODE_SLOT), cap);

    // Create untyped capability
    cap_t ut_cap = cap_untyped_cap_new(MAX_FREE_INDEX(cnode_size_bits),
                                    false, cnode_size_bits, untyped_memory);
    write_slot(SLOT_PTR(dummy_cnode_mem, DUMMY_UNTYPED_SLOT), ut_cap);

    /* we should have allocated all our memory */
    assert(rootserver_mem.start == rootserver_mem.end);
}

BOOT_CODE void write_slot(slot_ptr_t slot_ptr, cap_t cap)
{
    slot_ptr->cap = cap;

    slot_ptr->cteMDBNode = nullMDBNode;
    mdb_node_ptr_set_mdbRevocable(&slot_ptr->cteMDBNode, true);
    mdb_node_ptr_set_mdbFirstBadged(&slot_ptr->cteMDBNode, true);
}

/* Our root CNode needs to be able to fit all the initial caps and not
 * cover all of memory.
 */
compile_assert(root_cnode_size_valid,
               CONFIG_ROOT_CNODE_SIZE_BITS < 32 - seL4_SlotBits &&
               BIT(CONFIG_ROOT_CNODE_SIZE_BITS) >= seL4_NumInitialCaps &&
               BIT(CONFIG_ROOT_CNODE_SIZE_BITS) >= (seL4_PageBits - seL4_SlotBits))

BOOT_CODE cap_t
create_root_cnode(void)
{
    word_t freeRef;
    void* regionBase;

    /* write the number of root CNode slots to global state */
    ndks_boot.slot_pos_max = BIT(CONFIG_ROOT_CNODE_SIZE_BITS);

    cte_t *untyped_cte = SLOT_PTR(dummy_cnode_mem, DUMMY_UNTYPED_SLOT);
    word_t untyped_start = cap_untyped_cap_get_capPtr(untyped_cte->cap);

    createNewObjects(
        seL4_CapTableObject,            /* objectType   */
        untyped_cte,                    /* parent       */
        (cte_t*)dummy_cnode_mem,        /* destCNode    */
        DUMMY_ROOT_CNODE_SLOT,          /* destOffset   */
        1,                              /* destLength   */
        (void *)untyped_start,          /* retypeBase   */
        CONFIG_ROOT_CNODE_SIZE_BITS,    /* userSize     */
        false                           /* deviceMemory */
    );

    freeRef = GET_FREE_REF(cap_untyped_cap_get_capPtr(untyped_cte->cap), 0);
    freeRef += 1 << getObjectSize(seL4_CapTableObject, CONFIG_ROOT_CNODE_SIZE_BITS);
    regionBase = WORD_PTR(cap_untyped_cap_get_capPtr(untyped_cte->cap));
    untyped_cte->cap = cap_untyped_cap_set_capFreeIndex(untyped_cte->cap,
            GET_FREE_INDEX(regionBase, freeRef));

    cap_t cap = SLOT_PTR(dummy_cnode_mem, DUMMY_ROOT_CNODE_SLOT)->cap;
    cap = cap_cnode_cap_set_capCNodeRadix(cap, CONFIG_ROOT_CNODE_SIZE_BITS);
    cap = cap_cnode_cap_set_capCNodeGuardSize(cap, wordBits - CONFIG_ROOT_CNODE_SIZE_BITS);
    cap = cap_cnode_cap_set_capCNodeGuard(cap, 0);
    SLOT_PTR(dummy_cnode_mem, DUMMY_ROOT_CNODE_SLOT)->cap = cap;

    rootserver.cnode = cap_cnode_cap_get_capCNodePtr(cap);

    cteMove(cap,
        SLOT_PTR(dummy_cnode_mem, DUMMY_ROOT_CNODE_SLOT),
        SLOT_PTR(rootserver.cnode, seL4_CapInitThreadCNode));
    return cap;
}

/* Check domain scheduler assumptions. */
compile_assert(num_domains_valid,
               CONFIG_NUM_DOMAINS >= 1 && CONFIG_NUM_DOMAINS <= 256)
compile_assert(num_priorities_valid,
               CONFIG_NUM_PRIORITIES >= 1 && CONFIG_NUM_PRIORITIES <= 256)

BOOT_CODE void
create_domain_cap(cap_t root_cnode_cap)
{
    /* Check domain scheduler assumptions. */
    assert(ksDomScheduleLength > 0);
    for (word_t i = 0; i < ksDomScheduleLength; i++) {
        assert(ksDomSchedule[i].domain < CONFIG_NUM_DOMAINS);
        assert(ksDomSchedule[i].length > 0);
    }

    cap_t cap = cap_domain_cap_new();
    write_slot(SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapDomain), cap);
}


BOOT_CODE cap_t create_ipcbuf_frame_cap(cap_t root_cnode_cap, cap_t pd_cap, vptr_t vptr)
{
    cap_t cap;
    seL4_SlotPos ipc_untyped_pos;

    // Create untyped memory object for ipc buffer
    cap = create_rootserver_obj(seL4_UntypedObject, ndks_boot.slot_pos_cur, seL4_PageBits);
    rootserver.ipc_buf = cap_untyped_cap_get_capPtr(cap);
    cap = cap_untyped_cap_set_capFreeIndex(cap,
            GET_FREE_INDEX((void*)rootserver.ipc_buf, rootserver.ipc_buf + BIT(seL4_PageBits)));
    SLOT_PTR(pptr_of_cap(root_cnode_cap), ndks_boot.slot_pos_cur)->cap = cap;
    ipc_untyped_pos = ndks_boot.slot_pos_cur++;

    clearMemory((void *)rootserver.ipc_buf, PAGE_BITS);

    /* create a cap of it and write it into the root CNode */
    cap = create_mapped_it_frame_cap(pd_cap, rootserver.ipc_buf, vptr, IT_ASID, false, false);
    insertNewCap(
        SLOT_PTR(pptr_of_cap(root_cnode_cap), ipc_untyped_pos),
        SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapInitThreadIPCBuffer),
        cap);

    return cap;
}

BOOT_CODE void create_bi_frame_cap(cap_t root_cnode_cap, cap_t pd_cap, vptr_t vptr)
{
    /* create a cap of it and write it into the root CNode */
    cap_t cap = create_mapped_it_frame_cap(pd_cap, rootserver.boot_info, vptr, IT_ASID, false, false);
    write_slot(SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapBootInfoFrame), cap);
}

BOOT_CODE word_t calculate_extra_bi_size_bits(word_t extra_size)
{
    if (extra_size == 0) {
        return 0;
    }

    word_t clzl_ret = clzl(ROUND_UP(extra_size, seL4_PageBits));
    word_t msb = seL4_WordBits - 1 - clzl_ret;
    /* If region is bigger than a page, make sure we overallocate rather than underallocate */
    if (extra_size > BIT(msb)) {
        msb++;
    }
    return msb;
}

BOOT_CODE void populate_bi_frame(node_id_t node_id, word_t num_nodes, vptr_t ipcbuf_vptr,
                                 word_t extra_bi_size)
{
    clearMemory((void *) rootserver.boot_info, BI_FRAME_SIZE_BITS);
    if (extra_bi_size) {
        clearMemory((void *) rootserver.extra_bi, calculate_extra_bi_size_bits(extra_bi_size));
    }

    /* initialise bootinfo-related global state */
    ndks_boot.bi_frame = BI_PTR(rootserver.boot_info);
    ndks_boot.slot_pos_cur = seL4_NumInitialCaps;
    BI_PTR(rootserver.boot_info)->nodeID = node_id;
    BI_PTR(rootserver.boot_info)->numNodes = num_nodes;
    BI_PTR(rootserver.boot_info)->numIOPTLevels = 0;
    BI_PTR(rootserver.boot_info)->ipcBuffer = (seL4_IPCBuffer *) ipcbuf_vptr;
    BI_PTR(rootserver.boot_info)->initThreadCNodeSizeBits = CONFIG_ROOT_CNODE_SIZE_BITS;
    BI_PTR(rootserver.boot_info)->initThreadDomain = ksDomSchedule[ksDomScheduleIdx].domain;
    BI_PTR(rootserver.boot_info)->extraLen = extra_bi_size;
}

BOOT_CODE bool_t provide_cap(cap_t root_cnode_cap, cap_t cap)
{
    if (ndks_boot.slot_pos_cur >= ndks_boot.slot_pos_max) {
        printf("Kernel init failed: ran out of cap slots\n");
        return false;
    }
    write_slot(SLOT_PTR(pptr_of_cap(root_cnode_cap), ndks_boot.slot_pos_cur), cap);
    ndks_boot.slot_pos_cur++;
    return true;
}

BOOT_CODE create_frames_of_region_ret_t create_frames_of_region(
    cap_t    root_cnode_cap,
    cap_t    pd_cap,
    region_t reg,
    bool_t   do_map,
    sword_t  pv_offset
)
{
    pptr_t     f;
    cap_t      frame_cap;
    seL4_SlotPos slot_pos_before;
    seL4_SlotPos slot_pos_after;

    slot_pos_before = ndks_boot.slot_pos_cur;

    for (f = reg.start; f < reg.end; f += BIT(PAGE_BITS)) {
        if (do_map) {
            frame_cap = create_mapped_it_frame_cap(pd_cap, f, pptr_to_paddr((void *)(f - pv_offset)), IT_ASID, false, true);
        } else {
            frame_cap = create_unmapped_it_frame_cap(f, false);
        }
        if (!provide_cap(root_cnode_cap, frame_cap))
            return (create_frames_of_region_ret_t) {
            S_REG_EMPTY, false
        };
    }

    slot_pos_after = ndks_boot.slot_pos_cur;

    return (create_frames_of_region_ret_t) {
        (seL4_SlotRegion) { slot_pos_before, slot_pos_after }, true
    };
}

BOOT_CODE cap_t create_it_asid_pool(cap_t root_cnode_cap)
{
    cap_t ap_cap = cap_asid_pool_cap_new(IT_ASID >> asidLowBits, rootserver.asid_pool);
    write_slot(SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapInitThreadASIDPool), ap_cap);

    /* create ASID control cap */
    write_slot(
        SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapASIDControl),
        cap_asid_control_cap_new()
    );

    return ap_cap;
}

#ifdef CONFIG_KERNEL_MCS
BOOT_CODE static bool_t configure_sched_context(tcb_t *tcb, sched_context_t *sc_pptr, ticks_t timeslice, word_t core)
{
    tcb->tcbSchedContext = sc_pptr;
    REFILL_NEW(tcb->tcbSchedContext, MIN_REFILLS, timeslice, 0, core);

    tcb->tcbSchedContext->scTcb = tcb;
    return true;
}

BOOT_CODE bool_t init_sched_control(cap_t root_cnode_cap, word_t num_nodes)
{
    bool_t ret = true;
    seL4_SlotPos slot_pos_before = ndks_boot.slot_pos_cur;
    /* create a sched control cap for each core */
    for (int i = 0; i < num_nodes && ret; i++) {
        ret = provide_cap(root_cnode_cap, cap_sched_control_cap_new(i));
    }

    if (!ret) {
        return false;
    }

    /* update boot info with slot region for sched control caps */
    ndks_boot.bi_frame->schedcontrol = (seL4_SlotRegion) {
        .start = slot_pos_before,
        .end = ndks_boot.slot_pos_cur
    };

    return true;
}
#endif

BOOT_CODE bool_t create_idle_thread(void)
{
    pptr_t pptr;

#ifdef ENABLE_SMP_SUPPORT
    for (int i = 0; i < CONFIG_MAX_NUM_NODES; i++) {
#endif /* ENABLE_SMP_SUPPORT */
        pptr = (pptr_t) &ksIdleThreadTCB[SMP_TERNARY(i, 0)];
        NODE_STATE_ON_CORE(ksIdleThread, i) = TCB_PTR(pptr + TCB_OFFSET);
        configureIdleThread(NODE_STATE_ON_CORE(ksIdleThread, i));
#ifdef CONFIG_DEBUG_BUILD
        setThreadName(NODE_STATE_ON_CORE(ksIdleThread, i), "idle_thread");
#endif
        SMP_COND_STATEMENT(NODE_STATE_ON_CORE(ksIdleThread, i)->tcbAffinity = i);
#ifdef CONFIG_KERNEL_MCS
        bool_t result = configure_sched_context(NODE_STATE_ON_CORE(ksIdleThread, i), SC_PTR(&ksIdleThreadSC[SMP_TERNARY(i, 0)]),
                                                usToTicks(CONFIG_BOOT_THREAD_TIME_SLICE * US_IN_MS), SMP_TERNARY(i, 0));
        SMP_COND_STATEMENT(NODE_STATE_ON_CORE(ksIdleThread, i)->tcbSchedContext->scCore = i;)
        if (!result) {
            printf("Kernel init failed: Unable to allocate sc for idle thread\n");
            return false;
        }
#endif
#ifdef ENABLE_SMP_SUPPORT
    }
#endif /* ENABLE_SMP_SUPPORT */
    return true;
}

BOOT_CODE tcb_t *create_initial_thread(cap_t root_cnode_cap, cap_t it_pd_cap, vptr_t ui_v_entry, vptr_t bi_frame_vptr,
                                       vptr_t ipcbuf_vptr, cap_t ipcbuf_cap)
{
    cap_t cap = create_rootserver_obj(seL4_TCBObject, seL4_CapInitThreadTCB, 1);
    tcb_t *tcb = TCB_PTR(cap_thread_cap_get_capTCBPtr(cap));

    /* derive a copy of the IPC buffer cap for inserting */
    deriveCap_ret_t dc_ret = deriveCap(SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapInitThreadIPCBuffer), ipcbuf_cap);
    if (dc_ret.status != EXCEPTION_NONE) {
        printf("Failed to derive copy of IPC Buffer\n");
        return NULL;
    }

    /* initialise TCB (corresponds directly to abstract specification) */
    cteInsert(
        root_cnode_cap,
        SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapInitThreadCNode),
        TCB_PTR_CTE_PTR(tcb, tcbCTable)
    );
    cteInsert(
        it_pd_cap,
        SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapInitThreadVSpace),
        TCB_PTR_CTE_PTR(tcb, tcbVTable)
    );
    cteInsert(
        dc_ret.cap,
        SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapInitThreadIPCBuffer),
        TCB_PTR_CTE_PTR(tcb, tcbBuffer)
    );
    tcb->tcbIPCBuffer = ipcbuf_vptr;

    setRegister(tcb, capRegister, bi_frame_vptr);
    setNextPC(tcb, ui_v_entry);

    /* initialise TCB */
#ifdef CONFIG_KERNEL_MCS
    if (!configure_sched_context(tcb, SC_PTR(rootserver.sc), usToTicks(CONFIG_BOOT_THREAD_TIME_SLICE * US_IN_MS), 0)) {
        return NULL;
    }
#endif

    tcb->tcbPriority = seL4_MaxPrio;
    tcb->tcbMCP = seL4_MaxPrio;
    tcb->tcbDomain = ksDomSchedule[ksDomScheduleIdx].domain;
#ifndef CONFIG_KERNEL_MCS
    setupReplyMaster(tcb);
#endif
    setThreadState(tcb, ThreadState_Running);

    ksCurDomain = ksDomSchedule[ksDomScheduleIdx].domain;
#ifdef CONFIG_KERNEL_MCS
    ksDomainTime = usToTicks(ksDomSchedule[ksDomScheduleIdx].length * US_IN_MS);
#else
    ksDomainTime = ksDomSchedule[ksDomScheduleIdx].length;
#endif
    assert(ksCurDomain < CONFIG_NUM_DOMAINS && ksDomainTime > 0);

#ifndef CONFIG_KERNEL_MCS
    SMP_COND_STATEMENT(tcb->tcbAffinity = 0);
#endif

#ifdef CONFIG_KERNEL_MCS
    cap = cap_sched_context_cap_new(SC_REF(tcb->tcbSchedContext), seL4_MinSchedContextBits);
    write_slot(SLOT_PTR(pptr_of_cap(root_cnode_cap), seL4_CapInitThreadSC), cap);
#endif
#ifdef CONFIG_DEBUG_BUILD
    setThreadName(tcb, "rootserver");
#endif
    return tcb;
}

BOOT_CODE void init_core_state(tcb_t *scheduler_action)
{
#ifdef CONFIG_HAVE_FPU
    NODE_STATE(ksActiveFPUState) = NULL;
#endif
#ifdef CONFIG_DEBUG_BUILD
    /* add initial threads to the debug queue */
    NODE_STATE(ksDebugTCBs) = NULL;
    if (scheduler_action != SchedulerAction_ResumeCurrentThread &&
        scheduler_action != SchedulerAction_ChooseNewThread) {
        tcbDebugAppend(scheduler_action);
    }
    tcbDebugAppend(NODE_STATE(ksIdleThread));
#endif
    NODE_STATE(ksSchedulerAction) = scheduler_action;
    NODE_STATE(ksCurThread) = NODE_STATE(ksIdleThread);
#ifdef CONFIG_KERNEL_MCS
    NODE_STATE(ksCurSC) = NODE_STATE(ksCurThread->tcbSchedContext);
    NODE_STATE(ksConsumed) = 0;
    NODE_STATE(ksReprogram) = true;
    NODE_STATE(ksReleaseHead) = NULL;
    NODE_STATE(ksCurTime) = getCurrentTime();
#endif
}

BOOT_CODE static bool_t provide_untyped_cap(
    cap_t      root_cnode_cap,
    bool_t     device_memory,
    pptr_t     pptr,
    word_t     size_bits,
    seL4_SlotPos first_untyped_slot
)
{
    bool_t ret;
    cap_t ut_cap;
    word_t i = ndks_boot.slot_pos_cur - first_untyped_slot;
    if (i < CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS) {
        ndks_boot.bi_frame->untypedList[i] = (seL4_UntypedDesc) {
            pptr_to_paddr((void *)pptr), size_bits, device_memory, false, {0}
        };
        ut_cap = cap_untyped_cap_new(MAX_FREE_INDEX(size_bits),
                                     device_memory, size_bits, pptr);
        ret = provide_cap(root_cnode_cap, ut_cap);
    } else {
        printf("Kernel init: Too many untyped regions for boot info\n");
        ret = true;
    }
    return ret;
}

BOOT_CODE bool_t create_untypeds_for_region(
    cap_t      root_cnode_cap,
    bool_t     device_memory,
    region_t   reg,
    seL4_SlotPos first_untyped_slot
)
{
    word_t align_bits;
    word_t size_bits;

    while (!is_reg_empty(reg)) {
        /* Determine the maximum size of the region */
        size_bits = seL4_WordBits - 1 - clzl(reg.end - reg.start);

        /* Determine the alignment of the region */
        if (reg.start != 0) {
            align_bits = ctzl(reg.start);
        } else {
            align_bits = size_bits;
        }
        /* Reduce size bits to align if needed */
        if (align_bits < size_bits) {
            size_bits = align_bits;
        }
        if (size_bits > seL4_MaxUntypedBits) {
            size_bits = seL4_MaxUntypedBits;
        }

        if (size_bits >= seL4_MinUntypedBits) {
            if (!provide_untyped_cap(root_cnode_cap, device_memory, reg.start, size_bits, first_untyped_slot)) {
                return false;
            }
        }
        reg.start += BIT(size_bits);
    }
    return true;
}

BOOT_CODE bool_t create_device_untypeds(cap_t root_cnode_cap, seL4_SlotPos slot_pos_before)
{
    paddr_t start = 0;
    for (word_t i = 0; i < ndks_boot.resv_count; i++) {
        if (start < ndks_boot.reserved[i].start) {
            region_t reg = paddr_to_pptr_reg((p_region_t) {
                start, ndks_boot.reserved[i].start
            });
            if (!create_untypeds_for_region(root_cnode_cap, true, reg, slot_pos_before)) {
                return false;
            }
        }

        start = ndks_boot.reserved[i].end;
    }

    if (start < CONFIG_PADDR_USER_DEVICE_TOP) {
        region_t reg = paddr_to_pptr_reg((p_region_t) {
            start, CONFIG_PADDR_USER_DEVICE_TOP
        });
        /*
         * The auto-generated bitfield code will get upset if the
         * end pptr is larger than the maximum pointer size for this architecture.
         */
        if (reg.end > PPTR_TOP) {
            reg.end = PPTR_TOP;
        }
        if (!create_untypeds_for_region(root_cnode_cap, true, reg, slot_pos_before)) {
            return false;
        }
    }
    return true;
}

BOOT_CODE bool_t create_kernel_untypeds(cap_t root_cnode_cap, region_t boot_mem_reuse_reg,
                                        seL4_SlotPos first_untyped_slot)
{
    word_t     i;
    word_t     pptr;
    region_t   reg;
    cap_t      cap;
    current_untyped = ndks_boot.slot_pos_cur;

    /* if boot_mem_reuse_reg is not empty, we can create UT objs from boot code/data frames */
    if (!create_untypeds_for_region(root_cnode_cap, false, boot_mem_reuse_reg, first_untyped_slot)) {
        return false;
    }

    /* convert remaining freemem into UT objects and provide the caps */
    for (i = 0; i < MAX_NUM_FREEMEM_REG; i++) {
        reg = ndks_boot.freemem[i];
        ndks_boot.freemem[i] = REG_EMPTY;
        if (!create_untypeds_for_region(root_cnode_cap, false, reg, first_untyped_slot)) {
            return false;
        }
    }

    /* provide UT used by the intitial thread cnode */
    cap = SLOT_PTR(dummy_cnode_mem, DUMMY_UNTYPED_SLOT)->cap;

    i = ndks_boot.slot_pos_cur - first_untyped_slot;
    pptr = cap_untyped_cap_get_capPtr(cap);
    ndks_boot.bi_frame->untypedList[i] = (seL4_UntypedDesc) {
        pptr_to_paddr((void *)pptr), CONFIG_ROOT_CNODE_SIZE_BITS + seL4_SlotBits, false, true, {0}
    };

    cteMove(cap,
        SLOT_PTR(dummy_cnode_mem, DUMMY_UNTYPED_SLOT),
        SLOT_PTR(rootserver.cnode, ndks_boot.slot_pos_cur));
    ndks_boot.slot_pos_cur++;

    return true;
}

BOOT_CODE void bi_finalise(void)
{
    seL4_SlotPos slot_pos_start = ndks_boot.slot_pos_cur;
    seL4_SlotPos slot_pos_end = ndks_boot.slot_pos_max;
    ndks_boot.bi_frame->empty = (seL4_SlotRegion) {
        slot_pos_start, slot_pos_end
    };
}

BOOT_CODE static inline pptr_t ceiling_kernel_window(pptr_t p)
{
    /* Adjust address if it exceeds the kernel window
     * Note that we compare physical address in case of overflow.
     */
    if (pptr_to_paddr((void *)p) > PADDR_TOP) {
        p = PPTR_TOP;
    }
    return p;
}

/* we can't declare arrays on the stack, so this is space for
 * the function below to use. */
BOOT_BSS static region_t avail_reg[MAX_NUM_FREEMEM_REG];
/**
 * Dynamically initialise the available memory on the platform.
 * A region represents an area of memory.
 */
BOOT_CODE void init_freemem(word_t n_available, const p_region_t *available,
                            word_t n_reserved, region_t *reserved,
                            v_region_t it_v_reg, word_t extra_bi_size_bits)
{
    /* Force ordering and exclusivity of reserved regions */
    for (word_t i = 0; i < n_reserved; i++) {
        UNUSED region_t *r = &reserved[i];
        /* Reserved regions must be sane, the size is allowed to be zero */
        assert(r->start <= r->end);
        if (i > 0) {
            /* regions must be ordered and must not overlap */
            assert(r->start >= reserved[i - 1].end);
        }
    }

    /* Force ordering and exclusivity of available regions */
    for (word_t i = 0; i < n_available; i++) {
        UNUSED const p_region_t *r = &available[i];
        /* Available regions must be sane and have a size greater zero */
        assert(r->start < r->end);
        if (i > 0) {
            /* regions must be ordered and must not overlap */
            assert(r->start >= available[i - 1].end);
        }
    }

    for (word_t i = 0; i < MAX_NUM_FREEMEM_REG; i++) {
        ndks_boot.freemem[i] = REG_EMPTY;
    }

    /* convert the available regions to pptrs */
    for (word_t i = 0; i < n_available; i++) {
        avail_reg[i] = paddr_to_pptr_reg(available[i]);
        avail_reg[i].end = ceiling_kernel_window(avail_reg[i].end);
        avail_reg[i].start = ceiling_kernel_window(avail_reg[i].start);
    }

    word_t a = 0;
    word_t r = 0;
    /* Now iterate through the available regions, removing any reserved regions. */
    while (a < n_available && r < n_reserved) {
        if (reserved[r].start == reserved[r].end) {
            /* reserved region is empty - skip it */
            r++;
        } else if (avail_reg[a].start >= avail_reg[a].end) {
            /* skip the entire region - it's empty now after trimming */
            a++;
        } else if (reserved[r].end <= avail_reg[a].start) {
            /* the reserved region is below the available region - skip it*/
            reserve_region(pptr_to_paddr_reg(reserved[r]));
            r++;
        } else if (reserved[r].start >= avail_reg[a].end) {
            /* the reserved region is above the available region - take the whole thing */
            insert_region(avail_reg[a]);
            a++;
        } else {
            /* the reserved region overlaps with the available region */
            if (reserved[r].start <= avail_reg[a].start) {
                /* the region overlaps with the start of the available region.
                 * trim start of the available region */
                avail_reg[a].start = MIN(avail_reg[a].end, reserved[r].end);
                reserve_region(pptr_to_paddr_reg(reserved[r]));
                r++;
            } else {
                assert(reserved[r].start < avail_reg[a].end);
                /* take the first chunk of the available region and move
                 * the start to the end of the reserved region */
                region_t m = avail_reg[a];
                m.end = reserved[r].start;
                insert_region(m);
                if (avail_reg[a].end > reserved[r].end) {
                    avail_reg[a].start = reserved[r].end;
                    reserve_region(pptr_to_paddr_reg(reserved[r]));
                    r++;
                } else {
                    a++;
                }
            }
        }
    }

    for (; r < n_reserved; r++) {
        if (reserved[r].start < reserved[r].end) {
            reserve_region(pptr_to_paddr_reg(reserved[r]));
        }
    }

    /* no more reserved regions - add the rest */
    for (; a < n_available; a++) {
        if (avail_reg[a].start < avail_reg[a].end) {
            insert_region(avail_reg[a]);
        }
    }

    /* now try to fit the root server objects into a region */
    word_t i = MAX_NUM_FREEMEM_REG - 1;
    if (!is_reg_empty(ndks_boot.freemem[i])) {
        printf("Insufficient MAX_NUM_FREEMEM_REG\n");
        halt();
    }
    /* skip any empty regions */
    for (; is_reg_empty(ndks_boot.freemem[i]) && i >= 0; i--);

    /* try to grab the last available p region to create the root server objects
     * from. If possible, retain any left over memory as an extra p region */
    word_t untyped_size = BIT(CONFIG_ROOT_CNODE_SIZE_BITS + seL4_SlotBits);
    word_t max = rootserver_max_size_bits(extra_bi_size_bits);
    word_t size = untyped_size + calculate_necessary_size(it_v_reg, extra_bi_size_bits);

    for (; i >= 0; i--) {
        word_t next = i + 1;
        pptr_t start = ROUND_DOWN(ndks_boot.freemem[i].end - size, max);
        if (start >= ndks_boot.freemem[i].start) {
            create_rootserver_objects(start, it_v_reg, extra_bi_size_bits);
            if (i < MAX_NUM_FREEMEM_REG) {
                ndks_boot.freemem[next].end = ndks_boot.freemem[i].end;
                ndks_boot.freemem[next].start = start + size;
            }
            ndks_boot.freemem[i].end = start;
            break;
        } else if (i < MAX_NUM_FREEMEM_REG) {
            ndks_boot.freemem[next] = ndks_boot.freemem[i];
        }
    }
}
