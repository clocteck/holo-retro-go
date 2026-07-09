/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of version 2 of the GNU Library General
** Public License as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Library General Public License for more details.  To obtain a
** copy of the GNU Library General Public License, write to the Free
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
**
**
** map021.c: VRC4 mapper interface
**
*/

#include "nes/nes.h"

typedef enum
{
    VRC_VARIANT_VRC4A,
    VRC_VARIANT_VRC4B,
    VRC_VARIANT_VRC4C,
    VRC_VARIANT_VRC4D,
    VRC_VARIANT_VRC2C,
} vrc_variant_t;

static struct
{
    bool enabled, wait_state;
    int counter, latch;
} irq;

static vrc_variant_t vrc_variant;
static bool vrc_use_heuristics;
static uint8 prg_bank0;
static uint8 prg_bank1;
static uint8 prg_mode;
static uint8 lownybbles[8];
static uint8 highnybbles[8];

static bool vrc_is_vrc4(void)
{
    return vrc_variant != VRC_VARIANT_VRC2C;
}

static const char *vrc_variant_name(void)
{
    switch (vrc_variant)
    {
    case VRC_VARIANT_VRC4A: return "VRC4a";
    case VRC_VARIANT_VRC4B: return "VRC4b";
    case VRC_VARIANT_VRC4C: return "VRC4c";
    case VRC_VARIANT_VRC4D: return "VRC4d";
    case VRC_VARIANT_VRC2C: return "VRC2c";
    default: return "unknown";
    }
}

static uint32 vrc_translate_address(uint32 address)
{
    uint32 a0 = 0;
    uint32 a1 = 0;

    if (vrc_use_heuristics)
    {
        switch (vrc_variant)
        {
        case VRC_VARIANT_VRC4A:
        case VRC_VARIANT_VRC4C:
            a0 = ((address >> 1) & 1) | ((address >> 6) & 1);
            a1 = ((address >> 2) & 1) | ((address >> 7) & 1);
            break;
        case VRC_VARIANT_VRC4B:
        case VRC_VARIANT_VRC4D:
        case VRC_VARIANT_VRC2C:
            a0 = ((address >> 1) & 1) | ((address >> 3) & 1);
            a1 = (address & 1) | ((address >> 2) & 1);
            break;
        default:
            break;
        }
    }
    else
    {
        switch (vrc_variant)
        {
        case VRC_VARIANT_VRC4A:
            a0 = (address >> 1) & 1;
            a1 = (address >> 2) & 1;
            break;
        case VRC_VARIANT_VRC4B:
        case VRC_VARIANT_VRC2C:
            a0 = (address >> 1) & 1;
            a1 = address & 1;
            break;
        case VRC_VARIANT_VRC4C:
            a0 = (address >> 6) & 1;
            a1 = (address >> 7) & 1;
            break;
        case VRC_VARIANT_VRC4D:
            a0 = (address >> 3) & 1;
            a1 = (address >> 2) & 1;
            break;
        default:
            break;
        }
    }

    return (address & 0xFF00) | (a1 << 1) | a0;
}

static void vrc_update_prg(void)
{
    if (prg_mode)
    {
        mmc_bankrom(8, 0x8000, -2);
        mmc_bankrom(8, 0xA000, prg_bank1);
        mmc_bankrom(8, 0xC000, prg_bank0);
    }
    else
    {
        mmc_bankrom(8, 0x8000, prg_bank0);
        mmc_bankrom(8, 0xA000, prg_bank1);
        mmc_bankrom(8, 0xC000, -2);
    }

    mmc_bankrom(8, 0xE000, -1);
}

static void vrc_update_chr(int bank)
{
    int page = (highnybbles[bank] << 4) | lownybbles[bank];
    mmc_bankvrom(1, bank << 10, page);
}

