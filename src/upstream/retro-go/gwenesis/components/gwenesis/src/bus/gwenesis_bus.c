/*
Gwenesis : Genesis & megadrive Emulator.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

__author__ = "bzhxx"
__contact__ = "https://github.com/bzhxx"
__license__ = "GPLv3"

*/
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#if defined(RETRO_GO)
#include <rg_system.h>
#endif

#include "m68k.h"

#include "ym2612.h"
#include "z80inst.h"
#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_vdp.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_savestate.h"

#define GWENESIS_HOT
#if defined(RG_TARGET_HOLO_DYNMOD)
#define GWENESIS_IRAM_HOT __attribute__((section(".mod_iram"), noinline, used))
#else
#define GWENESIS_IRAM_HOT GWENESIS_HOT
#endif

#if GNW_TARGET_MARIO !=0 || GNW_TARGET_ZELDA!=0
  #pragma GCC optimize("Ofast")
#endif

#define BUS_DISABLE_LOGGING 1
#define GWENESIS_M68K_RAM_MEM MEM_SLOW
#define GWENESIS_Z80_RAM_MEM MEM_FAST
#define GWENESIS_M68K_RAM_PAGE_SIZE 0x1000
#define GWENESIS_M68K_RAM_FAST_SLOTS 6
#define GWENESIS_M68K_RAM_DEFAULT_PAGE_FIRST 0x0a
#define GWENESIS_M68K_RAM_CACHE_EMPTY 0xff
#define GWENESIS_M68K_RAM_CACHE_MIN_SWAP_EPOCHS 2

#if GWENESIS_M68K_ANY_PROFILE
gwenesis_m68k_profile_t gwenesis_m68k_profile;
#endif
#if GWENESIS_M68K_RAM_PROFILE && !GWENESIS_M68K_PROFILE
volatile uint8_t gwenesis_m68k_ram_profile_enabled = GWENESIS_M68K_RAM_SAMPLE_DEFAULT;
#endif

#if !BUS_DISABLE_LOGGING
#include <stdarg.h>
void bus_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s] vc:%04x hc:%04x hv:%04x ", frame_counter, scan_line, subs,gwenesis_vdp_vcounter(),gwenesis_vdp_hcounter(),gwenesis_vdp_hvcounter());

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
	#define bus_log(...)  do {} while(0)
#endif

// Setup M68k memories ROM & RAM
#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

#include "rom_manager.h"
unsigned char *M68K_RAM=(void *)(uint32_t)(0); // 68K RAM 
#else

unsigned char *ROM_DATA; // 68K Main Program (uncompressed)
unsigned int ROM_MASK = MAX_ROM_SIZE - 1;
unsigned int ROM_SIZE;
unsigned char *CART_SRAM_DATA;
unsigned int CART_SRAM_START = MAX_ROM_SIZE;
unsigned int CART_SRAM_END = 0;
static bool CART_SRAM_PARITY_ONLY;
static unsigned int CART_SRAM_PARITY;
static size_t CART_SRAM_SIZE;
#if defined(RETRO_GO)
unsigned char *M68K_RAM; // 68K RAM

typedef struct
{
    unsigned char *rom_page_ptr[GWENESIS_ROM_PAGE_COUNT];
    unsigned char *page_ptr[GWENESIS_M68K_RAM_PAGE_COUNT];
    unsigned char *fast_slot[GWENESIS_M68K_RAM_FAST_SLOTS];
    uint32_t cache_score[GWENESIS_M68K_RAM_PAGE_COUNT];
    uint32_t cache_swaps;
    uint32_t cache_update_epoch;
    uint32_t cache_last_swap_epoch;
    uint8_t fast_page[GWENESIS_M68K_RAM_FAST_SLOTS];
} gwenesis_m68k_ram_cache_state_t;

unsigned char **ROM_PAGE_PTR;
unsigned char **M68K_RAM_PAGE_PTR;
static gwenesis_m68k_ram_cache_state_t *m68k_ram_cache_state;

#define m68k_ram_fast_slot (m68k_ram_cache_state->fast_slot)
#define m68k_ram_fast_page (m68k_ram_cache_state->fast_page)
#define m68k_ram_cache_score (m68k_ram_cache_state->cache_score)
#define m68k_ram_cache_swaps (m68k_ram_cache_state->cache_swaps)
#define m68k_ram_cache_update_epoch (m68k_ram_cache_state->cache_update_epoch)
#define m68k_ram_cache_last_swap_epoch (m68k_ram_cache_state->cache_last_swap_epoch)
#else
unsigned char M68K_RAM[MAX_RAM_SIZE];    // 68K RAM
#endif
#endif


// Setup Z80 Memory
#if defined(RETRO_GO)
unsigned char *ZRAM; // Z80 RAM
#else
unsigned char ZRAM[MAX_Z80_RAM_SIZE]; // Z80 RAM
#endif
unsigned char TMSS[0x4];
extern unsigned short gwenesis_vdp_status;

// TMSS
int tmss_state = 0;
int tmss_count = 0;

#if defined(RETRO_GO)
static void gwenesis_m68k_ram_cache_reset_state(gwenesis_m68k_ram_cache_state_t *state)
{
    if (!state)
        return;

    memset(state, 0, sizeof(*state));
    for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
        state->fast_page[slot] = GWENESIS_M68K_RAM_CACHE_EMPTY;
}

static bool gwenesis_m68k_ram_cache_alloc_state(void)
{
    if (m68k_ram_cache_state)
        return true;

    gwenesis_m68k_ram_cache_state_t *state =
        rg_alloc(sizeof(*state), MEM_FAST | MEM_NOPANIC);
    if (state && PTR_IN_SPIRAM(state))
    {
        free(state);
        state = NULL;
    }
    if (!state)
    {
        ROM_PAGE_PTR = NULL;
        M68K_RAM_PAGE_PTR = NULL;
        return false;
    }

    gwenesis_m68k_ram_cache_reset_state(state);
    m68k_ram_cache_state = state;
    ROM_PAGE_PTR = state->rom_page_ptr;
    M68K_RAM_PAGE_PTR = state->page_ptr;
    return true;
}

static void gwenesis_m68k_ram_cache_free_state(void)
{
    free(m68k_ram_cache_state);
    m68k_ram_cache_state = NULL;
    ROM_PAGE_PTR = NULL;
    M68K_RAM_PAGE_PTR = NULL;
}

