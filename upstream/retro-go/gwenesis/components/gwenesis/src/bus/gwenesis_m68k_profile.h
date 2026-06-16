#ifndef GWENESIS_M68K_PROFILE_H
#define GWENESIS_M68K_PROFILE_H

#include <stdint.h>

#ifndef GWENESIS_M68K_PROFILE
#define GWENESIS_M68K_PROFILE 0
#endif

#ifndef GWENESIS_M68K_RAM_PROFILE
#define GWENESIS_M68K_RAM_PROFILE 1
#endif

#define GWENESIS_M68K_ANY_PROFILE (GWENESIS_M68K_PROFILE || GWENESIS_M68K_RAM_PROFILE)

#define GWENESIS_M68K_ROM_PAGE_COUNT 16
#define GWENESIS_M68K_RAM_PAGE_COUNT 16
#define GWENESIS_M68K_OP_HI_COUNT 256

typedef enum
{
    GWENESIS_M68K_ROM_KIND_OPCODE = 0,
    GWENESIS_M68K_ROM_KIND_IMM,
    GWENESIS_M68K_ROM_KIND_PCREL,
    GWENESIS_M68K_ROM_KIND_DATA,
    GWENESIS_M68K_ROM_KIND_COUNT
} gwenesis_m68k_rom_kind_t;

typedef struct
{
    uint32_t rom_kind[GWENESIS_M68K_ROM_KIND_COUNT];
    uint32_t rom_page[GWENESIS_M68K_ROM_PAGE_COUNT];
    uint32_t ram_page_r[GWENESIS_M68K_RAM_PAGE_COUNT];
    uint32_t ram_page_w[GWENESIS_M68K_RAM_PAGE_COUNT];
    uint32_t op_hi[GWENESIS_M68K_OP_HI_COUNT];
} gwenesis_m68k_profile_t;

#if GWENESIS_M68K_ANY_PROFILE
extern gwenesis_m68k_profile_t gwenesis_m68k_profile;
#endif

#if GWENESIS_M68K_RAM_PROFILE && !GWENESIS_M68K_PROFILE
extern volatile uint8_t gwenesis_m68k_ram_profile_enabled;
#endif

#if GWENESIS_M68K_PROFILE
#define GWENESIS_M68K_ROM_KIND_INC(kind) \
    (gwenesis_m68k_profile.rom_kind[(kind)]++)
#define GWENESIS_M68K_ROM_KIND_INC_IF_ROM(address, kind) \
    do { \
        if ((((unsigned int)(address)) & 0x800000U) == 0) \
            GWENESIS_M68K_ROM_KIND_INC(kind); \
    } while (0)
#define GWENESIS_M68K_ROM_PAGE_INC(address) \
    (gwenesis_m68k_profile.rom_page[(((unsigned int)(address)) >> 16) & 0x0f]++)
#define GWENESIS_M68K_OP_HI_INC(opcode) \
    (gwenesis_m68k_profile.op_hi[(((unsigned int)(opcode)) >> 8) & 0xff]++)
#else
#define GWENESIS_M68K_ROM_KIND_INC(kind) ((void)0)
#define GWENESIS_M68K_ROM_KIND_INC_IF_ROM(address, kind) ((void)0)
#define GWENESIS_M68K_ROM_PAGE_INC(address) ((void)0)
#define GWENESIS_M68K_OP_HI_INC(opcode) ((void)0)
#endif

#if GWENESIS_M68K_PROFILE
#define GWENESIS_M68K_RAM_PAGE_R_INC(address) \
    (gwenesis_m68k_profile.ram_page_r[(((unsigned int)(address)) >> 12) & 0x0f]++)
#define GWENESIS_M68K_RAM_PAGE_W_INC(address) \
    (gwenesis_m68k_profile.ram_page_w[(((unsigned int)(address)) >> 12) & 0x0f]++)
#elif GWENESIS_M68K_RAM_PROFILE
#define GWENESIS_M68K_RAM_PAGE_R_INC(address) \
    do { \
        if (__builtin_expect(gwenesis_m68k_ram_profile_enabled != 0, 0)) \
            gwenesis_m68k_profile.ram_page_r[(((unsigned int)(address)) >> 12) & 0x0f]++; \
    } while (0)
#define GWENESIS_M68K_RAM_PAGE_W_INC(address) \
    do { \
        if (__builtin_expect(gwenesis_m68k_ram_profile_enabled != 0, 0)) \
            gwenesis_m68k_profile.ram_page_w[(((unsigned int)(address)) >> 12) & 0x0f]++; \
    } while (0)
#else
#define GWENESIS_M68K_RAM_PAGE_R_INC(address) ((void)0)
#define GWENESIS_M68K_RAM_PAGE_W_INC(address) ((void)0)
#endif

#endif
