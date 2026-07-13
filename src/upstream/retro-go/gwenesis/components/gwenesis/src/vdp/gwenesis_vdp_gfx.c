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
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#if defined(RETRO_GO)
#include <rg_system.h>
#endif
#include "m68k.h"
#include "gwenesis_vdp.h"
#include "gwenesis_io.h"
#include "gwenesis_bus.h"
#include "gwenesis_savestate.h"

//#include <assert.h>

#define GWENESIS_HOT

#if GNW_TARGET_MARIO !=0 || GNW_TARGET_ZELDA!=0
  #pragma GCC optimize("Ofast")
#endif

#if GNW_TARGET_MARIO != 0 | GNW_TARGET_ZELDA != 0

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
#include "stm32h7b0xx.h"
extern unsigned char* VRAM;

#else

#include <stdint.h>
extern unsigned char *VRAM;

#endif

#if defined(RETRO_GO)
extern unsigned short *CRAM;            // CRAM - Palettes
extern unsigned char *SAT_CACHE;        // Sprite cache
extern unsigned char *gwenesis_vdp_regs; // Registers
extern unsigned short *CRAM565;         // CRAM - Palettes
extern unsigned short *VSRAM;           // VSRAM - Scrolling
#else
extern unsigned short CRAM[];            // CRAM - Palettes
extern unsigned char SAT_CACHE[]__attribute__((aligned(4)));        // Sprite cache
extern unsigned char gwenesis_vdp_regs[]; // Registers
extern unsigned short CRAM565[];    // CRAM - Palettes
extern unsigned short VSRAM[];        // VSRAM - Scrolling
#endif

// Define screen buffers: original and scaled for host RGB
unsigned char *screen, *scaled_screen;

    // Overflow is the maximum size we can draw outside to avoid
    // wasting time and code in clipping. The maximum object is a 4x4 sprite,
    // so 32 pixels (on both side) is enough.

enum { PIX_OVERFLOW = 32 };

#define VDP_GFX_LINE_BUFFER_SIZE (SCREEN_WIDTH + PIX_OVERFLOW * 2)
#if !defined(RETRO_GO)
static uint8_t render_buffer[VDP_GFX_LINE_BUFFER_SIZE];
static uint8_t sprite_buffer[VDP_GFX_LINE_BUFFER_SIZE];
#endif

#if defined(RG_TARGET_HOLO_DYNMOD)
#define VDP_TILE_ROW_CACHE_ENABLED 0
#else
#define VDP_TILE_ROW_CACHE_ENABLED 0
#endif

#if defined(RG_TARGET_HOLO_DYNMOD)
enum {
  VDP_SPRITE_VISIBLE_LINES = 240,
  VDP_SPRITE_LINE_MAX = 20,
#if VDP_TILE_ROW_CACHE_ENABLED
  VDP_TILE_ROW_CACHE_ENTRIES = 512,
};

typedef struct {
  uint16_t key;
  uint8_t opaque_mask;
  uint8_t flags;
  uint32_t pix_lo;
  uint32_t pix_hi;
} vdp_tile_row_cache_entry_t;
#else
};
#endif

#if VDP_TILE_ROW_CACHE_ENABLED
static void vdp_tile_row_cache_clear(void);
typedef char vdp_tile_row_cache_entry_size_must_be_12[(sizeof(vdp_tile_row_cache_entry_t) == 12) ? 1 : -1];
#endif
#endif

// Define VIDEO MODE
int mode_pal;

// Define screen W/H
int screen_width;
int screen_height;

int gwenesis_H32upscaler;

int sprite_overflow;
bool sprite_collision;

#if defined(RETRO_GO)
typedef struct
{
  uint8_t *screen_buffer_line;
  uint8_t *screen_buffer;
  uint8_t *render_buffer;
  uint8_t *sprite_buffer;
#if defined(RG_TARGET_HOLO_DYNMOD)
  uint8_t *sprite_line_count;
  uint8_t *sprite_line_table;
#if VDP_TILE_ROW_CACHE_ENABLED
  vdp_tile_row_cache_entry_t *tile_row_cache;
#endif
#endif
  int mode_h40;
#if GWENESIS_VDP_ASYNC_ENABLED
  const gwenesis_vdp_render_context_t *render_ctx;
  int ctx_sprite_overflow;
#if GWENESIS_VDP_GFX_ACTIVE_PTR_TEST
  unsigned char *active_vram;
  unsigned short *active_vsram;
  unsigned short *active_cram565;
  unsigned char *active_sat_cache;
  unsigned char *active_regs;
  int active_screen_width;
  int active_screen_height;
#endif
#endif
  int base_w;
  int PlanA_firstcol;
  int PlanA_lastcol;
  int Window_firstcol;
  int Window_lastcol;
  uint16_t ntwidth_x2;
  uint16_t ntw_mask;
  uint16_t nth_mask;
} gwenesis_vdp_gfx_fast_state_t;

static gwenesis_vdp_gfx_fast_state_t gwenesis_vdp_gfx_fast_state_fallback;
static gwenesis_vdp_gfx_fast_state_t *gwenesis_vdp_gfx_fast_state = &gwenesis_vdp_gfx_fast_state_fallback;

#define screen_buffer_line (gwenesis_vdp_gfx_fast_state->screen_buffer_line)
#define screen_buffer (gwenesis_vdp_gfx_fast_state->screen_buffer)
#define render_buffer (gwenesis_vdp_gfx_fast_state->render_buffer)
#define sprite_buffer (gwenesis_vdp_gfx_fast_state->sprite_buffer)
#if defined(RG_TARGET_HOLO_DYNMOD)
#define sprite_line_count (gwenesis_vdp_gfx_fast_state->sprite_line_count)
#define sprite_line_table (gwenesis_vdp_gfx_fast_state->sprite_line_table)
#if VDP_TILE_ROW_CACHE_ENABLED
#define tile_row_cache (gwenesis_vdp_gfx_fast_state->tile_row_cache)
#endif
#endif
#define mode_h40 (gwenesis_vdp_gfx_fast_state->mode_h40)
#if GWENESIS_VDP_ASYNC_ENABLED
#define gwenesis_vdp_gfx_render_ctx (gwenesis_vdp_gfx_fast_state->render_ctx)
#define gwenesis_vdp_gfx_ctx_sprite_overflow (gwenesis_vdp_gfx_fast_state->ctx_sprite_overflow)
#if GWENESIS_VDP_GFX_ACTIVE_PTR_TEST
#define gwenesis_vdp_gfx_active_vram (gwenesis_vdp_gfx_fast_state->active_vram)
#define gwenesis_vdp_gfx_active_vsram (gwenesis_vdp_gfx_fast_state->active_vsram)
#define gwenesis_vdp_gfx_active_cram565 (gwenesis_vdp_gfx_fast_state->active_cram565)
#define gwenesis_vdp_gfx_active_sat_cache (gwenesis_vdp_gfx_fast_state->active_sat_cache)
#define gwenesis_vdp_gfx_active_regs (gwenesis_vdp_gfx_fast_state->active_regs)
#define gwenesis_vdp_gfx_active_screen_width (gwenesis_vdp_gfx_fast_state->active_screen_width)
#define gwenesis_vdp_gfx_active_screen_height (gwenesis_vdp_gfx_fast_state->active_screen_height)
#endif
#endif
#define base_w (gwenesis_vdp_gfx_fast_state->base_w)
#define PlanA_firstcol (gwenesis_vdp_gfx_fast_state->PlanA_firstcol)
#define PlanA_lastcol (gwenesis_vdp_gfx_fast_state->PlanA_lastcol)
#define Window_firstcol (gwenesis_vdp_gfx_fast_state->Window_firstcol)
#define Window_lastcol (gwenesis_vdp_gfx_fast_state->Window_lastcol)
#define ntwidth_x2 (gwenesis_vdp_gfx_fast_state->ntwidth_x2)
#define ntw_mask (gwenesis_vdp_gfx_fast_state->ntw_mask)
#define nth_mask (gwenesis_vdp_gfx_fast_state->nth_mask)
#else
// Define screen buffers for embedded 565 format
static uint8_t *screen_buffer_line=0;
static uint8_t *screen_buffer=0;
static int mode_h40;
static int base_w;
static int PlanA_firstcol;
static int PlanA_lastcol;
static int Window_firstcol;
static int Window_lastcol;
static uint16_t ntwidth_x2;
static uint16_t ntw_mask, nth_mask;
#endif

#if GWENESIS_VDP_ASYNC_ENABLED
#if !defined(RETRO_GO)
static const gwenesis_vdp_render_context_t *gwenesis_vdp_gfx_render_ctx;
static int gwenesis_vdp_gfx_ctx_sprite_overflow;
#endif

#if GWENESIS_VDP_GFX_ACTIVE_PTR_TEST
#if !defined(RETRO_GO)
static unsigned char *gwenesis_vdp_gfx_active_vram;
static unsigned short *gwenesis_vdp_gfx_active_vsram;
static unsigned short *gwenesis_vdp_gfx_active_cram565;
static unsigned char *gwenesis_vdp_gfx_active_sat_cache;
static unsigned char *gwenesis_vdp_gfx_active_regs;
static int gwenesis_vdp_gfx_active_screen_width;
static int gwenesis_vdp_gfx_active_screen_height;
#endif

static inline bool gwenesis_vdp_gfx_has_render_context(void)
{
  return gwenesis_vdp_gfx_render_ctx != NULL;
}

static inline int gwenesis_vdp_gfx_sprite_overflow(void)
{
  return gwenesis_vdp_gfx_render_ctx ? gwenesis_vdp_gfx_ctx_sprite_overflow : sprite_overflow;
}

static inline void gwenesis_vdp_gfx_set_sprite_overflow(int line)
{
  if (gwenesis_vdp_gfx_render_ctx)
    gwenesis_vdp_gfx_ctx_sprite_overflow = line;
  else
    sprite_overflow = line;
}

void gwenesis_vdp_gfx_set_render_context(const gwenesis_vdp_render_context_t *ctx)
{
  gwenesis_vdp_gfx_render_ctx = ctx;
  gwenesis_vdp_gfx_ctx_sprite_overflow = 0;

  if (ctx)
  {
    gwenesis_vdp_gfx_active_vram = (unsigned char *)ctx->vram;
    gwenesis_vdp_gfx_active_vsram = (unsigned short *)ctx->vsram;
    gwenesis_vdp_gfx_active_cram565 = (unsigned short *)ctx->cram565;
    gwenesis_vdp_gfx_active_sat_cache = (unsigned char *)ctx->sat_cache;
    gwenesis_vdp_gfx_active_regs = (unsigned char *)ctx->regs;
    gwenesis_vdp_gfx_active_screen_width = ctx->screen_width;
    gwenesis_vdp_gfx_active_screen_height = ctx->screen_height;
  }
  else
  {
    gwenesis_vdp_gfx_active_vram = VRAM;
    gwenesis_vdp_gfx_active_vsram = VSRAM;
    gwenesis_vdp_gfx_active_cram565 = CRAM565;
    gwenesis_vdp_gfx_active_sat_cache = SAT_CACHE;
    gwenesis_vdp_gfx_active_regs = gwenesis_vdp_regs;
    gwenesis_vdp_gfx_active_screen_width = screen_width;
    gwenesis_vdp_gfx_active_screen_height = screen_height;
  }
}

