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
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#if defined(RG_TARGET_HOLO_DYNMOD)
#include "rg_utils.h"
#endif
#include "Z80.h"
#include "z80inst.h"
#include "m68k.h"
#include "gwenesis_bus.h"
#include "ym2612.h"
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

int zclk = 0;

unsigned char *Z80_RAM;

typedef struct
{
    int bus_ack;
    int reset;
    int reset_once;
    int initialized;
    int current_timeslice;
    bool emulation_enabled;
    unsigned char *bank_rom_base;
    unsigned int bank_base;
} z80_fast_state_t;

static z80_fast_state_t z80_fast_state_fallback = {
    .emulation_enabled = true,
};
static z80_fast_state_t *z80_fast_state = &z80_fast_state_fallback;

#define bus_ack (z80_fast_state->bus_ack)
#define reset (z80_fast_state->reset)
#define reset_once (z80_fast_state->reset_once)
#define initialized (z80_fast_state->initialized)
#define current_timeslice (z80_fast_state->current_timeslice)
#define z80_emulation_enabled (z80_fast_state->emulation_enabled)
#define Z80_BANK_ROM_BASE (z80_fast_state->bank_rom_base)
#define Z80_BANK_BASE (z80_fast_state->bank_base)

static Z80 z80_cpu_fallback;
static Z80 *z80_cpu_ptr = &z80_cpu_fallback;
#define cpu (*z80_cpu_ptr)

void ResetZ80(register Z80 *R);

static void z80_init_fast_ram(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    z80_init_hot_tables();

    if (z80_fast_state == &z80_fast_state_fallback)
    {
        z80_fast_state_t *ptr = rg_alloc(sizeof(*ptr), MEM_FAST | MEM_NOPANIC);
        if (ptr && PTR_IN_SPIRAM(ptr))
        {
            free(ptr);
            ptr = NULL;
        }
        if (ptr)
        {
            *ptr = *z80_fast_state;
            z80_fast_state = ptr;
            printf("[info] Genesis Z80 small state: ptr=%p size=%u requested=MEM_FAST addr_guess=%s\n",
                   z80_fast_state, (unsigned)sizeof(*z80_fast_state),
                   PTR_IN_SPIRAM(z80_fast_state) ? "SPIRAM" : "internal");
        }
        else
        {
            printf("[warn] Genesis Z80 small state: MEM_FAST allocation failed size=%u, using static fallback\n",
                   (unsigned)sizeof(*z80_fast_state));
        }
    }

    if (z80_cpu_ptr != &z80_cpu_fallback)
        return;

    Z80 *ptr = rg_alloc(sizeof(*ptr), MEM_FAST | MEM_NOPANIC);
    if (ptr && PTR_IN_SPIRAM(ptr))
    {
        free(ptr);
        ptr = NULL;
    }
    if (ptr)
    {
        z80_cpu_ptr = ptr;
        printf("[info] Genesis Z80 context: ptr=%p size=%u requested=MEM_FAST addr_guess=%s\n",
               z80_cpu_ptr, (unsigned)sizeof(*z80_cpu_ptr),
               PTR_IN_SPIRAM(z80_cpu_ptr) ? "SPIRAM" : "internal");
    }
    else
    {
        printf("[warn] Genesis Z80 context: MEM_FAST allocation failed size=%u, using static fallback\n",
               (unsigned)sizeof(*z80_cpu_ptr));
    }
#endif
}

void z80_deinit_fast_ram(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    z80_deinit_hot_tables();
    if (z80_cpu_ptr != &z80_cpu_fallback)
    {
        free(z80_cpu_ptr);
        z80_cpu_ptr = &z80_cpu_fallback;
    }
    memset(&z80_cpu_fallback, 0, sizeof(z80_cpu_fallback));
    if (z80_fast_state != &z80_fast_state_fallback)
    {
        free(z80_fast_state);
        z80_fast_state = &z80_fast_state_fallback;
    }
    memset(&z80_fast_state_fallback, 0, sizeof(z80_fast_state_fallback));
    z80_fast_state_fallback.emulation_enabled = true;
#endif
}

#define Z80_INST_DISABLE_LOGGING 1

#if !Z80_INST_DISABLE_LOGGING
#include <stdarg.h>
void z80_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s]", frame_counter, scan_line, subs);

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
	#define z80_log(...)  do {} while(0)
#endif

// Bank register used by Z80 to access M68K Memory space 1 BANK=32KByte
int Z80_BANK;

void z80_refresh_banked_rom_fast_path(void)
{
    Z80_BANK_BASE = (unsigned int)Z80_BANK << 15;
    Z80_BANK_ROM_BASE = NULL;

#if GNW_TARGET_MARIO == 0 && GNW_TARGET_ZELDA == 0
    if (!ROM_DATA || ROM_SIZE == 0)
        return;

    unsigned int offset = Z80_BANK_BASE & ROM_MASK;
    if (Z80_BANK_BASE >= 0x800000)
        return;
    if (CART_SRAM_TOUCHES(Z80_BANK_BASE, 0x8000))
        return;
    if (offset + 0x8000 > ROM_SIZE)
        return;

    Z80_BANK_ROM_BASE = ROM_DATA + offset;
#endif
}