static void gwenesis_rom_page_table_clear(void)
{
    if (ROM_PAGE_PTR)
        memset(ROM_PAGE_PTR, 0, sizeof(*ROM_PAGE_PTR) * GWENESIS_ROM_PAGE_COUNT);
}

static unsigned int gwenesis_rom_page_table_build(size_t size)
{
    gwenesis_rom_page_table_clear();

    if (!ROM_PAGE_PTR || !ROM_DATA || size == 0)
        return 0;

    unsigned int direct_pages = 0;

    for (unsigned int page = 0; page < GWENESIS_ROM_PAGE_COUNT; ++page)
    {
        const unsigned int page_address = page << GWENESIS_ROM_PAGE_SHIFT;
        size_t mapped_offset = page_address & ROM_MASK;

        if (mapped_offset >= size)
            mapped_offset %= size;

        if (mapped_offset + GWENESIS_ROM_PAGE_SIZE <= size)
        {
            ROM_PAGE_PTR[page] = ROM_DATA + mapped_offset;
            direct_pages++;
        }
    }

    return direct_pages;
}

static void gwenesis_m68k_ram_cache_bind_slow_pages(void)
{
    if (!m68k_ram_cache_state)
        return;

    for (unsigned int page = 0; page < GWENESIS_M68K_RAM_PAGE_COUNT; ++page)
        M68K_RAM_PAGE_PTR[page] = M68K_RAM ? (M68K_RAM + page * GWENESIS_M68K_RAM_PAGE_SIZE) : NULL;
}

static bool gwenesis_m68k_ram_cache_page_loaded(uint8_t page)
{
    if (!m68k_ram_cache_state)
        return false;

    for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
    {
        if (m68k_ram_fast_page[slot] == page)
            return true;
    }
    return false;
}

static void gwenesis_m68k_ram_cache_sync_slot(unsigned int slot)
{
    if (!m68k_ram_cache_state || !M68K_RAM ||
        slot >= GWENESIS_M68K_RAM_FAST_SLOTS || !m68k_ram_fast_slot[slot])
        return;

    uint8_t page = m68k_ram_fast_page[slot];
    if (page >= GWENESIS_M68K_RAM_PAGE_COUNT)
        return;

    memcpy(M68K_RAM + page * GWENESIS_M68K_RAM_PAGE_SIZE,
           m68k_ram_fast_slot[slot],
           GWENESIS_M68K_RAM_PAGE_SIZE);
}

static void gwenesis_m68k_ram_cache_sync_all(void)
{
    for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
        gwenesis_m68k_ram_cache_sync_slot(slot);
}

static void gwenesis_m68k_ram_cache_reload_slots(void)
{
    if (!m68k_ram_cache_state)
        return;

    gwenesis_m68k_ram_cache_bind_slow_pages();

    for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
    {
        uint8_t page = m68k_ram_fast_page[slot];
        if (!M68K_RAM || !m68k_ram_fast_slot[slot] || page >= GWENESIS_M68K_RAM_PAGE_COUNT)
            continue;

        memcpy(m68k_ram_fast_slot[slot],
               M68K_RAM + page * GWENESIS_M68K_RAM_PAGE_SIZE,
               GWENESIS_M68K_RAM_PAGE_SIZE);
        M68K_RAM_PAGE_PTR[page] = m68k_ram_fast_slot[slot];
    }
}

static bool gwenesis_m68k_ram_cache_install(unsigned int slot, uint8_t page)
{
    if (!m68k_ram_cache_state || !M68K_RAM || slot >= GWENESIS_M68K_RAM_FAST_SLOTS ||
        !m68k_ram_fast_slot[slot] || page >= GWENESIS_M68K_RAM_PAGE_COUNT)
        return false;

    if (m68k_ram_fast_page[slot] == page)
        return false;
    if (gwenesis_m68k_ram_cache_page_loaded(page))
        return false;

    uint8_t old_page = m68k_ram_fast_page[slot];
    if (old_page < GWENESIS_M68K_RAM_PAGE_COUNT)
    {
        gwenesis_m68k_ram_cache_sync_slot(slot);
        M68K_RAM_PAGE_PTR[old_page] = M68K_RAM + old_page * GWENESIS_M68K_RAM_PAGE_SIZE;
    }

    memcpy(m68k_ram_fast_slot[slot],
           M68K_RAM + page * GWENESIS_M68K_RAM_PAGE_SIZE,
           GWENESIS_M68K_RAM_PAGE_SIZE);
    m68k_ram_fast_page[slot] = page;
    M68K_RAM_PAGE_PTR[page] = m68k_ram_fast_slot[slot];
    ++m68k_ram_cache_swaps;
    return true;
}

static void gwenesis_m68k_ram_cache_reset_to_defaults(void)
{
    if (!m68k_ram_cache_state)
        return;

    gwenesis_m68k_ram_cache_bind_slow_pages();

    for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
        m68k_ram_fast_page[slot] = GWENESIS_M68K_RAM_CACHE_EMPTY;

    memset(m68k_ram_cache_score, 0, sizeof(m68k_ram_cache_score));
    m68k_ram_cache_swaps = 0;
    m68k_ram_cache_update_epoch = 0;
    m68k_ram_cache_last_swap_epoch = 0;
#if GWENESIS_M68K_RAM_PROFILE && !GWENESIS_M68K_PROFILE
    gwenesis_m68k_ram_profile_enabled = GWENESIS_M68K_RAM_SAMPLE_DEFAULT;
#endif

    for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
        gwenesis_m68k_ram_cache_install(
            slot, (uint8_t)(GWENESIS_M68K_RAM_DEFAULT_PAGE_FIRST + slot));
    m68k_ram_cache_swaps = 0;
}

static void gwenesis_m68k_ram_cache_clear(void)
{
    if (M68K_RAM)
        memset(M68K_RAM, 0, MAX_RAM_SIZE);

    if (!m68k_ram_cache_state)
        return;

    for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
    {
        if (m68k_ram_fast_slot[slot])
            memset(m68k_ram_fast_slot[slot], 0, GWENESIS_M68K_RAM_PAGE_SIZE);
    }

    gwenesis_m68k_ram_cache_reset_to_defaults();
}