#define VRAM gwenesis_vdp_gfx_active_vram
#define VSRAM gwenesis_vdp_gfx_active_vsram
#define CRAM565 gwenesis_vdp_gfx_active_cram565
#define SAT_CACHE gwenesis_vdp_gfx_active_sat_cache
#define gwenesis_vdp_regs gwenesis_vdp_gfx_active_regs
#define screen_width gwenesis_vdp_gfx_active_screen_width
#define screen_height gwenesis_vdp_gfx_active_screen_height

#else
static inline unsigned char *gwenesis_vdp_gfx_vram(void)
{
  return (unsigned char *)(gwenesis_vdp_gfx_render_ctx ? gwenesis_vdp_gfx_render_ctx->vram : VRAM);
}

static inline unsigned short *gwenesis_vdp_gfx_vsram(void)
{
  return (unsigned short *)(gwenesis_vdp_gfx_render_ctx ? gwenesis_vdp_gfx_render_ctx->vsram : VSRAM);
}

static inline unsigned short *gwenesis_vdp_gfx_cram565(void)
{
  return (unsigned short *)(gwenesis_vdp_gfx_render_ctx ? gwenesis_vdp_gfx_render_ctx->cram565 : CRAM565);
}

static inline unsigned char *gwenesis_vdp_gfx_sat_cache(void)
{
  return (unsigned char *)(gwenesis_vdp_gfx_render_ctx ? gwenesis_vdp_gfx_render_ctx->sat_cache : SAT_CACHE);
}

static inline unsigned char *gwenesis_vdp_gfx_regs(void)
{
  return (unsigned char *)(gwenesis_vdp_gfx_render_ctx ? gwenesis_vdp_gfx_render_ctx->regs : gwenesis_vdp_regs);
}

static inline int gwenesis_vdp_gfx_screen_width(void)
{
  return gwenesis_vdp_gfx_render_ctx ? gwenesis_vdp_gfx_render_ctx->screen_width : screen_width;
}

static inline int gwenesis_vdp_gfx_screen_height(void)
{
  return gwenesis_vdp_gfx_render_ctx ? gwenesis_vdp_gfx_render_ctx->screen_height : screen_height;
}

static inline bool gwenesis_vdp_gfx_has_render_context(void)
{
  return gwenesis_vdp_gfx_render_ctx != NULL;
}

static inline int gwenesis_vdp_gfx_sprite_overflow(void)
{
  return gwenesis_vdp_gfx_render_ctx ? gwenesis_vdp_gfx_ctx_sprite_overflow : sprite_overflow;
}

static inline void gwenesis_vdp_gfx_set_sprite_overflow(int line)
{
  if (gwenesis_vdp_gfx_render_ctx)
    gwenesis_vdp_gfx_ctx_sprite_overflow = line;
  else
    sprite_overflow = line;
}

void gwenesis_vdp_gfx_set_render_context(const gwenesis_vdp_render_context_t *ctx)
{
  gwenesis_vdp_gfx_render_ctx = ctx;
  gwenesis_vdp_gfx_ctx_sprite_overflow = 0;
}

#define VRAM gwenesis_vdp_gfx_vram()
#define VSRAM gwenesis_vdp_gfx_vsram()
#define CRAM565 gwenesis_vdp_gfx_cram565()
#define SAT_CACHE gwenesis_vdp_gfx_sat_cache()
#define gwenesis_vdp_regs gwenesis_vdp_gfx_regs()
#define screen_width gwenesis_vdp_gfx_screen_width()
#define screen_height gwenesis_vdp_gfx_screen_height()
#endif
#else
static inline int gwenesis_vdp_gfx_sprite_overflow(void)
{
  return sprite_overflow;
}

static inline void gwenesis_vdp_gfx_set_sprite_overflow(int line)
{
  sprite_overflow = line;
}

static inline bool gwenesis_vdp_gfx_has_render_context(void)
{
  return false;
}

void gwenesis_vdp_gfx_set_render_context(const gwenesis_vdp_render_context_t *ctx)
{
  (void)ctx;
}
#endif

static inline bool gwenesis_vdp_gfx_has_palette_remap(void)
{
#if GWENESIS_VDP_ASYNC_ENABLED
  return gwenesis_vdp_gfx_render_ctx &&
         gwenesis_vdp_gfx_render_ctx->palette_remap &&
         gwenesis_vdp_gfx_render_ctx->palette_remap->output_palette;
#else
  return false;
#endif
}

static inline uint8_t gwenesis_vdp_gfx_remap_palette_index(uint8_t source)
{
#if GWENESIS_VDP_ASYNC_ENABLED
  gwenesis_vdp_palette_remap_t *remap = gwenesis_vdp_gfx_render_ctx->palette_remap;
  const uint16_t color = CRAM565[source];
  if (!remap->source_valid[source] || remap->source_color[source] != color)
  {
    unsigned int mapped = 0;
    for (; mapped < remap->color_count; ++mapped)
    {
      if (remap->output_palette[mapped] == color)
        break;
    }
    if (mapped == remap->color_count)
    {
      if (remap->color_count < 256)
      {
        remap->output_palette[remap->color_count++] = color;
      }
      else
      {
        remap->overflow = true;
        mapped = remap->source_valid[source] ? remap->source_map[source] : 0;
      }
    }
    remap->source_color[source] = color;
    remap->source_map[source] = (uint8_t)mapped;
    remap->source_valid[source] = 1;
  }
  return remap->source_map[source];
#else
  return source;
#endif
}

// 16 bits access to VRAM
// #define FETCH16VRAM(A)  ({size_t addr = (A); (VRAM[addr+1]) | (VRAM[addr] << 8);})
#define FETCH16VRAM(A)  ( (VRAM[(A)+1]) | (VRAM[(A)] << 8) )
#define VDP_GFX_DISABLE_LOGGING 1

bool gwenesis_vdp_gfx_init_fast_ram(void)
{
#if defined(RETRO_GO)
    if (gwenesis_vdp_gfx_fast_state == &gwenesis_vdp_gfx_fast_state_fallback)
    {
        gwenesis_vdp_gfx_fast_state_t *ptr = rg_alloc(sizeof(*ptr), MEM_FAST | MEM_NOPANIC);
#if defined(RG_TARGET_HOLO_DYNMOD)
        if (ptr && PTR_IN_SPIRAM(ptr))
        {
            free(ptr);
            ptr = NULL;
        }
#endif
        if (!ptr)
            return false;
        *ptr = *gwenesis_vdp_gfx_fast_state;
        gwenesis_vdp_gfx_fast_state = ptr;
    }
    if (!render_buffer)
        render_buffer = rg_alloc(VDP_GFX_LINE_BUFFER_SIZE, MEM_FAST | MEM_NOPANIC);
    if (!sprite_buffer)
        sprite_buffer = rg_alloc(VDP_GFX_LINE_BUFFER_SIZE, MEM_FAST | MEM_NOPANIC);
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!sprite_line_count)
        sprite_line_count = rg_alloc(VDP_SPRITE_VISIBLE_LINES, MEM_FAST | MEM_NOPANIC);
    if (!sprite_line_table)
        sprite_line_table = rg_alloc(VDP_SPRITE_VISIBLE_LINES * VDP_SPRITE_LINE_MAX, MEM_FAST | MEM_NOPANIC);
#if VDP_TILE_ROW_CACHE_ENABLED
    if (!tile_row_cache)
        tile_row_cache = rg_alloc(sizeof(*tile_row_cache) * VDP_TILE_ROW_CACHE_ENTRIES, MEM_FAST | MEM_NOPANIC);
    if (tile_row_cache)
        vdp_tile_row_cache_clear();
    const bool ok = render_buffer && sprite_buffer && sprite_line_count && sprite_line_table && tile_row_cache;
#else
    const bool ok = render_buffer && sprite_buffer && sprite_line_count && sprite_line_table;
#endif
#if GWENESIS_VDP_GFX_ACTIVE_PTR_TEST
    if (ok)
        gwenesis_vdp_gfx_set_render_context(NULL);
#endif
    return ok;
#endif
#endif
    return render_buffer && sprite_buffer;
}

void gwenesis_vdp_gfx_deinit_fast_ram(void)
{
#if defined(RETRO_GO)
    free(render_buffer);
    free(sprite_buffer);
#if defined(RG_TARGET_HOLO_DYNMOD)
    free(sprite_line_count);
    free(sprite_line_table);
#if VDP_TILE_ROW_CACHE_ENABLED
    free(tile_row_cache);
#endif
#endif
    render_buffer = NULL;
    sprite_buffer = NULL;
#if defined(RG_TARGET_HOLO_DYNMOD)
    sprite_line_count = NULL;
    sprite_line_table = NULL;
#if VDP_TILE_ROW_CACHE_ENABLED
    tile_row_cache = NULL;
#endif
#endif
    if (gwenesis_vdp_gfx_fast_state != &gwenesis_vdp_gfx_fast_state_fallback)
    {
        free(gwenesis_vdp_gfx_fast_state);
        gwenesis_vdp_gfx_fast_state = &gwenesis_vdp_gfx_fast_state_fallback;
    }
    memset(&gwenesis_vdp_gfx_fast_state_fallback, 0, sizeof(gwenesis_vdp_gfx_fast_state_fallback));
#endif
}

#if !VDP_GFX_DISABLE_LOGGING
#include <stdarg.h>
void vdpg_log(const char *subs, const char *fmt, ...) {
  extern int frame_counter;
  extern int scan_line;

  va_list va;

  printf("%06d:%03d :[%s] vc:%03x hc:%03x", frame_counter, scan_line, subs,gwenesis_vdp_vcounter(),gwenesis_vdp_hcounter());

  va_start(va, fmt);
  vfprintf(stdout, fmt, va);
  va_end(va);
  printf("\n");
}
#else
	#define vdpg_log(...)  do {} while(0)
#endif
/******************************************************************************
 *
 *  set screen buffers in which the rendering occurs
 *  Set original and scaled screen buffer for host
 *
 ******************************************************************************/
//host
void gwenesis_vdp_set_buffers(unsigned char *host_screen_buffer, unsigned char *scaled_buffer)
{
    screen = host_screen_buffer;
    scaled_screen = scaled_buffer;
}
//embedded
void gwenesis_vdp_set_buffer(unsigned short *ptr_screen_buffer)
{
    screen_buffer_line = (uint8_t *)ptr_screen_buffer;
    screen_buffer = (uint8_t *)ptr_screen_buffer;
}