void z80_start() {
    z80_init_fast_ram();
    cpu.IPeriod = 1;
    cpu.ICount = 0;
    cpu.Trace = 0;
    cpu.Trap = 0x0009;
    ResetZ80(&cpu);
    reset=1;
    reset_once=0;
    bus_ack=0;
    zclk=0;
    z80_refresh_banked_rom_fast_path();
}

void z80_set_enabled(bool enabled)
{
    z80_emulation_enabled = enabled;
    if (!enabled)
    {
        cpu.IRequest = INT_NONE;
        zclk = 0;
    }
}

void z80_pulse_reset() {
  ResetZ80(&cpu);
}

void GWENESIS_HOT z80_run(int target) {

  // we are in advance,nothind to do
current_timeslice = 0;
  if (!z80_emulation_enabled) {
    zclk = target;
    return;
  }
  if (zclk >= target) {
 // z80_log("z80_skip time","%1d%1d%1d||zclk=%d,tgt=%d",reset_once,bus_ack,reset, zclk, target);
    return;
  }

  current_timeslice = target - zclk;

  int rem = 0;
  if ((reset_once == 1) && (bus_ack == 0) && (reset == 0)) {

   // z80_log("z80_run", "%1d%1d%1d||zclk=%d,tgt=%d",reset_once, bus_ack, reset, zclk, target);
    rem = ExecZ80(&cpu, current_timeslice / Z80_FREQ_DIVISOR);

  }

  zclk = target - rem * Z80_FREQ_DIVISOR;
}

void GWENESIS_HOT z80_sync(void) {
  /*
  get M68K cycles 
  Execute cycles on z80 to sync with m68K
  */

  z80_run(m68k_cycles_master());
}

void z80_set_memory(unsigned char *buffer)
{
    Z80_RAM = buffer;
    initialized = buffer != NULL;
}

static inline unsigned char *z80_get_memory(void)
{
    return Z80_RAM;
}

void GWENESIS_HOT z80_write_ctrl(unsigned int address, unsigned int value) {
  z80_sync();

  if (address == 0x1100) // BUSREQ
  {
    z80_log(__FUNCTION__,"BUSREQ = %d, current=%d", value,bus_ack);

    // Bus request. Z80 bus on hold.
    if (value) {
      bus_ack = 1;


    // Bus request cancel. Z80 runs.
    } else {
            bus_ack = 0;
    }

  } else if (address == 0x1200) // RESET
  {
    z80_log(__FUNCTION__,"RESET = %d, current=%d", value,reset);
  
    if (value == 0) {
      reset = 1;
    } else {

      z80_pulse_reset();
      reset = 0;
      reset_once = 1;
    }
  }
}

unsigned int GWENESIS_HOT z80_read_ctrl(unsigned int address) {

  z80_sync();

  if (address == 0x1100) {

    z80_log(__FUNCTION__,"RUNNING = %d ", bus_ack ? 0 : 1);
    return bus_ack == 1 ? 0 : 1;

  } else if (address == 0x1101) {
    return 0x00;

  } else if (address == 0x1200) {

    z80_log(__FUNCTION__,"RESET = %d ", reset );
    return reset;

  } else if (address == 0x1201) {
    return 0x00;
  }
  return 0xFF;
}

void GWENESIS_HOT z80_irq_line(unsigned int value)
{
    if (!z80_emulation_enabled) return;
    if (reset_once == 0) return;

    if (value)
        cpu.IRequest = INT_IRQ;
    else
        cpu.IRequest = INT_NONE;

    z80_log(__FUNCTION__,"Interrupt = %d ", value);

}

#if 0

word z80_get_reg(int reg_i) {
    switch(reg_i) {
        case 0: return cpu.AF.W; break;
        case 1: return cpu.BC.W; break;
        case 2: return cpu.DE.W; break;
        case 3: return cpu.HL.W; break;
        case 4: return cpu.IX.W; break;
        case 5: return cpu.IY.W; break;
        case 6: return cpu.PC.W; break;
        case 7: return cpu.SP.W; break;
    }
}
#endif

/********************************************
 * Z80 Bank
 ********************************************/

unsigned int zbankreg_mem_r8(unsigned int address)
{
      z80_log(__FUNCTION__,"Z80 bank read pointer : %06x", Z80_BANK);

    return Z80_BANK;
}

static inline void GWENESIS_HOT zbankreg_mem_w8(unsigned int value) {
  Z80_BANK >>= 1;
  Z80_BANK |= (value & 1) << 8;
  z80_refresh_banked_rom_fast_path();
  z80_log(__FUNCTION__,"Z80 bank points to: %06x", Z80_BANK << 15);
  return;
}

