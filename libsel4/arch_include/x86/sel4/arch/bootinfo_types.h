/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(D61_BSD)
 */

#ifndef __LIBSEL4_ARCH_BOOTINFO_TYPES_H
#define __LIBSEL4_ARCH_BOOTINFO_TYPES_H

typedef struct seL4_VBEInfoBlock {
    char        signature[4];
    seL4_Uint16 version;
    seL4_Uint32 oemStringPtr;
    seL4_Uint32 capabilities;
    seL4_Uint32 modeListPtr;
    seL4_Uint16 totalMemory;
    seL4_Uint16 oemSoftwareRev;
    seL4_Uint32 oemVendorNamePtr;
    seL4_Uint32 oemProductNamePtr;
    seL4_Uint32 oemProductRevPtr;
    seL4_Uint8  reserved[222];
    seL4_Uint8  oemData[256];
} SEL4_PACKED seL4_VBEInfoBlock_t;

typedef struct seL4_VBEModeInfoBlock {
    /* All VBE revisions */
    seL4_Uint16 modeAttr;
    seL4_Uint8  winAAttr;
    seL4_Uint8  winBAttr;
    seL4_Uint16 winGranularity;
    seL4_Uint16 winSize;
    seL4_Uint16 winASeg;
    seL4_Uint16 winBSeg;
    seL4_Uint32 winFuncPtr;
    seL4_Uint16 bytesPerScanLine;

    /* VBE 1.2+ */
    seL4_Uint16 xRes;
    seL4_Uint16 yRes;
    seL4_Uint8  xCharSize;
    seL4_Uint8  yCharSize;
    seL4_Uint8  planes;
    seL4_Uint8  bitsPerPixel;
    seL4_Uint8  banks;
    seL4_Uint8  memoryModel;
    seL4_Uint8  bankSize;
    seL4_Uint8  imagePages;
    seL4_Uint8  reserved1;

    seL4_Uint8  redLen;
    seL4_Uint8  redOff;
    seL4_Uint8  greenLen;
    seL4_Uint8  greenOff;
    seL4_Uint8  blueLen;
    seL4_Uint8  blueOff;
    seL4_Uint8  rsvdLen;
    seL4_Uint8  rsvdOff;
    seL4_Uint8  directColorInfo;  /* direct color mode attributes */

    /* VBE 2.0+ */
    seL4_Uint32 physBasePtr;
    seL4_Uint8  reserved2[6];

    /* VBE 3.0+ */
    seL4_Uint16 linBytesPerScanLine;
    seL4_Uint8  bnkImagePages;
    seL4_Uint8  linImagePages;
    seL4_Uint8  linRedLen;
    seL4_Uint8  linRedOff;
    seL4_Uint8  linGreenLen;
    seL4_Uint8  linGreenOff;
    seL4_Uint8  linBlueLen;
    seL4_Uint8  linBlueOff;
    seL4_Uint8  linRsvdLen;
    seL4_Uint8  linRsvdOff;
    seL4_Uint32 maxPixelClock;
    seL4_Uint16 modeId;
    seL4_Uint8  depth;

    seL4_Uint8 reserved3[187];
} SEL4_PACKED seL4_VBEModeInfoBlock_t;

typedef struct _seL4_X86_BootInfo_VBE {
    seL4_BootInfoHeader header;
    seL4_VBEInfoBlock_t vbeInfoBlock;
    seL4_VBEModeInfoBlock_t vbeModeInfoBlock;
    seL4_Uint32 vbeMode;
    seL4_Uint32 vbeInterfaceSeg;
    seL4_Uint32 vbeInterfaceOff;
    seL4_Uint32 vbeInterfaceLen;
} SEL4_PACKED seL4_X86_BootInfo_VBE;

#endif // __LIBSEL4_ARCH_BOOTINFO_TYPES_H