/******************************************************************************
 *
 *  Draw  Sprite character /8pixels in row
 *  without checking overdraw for pixels collision detection
 *  with Horizontal flip variation
 *  for Shadow/highlight :
 *    draw in fresh line buffer using draw_pattern_xxfliph_sprite(..)
 *  otherwise:
 *   draw over dirty planes using draw_pattern_xxfliph_sprite_over_planes(..)
 *
 ******************************************************************************/

 #define PIX0(P) ( ((P) & 0x000000F0 ) >>   4 )
 #define PIX1(P) ( ((P) & 0x0000000F ) >>   0 )
 #define PIX2(P) ( ((P) & 0x0000F000 ) >>  12 )
 #define PIX3(P) ( ((P) & 0x00000F00 ) >>   8 )
 #define PIX4(P) ( ((P) & 0x00F00000 ) >>  20 )
 #define PIX5(P) ( ((P) & 0x000F0000 ) >>  16 )
 #define PIX6(P) ( ((P) & 0xF0000000 ) >>  28 )
 #define PIX7(P) ( ((P) & 0x0F000000 ) >>  24 )

typedef uint32_t vdp_u32_alias_t __attribute__((may_alias));

static inline __attribute__((always_inline))
bool vdp_u32_aligned_ptr(const void *ptr)
{
  return (((uintptr_t)ptr & 3U) == 0);
}

static inline __attribute__((always_inline))
uint32_t vdp_load_u32_aligned(const void *src)
{
  return *(const vdp_u32_alias_t *)src;
}

static inline __attribute__((always_inline))
void vdp_store_u32_aligned(void *dst, uint32_t value)
{
  *(vdp_u32_alias_t *)dst = value;
}

#if defined(RG_TARGET_HOLO_DYNMOD) && VDP_TILE_ROW_CACHE_ENABLED
#define VDP_TILE_ROW_CACHE_INVALID_KEY 0xFFFFU
#define VDP_BYTE_WORD(V) ((uint32_t)(uint8_t)(V) * 0x01010101U)

static const uint32_t vdp_mask4_to_32[16] = {
  0x00000000U, 0x000000FFU, 0x0000FF00U, 0x0000FFFFU,
  0x00FF0000U, 0x00FF00FFU, 0x00FFFF00U, 0x00FFFFFFU,
  0xFF000000U, 0xFF0000FFU, 0xFF00FF00U, 0xFF00FFFFU,
  0xFFFF0000U, 0xFFFF00FFU, 0xFFFFFF00U, 0xFFFFFFFFU,
};

static inline __attribute__((always_inline))
uint32_t vdp_load_u32(const void *src)
{
  uint32_t value;
  memcpy(&value, src, sizeof(value));
  return value;
}

static inline __attribute__((always_inline))
void vdp_store_u32(void *dst, uint32_t value)
{
  memcpy(dst, &value, sizeof(value));
}

static inline __attribute__((always_inline))
uint16_t vdp_tile_row_cache_key(uint16_t tile, uint8_t row, bool hflip)
{
  return (uint16_t)(((tile & 0x07FFU) << 4) | ((row & 7U) << 1) | (hflip ? 1U : 0U));
}

static inline __attribute__((always_inline))
unsigned int vdp_tile_row_cache_index(uint16_t key)
{
  uint32_t hash = (uint32_t)key * 2654435761U;
  hash ^= hash >> 16;
  return hash & (VDP_TILE_ROW_CACHE_ENTRIES - 1);
}

static inline __attribute__((always_inline))
uint32_t vdp_pack4(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3)
{
  return (uint32_t)p0 | ((uint32_t)p1 << 8) | ((uint32_t)p2 << 16) | ((uint32_t)p3 << 24);
}

static inline __attribute__((always_inline))
uint8_t vdp_opaque_mask8(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3,
                         uint8_t p4, uint8_t p5, uint8_t p6, uint8_t p7)
{
  return (uint8_t)(((p0 != 0) << 0) | ((p1 != 0) << 1) |
                   ((p2 != 0) << 2) | ((p3 != 0) << 3) |
                   ((p4 != 0) << 4) | ((p5 != 0) << 5) |
                   ((p6 != 0) << 6) | ((p7 != 0) << 7));
}

static inline __attribute__((always_inline))
uint32_t vdp_fetch_tile_row_pattern(uint16_t tile, uint8_t row)
{
  return *(const uint32_t *)(VRAM + ((uint32_t)tile << 5) + ((uint32_t)(row & 7U) << 2));
}

static void vdp_tile_row_cache_clear(void)
{
  if (!tile_row_cache)
    return;

  for (unsigned int i = 0; i < VDP_TILE_ROW_CACHE_ENTRIES; ++i)
  {
    tile_row_cache[i].key = VDP_TILE_ROW_CACHE_INVALID_KEY;
    tile_row_cache[i].opaque_mask = 0;
    tile_row_cache[i].flags = 0;
  }
}

void gwenesis_vdp_gfx_invalidate_tile_cache(void)
{
  vdp_tile_row_cache_clear();
}

void gwenesis_vdp_gfx_invalidate_vram(unsigned int address)
{
  if (!tile_row_cache)
    return;

  const uint16_t tile = (uint16_t)((address & 0xFFFFU) >> 5);
  const uint8_t row = (uint8_t)((address >> 2) & 7U);
  const uint16_t key = vdp_tile_row_cache_key(tile, row, false);
  const uint16_t hflip_key = (uint16_t)(key | 1U);
  vdp_tile_row_cache_entry_t *entry = &tile_row_cache[vdp_tile_row_cache_index(key)];
  vdp_tile_row_cache_entry_t *hflip_entry = &tile_row_cache[vdp_tile_row_cache_index(hflip_key)];

  if (entry->key == key)
    entry->key = VDP_TILE_ROW_CACHE_INVALID_KEY;
  if (hflip_entry->key == hflip_key)
    hflip_entry->key = VDP_TILE_ROW_CACHE_INVALID_KEY;
}

static inline __attribute__((always_inline))
const vdp_tile_row_cache_entry_t *vdp_tile_row_cache_get(uint16_t name, int paty)
{
  const uint16_t tile = name & 0x07FFU;
  const uint8_t row = (name & 0x1000U) ? (uint8_t)(7 - (paty & 7)) : (uint8_t)(paty & 7);
  const bool hflip = (name & 0x0800U) != 0;
  const uint16_t key = vdp_tile_row_cache_key(tile, row, hflip);
  vdp_tile_row_cache_entry_t *entry = &tile_row_cache[vdp_tile_row_cache_index(key)];

  if (entry->key != key)
  {
    const uint32_t pattern = vdp_fetch_tile_row_pattern(tile, row);
    const uint8_t p0 = PIX0(pattern);
    const uint8_t p1 = PIX1(pattern);
    const uint8_t p2 = PIX2(pattern);
    const uint8_t p3 = PIX3(pattern);
    const uint8_t p4 = PIX4(pattern);
    const uint8_t p5 = PIX5(pattern);
    const uint8_t p6 = PIX6(pattern);
    const uint8_t p7 = PIX7(pattern);

    if (hflip)
    {
      entry->pix_lo = vdp_pack4(p7, p6, p5, p4);
      entry->pix_hi = vdp_pack4(p3, p2, p1, p0);
      entry->opaque_mask = vdp_opaque_mask8(p7, p6, p5, p4, p3, p2, p1, p0);
    }
    else
    {
      entry->pix_lo = vdp_pack4(p0, p1, p2, p3);
      entry->pix_hi = vdp_pack4(p4, p5, p6, p7);
      entry->opaque_mask = vdp_opaque_mask8(p0, p1, p2, p3, p4, p5, p6, p7);
    }

    entry->flags = 0;
    entry->key = key;
  }

  return entry;
}

static inline __attribute__((always_inline))
uint8_t vdp_u32_byte_any_mask4(uint32_t value, uint8_t bits)
{
  const uint32_t masked = value & VDP_BYTE_WORD(bits);
  return (uint8_t)(((masked & 0x000000FFU) ? 0x1U : 0U) |
                   ((masked & 0x0000FF00U) ? 0x2U : 0U) |
                   ((masked & 0x00FF0000U) ? 0x4U : 0U) |
                   ((masked & 0xFF000000U) ? 0x8U : 0U));
}

static inline __attribute__((always_inline))
void vdp_blend_cached_4(uint8_t *scr, uint32_t pix, uint8_t opaque_bits,
                        uint32_t attr_word, uint8_t block_bits)
{
  if (!opaque_bits)
    return;

  const uint32_t dst = vdp_load_u32(scr);
  uint8_t draw_bits = opaque_bits;

  if (block_bits)
    draw_bits &= (uint8_t)~vdp_u32_byte_any_mask4(dst, block_bits);
  if (!draw_bits)
    return;

  const uint32_t mask = vdp_mask4_to_32[draw_bits & 0x0F];
  const uint32_t src = pix | (attr_word & mask);
  vdp_store_u32(scr, (dst & ~mask) | (src & mask));
}

static inline __attribute__((always_inline))
void vdp_store_plane_b_cached_4(uint8_t *scr, uint32_t pix, uint8_t opaque_bits,
                                uint32_t attr_word, uint32_t back_word)
{
  const uint32_t mask = vdp_mask4_to_32[opaque_bits & 0x0F];
  const uint32_t src = pix | (attr_word & mask);
  vdp_store_u32(scr, (back_word & ~mask) | (src & mask));
}

static inline __attribute__((always_inline))
bool vdp_try_store_opaque_cached_8(uint8_t *scr, const vdp_tile_row_cache_entry_t *row,
                                   uint32_t attr_word, uint8_t block_bits)
{
  if (row->opaque_mask != 0xFF || !vdp_u32_aligned_ptr(scr))
    return false;

  if (block_bits)
  {
    const uint32_t dst_lo = vdp_load_u32_aligned(scr);
    const uint32_t dst_hi = vdp_load_u32_aligned(scr + 4);
    if (vdp_u32_byte_any_mask4(dst_lo, block_bits) ||
        vdp_u32_byte_any_mask4(dst_hi, block_bits))
      return false;
  }

  vdp_store_u32_aligned(scr, row->pix_lo | attr_word);
  vdp_store_u32_aligned(scr + 4, row->pix_hi | attr_word);
  return true;
}

static inline __attribute__((always_inline))
void draw_cached_pattern_sprite(uint8_t *scr, const vdp_tile_row_cache_entry_t *row, uint8_t attrs)
{
  const uint8_t opaque = row->opaque_mask;
  const uint32_t attr_word = VDP_BYTE_WORD(attrs);

  if (!opaque)
    return;

  vdp_blend_cached_4(scr, row->pix_lo, opaque & 0x0F, attr_word, PIXATTR_SPRITE);
  vdp_blend_cached_4(scr + 4, row->pix_hi, opaque >> 4, attr_word, PIXATTR_SPRITE);
}