static inline unsigned int GWENESIS_HOT zbank_mem_r8(unsigned int address)
{
    unsigned int offset = address & 0x7FFF;
    if (Z80_BANK_ROM_BASE)
        return Z80_BANK_ROM_BASE[offset ^ 1];

    address = Z80_BANK_BASE | offset;
    z80_log(__FUNCTION__,"Z80 bank read: %06x", address);
    return m68k_read_memory_8(address);
}

static inline void GWENESIS_HOT zbank_mem_w8(unsigned int address, unsigned int value) {
  address &= 0x7FFF;
  address |= (Z80_BANK << 15);

  z80_log(__FUNCTION__,"Z80 bank write %06x: %02x", address, value);
  m68k_write_memory_8(address, value);

}

// TODO ??
/*
unsigned int zvdp_mem_r8(unsigned int address)
{
    if (address >= 0x7F00 && address < 0x7F20)
        return vdp_mem_r8(address);
    return 0xFF;
}

void zvdp_mem_w8(unsigned int address, unsigned int value)
{
    if (address >= 0x7F00 && address < 0x7F20)
        vdp_mem_w8(address, value);
}

*/

word LoopZ80(register Z80 *R)
{
    return 0;
}

byte GWENESIS_HOT RdZ80(register word Addr) {

  unsigned char *ram = z80_get_memory();

  if (Addr < 0x4000)
    return ram ? ram[Addr & 0x1FFF] : 0xFF;

  if (Addr < 0x6000)
  {
    const int timestamp = zclk + current_timeslice - (cpu.ICount * Z80_FREQ_DIVISOR);
    return YM2612Read(timestamp);
  }

  z80_log(__FUNCTION__, "addr= %x", Addr);

  if (Addr >= 0x8000)
    return zbank_mem_r8(Addr);

  z80_log(__FUNCTION__, "addr= %x", Addr);

  return 0xFF;
}

extern int system_clock;

void GWENESIS_HOT WrZ80(register word Addr, register byte Value) {

  unsigned char *ram = z80_get_memory();

  // ZRAM & mirror
  if (Addr < 0x4000) {
    if (ram)
      ram[Addr&0x1FFF] = Value;
    return;
  }

  // @4000-4003
  if (Addr < 0x6000) {
    const int timestamp = zclk + current_timeslice - (cpu.ICount * Z80_FREQ_DIVISOR);
    z80_log("Z80","ZZYM(%x,%x) zk=%d,tgt=%d",Addr&0x3,Value, zclk, timestamp);
    YM2612Write(Addr&0x3, Value, timestamp);
    return;
  }

  // @6000
  if (Addr == 0x6000) {
    zbankreg_mem_w8(Value);
    return;
  }

  // @7F11
  if (Addr ==  0x7F11) {
#if GWENESIS_SN76489_RUN_ENABLED
    const int timestamp = zclk + current_timeslice - (cpu.ICount * Z80_FREQ_DIVISOR);
    z80_log("Z80","ZZSN zk=%d,tgt=%d", zclk, timestamp);
    gwenesis_SN76489_Write(Value, timestamp);
#endif
    return;
  }
 
  z80_log("Z80","WrZ80  %x %x", Addr, Value);

  if (Addr >= 0x8000) {
    zbank_mem_w8(Addr, Value);
    return;
  }
  z80_log("Z80","WrZ80  %x %x", Addr, Value);

}


byte InZ80(register word Port) {return 0;}
void OutZ80(register word Port, register byte Value) {;}
void PatchZ80(register Z80 *R) {;}
void DebugZ80(register Z80 *R) {;}

void gwenesis_z80inst_save_state() {
    SaveState* state;
    state = saveGwenesisStateOpenForWrite("z80inst");
    saveGwenesisStateSetBuffer(state, "cpu", &cpu, sizeof(Z80));
    saveGwenesisStateSet(state, "bus_ack", bus_ack);
    saveGwenesisStateSet(state, "reset", reset);
    saveGwenesisStateSet(state, "reset_once", reset_once);
    saveGwenesisStateSet(state, "zclk", zclk);
    saveGwenesisStateSet(state, "initialized", initialized);
    saveGwenesisStateSet(state, "Z80_BANK", Z80_BANK);
    saveGwenesisStateSet(state, "current_timeslice",current_timeslice);
}

void gwenesis_z80inst_load_state() {
    SaveState* state = saveGwenesisStateOpenForRead("z80inst");
    saveGwenesisStateGetBuffer(state, "cpu", &cpu, sizeof(Z80));
    bus_ack = saveGwenesisStateGet(state, "bus_ack");
    reset = saveGwenesisStateGet(state, "reset");
    reset_once = saveGwenesisStateGet(state, "reset_once");
    zclk = saveGwenesisStateGet(state, "zclk");
    initialized = saveGwenesisStateGet(state, "initialized");
    Z80_BANK = saveGwenesisStateGet(state, "Z80_BANK");
    current_timeslice = saveGwenesisStateGet(state, "current_timeslice");
    z80_refresh_banked_rom_fast_path();

}