static void gwenesis_m68k_ram_cache_alloc_slots(void)
{
    if (!m68k_ram_cache_state)
        return;

    for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
    {
        if (m68k_ram_fast_slot[slot])
            continue;

        void *ptr = rg_alloc(GWENESIS_M68K_RAM_PAGE_SIZE, MEM_FAST | MEM_NOPANIC);
        if (ptr && PTR_IN_SPIRAM(ptr))
        {
            free(ptr);
            ptr = NULL;
        }
        m68k_ram_fast_slot[slot] = ptr;
    }
}

static void gwenesis_m68k_ram_clear(void)
{
    gwenesis_m68k_ram_cache_clear();
}

static unsigned char *gwenesis_bus_alloc_zram(void)
{
#if defined(ESP_PLATFORM)
    const size_t internal_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    unsigned char *ptr = rg_alloc(MAX_Z80_RAM_SIZE, GWENESIS_Z80_RAM_MEM | MEM_NOPANIC);
#if defined(ESP_PLATFORM)
    const size_t internal_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif

    if (ptr)
    {
#if defined(ESP_PLATFORM)
        const char *location = PTR_IN_SPIRAM(ptr) ? "SPIRAM" : "internal";
        printf("[info] Genesis Z80 RAM: %s ptr=%p size=%u requested=MEM_FAST largest_internal %u->%u\n",
               location, ptr, (unsigned)MAX_Z80_RAM_SIZE,
               (unsigned)internal_before, (unsigned)internal_after);
        RG_LOGI("Genesis Z80 RAM: %s ptr=%p size=%u requested=MEM_FAST largest_internal %u->%u",
                location, ptr, (unsigned)MAX_Z80_RAM_SIZE,
                (unsigned)internal_before, (unsigned)internal_after);
#else
        RG_LOGI("Genesis Z80 RAM: ptr=%p size=%u", ptr, (unsigned)MAX_Z80_RAM_SIZE);
#endif
    }
    else
    {
#if defined(ESP_PLATFORM)
        RG_LOGW("Genesis Z80 RAM: internal allocation failed size=%u largest_internal_before=%u",
                (unsigned)MAX_Z80_RAM_SIZE, (unsigned)internal_before);
#else
        RG_LOGW("Genesis Z80 RAM: allocation failed size=%u", (unsigned)MAX_Z80_RAM_SIZE);
#endif
    }

    return ptr;
}
#else
static void gwenesis_m68k_ram_clear(void)
{
    memset(M68K_RAM, 0, MAX_RAM_SIZE);
}
#endif

bool gwenesis_bus_init_fast_ram(void)
{
#if defined(RETRO_GO)
    bool ram_was_missing = !M68K_RAM;
    bool cache_state_was_missing = !m68k_ram_cache_state;
    if (!gwenesis_m68k_ram_cache_alloc_state())
        return false;
    if (!M68K_RAM)
        M68K_RAM = rg_alloc(MAX_RAM_SIZE, GWENESIS_M68K_RAM_MEM | MEM_NOPANIC);
    if (!ZRAM)
        ZRAM = gwenesis_bus_alloc_zram();
    if (ZRAM)
        z80_set_memory(ZRAM);
    if (M68K_RAM && (ram_was_missing || cache_state_was_missing))
    {
        gwenesis_m68k_ram_cache_bind_slow_pages();
        gwenesis_m68k_ram_cache_alloc_slots();
        gwenesis_m68k_ram_cache_reset_to_defaults();
    }
    else if (M68K_RAM)
    {
        gwenesis_m68k_ram_cache_alloc_slots();
    }
#endif
    return M68K_RAM != NULL && ZRAM != NULL;
}

void gwenesis_bus_deinit_fast_ram(void)
{
#if defined(RETRO_GO)
    gwenesis_m68k_ram_cache_sync_all();
    z80_set_memory(NULL);
    if (m68k_ram_cache_state)
    {
        for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
        {
            free(m68k_ram_fast_slot[slot]);
            m68k_ram_fast_slot[slot] = NULL;
            m68k_ram_fast_page[slot] = GWENESIS_M68K_RAM_CACHE_EMPTY;
        }
    }
    free(M68K_RAM);
    free(ZRAM);
    M68K_RAM = NULL;
    ZRAM = NULL;
    gwenesis_m68k_ram_cache_bind_slow_pages();
    gwenesis_m68k_ram_cache_free_state();
#endif
}

/******************************************************************************
 *
 *   Load a Sega Genesis Cartridge into CPU Memory
 *
 ******************************************************************************/


#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

void load_cartridge()
{
    if (!gwenesis_bus_init_fast_ram())
        return;

    // Clear all volatile memory
    gwenesis_m68k_ram_clear();
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    // Set Z80 Memory as Z80_RAM
    z80_set_memory(ZRAM);

    z80_pulse_reset();

    set_region();

}
#else

static size_t gwenesis_rom_mirror_size(size_t size)
{
    size_t mirror_size = 0x10000;

    while (mirror_size < size && mirror_size < MAX_ROM_SIZE)
        mirror_size <<= 1;

    return mirror_size > MAX_ROM_SIZE ? MAX_ROM_SIZE : mirror_size;
}

static unsigned int gwenesis_rom_read_be32(size_t offset, size_t size)
{
    if (offset + 3 >= size)
        return 0;

    return ((unsigned int)ROM_DATA[offset + 0] << 24) |
           ((unsigned int)ROM_DATA[offset + 1] << 16) |
           ((unsigned int)ROM_DATA[offset + 2] << 8) |
           ((unsigned int)ROM_DATA[offset + 3]);
}

static void gwenesis_sram_reset(void)
{
    free(CART_SRAM_DATA);
    CART_SRAM_DATA = NULL;
    CART_SRAM_START = MAX_ROM_SIZE;
    CART_SRAM_END = 0;
    CART_SRAM_PARITY_ONLY = false;
    CART_SRAM_PARITY = 0;
    CART_SRAM_SIZE = 0;
}

