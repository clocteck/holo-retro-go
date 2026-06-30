#ifndef GWENESIS_M68K_PROFILE_H
#define GWENESIS_M68K_PROFILE_H

#include <stdint.h>

#define GWENESIS_M68K_PROFILE_MODE_OFF 0
#define GWENESIS_M68K_PROFILE_MODE_RAM 1
#define GWENESIS_M68K_PROFILE_MODE_FULL 2

#ifndef GWENESIS_M68K_PROFILE_MODE
#define GWENESIS_M68K_PROFILE_MODE GWENESIS_M68K_PROFILE_MODE_OFF
#endif

#ifndef GWENESIS_M68K_RAM_SAMPLE_DEFAULT
#define GWENESIS_M68K_RAM_SAMPLE_DEFAULT 0
#endif

#if GWENESIS_M68K_PROFILE_MODE < GWENESIS_M68K_PROFILE_MODE_OFF || \
    GWENESIS_M68K_PROFILE_MODE > GWENESIS_M68K_PROFILE_MODE_FULL
#error "Unsupported GWENESIS_M68K_PROFILE_MODE"
#endif

#define GWENESIS_M68K_PROFILE \
    (GWENESIS_M68K_PROFILE_MODE == GWENESIS_M68K_PROFILE_MODE_FULL)
#define GWENESIS_M68K_RAM_PROFILE \
    (GWENESIS_M68K_PROFILE_MODE >= GWENESIS_M68K_PROFILE_MODE_RAM)

#define GWENESIS_M68K_ANY_PROFILE (GWENESIS_M68K_PROFILE || GWENESIS_M68K_RAM_PROFILE)

#define GWENESIS_M68K_ROM_PAGE_COUNT 16
#define GWENESIS_M68K_RAM_PAGE_COUNT 16
#define GWENESIS_M68K_OP_HI_COUNT 256
#define GWENESIS_M68K_OP_SLOT_COUNT 512

typedef enum
{
    GWENESIS_M68K_ROM_KIND_OPCODE = 0,
    GWENESIS_M68K_ROM_KIND_IMM,
    GWENESIS_M68K_ROM_KIND_PCREL,
    GWENESIS_M68K_ROM_KIND_DATA,
    GWENESIS_M68K_ROM_KIND_COUNT
} gwenesis_m68k_rom_kind_t;

typedef enum
{
    GWENESIS_M68K_EA_NONE = 0,
    GWENESIS_M68K_EA_DREG,
    GWENESIS_M68K_EA_AREG,
    GWENESIS_M68K_EA_AI,
    GWENESIS_M68K_EA_PI,
    GWENESIS_M68K_EA_PD,
    GWENESIS_M68K_EA_DI,
    GWENESIS_M68K_EA_IX,
    GWENESIS_M68K_EA_AW,
    GWENESIS_M68K_EA_AL,
    GWENESIS_M68K_EA_PCDI,
    GWENESIS_M68K_EA_PCIX,
    GWENESIS_M68K_EA_IMM,
    GWENESIS_M68K_EA_BRANCH,
    GWENESIS_M68K_EA_DBCC,
    GWENESIS_M68K_EA_OTHER,
    GWENESIS_M68K_EA_COUNT
} gwenesis_m68k_ea_kind_t;

typedef enum
{
    GWENESIS_M68K_MEM_ROM = 0,
    GWENESIS_M68K_MEM_RAM_R,
    GWENESIS_M68K_MEM_RAM_W,
    GWENESIS_M68K_MEM_SRAM,
    GWENESIS_M68K_MEM_VDP,
    GWENESIS_M68K_MEM_IO,
    GWENESIS_M68K_MEM_Z80,
    GWENESIS_M68K_MEM_YM2612,
    GWENESIS_M68K_MEM_PSG,
    GWENESIS_M68K_MEM_TMSS,
    GWENESIS_M68K_MEM_OTHER,
    GWENESIS_M68K_MEM_COUNT
} gwenesis_m68k_mem_kind_t;

typedef struct
{
    uint32_t rom_kind[GWENESIS_M68K_ROM_KIND_COUNT];
    uint32_t rom_page[GWENESIS_M68K_ROM_PAGE_COUNT];
    uint32_t ram_page_r[GWENESIS_M68K_RAM_PAGE_COUNT];
    uint32_t ram_page_w[GWENESIS_M68K_RAM_PAGE_COUNT];
    uint32_t op_hi[GWENESIS_M68K_OP_HI_COUNT];
#if GWENESIS_M68K_PROFILE
    uint16_t op_slot_code[GWENESIS_M68K_OP_SLOT_COUNT];
    uint32_t op_slot_count[GWENESIS_M68K_OP_SLOT_COUNT];
    uint32_t op_slot_overflow;
    uint32_t ea_low[GWENESIS_M68K_EA_COUNT];
    uint32_t ea_dst[GWENESIS_M68K_EA_COUNT];
    uint32_t mem_kind[GWENESIS_M68K_MEM_COUNT];
#endif
} gwenesis_m68k_profile_t;