static void map_write(uint32 address, uint8 value)
{
    address = vrc_translate_address(address) & 0xF00F;

    if (address >= 0x8000 && address <= 0x8003)
    {
        prg_bank0 = value & 0x1F;
        vrc_update_prg();
    }
    else if (address >= 0x9000 && address <= 0x9001)
    {
        uint8 mask = vrc_is_vrc4() ? 3 : 1;
        switch (value & mask)
        {
            case 0: ppu_setmirroring(PPU_MIRROR_VERT); break;
            case 1: ppu_setmirroring(PPU_MIRROR_HORI); break;
            case 2: ppu_setmirroring(PPU_MIRROR_SCR0); break;
            case 3: ppu_setmirroring(PPU_MIRROR_SCR1); break;
        }
    }
    else if (vrc_is_vrc4() && address >= 0x9002 && address <= 0x9003)
    {
        prg_mode = (value >> 1) & 1;
        vrc_update_prg();
    }
    else if (address >= 0xA000 && address <= 0xA003)
    {
        prg_bank1 = value & 0x1F;
        vrc_update_prg();
    }
    else if (address >= 0xB000 && address <= 0xE003)
    {
        int bank = ((((address >> 12) & 7) - 3) << 1) + ((address >> 1) & 1);
        if ((address & 1) == 0)
            lownybbles[bank] = value & 0x0F;
        else
            highnybbles[bank] = value & (vrc_is_vrc4() ? 0x1F : 0x0F);
        vrc_update_chr(bank);
    }
    else if (vrc_is_vrc4())
    {
        switch (address)
        {
        case 0xF000:
        irq.latch &= 0xF0;
        irq.latch |= (value & 0x0F);
        break;
        case 0xF001:
        irq.latch &= 0x0F;
        irq.latch |= ((value & 0x0F) << 4);
        break;
        case 0xF002:
        irq.enabled = (value >> 1) & 0x01;
        irq.wait_state = value & 0x01;
        irq.counter = irq.latch;
        break;
        case 0xF003:
        irq.enabled = irq.wait_state;
        break;
        default:
            MESSAGE_DEBUG("wrote $%02X to $%04X", value, address);
        break;
        }
    }
    else
    {
        MESSAGE_DEBUG("wrote $%02X to $%04X", value, address);
    }
}

static void map_hblank(nes_t *nes)
{
    if (vrc_is_vrc4() && irq.enabled)
    {
        if (256 == ++irq.counter)
        {
            irq.counter = irq.latch;
            nes6502_irq();
            //irq.enabled = false;
            irq.enabled = irq.wait_state;
        }
    }
}

static void map_getstate(uint8 *state)
{
    state[0] = irq.counter;
    state[1] = irq.enabled;
    state[2] = irq.wait_state;
    state[3] = irq.latch;
    state[4] = prg_bank0;
    state[5] = prg_bank1;
    state[6] = prg_mode;
    for (int i = 0; i < 8; ++i)
    {
        state[8 + i] = lownybbles[i];
        state[16 + i] = highnybbles[i];
    }
}

static void map_setstate(uint8 *state)
{
    irq.counter = state[0];
    irq.enabled = state[1];
    irq.wait_state = state[2];
    irq.latch = state[3];
    prg_bank0 = state[4];
    prg_bank1 = state[5];
    prg_mode = state[6];
    for (int i = 0; i < 8; ++i)
    {
        lownybbles[i] = state[8 + i];
        highnybbles[i] = state[16 + i];
        vrc_update_chr(i);
    }
    vrc_update_prg();
}

static void map_init(rom_t *cart)
{
    irq.enabled = irq.wait_state = 0;
    irq.counter = irq.latch = 0;

    prg_bank0 = 0;
    prg_bank1 = 1;
    prg_mode = 0;
    for (int i = 0; i < 8; ++i)
    {
        lownybbles[i] = 0;
        highnybbles[i] = 0;
    }

    vrc_variant = VRC_VARIANT_VRC4A;
    if (cart->mapper_number == 25)
    {
        switch (cart->submapper)
        {
        case 2:
            vrc_variant = VRC_VARIANT_VRC4D;
            break;
        case 3:
            vrc_variant = VRC_VARIANT_VRC2C;
            break;
        case 0:
        case 1:
        default:
            vrc_variant = VRC_VARIANT_VRC4B;
            break;
        }
    }
    else if (cart->mapper_number == 21)
    {
        vrc_variant = (cart->submapper == 2) ? VRC_VARIANT_VRC4C : VRC_VARIANT_VRC4A;
    }

    vrc_use_heuristics = (cart->submapper == 0);
    MESSAGE_INFO("VRC2/4: mapper=%d submapper=%d variant=%s heuristics=%d\n",
                 cart->mapper_number, cart->submapper, vrc_variant_name(), vrc_use_heuristics);
    vrc_update_prg();
}


mapintf_t map21_intf =
{
    .number     = 21,
    .name       = "Konami VRC4 A",
    .init       = map_init,
    .vblank     = NULL,
    .hblank     = map_hblank,
    .get_state  = map_getstate,
    .set_state  = map_setstate,
    .mem_read   = {},
    .mem_write  = {
        { 0x8000, 0xFFFF, map_write }
    },
};

mapintf_t map25_intf =
{
    .number     = 25,
    .name       = "Konami VRC4 B",
    .init       = map_init,
    .vblank     = NULL,
    .hblank     = map_hblank,
    .get_state  = map_getstate,
    .set_state  = map_setstate,
    .mem_read   = {},
    .mem_write  = {
        { 0x8000, 0xFFFF, map_write }
    },
};