static void gwenesis_sram_configure(size_t rom_size)
{
    gwenesis_sram_reset();

    if (!ROM_DATA || rom_size < 0x1BC || ROM_DATA[0x1B0] != 'R' || ROM_DATA[0x1B1] != 'A')
        return;

    unsigned int start = gwenesis_rom_read_be32(0x1B4, rom_size) & 0xFFFFFF;
    unsigned int end = gwenesis_rom_read_be32(0x1B8, rom_size) & 0xFFFFFF;

    if (start >= MAX_ROM_SIZE || end < start)
        return;
    if (end >= MAX_ROM_SIZE)
        end = MAX_ROM_SIZE - 1;

    const bool parity_only = ((start ^ end) & 1) == 0;
    size_t sram_size = parity_only ? (((end - start) >> 1) + 1) : ((end - start) + 1);

    if (sram_size == 0)
        return;
    if (sram_size > 0x20000)
        sram_size = 0x20000;

#if defined(RETRO_GO)
    CART_SRAM_DATA = rg_alloc(sram_size, MEM_SLOW | MEM_NOPANIC);
#else
    CART_SRAM_DATA = malloc(sram_size);
#endif
    if (!CART_SRAM_DATA)
        return;

    memset(CART_SRAM_DATA, 0xff, sram_size);
    CART_SRAM_START = start;
    CART_SRAM_END = end;
    CART_SRAM_PARITY_ONLY = parity_only;
    CART_SRAM_PARITY = start & 1;
    CART_SRAM_SIZE = sram_size;

    printf("Genesis SRAM start=0x%06x end=0x%06x size=%u parity=%s\n",
           CART_SRAM_START, CART_SRAM_END, (unsigned)CART_SRAM_SIZE,
           CART_SRAM_PARITY_ONLY ? (CART_SRAM_PARITY ? "odd" : "even") : "both");
}

static inline bool gwenesis_sram_contains(unsigned int address)
{
    address &= 0xFFFFFF;
    if (!CART_SRAM_DATA || address < CART_SRAM_START || address > CART_SRAM_END)
        return false;
    return !CART_SRAM_PARITY_ONLY || ((address & 1) == CART_SRAM_PARITY);
}

static inline bool gwenesis_sram_overlaps(unsigned int address, unsigned int width)
{
    address &= 0xFFFFFF;
    return CART_SRAM_DATA && address <= CART_SRAM_END && (address + width - 1) >= CART_SRAM_START;
}

static inline size_t gwenesis_sram_offset(unsigned int address)
{
    address &= 0xFFFFFF;
    size_t offset = address - CART_SRAM_START;
    if (CART_SRAM_PARITY_ONLY)
        offset >>= 1;
    return offset;
}

static inline unsigned int gwenesis_sram_read_8(unsigned int address)
{
    if (!gwenesis_sram_contains(address))
        return 0xff;

    const size_t offset = gwenesis_sram_offset(address);
    return offset < CART_SRAM_SIZE ? CART_SRAM_DATA[offset] : 0xff;
}

static inline void gwenesis_sram_write_8(unsigned int address, unsigned int value)
{
    if (!gwenesis_sram_contains(address))
        return;

    const size_t offset = gwenesis_sram_offset(address);
    if (offset < CART_SRAM_SIZE)
        CART_SRAM_DATA[offset] = value & 0xff;
}

static void gwenesis_rom_finalize(size_t size)
{
    if (!ROM_DATA || size == 0)
        return;

    if (size > MAX_ROM_SIZE)
    {
        printf("Genesis ROM larger than 8MB, truncating visible map from %u to %u bytes\n",
               (unsigned)size, (unsigned)MAX_ROM_SIZE);
        size = MAX_ROM_SIZE;
    }

    const size_t mirror_size = gwenesis_rom_mirror_size(size);
    ROM_SIZE = (unsigned int)size;
    ROM_MASK = (unsigned int)(mirror_size - 1);

#if defined(RETRO_GO)
    const unsigned int direct_pages = gwenesis_rom_page_table_build(size);
#endif

    printf("Genesis ROM size=%u virtual_mirror=%u mask=0x%06x\n",
           (unsigned)size, (unsigned)mirror_size, ROM_MASK);
#if defined(RETRO_GO)
    printf("Genesis ROM 64KB page table: %u/%u direct pages (%u bytes internal)\n",
           direct_pages, GWENESIS_ROM_PAGE_COUNT,
           (unsigned)(sizeof(*ROM_PAGE_PTR) * GWENESIS_ROM_PAGE_COUNT));
#endif
}

void load_cartridge(unsigned char *buffer, size_t size)
{
    if (!gwenesis_bus_init_fast_ram())
        return;

    // Clear all volatile memory
    gwenesis_m68k_ram_clear();
    memset(ZRAM, 0, MAX_Z80_RAM_SIZE);

    // Set Z80 Memory as ZRAM
    z80_set_memory(ZRAM);
    z80_pulse_reset();

    if (!buffer || size == 0)
        return;

    // Copy file contents to CPU ROM memory
    #ifdef RETRO_GO
    ROM_DATA = buffer;
    #else
    unsigned char *rom = realloc(ROM_DATA, size);
    if (!rom)
        return;
    ROM_DATA = rom;
    memcpy(ROM_DATA, buffer, size);
    #endif

    // https://github.com/franckverrot/EmulationResources/blob/master/consoles/megadrive/genesis_rom.txt
    if (size > 512 && ROM_DATA[1] == 0x03 && ROM_DATA[8] == 0xAA && ROM_DATA[9] == 0xBB)
    {
      printf("--SMD de-interleave mode--\n");
      size -= 512;
      memmove(ROM_DATA, ROM_DATA + 512, size);
      uint8 *temp = malloc(0x4000);
      if (temp)
      {
        for (size_t i = 0; i + 0x4000 <= size; i += 0x4000)
        {
          memcpy(temp, ROM_DATA + i, 0x4000);
          for (size_t j = 0; j < 0x2000; ++j)
          {
            ROM_DATA[i + (j * 2) + 0] = temp[0x2000 + j];
            ROM_DATA[i + (j * 2) + 1] = temp[0x0000 + j];
          }
        }
        free(temp);
      }
    }

    gwenesis_sram_configure(size);

    #ifdef ROM_SWAP
    bus_log(__FUNCTION__,"--ROM swap mode--");
    for (size_t i = 0; i + 1 < size; i += 2)
    {   
        char z = ROM_DATA[i];
        ROM_DATA[i]=ROM_DATA[i+1];
        ROM_DATA[i+1]=z;
    }
    #endif

    gwenesis_rom_finalize(size);
    z80_refresh_banked_rom_fast_path();

    set_region();
}