#if GWENESIS_M68K_ANY_PROFILE
extern gwenesis_m68k_profile_t gwenesis_m68k_profile;
#endif

#if GWENESIS_M68K_RAM_PROFILE && !GWENESIS_M68K_PROFILE
extern volatile uint8_t gwenesis_m68k_ram_profile_enabled;
#endif

#if GWENESIS_M68K_PROFILE
static inline unsigned int gwenesis_m68k_profile_opcode_slot(unsigned int opcode)
{
    return ((opcode * 2654435761u) >> 23) & (GWENESIS_M68K_OP_SLOT_COUNT - 1);
}

static inline void gwenesis_m68k_profile_opcode_inc(unsigned int opcode)
{
    const uint16_t code = (uint16_t)opcode;
    unsigned int slot = gwenesis_m68k_profile_opcode_slot(code);

    for (unsigned int i = 0; i < GWENESIS_M68K_OP_SLOT_COUNT; ++i)
    {
        if (gwenesis_m68k_profile.op_slot_count[slot] == 0)
        {
            gwenesis_m68k_profile.op_slot_code[slot] = code;
            gwenesis_m68k_profile.op_slot_count[slot] = 1;
            return;
        }
        if (gwenesis_m68k_profile.op_slot_code[slot] == code)
        {
            gwenesis_m68k_profile.op_slot_count[slot]++;
            return;
        }
        slot = (slot + 1) & (GWENESIS_M68K_OP_SLOT_COUNT - 1);
    }

    gwenesis_m68k_profile.op_slot_overflow++;
}

static inline gwenesis_m68k_ea_kind_t gwenesis_m68k_profile_ea_kind(unsigned int ea)
{
    const unsigned int mode = (ea >> 3) & 7;
    const unsigned int reg = ea & 7;

    switch (mode)
    {
    case 0:
        return GWENESIS_M68K_EA_DREG;
    case 1:
        return GWENESIS_M68K_EA_AREG;
    case 2:
        return GWENESIS_M68K_EA_AI;
    case 3:
        return GWENESIS_M68K_EA_PI;
    case 4:
        return GWENESIS_M68K_EA_PD;
    case 5:
        return GWENESIS_M68K_EA_DI;
    case 6:
        return GWENESIS_M68K_EA_IX;
    case 7:
        switch (reg)
        {
        case 0:
            return GWENESIS_M68K_EA_AW;
        case 1:
            return GWENESIS_M68K_EA_AL;
        case 2:
            return GWENESIS_M68K_EA_PCDI;
        case 3:
            return GWENESIS_M68K_EA_PCIX;
        case 4:
            return GWENESIS_M68K_EA_IMM;
        default:
            return GWENESIS_M68K_EA_OTHER;
        }
    default:
        return GWENESIS_M68K_EA_OTHER;
    }
}

static inline void gwenesis_m68k_profile_opcode_ea(unsigned int opcode)
{
    const unsigned int top = (opcode >> 12) & 0x0f;

    if ((opcode & 0xf000) == 0x6000)
    {
        gwenesis_m68k_profile.ea_low[GWENESIS_M68K_EA_BRANCH]++;
        return;
    }

    if ((opcode & 0xf0f8) == 0x50c8)
    {
        gwenesis_m68k_profile.ea_low[GWENESIS_M68K_EA_DBCC]++;
        return;
    }

    gwenesis_m68k_profile.ea_low[gwenesis_m68k_profile_ea_kind(opcode & 0x3f)]++;

    if (top >= 1 && top <= 3)
    {
        const unsigned int dst_ea = ((opcode >> 3) & 0x38) | ((opcode >> 9) & 7);
        gwenesis_m68k_profile.ea_dst[gwenesis_m68k_profile_ea_kind(dst_ea)]++;
    }
}

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
#define GWENESIS_M68K_OP_INC(opcode) \
    do { \
        GWENESIS_M68K_OP_HI_INC(opcode); \
        gwenesis_m68k_profile_opcode_inc(opcode); \
        gwenesis_m68k_profile_opcode_ea(opcode); \
    } while (0)
#define GWENESIS_M68K_MEM_KIND_INC(kind) \
    (gwenesis_m68k_profile.mem_kind[(kind)]++)
#else
#define GWENESIS_M68K_ROM_KIND_INC(kind) ((void)0)
#define GWENESIS_M68K_ROM_KIND_INC_IF_ROM(address, kind) ((void)0)
#define GWENESIS_M68K_ROM_PAGE_INC(address) ((void)0)
#define GWENESIS_M68K_OP_HI_INC(opcode) ((void)0)
#define GWENESIS_M68K_OP_INC(opcode) ((void)0)
#define GWENESIS_M68K_MEM_KIND_INC(kind) ((void)0)
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