static inline __attribute__((always_inline))
void draw_cached_pattern_sprite_over_planes(uint8_t *scr, const vdp_tile_row_cache_entry_t *row, uint8_t attrs)
{
  const uint8_t opaque = row->opaque_mask;
  const uint8_t block_bits = (attrs & PIXATTR_HIPRI) ? PIXATTR_SPRITE : PIXATTR_SPRITE_HIPRI;
  const uint32_t attr_word = VDP_BYTE_WORD(attrs);

  if (!opaque)
    return;

  vdp_blend_cached_4(scr, row->pix_lo, opaque & 0x0F, attr_word, block_bits);
  vdp_blend_cached_4(scr + 4, row->pix_hi, opaque >> 4, attr_word, block_bits);
}

static inline __attribute__((always_inline))
void draw_cached_pattern_planeB(uint8_t *scr, const vdp_tile_row_cache_entry_t *row, uint8_t attrs)
{
  const uint8_t opaque = row->opaque_mask;
  const uint32_t attr_word = VDP_BYTE_WORD(attrs);
  const uint32_t back_word = VDP_BYTE_WORD(gwenesis_vdp_regs[7]);

  if (vdp_try_store_opaque_cached_8(scr, row, attr_word, 0))
    return;

  vdp_store_plane_b_cached_4(scr, row->pix_lo, opaque & 0x0F, attr_word, back_word);
  vdp_store_plane_b_cached_4(scr + 4, row->pix_hi, opaque >> 4, attr_word, back_word);
}

static inline __attribute__((always_inline))
void draw_cached_pattern_planeAoverB(uint8_t *scr, const vdp_tile_row_cache_entry_t *row, uint8_t attrs)
{
  const uint8_t opaque = row->opaque_mask;
  const uint8_t block_bits = (attrs & PIXATTR_HIPRI) ? 0 : PIXATTR_HIPRI;
  const uint32_t attr_word = VDP_BYTE_WORD(attrs);

  if (!opaque)
    return;
  if ((attrs & PIXATTR_HIPRI) && vdp_try_store_opaque_cached_8(scr, row, attr_word, 0))
    return;

  vdp_blend_cached_4(scr, row->pix_lo, opaque & 0x0F, attr_word, block_bits);
  vdp_blend_cached_4(scr + 4, row->pix_hi, opaque >> 4, attr_word, block_bits);
}
#else
void gwenesis_vdp_gfx_invalidate_tile_cache(void)
{
}

void gwenesis_vdp_gfx_invalidate_vram(unsigned int address)
{
  (void)address;
}
#endif

static inline __attribute__((always_inline))
void draw_pattern_nofliph_sprite(uint8_t *scr, uint32_t p, uint8_t attrs)
{
  if (p == 0) return;

  /*  not transparent pixel to write AND not already a sprite*/
  if (((PIX0(p))) && ((scr[0] & PIXATTR_SPRITE) == 0)) scr[0] = attrs | (PIX0(p));
  if (((PIX1(p))) && ((scr[1] & PIXATTR_SPRITE) == 0)) scr[1] = attrs | (PIX1(p));
  if (((PIX2(p))) && ((scr[2] & PIXATTR_SPRITE) == 0)) scr[2] = attrs | (PIX2(p));
  if (((PIX3(p))) && ((scr[3] & PIXATTR_SPRITE) == 0)) scr[3] = attrs | (PIX3(p));
  if (((PIX4(p))) && ((scr[4] & PIXATTR_SPRITE) == 0)) scr[4] = attrs | (PIX4(p));
  if (((PIX5(p))) && ((scr[5] & PIXATTR_SPRITE) == 0)) scr[5] = attrs | (PIX5(p));
  if (((PIX6(p))) && ((scr[6] & PIXATTR_SPRITE) == 0)) scr[6] = attrs | (PIX6(p));
  if (((PIX7(p))) && ((scr[7] & PIXATTR_SPRITE) == 0)) scr[7] = attrs | (PIX7(p));
}

static inline __attribute__((always_inline))
void draw_pattern_fliph_sprite(uint8_t *scr, uint32_t p, uint8_t attrs)
{
  if (p == 0) return;

  /*  not transparent pixel to write AND not already a sprite*/
  if (((PIX7(p))) && ((scr[0] & PIXATTR_SPRITE) == 0)) scr[0] = attrs | (PIX7(p));
  if (((PIX6(p))) && ((scr[1] & PIXATTR_SPRITE) == 0)) scr[1] = attrs | (PIX6(p));
  if (((PIX5(p))) && ((scr[2] & PIXATTR_SPRITE) == 0)) scr[2] = attrs | (PIX5(p));
  if (((PIX4(p))) && ((scr[3] & PIXATTR_SPRITE) == 0)) scr[3] = attrs | (PIX4(p));
  if (((PIX3(p))) && ((scr[4] & PIXATTR_SPRITE) == 0)) scr[4] = attrs | (PIX3(p));
  if (((PIX2(p))) && ((scr[5] & PIXATTR_SPRITE) == 0)) scr[5] = attrs | (PIX2(p));
  if (((PIX1(p))) && ((scr[6] & PIXATTR_SPRITE) == 0)) scr[6] = attrs | (PIX1(p));
  if (((PIX0(p))) && ((scr[7] & PIXATTR_SPRITE) == 0)) scr[7] = attrs | (PIX0(p));

}

static inline __attribute__((always_inline))
void draw_pattern_nofliph_sprite_over_planes(uint8_t *scr, uint32_t p, uint8_t attrs)
{
  if (p == 0) return; 

  /* High priority */
  if (attrs & PIXATTR_HIPRI) {

  /*  not transparent pixel to write AND not already a sprite*/
  if (((PIX0(p))) && ((scr[0] & PIXATTR_SPRITE) == 0)) scr[0] = attrs | (PIX0(p));
  if (((PIX1(p))) && ((scr[1] & PIXATTR_SPRITE) == 0)) scr[1] = attrs | (PIX1(p));
  if (((PIX2(p))) && ((scr[2] & PIXATTR_SPRITE) == 0)) scr[2] = attrs | (PIX2(p));
  if (((PIX3(p))) && ((scr[3] & PIXATTR_SPRITE) == 0)) scr[3] = attrs | (PIX3(p));
  if (((PIX4(p))) && ((scr[4] & PIXATTR_SPRITE) == 0)) scr[4] = attrs | (PIX4(p));
  if (((PIX5(p))) && ((scr[5] & PIXATTR_SPRITE) == 0)) scr[5] = attrs | (PIX5(p));
  if (((PIX6(p))) && ((scr[6] & PIXATTR_SPRITE) == 0)) scr[6] = attrs | (PIX6(p));
  if (((PIX7(p))) && ((scr[7] & PIXATTR_SPRITE) == 0)) scr[7] = attrs | (PIX7(p));

  }
  /* Low priority */
  else {

  /*  not transparent pixel to write AND not already a sprite or higher priority*/
  if (((PIX0(p))) && ((scr[0] & PIXATTR_SPRITE_HIPRI) == 0)) scr[0] = attrs | (PIX0(p));
  if (((PIX1(p))) && ((scr[1] & PIXATTR_SPRITE_HIPRI) == 0)) scr[1] = attrs | (PIX1(p));
  if (((PIX2(p))) && ((scr[2] & PIXATTR_SPRITE_HIPRI) == 0)) scr[2] = attrs | (PIX2(p));
  if (((PIX3(p))) && ((scr[3] & PIXATTR_SPRITE_HIPRI) == 0)) scr[3] = attrs | (PIX3(p));
  if (((PIX4(p))) && ((scr[4] & PIXATTR_SPRITE_HIPRI) == 0)) scr[4] = attrs | (PIX4(p));
  if (((PIX5(p))) && ((scr[5] & PIXATTR_SPRITE_HIPRI) == 0)) scr[5] = attrs | (PIX5(p));
  if (((PIX6(p))) && ((scr[6] & PIXATTR_SPRITE_HIPRI) == 0)) scr[6] = attrs | (PIX6(p));
  if (((PIX7(p))) && ((scr[7] & PIXATTR_SPRITE_HIPRI) == 0)) scr[7] = attrs | (PIX7(p));
  
  }
}

static inline __attribute__((always_inline))
void draw_pattern_fliph_sprite_over_planes(uint8_t *scr, uint32_t p, uint8_t attrs)
{
  if (p == 0) return;

  /* High priority */
  if (attrs & PIXATTR_HIPRI) {

  /*  not transparent pixel to write AND not already a sprite*/
  if (((PIX7(p))) && ((scr[0] & PIXATTR_SPRITE) == 0)) scr[0] = attrs | (PIX7(p));
  if (((PIX6(p))) && ((scr[1] & PIXATTR_SPRITE) == 0)) scr[1] = attrs | (PIX6(p));
  if (((PIX5(p))) && ((scr[2] & PIXATTR_SPRITE) == 0)) scr[2] = attrs | (PIX5(p));
  if (((PIX4(p))) && ((scr[3] & PIXATTR_SPRITE) == 0)) scr[3] = attrs | (PIX4(p));
  if (((PIX3(p))) && ((scr[4] & PIXATTR_SPRITE) == 0)) scr[4] = attrs | (PIX3(p));
  if (((PIX2(p))) && ((scr[5] & PIXATTR_SPRITE) == 0)) scr[5] = attrs | (PIX2(p));
  if (((PIX1(p))) && ((scr[6] & PIXATTR_SPRITE) == 0)) scr[6] = attrs | (PIX1(p));
  if (((PIX0(p))) && ((scr[7] & PIXATTR_SPRITE) == 0)) scr[7] = attrs | (PIX0(p));

  }
  /* Low priority */
  else {

  /*  not transparent pixel to write AND not already a sprite or higher priority*/
  if (((PIX7(p))) && ((scr[0] & PIXATTR_SPRITE_HIPRI) == 0)) scr[0] = attrs | (PIX7(p));
  if (((PIX6(p))) && ((scr[1] & PIXATTR_SPRITE_HIPRI) == 0)) scr[1] = attrs | (PIX6(p));
  if (((PIX5(p))) && ((scr[2] & PIXATTR_SPRITE_HIPRI) == 0)) scr[2] = attrs | (PIX5(p));
  if (((PIX4(p))) && ((scr[3] & PIXATTR_SPRITE_HIPRI) == 0)) scr[3] = attrs | (PIX4(p));
  if (((PIX3(p))) && ((scr[4] & PIXATTR_SPRITE_HIPRI) == 0)) scr[4] = attrs | (PIX3(p));
  if (((PIX2(p))) && ((scr[5] & PIXATTR_SPRITE_HIPRI) == 0)) scr[5] = attrs | (PIX2(p));
  if (((PIX1(p))) && ((scr[6] & PIXATTR_SPRITE_HIPRI) == 0)) scr[6] = attrs | (PIX1(p));
  if (((PIX0(p))) && ((scr[7] & PIXATTR_SPRITE_HIPRI) == 0)) scr[7] = attrs | (PIX0(p));
  
  }

}