void unload_cartridge(void)
{
#if defined(RETRO_GO)
    gwenesis_rom_page_table_clear();
#endif
    if (ROM_DATA)
    {
        free(ROM_DATA);
        ROM_DATA = NULL;
    }
    ROM_MASK = MAX_ROM_SIZE - 1;
    ROM_SIZE = 0;
    gwenesis_sram_reset();
    z80_refresh_banked_rom_fast_path();
}

#endif

/******************************************************************************
 *
 *   Power ON the CPU
 *   Initialize 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void power_on() {
  // Set M68K CPU as original MOTOROLA 68000
  //m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  // Initialize M68K CPU
  m68k_init();
  // Initialize Z80 CPU
  z80_start();
  // Initialize YM2612 chip
#if GWENESIS_AUDIO_EMULATION
  YM2612Init();
  YM2612Config(9);
#endif
  // Initialize PSG SN76489 chip when it is part of the audio path.
  //CLOCK_NTSC      = 3579545,
  //CLOCK_PAL       = 3546895,
 // CLOCK_NTSC_SMS1 = 3579527

//  if (mode_pal) {
//     gwenesis_SN76489_Init(3546895, GWENESIS_AUDIO_BUFFER_LENGTH_PAL*50,AUDIO_FREQ_DIVISOR);
//   } else{
//     gwenesis_SN76489_Init(3579545, GWENESIS_AUDIO_BUFFER_LENGTH_NTSC*60,AUDIO_FREQ_DIVISOR);
//   }
  
#if GWENESIS_AUDIO_EMULATION && GWENESIS_SN76489_RUN_ENABLED
  gwenesis_SN76489_Init(3579545, GWENESIS_AUDIO_BUFFER_LENGTH_NTSC * 60, AUDIO_FREQ_DIVISOR);
#endif

}

/******************************************************************************
 *
 *   Reset the CPU Emulation
 *   Send a pulse reset to 68K, Z80 and YM2612 Cores
 *
 ******************************************************************************/
void reset_emulation() {
  // Send a reset pulse to Z80 CPU
  z80_pulse_reset();
  // Send a reset pulse to Z80 M68K
  m68k_pulse_reset();
  // Send a reset pulse to YM2612 chip
#if GWENESIS_AUDIO_EMULATION
  YM2612ResetChip();
#endif
  // Send a reset pulse to SEGA 315-5313 chip
  gwenesis_vdp_reset();
#if GWENESIS_AUDIO_EMULATION && GWENESIS_SN76489_RUN_ENABLED
  gwenesis_SN76489_Reset();
#endif
}

/******************************************************************************
 *
 *   Set Region
 *   Look at ROM to set console compatible region
 *
 ******************************************************************************/
void set_region()
{    
  /*
    old style : JUE characters
    J : Domestic 60Hz (Asia)
    U : Oversea  60Hz (USA) 
    E : Oversea  50Hz (Europe) 

    new style : 1st character
    bit 0 : +1 Domestic 60Hz (Asia)
    bit 1 : +2 Domestc  50Hz (Asia)
    bit 2:  +4 Oversea  60Hz (USA) 
    bit 3:  +4 Oversea  50Hz (Europe) 
  */

   // extern int mode_pal;

    int country = 0;

    char rom_str[3];

    printf("ROM game  : ");
    for (int j=0; j < 48;j++) printf("%c",(char)FETCH8ROM(0x150+j));
    printf("\n");

    rom_str[0]=FETCH8ROM(0x1F0);
    rom_str[1]=FETCH8ROM(0x1F1);
    rom_str[2]=FETCH8ROM(0x1F2);

    printf("ROM region:%c%c%c (0x%02x 0x%02x 0x%02x)\n", rom_str[0],rom_str[1],rom_str[2],rom_str[0],rom_str[1],rom_str[2]);

    /* from Gens */
    if (!memcmp(rom_str, "eur", 3)) country |= 8;
    else if (!memcmp(rom_str, "EUR", 3)) country |= 8;
    else if (!memcmp(rom_str, "Europe", 3)) country |= 8;
    else if (!memcmp(rom_str, "jap", 3)) country |= 1;
    else if (!memcmp(rom_str, "JAP", 3)) country |= 1;
    else if (!memcmp(rom_str, "usa", 3)) country |= 4;
    else if (!memcmp(rom_str, "USA", 3)) country |= 4;
    else
    {
      int i;
      unsigned char c;

      /* look for each characters */
      for(i = 0; i < 3; i++)
      {
        c = rom_str[i];

        if (c == 'U') country |= 4;
        else if (c == 'E' || c == 'e' ) country |= 8;
        else if (c == 'J' || c == 'j' ) country |= 1;
        else if (c == 'K' || c == 'k' ) country |= 1;
        else if (c < 16) country |= c;
        else if ((c >= '0') && (c <= '9')) country |= c - '0';
        else if ((c >= 'A') && (c <= 'F')) country |= c - 'A' + 10;
      }
    }
    printf("country code=%01x : ",country);
      /* set default console region (USA > EUROPE > JAPAN) */
      /*
      IO REG0	:	MODE 	VMOD 	DISK 	RSV 	VER3 	VER2 	VER1 	VER0
      MODE (R) 	0: Domestic Model
  	            1: Overseas Model
      VMOD (R) 	0: NTSC CPU clock 7.67 MHz
  	            1: PAL CPU clock 7.60 MHz
      */

    /* USA 60Hz*/
    if (country & 4){
      printf("Oversea-NTSC USA 60Hz\n");
      gwenesis_io_set_reg(0, 0x81);
   //   gwenesis_vdp_status &= 0xFFFE;
     // mode_pal = 0;
      return;
    }
    /* EUROPE 50Hz */
    if (country & 8){
      printf("Oversea-PAL Europe 50Hz\n");
      gwenesis_io_set_reg(0, 0xC1);
    //  gwenesis_vdp_status |= 0x1;
      //mode_pal = 1;
      return;
    }
    /* set Asia 60HZ */
    if (country & 1){
      printf("Domestic-NTSC Asia 60Hz\n");
      gwenesis_io_set_reg(0, 0x1);
    //  gwenesis_vdp_status &= 0xFFFE;
      //mode_pal = 0;
      return;
    }
      printf("Oversea-NTSC USA 60Hz no detection>> default mode\n");
      gwenesis_io_set_reg(0, 0x81);
     // gwenesis_vdp_status &= 0xFFFE;
     // mode_pal = 0;

}
/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *   68K Access to Z80 Memory
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_z80_address(unsigned int address) {

  unsigned int range = (address & 0xF000);
  switch (range) {
  case 0:
  case 0x1000:
    return Z80_RAM_ADDR;
  case 0x2000:
  case 0x3000:
    return Z80_RAM_ADDR1K;
  case 0x4000:
    return Z80_YM2612_ADDR;
  case 0x6000:
    return Z80_BANK_ADDR;
  case 0x7000:
    return Z80_SN76489_ADDR;
  default:
    bus_log(__FUNCTION__,"no map Z80 %x",address);
    assert(0);
    return NONE;
  }
}

