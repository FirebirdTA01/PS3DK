/* sys/fixed_addr.h - the fixed regions of an LV2 process's effective
 * address space. */
#ifndef PS3TC_SYS_FIXED_ADDR_H
#define PS3TC_SYS_FIXED_ADDR_H

#define TEXT_SEGMENT_BASE_ADDR                    0x00010000UL

#define OVERLAY_PPU_SPU_SHARED_SEGMENT_BASE_ADDR  0x30000000UL
#define FIXEDADDR_PRX_BASE_ADDR                   0x30000000UL
#define FIXEDADDR_PRX_BASE_ADDR_SIZE              0x10000000UL

#define MMAPPER_FIXED_AREA_BASE_ADDR              0xB0000000UL
#define MMAPPER_FIXED_AREA_SIZE                   0x10000000UL

#define RSX_FB_BASE_ADDR                          0xC0000000UL

/* raw SPU n at RAW_SPU_BASE_ADDR + n * RAW_SPU_OFFSET (<sys/raw_spu.h>) */
#define RAW_SPU_BASE_ADDR                         0xE0000000UL

#define SPU_THREAD_BASE_ADDR                      0xF0000000UL

#endif /* PS3TC_SYS_FIXED_ADDR_H */