/******************************************************************************
 *
 *  Draw  characters/8pixels in row
 *  without checking overdraw for pixels collision detection
 *  with Horizontal flip variation for plane A & B
 *
 ******************************************************************************/


static inline __attribute__((always_inline)) void
draw_pattern_nofliph_planeB(uint8_t *scr, uint32_t p, uint8_t attrs) {

  const uint8_t back = gwenesis_vdp_regs[7];

  if (p == 0) {

    scr[0] = back;
    scr[1] = back;
    scr[2] = back;
    scr[3] = back;
    scr[4] = back;
    scr[5] = back;
    scr[6] = back;
    scr[7] = back;

    return;
  }

  scr[0] = PIX0(p) ? attrs | (PIX0(p)) : back;
  scr[1] = PIX1(p) ? attrs | (PIX1(p)) : back;
  scr[2] = PIX2(p) ? attrs | (PIX2(p)) : back;
  scr[3] = PIX3(p) ? attrs | (PIX3(p)) : back;
  scr[4] = PIX4(p) ? attrs | (PIX4(p)) : back;
  scr[5] = PIX5(p) ? attrs | (PIX5(p)) : back;
  scr[6] = PIX6(p) ? attrs | (PIX6(p)) : back;
  scr[7] = PIX7(p) ? attrs | (PIX7(p)) : back;
}

static inline __attribute__((always_inline)) void
draw_pattern_fliph_planeB(uint8_t *scr, uint32_t p, uint8_t attrs) {

  const uint8_t back = gwenesis_vdp_regs[7];
  if (p == 0) {

    scr[0] = back;
    scr[1] = back;
    scr[2] = back;
    scr[3] = back;
    scr[4] = back;
    scr[5] = back;
    scr[6] = back;
    scr[7] = back;

    return;
  }

  scr[0] = PIX7(p) ? attrs | (PIX7(p)) : back;
  scr[1] = PIX6(p) ? attrs | (PIX6(p)) : back;
  scr[2] = PIX5(p) ? attrs | (PIX5(p)) : back;
  scr[3] = PIX4(p) ? attrs | (PIX4(p)) : back;
  scr[4] = PIX3(p) ? attrs | (PIX3(p)) : back;
  scr[5] = PIX2(p) ? attrs | (PIX2(p)) : back;
  scr[6] = PIX1(p) ? attrs | (PIX1(p)) : back;
  scr[7] = PIX0(p) ? attrs | (PIX0(p)) : back;


}

static inline __attribute__((always_inline)) void
draw_pattern_nofliph_planeAoverB(uint8_t *scr, uint32_t p, uint8_t attrs) {

  if (p == 0) return;

  if (attrs & PIXATTR_HIPRI) {

    if (PIX0(p)) scr[0] = attrs | (PIX0(p));
    if (PIX1(p)) scr[1] = attrs | (PIX1(p));
    if (PIX2(p)) scr[2] = attrs | (PIX2(p));
    if (PIX3(p)) scr[3] = attrs | (PIX3(p));
    if (PIX4(p)) scr[4] = attrs | (PIX4(p));
    if (PIX5(p)) scr[5] = attrs | (PIX5(p));
    if (PIX6(p)) scr[6] = attrs | (PIX6(p));
    if (PIX7(p)) scr[7] = attrs | (PIX7(p));

  } else {

    if (PIX0(p) && ((scr[0] & PIXATTR_HIPRI) == 0)) scr[0] = attrs | (PIX0(p));
    if (PIX1(p) && ((scr[1] & PIXATTR_HIPRI) == 0)) scr[1] = attrs | (PIX1(p));
    if (PIX2(p) && ((scr[2] & PIXATTR_HIPRI) == 0)) scr[2] = attrs | (PIX2(p));
    if (PIX3(p) && ((scr[3] & PIXATTR_HIPRI) == 0)) scr[3] = attrs | (PIX3(p));
    if (PIX4(p) && ((scr[4] & PIXATTR_HIPRI) == 0)) scr[4] = attrs | (PIX4(p));
    if (PIX5(p) && ((scr[5] & PIXATTR_HIPRI) == 0)) scr[5] = attrs | (PIX5(p));
    if (PIX6(p) && ((scr[6] & PIXATTR_HIPRI) == 0)) scr[6] = attrs | (PIX6(p));
    if (PIX7(p) && ((scr[7] & PIXATTR_HIPRI) == 0)) scr[7] = attrs | (PIX7(p));

  }
}

static inline __attribute__((always_inline)) void
draw_pattern_fliph_planeAoverB(uint8_t *scr, uint32_t p, uint8_t attrs) {

    if (p == 0) return;

    if (attrs & PIXATTR_HIPRI) {

    if (PIX7(p)) scr[0] = attrs | (PIX7(p));
    if (PIX6(p)) scr[1] = attrs | (PIX6(p));
    if (PIX5(p)) scr[2] = attrs | (PIX5(p));
    if (PIX4(p)) scr[3] = attrs | (PIX4(p));
    if (PIX3(p)) scr[4] = attrs | (PIX3(p));
    if (PIX2(p)) scr[5] = attrs | (PIX2(p));
    if (PIX1(p)) scr[6] = attrs | (PIX1(p));
    if (PIX0(p)) scr[7] = attrs | (PIX0(p));

  } else {

    if (PIX7(p) && ((scr[0] & PIXATTR_HIPRI) == 0)) scr[0] = attrs | (PIX7(p));
    if (PIX6(p) && ((scr[1] & PIXATTR_HIPRI) == 0)) scr[1] = attrs | (PIX6(p));
    if (PIX5(p) && ((scr[2] & PIXATTR_HIPRI) == 0)) scr[2] = attrs | (PIX5(p));
    if (PIX4(p) && ((scr[3] & PIXATTR_HIPRI) == 0)) scr[3] = attrs | (PIX4(p));
    if (PIX3(p) && ((scr[4] & PIXATTR_HIPRI) == 0)) scr[4] = attrs | (PIX3(p));
    if (PIX2(p) && ((scr[5] & PIXATTR_HIPRI) == 0)) scr[5] = attrs | (PIX2(p));
    if (PIX1(p) && ((scr[6] & PIXATTR_HIPRI) == 0)) scr[6] = attrs | (PIX1(p));
    if (PIX0(p) && ((scr[7] & PIXATTR_HIPRI) == 0)) scr[7] = attrs | (PIX0(p));

  }

}

/******************************************************************************
 *
 *  Draw  characters/8pixels in row
 *  with/without checking overdraw for pixels collision detection
 *  used for sprites and planes drawing
 *
 ******************************************************************************/
static inline __attribute__((always_inline))
void draw_pattern_sprite(uint8_t *scr, uint16_t name, int paty) {

  // uint16_t pat_addr = name << 5; //name * 32;
  // uint8_t pat_palette = BITS(name, 13, 2);
  // //unsigned int is_pat_pri = name & 0x8000;
  // uint8_t *pattern = VRAM + pat_addr;
 // uint8_t attrs = (pat_palette << 4) | ((name & 0x8000) ? PIXATTR_SPRITE_HIPRI : PIXATTR_SPRITE);
  uint8_t attrs = ( (name & 0x6000 ) >> 9 ) + ((name & 0x8000) >> 8) + PIXATTR_SPRITE;

#if defined(RG_TARGET_HOLO_DYNMOD) && VDP_TILE_ROW_CACHE_ENABLED
  const vdp_tile_row_cache_entry_t *row = vdp_tile_row_cache_get(name, paty);
  draw_cached_pattern_sprite(scr, row, attrs);
  return;
#endif

  unsigned int  pattern;

  // Vertical flip ?
  // if (name & 0x1000)
  //   pattern += (7 - paty) * 4;
  // else
  //   pattern += paty * 4;

  // unsigned int  pattern;

  // Vertical flip ?
  if (name & 0x1000)
    pattern = *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + ((7 - paty) * 4)); //) pat_addr;
  else
    pattern = *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + (paty * 4));

  // Horizontal flip ?
  if (name & 0x0800)
    draw_pattern_fliph_sprite(scr, pattern, attrs);
  else
    draw_pattern_nofliph_sprite(scr, pattern, attrs);
}

static inline __attribute__((always_inline))
void draw_pattern_sprite_over_planes(uint8_t *scr, uint16_t name, int paty) {

  // uint16_t pat_addr = name << 5 ; //* 32;
  // int pat_palette = BITS(name, 13, 2);
  // int is_pat_pri = name & 0x8000;
  // uint8_t *pattern = VRAM + pat_addr;
  // uint8_t attrs = (pat_palette << 4) | (is_pat_pri ? PIXATTR_SPRITE_HIPRI : PIXATTR_SPRITE);

  // Vertical flip ?
  // if (name & 0x1000)
  //   pattern += (7 - paty) * 4;
  // else
  //   pattern += paty * 4;

  //uint8_t attrs = ( (name & 0x6000 ) >> 9 ) | ((name & 0x8000) ? PIXATTR_SPRITE_HIPRI : PIXATTR_SPRITE);
  uint8_t attrs = ( (name & 0x6000 ) >> 9 ) + ((name & 0x8000) >> 8) + PIXATTR_SPRITE;
  //uint8_t attrs = ( (name >>9) & 0x70 ) | PIXATTR_SPRITE;

#if defined(RG_TARGET_HOLO_DYNMOD) && VDP_TILE_ROW_CACHE_ENABLED
  const vdp_tile_row_cache_entry_t *row = vdp_tile_row_cache_get(name, paty);
  draw_cached_pattern_sprite_over_planes(scr, row, attrs);
  return;
#endif

  unsigned int  pattern;

  // Vertical flip ?
  if (name & 0x1000)
    pattern = *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + ((7 - paty) * 4)); //) pat_addr;
  else
    pattern = *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + (paty * 4));

  // Horizontal flip ?
  if (name & 0x0800)
    draw_pattern_fliph_sprite_over_planes (scr, pattern, attrs);
  else
    draw_pattern_nofliph_sprite_over_planes (scr, pattern, attrs);
}

static inline __attribute__((always_inline))
void draw_pattern_planeB(uint8_t *scr, uint16_t name, int paty) {
 // uint16_t pat_addr = name  << 5; // * 32;
 // uint8_t pat_palette = BITS(name, 13, 2);
 // unsigned int is_pat_pri = name & 0x8000;
  //uint8_t *pattern = VRAM + pat_addr;

  uint8_t attrs = ( (name & 0x6000 ) >> 9 ) + ((name & 0x8000) >> 8);

#if defined(RG_TARGET_HOLO_DYNMOD) && VDP_TILE_ROW_CACHE_ENABLED
  const vdp_tile_row_cache_entry_t *row = vdp_tile_row_cache_get(name, paty);
  draw_cached_pattern_planeB(scr, row, attrs);
  return;
#endif

  unsigned int  pattern;

  // Vertical flip ?
  if (name & 0x1000)
    pattern = *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + ((7 - paty) * 4)); //) pat_addr;
  else
    pattern = *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + (paty * 4));