/******************************************************************************
 *
 *   IO memory address mapper
 *   Map all input/output region address for CPU program
 *
 ******************************************************************************/
static inline unsigned int gwenesis_bus_map_io_address(unsigned int address)
{
  unsigned int range = (address & 0x1000) ;
  switch (range) {
  case 0:      return IO_CTRL;
  case 0x1000: return Z80_CTRL;
  default:
      // if (address >= 0xa14000 && address < 0xa11404)
      // return (tmss_state == 0) ? TMSS_CTRL : NONE;
      bus_log(__FUNCTION__,"no map io %x",address);

    return NONE;
  }
}

/******************************************************************************
 *
 *   Main memory address mapper
 *   Map all main memory region address for CPU program
 *
 ******************************************************************************/

static inline 
unsigned int gwenesis_bus_map_address(unsigned int address) {
  // Mask address page
  unsigned int range = (address & 0xFF0000) >> 16;

  // Check mask and select memory type
  if (range < 0x80) //        ROM ADDRESS 0x000000 - 0x3FFFFF
    return ROM_ADDR;

  else if (range == 0xA0) // Z80 ADDRESS 0xA00000 - 0xA0FFFF
    return gwenesis_bus_map_z80_address(address);


  else if (range == 0xA1) //                  IO ADDRESS  0xA10000 - 0xA1FFFF
    return gwenesis_bus_map_io_address(address);

  else if (range == 0xC0) // VDP ADDRESS 0xC00000 - 0xDFFFFFF
    return VDP_ADDR;
  else if (range >= 0xE0) // RAM ADDRESS 0xE00000 - 0xFFFFFF
    return RAM_ADDR;
  // If not a valid address return 0
  bus_log(__FUNCTION__,"M68K > ?? unnmap address %x", address);
  //assert(0);
  return NONE;
}
/******************************************************************************
 *
 *   Main read address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline unsigned int GWENESIS_HOT gwenesis_bus_read_memory_8(unsigned int address) {
 bus_log(__FUNCTION__,"read8  %x", address);

  if (gwenesis_sram_contains(address))
  {
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_SRAM);
    return gwenesis_sram_read_8(address);
  }

  switch (gwenesis_bus_map_address(address)) {
  
  case VDP_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_VDP);
    return gwenesis_vdp_read_memory_8(address);

  case ROM_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_ROM);
    return FETCH8ROM(address);

  case RAM_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_R);
    return FETCH8RAM(address);

  case IO_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_IO);
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
    return z80_read_ctrl(address & 0xFFFF);

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
    return ZRAM[address & 0x1FFF];

  case Z80_YM2612_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_YM2612);
    return YM2612Read(m68k_cycles_master());

  case Z80_SN76489_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_PSG);
    return 0xff;

  case Z80_BANK_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
    return 0xff;

  case TMSS_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_TMSS);
    bus_log(__FUNCTION__,"TMS");
    if (tmss_state == 0)
      return TMSS[address & 0x4];
    return 0xFF;

  default:
     GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_OTHER);
     bus_log(__FUNCTION__," default read 8 %x", address);
    return 0x00;
  }
  return 0x00;
}

static inline unsigned int GWENESIS_HOT gwenesis_bus_read_memory_16(unsigned int address) {
   bus_log(__FUNCTION__,"read16 %x", address);
   unsigned int ret_value;

  if (gwenesis_sram_overlaps(address, 2))
  {
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_SRAM);
    return (gwenesis_sram_read_8(address) << 8) | gwenesis_sram_read_8(address + 1);
  }

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_VDP);
    return gwenesis_vdp_read_memory_16(address);

  case RAM_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_R);
    return FETCH16RAM(address);

  case ROM_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_ROM);
    return FETCH16ROM(address);

  case IO_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_IO);
    return gwenesis_io_read_ctrl(address & 0x1F);

  case Z80_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
  //  ret_value = z80_read_ctrl(address & 0xFFFF); 
   // return ret_value | ret_value << 8;
    address &=0xFFFF;
        return (z80_read_ctrl(address) << 8) | z80_read_ctrl(address | 1);


  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
    return ZRAM[address & 0X1FFF] | (ZRAM[address & 0X1FFF] << 8);

  case Z80_YM2612_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_YM2612);
    ret_value = YM2612Read(m68k_cycles_master());
    return ret_value | ret_value << 8;


  case Z80_SN76489_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_PSG);
    return 0xff;

  case Z80_BANK_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
    return 0xff;

  default:
    bus_log(__FUNCTION__,"read mem 16 default %x", address);
    return (gwenesis_bus_read_memory_8(address) << 8) |
           gwenesis_bus_read_memory_8(address + 1);
  }
  return 0x00;
}

/******************************************************************************
 *
 *   Main write address routine
 *   Write an value to memory mapped on specified address
 *
 ******************************************************************************/
static inline void GWENESIS_HOT gwenesis_bus_write_memory_8(unsigned int address,
                                              unsigned int value) {
  bus_log(__FUNCTION__,"write8  @%x:%x", address,value);

  if (gwenesis_sram_contains(address))
  {
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_SRAM);
    gwenesis_sram_write_8(address, value);
    return;
  }

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_VDP);
    gwenesis_vdp_write_memory_16(address & ~1, (value << 8) | value);
    return;

  case RAM_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_W);
    WRITE8RAM(address, value);
    return;

  case IO_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_IO);
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
    z80_write_ctrl(address & 0x1FFF, value);
    return;

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
    ZRAM[address & 0x1FFF] = value;
    return;

  case Z80_YM2612_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_YM2612);
    bus_log(__FUNCTION__,"CPUZ80PSG8 ,m68kclk= %d", m68k_cycles_master());
    YM2612Write(address & 0x3, value & 0Xff,m68k_cycles_master());
    return;

  case Z80_SN76489_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_PSG);
#if GWENESIS_SN76489_RUN_ENABLED
    bus_log(__FUNCTION__,"CPUZ80FM8  ,m68kclk= %d", m68k_cycles_master());
    gwenesis_SN76489_Write( value & 0Xff, m68k_cycles_master());
#endif
    return;

  case Z80_BANK_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
  //TODO
    return;

  case TMSS_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_TMSS);

    if (tmss_state == 0) {
      TMSS[address & 0x4] = value;
      tmss_count++;
      if (tmss_count == 4)
        tmss_state = 1;
    }
    return;



  default:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_OTHER);
    //printf("write(%x, %x)\n", address, value);
    return;
  }
  return;
}

static inline void GWENESIS_HOT gwenesis_bus_write_memory_16(unsigned int address,
                                               unsigned int value) {
  bus_log(__FUNCTION__,"write16  @%x:%x", address,value);

  if (gwenesis_sram_overlaps(address, 2))
  {
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_SRAM);
    gwenesis_sram_write_8(address, (value >> 8) & 0xff);
    gwenesis_sram_write_8(address + 1, value & 0xff);
    return;
  }

  switch (gwenesis_bus_map_address(address)) {

  case VDP_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_VDP);
    gwenesis_vdp_write_memory_16(address, value);
    return;

  case RAM_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_W);
    WRITE16RAM(address, value);
    return;

  case Z80_RAM_ADDR:
  case Z80_RAM_ADDR1K:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
    ZRAM[address & 0X1FFF]= value >> 8;
    return;

  case IO_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_IO);
    gwenesis_io_write_ctrl(address & 0x1F, value);
    return;

  case Z80_CTRL:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_Z80);
    z80_write_ctrl(address & 0xFFFF, value >> 8) ;
    return;

  case Z80_YM2612_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_YM2612);
    bus_log(__FUNCTION__,"CZYM16 ,mclk=%d",  m68k_cycles_master());
    YM2612Write(address & 0x3, value >> 8,m68k_cycles_master() );
    return;

  case Z80_SN76489_ADDR:
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_PSG);
#if GWENESIS_SN76489_RUN_ENABLED
    bus_log(__FUNCTION__,"CZSN16 ,mclk=%d", m68k_cycles_master());
    gwenesis_SN76489_Write(value >> 8,m68k_cycles_master() );
#endif
    return;

  default:
    bus_log(__FUNCTION__,"write mem 16 default %x ", address);
    gwenesis_bus_write_memory_8(address, (value >> 8) & 0xff);
    gwenesis_bus_write_memory_8(address + 1, (value)&0xff);

    return;
  }
  return;
}

/******************************************************************************
 *
 *   68K CPU read address R8
 *   Read an address from memory mapped and return value as byte
 *
 ******************************************************************************/
unsigned int GWENESIS_HOT m68k_read_memory_8(unsigned int address)
{
    address &= 0xFFFFFF;
    if (address < 0x800000)
    {
        if (CART_SRAM_TOUCHES(address, 1))
            return gwenesis_bus_read_memory_8(address);
        GWENESIS_M68K_ROM_KIND_INC(GWENESIS_M68K_ROM_KIND_DATA);
        GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_ROM);
        return FETCH8ROM(address);
    }
    if ((address & 0xE00000) == 0xE00000)
    {
        GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_R);
        return FETCH8RAM(address);
    }
    return gwenesis_bus_read_memory_8(address);
}

/******************************************************************************
 *
 *   68K CPU read address R16
 *   Read an address from memory mapped and return value as word
 *
 ******************************************************************************/
unsigned int GWENESIS_HOT m68k_read_memory_16(unsigned int address)
{
    address &= 0xFFFFFF;
    if (address < 0x800000)
    {
        if (CART_SRAM_TOUCHES(address, 2))
            return gwenesis_bus_read_memory_16(address);
        GWENESIS_M68K_ROM_KIND_INC(GWENESIS_M68K_ROM_KIND_DATA);
        GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_ROM);
        return FETCH16ROM(address);
    }
    if ((address & 0xE00000) == 0xE00000)
    {
        GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_R);
        return FETCH16RAM(address);
    }
    return gwenesis_bus_read_memory_16(address);
}

/******************************************************************************
 *
 *   68K CPU read address R32
 *   Read an address from memory mapped and return value as long
 *
 ******************************************************************************/
unsigned int GWENESIS_HOT m68k_read_memory_32(unsigned int address)
{
  address &= 0xFFFFFF;
  if (address < 0x800000 && (address + 3) < 0x800000) {
    if (CART_SRAM_TOUCHES(address, 4))
      return (gwenesis_bus_read_memory_16(address) << 16) | gwenesis_bus_read_memory_16(address + 2);
    GWENESIS_M68K_ROM_KIND_INC(GWENESIS_M68K_ROM_KIND_DATA);
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_ROM);
    return FETCH32ROM(address);
  }
  if ((address & 0xE00000) == 0xE00000 && ((address + 2) & 0xE00000) == 0xE00000) {
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_R);
    return FETCH32RAM(address);
  }
  return (gwenesis_bus_read_memory_16(address) << 16) | gwenesis_bus_read_memory_16(address + 2);
}

/******************************************************************************
 *
 *   68K CPU write address W8
 *   Write an value as byte to memory mapped on specified address
 *
 ******************************************************************************/
void GWENESIS_HOT m68k_write_memory_8(unsigned int address, unsigned int value) {
  if ((address & 0xE00000) == 0xE00000) {
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_W);
    WRITE8RAM(address, value);
    return;
  }
  gwenesis_bus_write_memory_8(address, value);
  return;
}

/******************************************************************************
 *
 *   68K CPU write address W16
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void GWENESIS_HOT m68k_write_memory_16(unsigned int address, unsigned int value) {
  if ((address & 0xE00000) == 0xE00000) {
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_W);
    WRITE16RAM(address, value);
    return;
  }
  gwenesis_bus_write_memory_16(address, value);
  return;
}
/******************************************************************************
 *
 *   68K CPU write address W32
 *   Write an value as word to memory mapped on specified address
 *
 ******************************************************************************/