//  if ((*(unsigned int *)pattern) == 0 ) return;
 // uint8_t *pattern = VRAM + ((name << 5) & 0xFFFF); //) pat_addr;
  //uint32_t pattern = VRAM[(name & 0x07FF) << 5]; //) pat_addr;

  // Horizontal flip ?
  if (name & 0x0800)
    draw_pattern_fliph_planeB(scr, pattern, attrs);

  else
    draw_pattern_nofliph_planeB(scr, pattern, attrs);

}

static inline __attribute__((always_inline))
void draw_pattern_planeA(uint8_t *scr, uint16_t name, int paty) {
  // uint16_t pat_addr = name << 5; //* 32;
  // uint8_t pat_palette = BITS(name, 13, 2);
  // unsigned int is_pat_pri = name & 0x8000;
  // uint8_t *pattern = VRAM + pat_addr;

    uint8_t attrs = ( (name & 0x6000 ) >> 9 ) + ((name & 0x8000) >> 8);



#if defined(RG_TARGET_HOLO_DYNMOD) && VDP_TILE_ROW_CACHE_ENABLED
  const vdp_tile_row_cache_entry_t *row = vdp_tile_row_cache_get(name, paty);
  draw_cached_pattern_planeAoverB(scr, row, attrs);
  return;
#endif


  unsigned int  pattern;

  // Vertical flip ?
  // if (name & 0x1000)
  //   pattern += (7 - paty) * 4;
  // else
  //   pattern += paty * 4;

  // Vertical flip ?
  if (name & 0x1000)
    pattern = *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + ((7 - paty) * 4)); //) pat_addr;
  else
    pattern = *(unsigned int *)(VRAM + ((name & 0x07FF) << 5) + (paty * 4));

  // Horizontal flip ?
  if (name & 0x0800)
    draw_pattern_fliph_planeAoverB(scr, pattern, attrs);

  else
    draw_pattern_nofliph_planeAoverB(scr, pattern, attrs);

}

/******************************************************************************
 *
 *  Return the Horizontal scrolling
 *
 ******************************************************************************/

static inline __attribute__((always_inline))
unsigned int get_hscroll_vram(int line)
{

    int mode = REG11_HSCROLL_MODE;
    unsigned int table = REG13_HSCROLL_ADDRESS ;
    int idx;

    switch (mode)
    {
    case 0: // Full screen scrolling
        idx = 0;
        break;
    case 1: // First 8 lines
        idx = (line & 7);
        break;
    case 2: // Every row
        idx = (line & ~7);
        break;
    case 3: // Every line
        idx = line;
        break;
    }

    return table + idx*4;
}
/******************************************************************************
 *
 *  Render PLANE B on screen line
 *
 ******************************************************************************/
 //__attribute__((optimize("unroll-loops")))
static inline __attribute__((always_inline))
void draw_line_b(int line)
{
  uint8_t *scr  = &render_buffer[PIX_OVERFLOW];

  unsigned int ntaddr = REG4_NAMETABLE_B;
  uint16_t scrollx=FETCH16VRAM(get_hscroll_vram(line) + 2) & 0x3FF;
  uint16_t *vsram = &VSRAM[1];
  uint8_t *end = scr + screen_width;

  //bool column_scrolling = BIT(gwenesis_vdp_regs[11], 2);
  const unsigned int column_scrolling = gwenesis_vdp_regs[11] & 0x4;

  // Invert horizontal scrolling (because it goes right, but we need to offset
  // of the first screen pixel)
  scrollx = -scrollx;
  uint8_t col = (scrollx >> 3) & ntw_mask;
  uint8_t patx = scrollx & 7;

  unsigned int numcell = 0;
  uint16_t scrolly = *vsram + line;
  uint8_t row = (scrolly >> 3) & nth_mask;
  uint8_t paty = scrolly & 7;
  unsigned int nt = ntaddr + row * ntwidth_x2;
  scr -= patx;
  while (scr < end) {
    draw_pattern_planeB(scr, FETCH16VRAM(nt + col * 2), paty);
    col = (col + 1) & ntw_mask;
    scr += 8;
    numcell++;

    // If per-column scrolling is active, increment VSRAM pointer
    if (column_scrolling && (numcell & 1) == 0 && scr < end)
    {
      vsram += 2;
      scrolly = *vsram + line;
      row = (scrolly >> 3) & nth_mask;
      paty = scrolly & 7;
      nt = ntaddr + row * ntwidth_x2;
    }
    }
}
/******************************************************************************
 *
 *  Render PLANE A and Window on screen line
 *
 ******************************************************************************/
//_attribute__((optimize("unroll-loops")))
static inline __attribute__((always_inline))
void draw_line_aw(int line) {

  uint8_t *scr  = &render_buffer[PIX_OVERFLOW];

  unsigned int ntaddr = REG2_NAMETABLE_A;
  uint16_t scrollx=FETCH16VRAM(get_hscroll_vram(line) + 0) & 0x3FF;
  uint16_t *vsram = &VSRAM[0];

  // Check if we are in the window region only
  // if it's the case, we cancel the plane A drawing
  int Window_line = REG18_WINDOW_VPOS * 8;
  //bool window_down = BIT(gwenesis_vdp_regs[18], 7);
  int window_down = gwenesis_vdp_regs[18] & 0x80;

  int PlanA_first = PlanA_firstcol;
  int PlanA_last = PlanA_lastcol;
  int Window_last = Window_lastcol;
  int Window_first = Window_firstcol;

  if (window_down) {
    if (line > Window_line) {
      PlanA_first = PlanA_last = 0;
      Window_last = screen_width;
      Window_first = 0;
    }
  } else {

    if (line < Window_line) {
      PlanA_first = PlanA_last = 0;
      Window_last = screen_width;
      Window_first = 0;
    }
  }

  // First draw A plane
  uint8_t *pos = scr + PlanA_first; // scr + screen_width;
  uint8_t *end = scr + PlanA_last;  // scr + screen_width

   //bool column_scrolling = BIT(gwenesis_vdp_regs[11], 2);
  const unsigned int column_scrolling = gwenesis_vdp_regs[11] & 0x4;

  // Invert horizontal scrolling (because it goes right, but we need to offset
  // of the first screen pixel)
  scrollx = -scrollx;
  uint8_t col = (scrollx >> 3) & ntw_mask;
  uint8_t patx = scrollx & 7;

  unsigned int numcell = 0;
  uint16_t scrolly = *vsram + line;
  uint8_t plane_row = (scrolly >> 3) & nth_mask;
  uint8_t plane_paty = scrolly & 7;
  unsigned int plane_nt = ntaddr + plane_row * ntwidth_x2;
  pos -= patx;
  while (pos < end) {
    draw_pattern_planeA(pos, FETCH16VRAM(plane_nt + col * 2), plane_paty);

    col = (col + 1) & ntw_mask;
    pos += 8;
    numcell++;

    // If per-column scrolling is active, increment VSRAM pointer
    if (column_scrolling && (numcell & 1) == 0 && pos < end)
    {
      vsram += 2;
      scrolly = *vsram + line;
      plane_row = (scrolly >> 3) & nth_mask;
      plane_paty = scrolly & 7;
      plane_nt = ntaddr + plane_row * ntwidth_x2;
    }
  }

  // Second Draw Window Plane
  int row = line >> 3;
  int paty = line & 7;
  //int wdwidth = (screen_width == 320 ? 64 : 32);
  //unsigned int nt = base_w + row * 2 * wdwidth + Window_first / 4;

  int wdwidth_x2 = (screen_width == 320 ? 128 : 64);

  unsigned int nt = base_w + row * wdwidth_x2 + Window_first / 4;

  uint8_t *wpos = scr + Window_first;

  for (int i = Window_first / 8; i < Window_last / 8; ++i) {
    draw_pattern_planeA(wpos, FETCH16VRAM(nt), paty);
    nt += 2;
    wpos += 8;
  }
}

/******************************************************************************
 *
 *  Render SPRITES on screen line
 *
 ******************************************************************************/

#if defined(RG_TARGET_HOLO_DYNMOD)
static void GWENESIS_HOT build_sprite_line_list(void)
{
  if (!sprite_line_count || !sprite_line_table)
    return;

  memset(sprite_line_count, 0, VDP_SPRITE_VISIBLE_LINES);

  uint8_t *start_table = VRAM + REG5_SAT_ADDRESS;
  uint8_t *cache_base = MODE_SHI ? start_table : SAT_CACHE;
  const int sprite_table_size = (screen_width == 320) ? 80 : 64;
  const int max_sprites_per_line = (screen_width == 320) ? 20 : 16;
  int sidx = 0;

  for (int i = 0; i < sprite_table_size && sidx < sprite_table_size; ++i)
  {
    uint8_t *cache = cache_base + sidx * 8;
    int sy = (((cache[0] & 0x3) << 8) | cache[1]) - 128;
    int sh = BITS(cache[2], 0, 2) + 1;
    int link = BITS(cache[3], 0, 7);
    int first = sy;
    int last = sy + sh * 8;

    if (first < 0)
      first = 0;
    if (last > screen_height)
      last = screen_height;
    if (last > VDP_SPRITE_VISIBLE_LINES)
      last = VDP_SPRITE_VISIBLE_LINES;

    for (int line = first; line < last; ++line)
    {
      uint8_t count = sprite_line_count[line];
      if (count < max_sprites_per_line)
      {
        sprite_line_table[line * VDP_SPRITE_LINE_MAX + count] = (uint8_t)sidx;
        sprite_line_count[line] = count + 1;
      }
    }

    if (link == 0)
      break;
    sidx = link;
  }
}
#endif