void GWENESIS_HOT m68k_write_memory_32(unsigned int address, unsigned int value) {

  if ((address & 0xE00000) == 0xE00000 && ((address + 2) & 0xE00000) == 0xE00000) {
    GWENESIS_M68K_MEM_KIND_INC(GWENESIS_M68K_MEM_RAM_W);
    WRITE32RAM(address, value);
    return;
  }
  gwenesis_bus_write_memory_16(address, (value >> 16) & 0xffff);
  gwenesis_bus_write_memory_16(address + 2, (value)&0xffff);

  return;
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
    return m68k_read_memory_16(address);
}
unsigned int m68k_read_disassembler_32(unsigned int address)
{
    return m68k_read_memory_32(address);
}

void gwenesis_bus_m68k_ram_cache_update(const uint32_t ram_reads[16], const uint32_t ram_writes[16])
{
#if defined(RETRO_GO)
  if (!M68K_RAM || !m68k_ram_cache_state)
    return;

  ++m68k_ram_cache_update_epoch;

  for (unsigned int page = 0; page < GWENESIS_M68K_RAM_PAGE_COUNT; ++page)
    m68k_ram_cache_score[page] = ram_reads[page] + ram_writes[page] * 2;

  unsigned int best_page = GWENESIS_M68K_RAM_PAGE_COUNT;
  uint32_t best_score = 0;
  for (unsigned int page = 0; page < GWENESIS_M68K_RAM_PAGE_COUNT; ++page)
  {
    if (!gwenesis_m68k_ram_cache_page_loaded(page) && m68k_ram_cache_score[page] > best_score)
    {
      best_page = page;
      best_score = m68k_ram_cache_score[page];
    }
  }
  if (best_page >= GWENESIS_M68K_RAM_PAGE_COUNT || best_score == 0)
    return;

  unsigned int cold_slot = GWENESIS_M68K_RAM_FAST_SLOTS;
  uint32_t cold_score = 0xffffffffU;
  for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS; ++slot)
  {
    if (!m68k_ram_fast_slot[slot])
      continue;

    uint8_t page = m68k_ram_fast_page[slot];
    uint32_t score = page < GWENESIS_M68K_RAM_PAGE_COUNT ? m68k_ram_cache_score[page] : 0;
    if (score < cold_score)
    {
      cold_slot = slot;
      cold_score = score;
    }
  }
  if (cold_slot >= GWENESIS_M68K_RAM_FAST_SLOTS)
    return;

  if (cold_score != 0 &&
      m68k_ram_cache_update_epoch - m68k_ram_cache_last_swap_epoch < GWENESIS_M68K_RAM_CACHE_MIN_SWAP_EPOCHS)
    return;
  if (cold_score != 0 && best_score <= cold_score + cold_score / 2)
    return;

  if (gwenesis_m68k_ram_cache_install(cold_slot, best_page))
    m68k_ram_cache_last_swap_epoch = m68k_ram_cache_update_epoch;
#else
  (void)ram_reads;
  (void)ram_writes;
#endif
}

void gwenesis_bus_m68k_ram_cache_status(char *out, size_t out_size)
{
  if (!out || out_size == 0)
    return;

#if defined(RETRO_GO)
  if (!m68k_ram_cache_state)
  {
    snprintf(out, out_size, "off");
    return;
  }

  size_t used = 0;
  int written = snprintf(out, out_size, "slots=");
  if (written < 0)
  {
    out[0] = 0;
    return;
  }
  used = (size_t)written < out_size ? (size_t)written : out_size - 1;

  for (unsigned int slot = 0; slot < GWENESIS_M68K_RAM_FAST_SLOTS && used < out_size; ++slot)
  {
    uint8_t page = m68k_ram_fast_page[slot];
    if (m68k_ram_fast_slot[slot] && page < GWENESIS_M68K_RAM_PAGE_COUNT)
      written = snprintf(out + used, out_size - used, "%s%u:%x:%u",
                         slot ? "," : "", slot, page, (unsigned)m68k_ram_cache_score[page]);
    else
      written = snprintf(out + used, out_size - used, "%s%u:-",
                         slot ? "," : "", slot);
    if (written < 0)
      break;
    if ((size_t)written >= out_size - used)
    {
      out[out_size - 1] = 0;
      return;
    }
    used += (size_t)written;
  }

  if (used < out_size)
  {
    written = snprintf(out + used, out_size - used, " swaps=%u",
                       (unsigned)m68k_ram_cache_swaps);
    if (written < 0 || (size_t)written >= out_size - used)
      out[out_size - 1] = 0;
  }
#else
  snprintf(out, out_size, "off");
#endif
}

void gwenesis_bus_save_state() {
  SaveState* state;
  state = saveGwenesisStateOpenForWrite("bus");
#if defined(RETRO_GO)
  gwenesis_m68k_ram_cache_sync_all();
#endif
  saveGwenesisStateSetBuffer(state, "M68K_RAM", M68K_RAM, MAX_RAM_SIZE);
  saveGwenesisStateSetBuffer(state, "ZRAM", ZRAM, MAX_Z80_RAM_SIZE);
  saveGwenesisStateSetBuffer(state, "TMSS", TMSS, sizeof(TMSS));
  saveGwenesisStateSet(state, "tmss_state", tmss_state);
  saveGwenesisStateSet(state, "tmss_count", tmss_count);
}

void gwenesis_bus_load_state() {
    SaveState* state = saveGwenesisStateOpenForRead("bus");
    saveGwenesisStateGetBuffer(state, "M68K_RAM", M68K_RAM, MAX_RAM_SIZE);
#if defined(RETRO_GO)
    gwenesis_m68k_ram_cache_reload_slots();
#endif
    saveGwenesisStateGetBuffer(state, "ZRAM", ZRAM, MAX_Z80_RAM_SIZE);
    saveGwenesisStateGetBuffer(state, "TMSS", TMSS, sizeof(TMSS));
    tmss_state = saveGwenesisStateGet(state, "tmss_state");
    tmss_count = saveGwenesisStateGet(state, "tmss_count");
}