//__attribute__((optimize("unroll-loops")))
static inline __attribute__((always_inline)) 
void draw_sprites_over_planes(int line)
{
    uint8_t *scr;

    scr = &render_buffer[PIX_OVERFLOW];

   // uint8_t mask = mode_h40 ? 0x7E : 0x7F;
   // uint8_t *start_table = VRAM + ((gwenesis_vdp_regs[5] & mask) << 9);

    uint8_t *start_table = VRAM + REG5_SAT_ADDRESS;

    // This is both the size of the table as seen by the VDP
    // *and* the maximum number of sprites that are processed
    // (important in case of infinite loops in links).
#if !defined(RG_TARGET_HOLO_DYNMOD)
    const int SPRITE_TABLE_SIZE     = (screen_width == 320) ?  80 :  64;
#endif
    const int MAX_SPRITES_PER_LINE  = (screen_width == 320) ?  20 :  16;
    const int MAX_PIXELS_PER_LINE   = (screen_width == 320) ? 320 : 256;

    bool masking = false, one_sprite_nonzero = false; // overdraw = false;
    int sidx = 0, num_sprites = 0, num_pixels = 0;

#if defined(RG_TARGET_HOLO_DYNMOD)
    const int line_count = sprite_line_count[line];
    for (int i = 0; i < line_count; ++i)
    {
        sidx = sprite_line_table[line * VDP_SPRITE_LINE_MAX + i];
        uint8_t *table = start_table + sidx * 8;
        uint8_t *cache = SAT_CACHE + sidx * 8;

        int sy = ((cache[0] & 0x3) << 8) | cache[1];
        int sx = ((table[6] & 0x3) << 8) | table[7];
        uint16_t name = (table[4] << 8) | table[5];
        int sh = BITS(cache[2], 0, 2) + 1;
        int isflipv = table[4] & 0x10;
        int isfliph = table[4] & 0x8;
        int sw = BITS(table[2], 2, 2) + 1;

        sy -= 128;
        if (sx == 0)
        {
          if (one_sprite_nonzero || (gwenesis_vdp_gfx_sprite_overflow() == line - 1))
            masking = true;
        }
        else
          one_sprite_nonzero = true;

        int row = (line - sy) >> 3;
        int paty = (line - sy) & 7;
        if (isflipv)
          row = sh - row - 1;

        sx -= 128;
        if ((sx > (-sw * 8)) && (sx < screen_width) && !masking)
        {
          name += row;

          if (isfliph)
          {
            name += sh * (sw - 1);
            for (int p = 0; (p < sw) && (num_pixels < MAX_PIXELS_PER_LINE); p++)
            {
              draw_pattern_sprite_over_planes(scr + sx + p * 8, name, paty);
              name -= sh;
              num_pixels += 8;
            }
          }
          else
          {
            for (int p = 0; (p < sw) && (num_pixels < MAX_PIXELS_PER_LINE); p++)
            {
              draw_pattern_sprite_over_planes(scr + sx + p * 8, name, paty);
              name += sh;
              num_pixels += 8;
            }
          }
        }
        else
          num_pixels += sw * 8;

        if (num_pixels >= MAX_PIXELS_PER_LINE)
        {
          gwenesis_vdp_gfx_set_sprite_overflow(line);
          break;
        }
        if (++num_sprites >= MAX_SPRITES_PER_LINE)
          break;
    }
    return;
#else
    for (int i = 0; (i < SPRITE_TABLE_SIZE) && sidx < (SPRITE_TABLE_SIZE); ++i)
    {
        uint8_t *table = start_table + sidx*8;
        uint8_t *cache = SAT_CACHE + sidx*8;
        //uint8_t *cache = start_table + sidx*8;
        

        int sy = ((cache[0] & 0x3) << 8) | cache[1];
        int sx = ((table[6] & 0x3) << 8) | table[7];
        uint16_t name = (table[4] << 8) | table[5];


        int sh = BITS(cache[2], 0, 2) + 1;
        int link = BITS(cache[3], 0, 7);

        int isflipv = table[4] & 0x10;
        int isfliph = table[4] & 0x8;

        int sw = BITS(table[2], 2, 2) + 1;

        sy -= 128;
        if ((line >= sy) && (line < sy+sh*8))
        {
            // Sprite masking: a sprite on column 0 masks
            // any lower-priority sprite, but with the following conditions
            //   * it only works from the second visible sprite on each line
            //   * if the previous line had a sprite pixel overflow, it
            //     works even on the first sprite
            // Notice that we need to continue parsing the table after masking
            // to see if we reach a pixel overflow (because it would affect masking
            // on next line).
            if (sx == 0)
            {
                if (one_sprite_nonzero || (gwenesis_vdp_gfx_sprite_overflow() == line-1))
                    masking = true;
            }
            else
                one_sprite_nonzero = true;

            int row = (line - sy) >> 3;
            int paty = (line - sy) & 7;
            if (isflipv)
                row = sh - row - 1;

            sx -= 128;
            if ((sx > (-sw * 8)) && (sx < screen_width) && !masking) {

              name += row;

              if (isfliph) {
                name += sh * (sw - 1);
                for (int p = 0; (p < sw) && (num_pixels < MAX_PIXELS_PER_LINE); p++) {

                  draw_pattern_sprite_over_planes(scr + sx + p * 8, name, paty);
                  name -= sh;
                  num_pixels += 8;

                }
              } else {
                for (int p = 0; (p < sw) && (num_pixels < MAX_PIXELS_PER_LINE); p++) {

                  draw_pattern_sprite_over_planes(scr + sx + p * 8, name, paty);
                  name += sh;
                  num_pixels += 8;

                }
              }
            }
            else
                num_pixels += sw*8;

            if (num_pixels >= MAX_PIXELS_PER_LINE)
            {
                gwenesis_vdp_gfx_set_sprite_overflow(line);
                break;
            }
            if (++num_sprites >= MAX_SPRITES_PER_LINE)
                break;
        }

        if (link == 0) break;
        sidx = link;
    }
#endif

  //  if (overdraw)
  //      sprite_collision = true;
}
static inline __attribute__((always_inline)) 
void draw_sprites(int line)
{
  uint8_t *scr;

  scr = &sprite_buffer[PIX_OVERFLOW];

  // uint8_t mask = mode_h40 ? 0x7E : 0x7F;
  // uint8_t *start_table = VRAM + ((gwenesis_vdp_regs[5] & mask) << 9);

  uint8_t *start_table = VRAM + REG5_SAT_ADDRESS;

  // This is both the size of the table as seen by the VDP
  // *and* the maximum number of sprites that are processed
  // (important in case of infinite loops in links).
#if !defined(RG_TARGET_HOLO_DYNMOD)
  const int SPRITE_TABLE_SIZE = (screen_width == 320) ? 80 : 64;
#endif
  const int MAX_SPRITES_PER_LINE = (screen_width == 320) ? 20 : 16;
  const int MAX_PIXELS_PER_LINE = (screen_width == 320) ? 320 : 256;

  bool masking = false, one_sprite_nonzero = false; // overdraw = false;
  int sidx = 0, num_sprites = 0, num_pixels = 0;

#if defined(RG_TARGET_HOLO_DYNMOD)
    const int line_count = sprite_line_count[line];
    for (int i = 0; i < line_count; ++i)
    {
      sidx = sprite_line_table[line * VDP_SPRITE_LINE_MAX + i];
      uint8_t *table = start_table + sidx * 8;
      uint8_t *cache = start_table + sidx * 8;

      int sy = ((cache[0] & 0x3) << 8) | cache[1];
      int sx = ((table[6] & 0x3) << 8) | table[7];
      uint16_t name = (table[4] << 8) | table[5];
      int sh = BITS(cache[2], 0, 2) + 1;
      int isflipv = table[4] & 0x10;
      int isfliph = table[4] & 0x8;
      int sw = BITS(table[2], 2, 2) + 1;

      sy -= 128;
      if (sx == 0)
      {
        if (one_sprite_nonzero || gwenesis_vdp_gfx_sprite_overflow() == line - 1)
          masking = true;
      }
      else
        one_sprite_nonzero = true;

      int row = (line - sy) >> 3;
      int paty = (line - sy) & 7;
      if (isflipv)
        row = sh - row - 1;

      sx -= 128;
      if (sx > -sw * 8 && sx < screen_width && !masking)
      {
        name += row;

        if (isfliph)
        {
          name += sh * (sw - 1);
          for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++)
          {
            draw_pattern_sprite(scr + sx + p * 8, name, paty);
            name -= sh;
            num_pixels += 8;
          }
        }
        else
        {
          for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++)
          {
            draw_pattern_sprite(scr + sx + p * 8, name, paty);
            name += sh;
            num_pixels += 8;
          }
        }
      }
      else
        num_pixels += sw * 8;

      if (num_pixels >= MAX_PIXELS_PER_LINE)
      {
        gwenesis_vdp_gfx_set_sprite_overflow(line);
        break;
      }
      if (++num_sprites >= MAX_SPRITES_PER_LINE)
        break;
    }
    return;
#else
  for (int i = 0; i < SPRITE_TABLE_SIZE && sidx < SPRITE_TABLE_SIZE; ++i) {
    uint8_t *table = start_table + sidx * 8;
    uint8_t *cache = start_table + sidx * 8;

    //uint8_t *cache = SAT_CACHE + sidx * 8;

    int sy = ((cache[0] & 0x3) << 8) | cache[1];
    int sx = ((table[6] & 0x3) << 8) | table[7];
    uint16_t name = (table[4] << 8) | table[5];

    int sh = BITS(cache[2], 0, 2) + 1;
    int link = BITS(cache[3], 0, 7);

    int isflipv = table[4] & 0x10;
    int isfliph = table[4] & 0x8;

    int sw = BITS(table[2], 2, 2) + 1;

    sy -= 128;
    if (line >= sy && line < sy + sh * 8) {
      // Sprite masking: a sprite on column 0 masks
      // any lower-priority sprite, but with the following conditions
      //   * it only works from the second visible sprite on each line
      //   * if the previous line had a sprite pixel overflow, it
      //     works even on the first sprite
      // Notice that we need to continue parsing the table after masking
      // to see if we reach a pixel overflow (because it would affect masking
      // on next line).
      if (sx == 0) {
        if (one_sprite_nonzero || gwenesis_vdp_gfx_sprite_overflow() == line - 1)
          masking = true;
      } else
        one_sprite_nonzero = true;

      int row = (line - sy) >> 3;
      int paty = (line - sy) & 7;
      if (isflipv)
        row = sh - row - 1;

      sx -= 128;
      if (sx > -sw * 8 && sx < screen_width && !masking) {

        name += row;

        if (isfliph) {
          name += sh * (sw - 1);
          for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++) {

            draw_pattern_sprite(scr + sx + p * 8, name, paty);
            name -= sh;
            num_pixels += 8;
          }
        } else {
          for (int p = 0; p < sw && num_pixels < MAX_PIXELS_PER_LINE; p++) {

            draw_pattern_sprite(scr + sx + p * 8, name, paty);
            name += sh;
            num_pixels += 8;
          }
        }
      } else
        num_pixels += sw * 8;

      if (num_pixels >= MAX_PIXELS_PER_LINE) {
        gwenesis_vdp_gfx_set_sprite_overflow(line);
        break;
      }
      if (++num_sprites >= MAX_SPRITES_PER_LINE)
        break;
    }

    if (link == 0)
      break;
    sidx = link;
    }
#endif

  //  if (overdraw)
  //      sprite_collision = true;
}
/******************************************************************************
 *
 *  Parse PLANE A/B size,scrolling at the start of image rendering
 *
 ******************************************************************************/
//static unsigned short current_line[320];

void GWENESIS_HOT gwenesis_vdp_render_config()
{
    const int render_mode_h40 = REG12_MODE_H40;
    if (!gwenesis_vdp_gfx_has_render_context())
    {
      mode_h40 = render_mode_h40;
      mode_pal = REG1_PAL;
    }

    int ntwidth = BITS(gwenesis_vdp_regs[16], 0, 2);
    int ntheight = BITS(gwenesis_vdp_regs[16], 4, 2);
    ntwidth = (ntwidth + 1) * 32;
    ntheight = (ntheight + 1) * 32;
    ntw_mask = ntwidth - 1;
    nth_mask = ntheight - 1;
    ntwidth_x2= ntwidth *2;

    // Window & A planes separation

    if (render_mode_h40)
        base_w = ((REG3_NAMETABLE_W & 0x1e) << 11);
    else
        base_w = ((REG3_NAMETABLE_W & 0x1f) << 11);


    bool window_right = BIT(gwenesis_vdp_regs[17], 7);

    // int window_is_bugged = 0;
    PlanA_firstcol = 0;
    PlanA_lastcol = screen_width;

    Window_firstcol = 0;
    Window_lastcol = 0;

    if (window_right) {

      Window_firstcol = REG17_WINDOW_HPOS * 16;
      Window_lastcol = screen_width;

      if (Window_firstcol > Window_lastcol)
        Window_firstcol = Window_lastcol;

      PlanA_firstcol = 0;
      PlanA_lastcol = Window_firstcol;

    } else {
      Window_firstcol = 0;
      Window_lastcol = REG17_WINDOW_HPOS * 16;
      if (Window_lastcol > screen_width)
        Window_lastcol = screen_width;

      PlanA_firstcol = Window_lastcol;
      PlanA_lastcol = screen_width;
      // if (Window_lastcol != 0)
      //      window_is_bugged = 1;
    }

#if defined(RG_TARGET_HOLO_DYNMOD)
    build_sprite_line_list();
#endif
}

/******************************************************************************
 *
 *  Render a line on screen
 *  Get selected line and render it on screen processing each plane.
 *
 ******************************************************************************/

//#define CONV(b)   ((0b11 1110 0000 0000 0000 0000 0000 & b)>>10) | ((0b00000 1111 1100 0000 0000 & b)>>5) | ((0b0000000000011111 & b))
//#define SPACE(c)  ((0b00 0000 0000 1111 1000 0000 0000 & c)<<10) | ((0b00000 0000 0111 1110 0000 & c)<<5) | ((0b0000000000011111 & c))

#define CONV(b)   ((0x3e00000 & b)>>10) | ((0xfc00 & b)>>5) | ((0x1f & b))
#define SPACE(c)  ((0xe800 & c)<<10) | ((0x7e0 & c)<<5) | ((0x1f & c))

/* Function unused

__attribute__((optimize("unroll-loops"))) static void
blit_4to5_line(uint16_t *in, uint16_t *out) {

  uint16_t *src_row = in;
  uint16_t *dest_row = out;
  for (int x_src = 0, x_dst = 0; x_src < 256; x_src += 4, x_dst += 5) {
    uint32_t b0 = SPACE(src_row[x_src]);
    uint32_t b1 = SPACE(src_row[x_src + 1]);
    uint32_t b2 = SPACE(src_row[x_src + 2]);
    uint32_t b3 = SPACE(src_row[x_src + 3]);

    dest_row[x_dst] = CONV(b0);
    dest_row[x_dst + 1] = CONV((b0 + b0 + b0 + b1) >> 2);
    dest_row[x_dst + 2] = CONV((b1 + b2) >> 1);
    dest_row[x_dst + 3] = CONV((b2 + b2 + b2 + b3) >> 2);
    dest_row[x_dst + 4] = CONV(b3);
  }
}
*/

void GWENESIS_HOT gwenesis_vdp_render_line(int line)
{
  if (!gwenesis_vdp_gfx_has_render_context())
    mode_h40 = REG12_MODE_H40;
  //mode_pal = REG1_PAL;

  vdpg_log(__FUNCTION__,": %3d",line);

  //unsigned int line = scan_line;
  //  if (line == 0) gwenesis_vdp_render_config();

  // interlace mode not implemented
  if (BITS(gwenesis_vdp_regs[12], 1, 2) != 0)
    return;

  if (line >= (REG1_PAL ? 240 : 224))
    return;

#ifdef _HOST_
  memset(screen, 0, SCREEN_WIDTH * 4);

// Embedded RGB565
#else
#if defined(RG_TARGET_HOLO_DYNMOD)
  screen_buffer_line = &screen_buffer[(((240 - screen_height) / 2) + line) * 320 +
                                      ((320 - screen_width) / 2)];
#else
  screen_buffer_line = &screen_buffer[line * 320];
  /* clean up line screen not refreshed when mode is !H40 */
  if (REG12_MODE_H40 == 0) memset(screen_buffer_line - (320-256)/2, 0, 320 * sizeof(screen_buffer_line[0]));
#endif

#endif

  if (REG0_DISABLE_DISPLAY)
  return;

#ifdef _HOST_
    memset(screen, 0, SCREEN_WIDTH * 4);
#else
 //   memset(screen_buffer_line, 0, 320 * 2);
#endif

  uint8_t *pb = &render_buffer[PIX_OVERFLOW];
  uint8_t *ps = &sprite_buffer[PIX_OVERFLOW];

  if (MODE_SHI)
    memset(ps, 0, screen_width);

  draw_line_b(line);
  draw_line_aw(line);

  if (MODE_SHI)
    draw_sprites(line);
  else
    draw_sprites_over_planes(line);

#ifdef _HOST_
  uint16_t rgb565;

  /* Mode Highlight/shadow is enabled */
  if (MODE_SHI) {
    for (int x = 0; x < screen_width; x++) {
      uint8_t plane = pb[x];
      uint8_t sprite = ps[x];

      if ((plane & 0xC0) < (sprite & 0xC0) ) {
        switch (sprite & 0x3F) {
        // Palette=3, Sprite=14 :> draw plane, force highlight
        case 0x3E:
          rgb565 = 0x8410 | CRAM565[plane] >> 1;
          break;
        // Palette=3, Sprite=15 :> draw plane, force shadow
        case 0x3F:
          rgb565 = CRAM565[plane] >> 1;
          break;
        // draw sprite, normal
        default:
          rgb565 = CRAM565[sprite];
          break;
        }
      } else {
        rgb565 = CRAM565[plane];
      }
      uint8_t r, g, b;
      r = (rgb565 & 0xF800) >> 8;
      g = (rgb565 & 0X07E0) >> 3;
      b = (rgb565 & 0x001F) << 3;
      int pixel = ((240 - screen_height) / 2 + (line)) * 320 + (x) +
                  (320 - screen_width) / 2;
      screen[pixel * 4 + 2] = r;
      screen[pixel * 4 + 1] = g;
      screen[pixel * 4 + 0] = b;
    }

    /* Normal mode*/
  } else {
    for (int x = 0; x < screen_width; x++) {
      rgb565 = CRAM565[pb[x]];
      uint8_t r, g, b;
      r = (rgb565 & 0xF800) >> 8;
      g = (rgb565 & 0X07E0) >> 3;
      b = (rgb565 & 0x001F) << 3;
      int pixel = ((240 - screen_height) / 2 + (line)) * 320 + (x) +
                  (320 - screen_width) / 2;
      screen[pixel * 4 + 2] = r;
      screen[pixel * 4 + 1] = g;
      screen[pixel * 4 + 0] = b;
    }
  }

  #else

  const bool remap_palette = gwenesis_vdp_gfx_has_palette_remap();
  /* Mode Highlight/shadow is enabled */
  if (MODE_SHI) {
    for (int x = 0; x < screen_width; x++) {
      uint8_t plane = pb[x];
      uint8_t sprite = ps[x];

      uint8_t output;
      if ((plane & 0xC0) < (sprite & 0xC0)) {
        uint8_t s = sprite & 0x3F;
        output = (s >= 0x3E) ? plane : sprite;
      } else {
        output = plane;
      }
      screen_buffer_line[x] = remap_palette
                                  ? gwenesis_vdp_gfx_remap_palette_index(output)
                                  : output;
    }

    /* Normal mode*/
  } else {
#if 0
    uint32_t *video_out = (uint32_t *) &screen_buffer_line[0];

    for (int x = 0; x < screen_width; x+=2) {

      //screen_buffer_line[x] = CRAM565[pb[x]];
      // 2 pixels : 32 bits write  access is faster
      *video_out++ = CRAM565[pb[x]] | CRAM565[pb[x+1]] << 16;
    }
#else
  if (remap_palette)
  {
    for (int x = 0; x < screen_width; ++x)
      screen_buffer_line[x] = gwenesis_vdp_gfx_remap_palette_index(pb[x]);
  }
  else
  {
    memcpy(screen_buffer_line, pb, screen_width);
  }
#endif
  }

  #endif
}

void gwenesis_vdp_gfx_save_state() {
  /*
  SaveState* state;
  state = saveGwenesisStateOpenForWrite("vdp_gfx");
  saveGwenesisStateSetBuffer(state, "render_buffer", render_buffer, VDP_GFX_LINE_BUFFER_SIZE);
  saveGwenesisStateSetBuffer(state, "sprite_buffer", sprite_buffer, VDP_GFX_LINE_BUFFER_SIZE);
  saveGwenesisStateSet(state, "mode_h40", mode_h40);
  saveGwenesisStateSet(state, "mode_pal", mode_pal);
  saveGwenesisStateSet(state, "screen_width", screen_width);
  saveGwenesisStateSet(state, "screen_height", screen_height);
  saveGwenesisStateSet(state, "sprite_overflow", sprite_overflow);
  saveGwenesisStateSet(state, "sprite_collision", sprite_collision);
  saveGwenesisStateSet(state, "base_w", base_w);
  saveGwenesisStateSet(state, "PlanA_firstcol", PlanA_firstcol);
  saveGwenesisStateSet(state, "PlanA_lastcol", PlanA_lastcol);
  saveGwenesisStateSet(state, "Window_firstcol", Window_firstcol);
  saveGwenesisStateSet(state, "Window_lastcol", Window_lastcol);
  */
}

void gwenesis_vdp_gfx_load_state() {
  /*
    SaveState* state = saveGwenesisStateOpenForRead("vdp_gfx");
    saveGwenesisStateGetBuffer(state, "render_buffer", render_buffer, VDP_GFX_LINE_BUFFER_SIZE);
    saveGwenesisStateGetBuffer(state, "sprite_buffer", sprite_buffer, VDP_GFX_LINE_BUFFER_SIZE);
    mode_h40 = saveGwenesisStateGet(state, "mode_h40");
    mode_pal = saveGwenesisStateGet(state, "mode_pal");
    screen_width = saveGwenesisStateGet(state, "screen_width");
    screen_height = saveGwenesisStateGet(state, "screen_height");
    sprite_overflow = saveGwenesisStateGet(state, "sprite_overflow");
    sprite_collision = saveGwenesisStateGet(state, "sprite_collision");
    base_w = saveGwenesisStateGet(state, "base_w");
    PlanA_firstcol = saveGwenesisStateGet(state, "PlanA_firstcol");
    PlanA_lastcol = saveGwenesisStateGet(state, "PlanA_lastcol");
    Window_firstcol = saveGwenesisStateGet(state, "Window_firstcol");
    Window_lastcol = saveGwenesisStateGet(state, "Window_lastcol");
    */
}
