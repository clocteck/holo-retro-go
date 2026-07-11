#include <rg_system.h>
#include <rg_audio.h>
#include <rg_display.h>
#include <rg_utils.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#undef BIT
#endif

#include <gwenesis.h>
#include "gwenesis_m68k_profile.h"
#include "gwenesis_bus.h"
#include "z80inst.h"
#if defined(RG_TARGET_HOLO_DYNMOD)
#include <module_abi.h>
#include "holo_port.h"
#endif

#define AUDIO_SYNTH_SAMPLE_RATE (GWENESIS_AUDIO_OUTPUT_RATE)
#if defined(RG_TARGET_HOLO_DYNMOD) && defined(GWENESIS_AUDIO_HOST_RATE)
#define AUDIO_OUTPUT_SAMPLE_RATE (GWENESIS_AUDIO_HOST_RATE)
#else
#define AUDIO_OUTPUT_SAMPLE_RATE (AUDIO_SYNTH_SAMPLE_RATE)
#endif
#define AUDIO_BUFFER_LENGTH GWENESIS_AUDIO_BUFFER_CAPACITY

extern unsigned char *VRAM;
extern unsigned char *gwenesis_vdp_regs;
extern unsigned char *SAT_CACHE;
extern unsigned short *CRAM565;
extern unsigned short *VSRAM;
extern int screen_width, screen_height;
extern int zclk;
int system_clock;
int scan_line;

#if GWENESIS_SN76489_RUN_ENABLED
int16_t *gwenesis_sn76489_buffer;
int sn76489_index;
int sn76489_clock;
#endif
int16_t *gwenesis_ym2612_buffer;
int ym2612_index;
int ym2612_clock;

static FILE *savestate_fp = NULL;
static int savestate_errors = 0;

typedef enum
{
    GWENESIS_AUDIO_MODE_FAST = 0,
    GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE = 1,
    GWENESIS_AUDIO_MODE_BALANCE = 2,
} gwenesis_audio_mode_t;

static gwenesis_audio_mode_t gwenesis_audio_mode = GWENESIS_AUDIO_MODE_BALANCE;

static const char *gwenesis_audio_mode_name(gwenesis_audio_mode_t mode)
{
    switch (mode)
    {
    case GWENESIS_AUDIO_MODE_FAST:
        return "Fast";
    case GWENESIS_AUDIO_MODE_BALANCE:
        return "Balance";
    case GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE:
    default:
        return "Mute";
    }
}

#define GWENESIS_SURFACE_COUNT 2

static rg_surface_t *updates[GWENESIS_SURFACE_COUNT];
static rg_surface_t *currentUpdate;
static rg_surface_t *displayUpdate;
static rg_app_t *app;
static void *update_data_base[GWENESIS_SURFACE_COUNT];
static int update_height_base[GWENESIS_SURFACE_COUNT];
static rg_audio_frame_t *gwenesis_audio_mix_buffer;

static const char *SETTING_AUDIO_MODE = "audio_mode";
static const char *SETTING_Z80_ENABLE = "z80_enable";
static const char *SETTING_PERF_OVERLAY = "perf_overlay";
static bool gwenesis_z80_enabled = true;
static bool gwenesis_perf_overlay_enabled;

#if defined(RG_TARGET_HOLO_DYNMOD)
#define GWENESIS_SURFACE_MEM MEM_SLOW
#define GWENESIS_SURFACE_WIDTH 320
#define GWENESIS_SURFACE_HEIGHT 240
#define GWENESIS_SURFACE_FORMAT RG_PIXEL_PAL565_LE
#else
#define GWENESIS_SURFACE_MEM MEM_FAST
#define GWENESIS_SURFACE_WIDTH 320
#define GWENESIS_SURFACE_HEIGHT 241
#define GWENESIS_SURFACE_FORMAT RG_PIXEL_PAL565_BE
#endif
#define GWENESIS_AUDIO_QUEUE_PACKETS 5
#define GWENESIS_AUDIO_PREBUFFER_PACKETS 4
#define GWENESIS_AUDIO_UNDERFLOW_CHUNK_FRAMES 64
#if defined(RG_TARGET_HOLO_DYNMOD)
#define GWENESIS_AUDIO_TASK_STACK (3 * 1024)
#define GWENESIS_AUDIO_TASK_CORE 0
#define GWENESIS_VDP_ASYNC_TASK_CORE 0
#define GWENESIS_VRAM_MEM MEM_SLOW
#define GWENESIS_VRAM_MEM_NAME "MEM_SLOW"
#define GWENESIS_VRAM_FALLBACK_MEM MEM_SLOW
#define GWENESIS_VRAM_FALLBACK_MEM_NAME "MEM_SLOW"
#else
#define GWENESIS_AUDIO_TASK_STACK (4 * 1024 - 256)
#define GWENESIS_AUDIO_TASK_CORE 0
#define GWENESIS_VDP_ASYNC_TASK_CORE 0
#define GWENESIS_VRAM_MEM MEM_FAST
#define GWENESIS_VRAM_MEM_NAME "MEM_FAST"
#define GWENESIS_VRAM_FALLBACK_MEM MEM_FAST
#define GWENESIS_VRAM_FALLBACK_MEM_NAME "MEM_FAST"
#endif
#if defined(RG_TARGET_HOLO_DYNMOD)
#define GWENESIS_AUDIO_QUEUE_MEM MEM_FAST
#ifndef GWENESIS_PROFILER_DETAILED
#define GWENESIS_PROFILER_DETAILED 1
#endif
#ifndef GWENESIS_AUDIO_EQ_ENABLED
#define GWENESIS_AUDIO_EQ_ENABLED 1
#endif
#ifndef GWENESIS_MONITOR_EXTRA_ENABLED
#define GWENESIS_MONITOR_EXTRA_ENABLED 0
#endif
#else
#define GWENESIS_AUDIO_QUEUE_MEM MEM_FAST
#ifndef GWENESIS_PROFILER_DETAILED
#define GWENESIS_PROFILER_DETAILED 0
#endif
#ifndef GWENESIS_AUDIO_EQ_ENABLED
#define GWENESIS_AUDIO_EQ_ENABLED 1
#endif
#ifndef GWENESIS_MONITOR_EXTRA_ENABLED
#define GWENESIS_MONITOR_EXTRA_ENABLED 1
#endif
#endif
#ifndef GWENESIS_SN76489_RUN_ENABLED
#define GWENESIS_SN76489_RUN_ENABLED 0
#endif
#define GWENESIS_RAM_CACHE_SAMPLE_LOGS 2
#define GWENESIS_RAM_CACHE_HOLD_LOGS 6
#if GWENESIS_VDP_ASYNC_ENABLED
#define GWENESIS_VDP_ASYNC_JOBS 2
#define GWENESIS_VDP_ASYNC_TASK_STACK ((11 * 1024) / 2)
#define GWENESIS_VDP_ASYNC_TASK_PRIORITY RG_TASK_PRIORITY_5
#define GWENESIS_VDP_ASYNC_RENDER_FRAME_DELAY_MS 0
#define GWENESIS_VDP_ASYNC_UNSAFE_FRAMES 8
#define GWENESIS_VDP_ASYNC_SERIAL_TEST 0
#define GWENESIS_VDP_ASYNC_VRAM_MEM MEM_SLOW
#define GWENESIS_VDP_ASYNC_VRAM_MEM_NAME "MEM_SLOW"
#define GWENESIS_VDP_SNAPSHOT_PAGE_SHIFT 8U
#define GWENESIS_VDP_SNAPSHOT_PAGE_SIZE (1U << GWENESIS_VDP_SNAPSHOT_PAGE_SHIFT)
#define GWENESIS_VDP_SNAPSHOT_PAGE_COUNT (VRAM_MAX_SIZE / GWENESIS_VDP_SNAPSHOT_PAGE_SIZE)
#define GWENESIS_VDP_SNAPSHOT_DIRTY_WORDS (GWENESIS_VDP_SNAPSHOT_PAGE_COUNT / 32U)
#define GWENESIS_VDP_SNAPSHOT_FULL_THRESHOLD_PAGES 192U
#endif
// --- MAIN

#define GWENESIS_FRAME_TARGET_FPS 55
static const int frame_target_us = 1000000 / GWENESIS_FRAME_TARGET_FPS;
#if defined(RG_TARGET_HOLO_DYNMOD)
#ifndef GWENESIS_FIXED_DRAW_SKIP
#define GWENESIS_FIXED_DRAW_SKIP 1
#endif
#define GWENESIS_RENDER_MIN_FPS 30
#define GWENESIS_RENDER_TARGET_FPS 30
#define GWENESIS_RENDER_MAX_FPS 30
#define GWENESIS_RENDER_SKIP_DEBT_US 5000
static const int render_target_us = 1000000 / GWENESIS_RENDER_TARGET_FPS;
static const int frame_min_yield_ms = 1;
static const int frame_min_yield_interval_frames = 5;
static const int frameskip_audio_off_debt_us = 9000;
static const int frameskip_audio_off_force_debt_us = 13000;
static const int frameskip_audio_on_debt_us = 4500;
static const int frameskip_audio_on_force_debt_us = 9000;
static const int frameskip_max_consecutive_skips = 3;
#else
static const int frame_min_yield_ms = 0;
static const int frame_min_yield_interval_frames = 0;
static const int frameskip_audio_off_debt_us = 12000;
static const int frameskip_audio_off_force_debt_us = 20000;
static const int frameskip_audio_off_min_draws = 1;
static const int frameskip_audio_on_debt_us = 9000;
static const int frameskip_audio_on_force_debt_us = 16000;
static const int frameskip_audio_on_min_draws = 1;
static const int frameskip_max_consecutive_skips = 1;
#endif
static int64_t frame_pacer_next_us;
#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_FIXED_DRAW_SKIP
static int frameskip_fixed_phase;
#else
static int frameskip_draw_streak;
static int frameskip_skip_streak;
static int frameskip_last_debt_us;
#if defined(RG_TARGET_HOLO_DYNMOD)
static int64_t render_pacer_next_us;
#endif
#endif
static int frame_yield_count;
#if defined(RG_TARGET_HOLO_DYNMOD)
#define GWENESIS_Z80_BATCH_LINES_FAST 2
#define GWENESIS_Z80_BATCH_LINES_BALANCE 1
#define GWENESIS_Z80_BATCH_LINES_MUTED 16
#define GWENESIS_YM_BATCH_LINES_FAST 2
#define GWENESIS_YM_BATCH_LINES_BALANCE 1
#else
#define GWENESIS_Z80_BATCH_LINES_FAST 1
#define GWENESIS_Z80_BATCH_LINES_BALANCE 1
#define GWENESIS_Z80_BATCH_LINES_MUTED 1
#define GWENESIS_YM_BATCH_LINES_FAST 1
#define GWENESIS_YM_BATCH_LINES_BALANCE 1
#endif
typedef struct
{
    size_t count;
    rg_audio_frame_t frames[AUDIO_BUFFER_LENGTH];
} gwenesis_audio_packet_t;

static gwenesis_audio_packet_t *gwenesis_audio_queue;
static rg_task_t *gwenesis_audio_task_handle;
static volatile bool gwenesis_audio_task_running;
static uint32_t gwenesis_audio_queue_read;
static uint32_t gwenesis_audio_queue_write;
static bool gwenesis_cleaned_up;
#if GWENESIS_VDP_ASYNC_ENABLED
static rg_task_t *gwenesis_vdp_async_task_handle;
static volatile bool gwenesis_vdp_async_task_running;
static volatile uint32_t gwenesis_vdp_async_current_frame_id;
static volatile uint32_t gwenesis_vdp_async_unsafe_frames;
#if defined(RG_TARGET_HOLO_DYNMOD)
static volatile bool gwenesis_vdp_async_paused;
#endif
#if GWENESIS_PROFILER_DETAILED
static volatile uint32_t gwenesis_vdp_async_render_count;
static volatile uint32_t gwenesis_vdp_async_render_us;
#endif
#endif

static inline int gwenesis_z80_batch_lines_current(bool yfm_run_enabled)
{
    if (!yfm_run_enabled)
        return GWENESIS_Z80_BATCH_LINES_MUTED;
    if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_BALANCE)
        return GWENESIS_Z80_BATCH_LINES_BALANCE;
    return GWENESIS_Z80_BATCH_LINES_FAST;
}

static inline int gwenesis_ym_batch_lines_current(void)
{
    if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_BALANCE)
        return GWENESIS_YM_BATCH_LINES_BALANCE;
    return GWENESIS_YM_BATCH_LINES_FAST;
}

static int gwenesis_audio_divisor(bool pal_mode)
{
    const int base_divisor = pal_mode ? AUDIO_FREQ_DIVISOR_PAL : AUDIO_FREQ_DIVISOR_NTSC;
    return (base_divisor * GWENESIS_FRAME_TARGET_FPS + (GWENESIS_REFRESH_RATE_PAL / 2)) /
           GWENESIS_REFRESH_RATE_PAL;
}

static void gwenesis_close_savestate(void)
{
    FILE *fp = savestate_fp;
    savestate_fp = NULL;
    if (fp)
        fclose(fp);
}

typedef struct
{
    int64_t last_log_us;
    int64_t m68k_us;
    int64_t z80_us;
    int64_t ym_us;
    int64_t vdp_us;
    uint32_t frames;
    uint32_t draw_frames;
    uint32_t vdp_sync_frames;
} gwenesis_profiler_t;

static gwenesis_profiler_t gwenesis_profiler;

typedef struct
{
    int m68k_ms100;
    int z80_ms100;
    int vdp_ms100;
    int ym_ms100;
    uint32_t frames;
    uint32_t draw_frames;
} gwenesis_perf_overlay_t;

static gwenesis_perf_overlay_t gwenesis_perf_overlay;

#if GWENESIS_PROFILER_DETAILED
static inline bool gwenesis_profiler_active(void)
{
    return gwenesis_perf_overlay_enabled;
}
#endif

static inline int64_t gwenesis_profiler_now(void)
{
#if GWENESIS_PROFILER_DETAILED
    return rg_system_timer();
#else
    return 0;
#endif
}

#if GWENESIS_PROFILER_DETAILED
static inline int gwenesis_profiler_avg_us(int64_t total_us, uint32_t count)
{
    return count > 0 ? (int)((total_us + (count / 2)) / count) : 0;
}

static inline int gwenesis_profiler_centi_ms(int64_t us)
{
    return (int)((us + 5) / 10);
}
#endif

static inline void gwenesis_profiler_add(int64_t *field, int64_t start_us)
{
#if GWENESIS_PROFILER_DETAILED
    *field += rg_system_timer() - start_us;
#else
    (void)field;
    (void)start_us;
#endif
}

#if GWENESIS_PROFILER_DETAILED
#define GWENESIS_PROFILER_INC(field) \
    do { \
        if (gwenesis_profiler_active()) \
            gwenesis_profiler.field++; \
    } while (0)
#define GWENESIS_PROFILER_TIME_START(name) \
    int64_t name = gwenesis_profiler_active() ? gwenesis_profiler_now() : 0
#define GWENESIS_PROFILER_TIME_RESTART(name) \
    do { \
        if (gwenesis_profiler_active()) \
            (name) = gwenesis_profiler_now(); \
    } while (0)
#define GWENESIS_PROFILER_TIME_ADD(field, start_us) \
    do { \
        if (gwenesis_profiler_active() && (start_us) > 0) \
            gwenesis_profiler_add(&gwenesis_profiler.field, (start_us)); \
    } while (0)
#else
#define GWENESIS_PROFILER_INC(field) ((void)0)
#define GWENESIS_PROFILER_TIME_START(name) ((void)0)
#define GWENESIS_PROFILER_TIME_RESTART(name) ((void)0)
#define GWENESIS_PROFILER_TIME_ADD(field, start_us) ((void)0)
#endif

static inline int gwenesis_current_core_id(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    return 1;
#else
    return -1;
#endif
}

#if GWENESIS_PROFILER_DETAILED
static void gwenesis_profiler_format_top_counts(char *out, size_t out_size,
                                                const uint32_t *counts, size_t count,
                                                int hex_width, int limit)
{
    uint8_t selected[GWENESIS_M68K_OP_HI_COUNT] = {0};
    size_t used = 0;

    if (!out || out_size == 0)
        return;

    out[0] = 0;
    if (count > sizeof(selected))
        count = sizeof(selected);

    for (int rank = 0; rank < limit; ++rank)
    {
        size_t best = count;
        uint32_t best_value = 0;

        for (size_t i = 0; i < count; ++i)
        {
            if (!selected[i] && counts[i] > best_value)
            {
                best = i;
                best_value = counts[i];
            }
        }

        if (best == count || best_value == 0)
            break;

        selected[best] = 1;
        int written = snprintf(out + used, out_size - used, "%s%0*x:%u",
                               used ? "," : "", hex_width, (unsigned)best,
                               (unsigned)best_value);
        if (written < 0)
            break;
        if ((size_t)written >= out_size - used)
        {
            out[out_size - 1] = 0;
            return;
        }
        used += (size_t)written;
    }

    if (used == 0)
        snprintf(out, out_size, "-");
}

#if GWENESIS_M68K_PROFILE
static uint32_t gwenesis_profiler_sum_counts(const uint32_t *counts, size_t count)
{
    uint32_t total = 0;

    for (size_t i = 0; i < count; ++i)
        total += counts[i];

    return total;
}

static int gwenesis_profiler_pct_u32(uint32_t part, uint32_t total)
{
    return total > 0 ? (int)(((uint64_t)part * 100 + (total / 2)) / total) : 0;
}

static void gwenesis_profiler_format_top_opcodes(char *out, size_t out_size,
                                                 const gwenesis_m68k_profile_t *profile,
                                                 int limit)
{
    uint8_t selected[GWENESIS_M68K_OP_SLOT_COUNT] = {0};
    size_t used = 0;

    if (!out || out_size == 0)
        return;

    out[0] = 0;

    for (int rank = 0; rank < limit; ++rank)
    {
        size_t best = GWENESIS_M68K_OP_SLOT_COUNT;
        uint32_t best_value = 0;

        for (size_t i = 0; i < GWENESIS_M68K_OP_SLOT_COUNT; ++i)
        {
            if (!selected[i] && profile->op_slot_count[i] > best_value)
            {
                best = i;
                best_value = profile->op_slot_count[i];
            }
        }

        if (best == GWENESIS_M68K_OP_SLOT_COUNT || best_value == 0)
            break;

        selected[best] = 1;
        int written = snprintf(out + used, out_size - used, "%s%04x:%u",
                               used ? "," : "",
                               (unsigned)profile->op_slot_code[best],
                               (unsigned)best_value);
        if (written < 0)
            break;
        if ((size_t)written >= out_size - used)
        {
            out[out_size - 1] = 0;
            return;
        }
        used += (size_t)written;
    }

    if (used == 0)
        snprintf(out, out_size, "-");
}
#endif

static void gwenesis_profiler_log_m68k_mem(void)
{
#if GWENESIS_M68K_ANY_PROFILE
#if GWENESIS_M68K_RAM_PROFILE && !GWENESIS_M68K_PROFILE
    static int sample_logs_left = GWENESIS_RAM_CACHE_SAMPLE_LOGS;
    static int hold_logs_left = 0;
    bool ram_sample_active = gwenesis_m68k_ram_profile_enabled != 0;
    if (ram_sample_active && sample_logs_left <= 0)
        sample_logs_left = GWENESIS_RAM_CACHE_SAMPLE_LOGS;
#else
    const bool ram_sample_active = true;
#endif
    gwenesis_m68k_profile_t profile = gwenesis_m68k_profile;
    memset(&gwenesis_m68k_profile, 0, sizeof(gwenesis_m68k_profile));

#if GWENESIS_M68K_PROFILE
    char rom_pages[80];
    char op_hi[128];
    char op_full[160];
#endif
    char ram_reads[96];
    char ram_writes[96];
    char ram_cache[96];

    if (ram_sample_active)
        gwenesis_bus_m68k_ram_cache_update(profile.ram_page_r, profile.ram_page_w);

#if GWENESIS_M68K_PROFILE
    gwenesis_profiler_format_top_counts(rom_pages, sizeof(rom_pages),
                                        profile.rom_page, GWENESIS_M68K_ROM_PAGE_COUNT, 2, 4);
#endif
    if (ram_sample_active)
    {
        gwenesis_profiler_format_top_counts(ram_reads, sizeof(ram_reads),
                                            profile.ram_page_r, GWENESIS_M68K_RAM_PAGE_COUNT, 1, 4);
        gwenesis_profiler_format_top_counts(ram_writes, sizeof(ram_writes),
                                            profile.ram_page_w, GWENESIS_M68K_RAM_PAGE_COUNT, 1, 4);
    }
    else
    {
        snprintf(ram_reads, sizeof(ram_reads), "-");
        snprintf(ram_writes, sizeof(ram_writes), "-");
    }
#if GWENESIS_M68K_PROFILE
    gwenesis_profiler_format_top_counts(op_hi, sizeof(op_hi),
                                        profile.op_hi, GWENESIS_M68K_OP_HI_COUNT, 2, 6);
    gwenesis_profiler_format_top_opcodes(op_full, sizeof(op_full), &profile, 8);
#endif
    gwenesis_bus_m68k_ram_cache_status(ram_cache, sizeof(ram_cache));

#if GWENESIS_M68K_PROFILE
    const uint32_t mem_total = gwenesis_profiler_sum_counts(profile.mem_kind, GWENESIS_M68K_MEM_COUNT);
    const uint32_t mem_direct = profile.mem_kind[GWENESIS_M68K_MEM_ROM] +
                                profile.mem_kind[GWENESIS_M68K_MEM_RAM_R] +
                                profile.mem_kind[GWENESIS_M68K_MEM_RAM_W];
    const uint32_t mem_fallback = mem_total > mem_direct ? mem_total - mem_direct : 0;
    RG_LOGI("gwen mem: romkind op=%u imm=%u pc=%u data=%u rompg=%s ramr=%s ramw=%s",
            (unsigned)profile.rom_kind[GWENESIS_M68K_ROM_KIND_OPCODE],
            (unsigned)profile.rom_kind[GWENESIS_M68K_ROM_KIND_IMM],
            (unsigned)profile.rom_kind[GWENESIS_M68K_ROM_KIND_PCREL],
            (unsigned)profile.rom_kind[GWENESIS_M68K_ROM_KIND_DATA],
            rom_pages,
            ram_reads,
            ram_writes);
    RG_LOGI("gwen op: hi=%s full=%s ovf=%u", op_hi, op_full,
            (unsigned)profile.op_slot_overflow);
    RG_LOGI("gwen ea-low: d=%u a=%u ai=%u pi=%u pd=%u di=%u ix=%u aw=%u al=%u pcdi=%u pcix=%u imm=%u br=%u db=%u oth=%u",
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_DREG],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_AREG],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_AI],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_PI],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_PD],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_DI],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_IX],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_AW],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_AL],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_PCDI],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_PCIX],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_IMM],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_BRANCH],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_DBCC],
            (unsigned)profile.ea_low[GWENESIS_M68K_EA_OTHER]);
    RG_LOGI("gwen ea-dst: d=%u a=%u ai=%u pi=%u pd=%u di=%u ix=%u aw=%u al=%u oth=%u",
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_DREG],
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_AREG],
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_AI],
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_PI],
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_PD],
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_DI],
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_IX],
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_AW],
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_AL],
            (unsigned)profile.ea_dst[GWENESIS_M68K_EA_OTHER]);
    RG_LOGI("gwen memmix: direct=%d%% fb=%d%% rom=%u ramr=%u ramw=%u sram=%u vdp=%u io=%u z80=%u ym=%u psg=%u tmss=%u other=%u",
            gwenesis_profiler_pct_u32(mem_direct, mem_total),
            gwenesis_profiler_pct_u32(mem_fallback, mem_total),
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_ROM],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_RAM_R],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_RAM_W],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_SRAM],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_VDP],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_IO],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_Z80],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_YM2612],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_PSG],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_TMSS],
            (unsigned)profile.mem_kind[GWENESIS_M68K_MEM_OTHER]);
#else
    RG_LOGI("gwen mem: ramr=%s ramw=%s mode=%s next=%d",
            ram_reads,
            ram_writes,
            ram_sample_active ? "sample" : "hold",
            ram_sample_active ? sample_logs_left : hold_logs_left);
#endif
    RG_LOGI("gwen ramcache: %s", ram_cache);

#if GWENESIS_M68K_RAM_PROFILE && !GWENESIS_M68K_PROFILE
    if (ram_sample_active)
    {
        if (--sample_logs_left <= 0)
        {
            gwenesis_m68k_ram_profile_enabled = 0;
            hold_logs_left = GWENESIS_RAM_CACHE_HOLD_LOGS;
        }
    }
    else if (--hold_logs_left <= 0)
    {
        memset(&gwenesis_m68k_profile, 0, sizeof(gwenesis_m68k_profile));
        gwenesis_m68k_ram_profile_enabled = GWENESIS_M68K_RAM_SAMPLE_DEFAULT;
        sample_logs_left = GWENESIS_RAM_CACHE_SAMPLE_LOGS;
    }
#endif
#endif
}

static void gwenesis_profiler_reset(int64_t now)
{
    memset(&gwenesis_profiler, 0, sizeof(gwenesis_profiler));
    gwenesis_profiler.last_log_us = now;
#if GWENESIS_VDP_ASYNC_ENABLED
    gwenesis_vdp_async_render_count = 0;
    gwenesis_vdp_async_render_us = 0;
#endif
}

static void gwenesis_profiler_maybe_log(void)
{
    const int64_t now = rg_system_timer();
    if (gwenesis_profiler.last_log_us == 0)
    {
        gwenesis_profiler.last_log_us = now;
        return;
    }
    if (now - gwenesis_profiler.last_log_us < 1000000 || gwenesis_profiler.frames == 0)
        return;

#if GWENESIS_VDP_ASYNC_ENABLED
    const uint32_t async_vdp_renders = gwenesis_vdp_async_render_count;
    const uint32_t async_vdp_render_us = gwenesis_vdp_async_render_us;
#else
    const uint32_t async_vdp_renders = 0;
    const uint32_t async_vdp_render_us = 0;
#endif
    const int m68k_avg_us = gwenesis_profiler_avg_us(gwenesis_profiler.m68k_us,
                                                     gwenesis_profiler.frames);
    const int z80_avg_us = gwenesis_profiler_avg_us(gwenesis_profiler.z80_us,
                                                    gwenesis_profiler.frames);
    const uint32_t vdp_sync_frames = gwenesis_profiler.vdp_sync_frames;
    const uint32_t vdp_total_render_frames = vdp_sync_frames + async_vdp_renders;
    const int vdp_avg_us = vdp_total_render_frames > 0
                               ? (int)((gwenesis_profiler.vdp_us + async_vdp_render_us +
                                         (vdp_total_render_frames / 2)) /
                                       vdp_total_render_frames)
                               : 0;
    const int ym_main_avg_us = gwenesis_profiler_avg_us(gwenesis_profiler.ym_us,
                                                        gwenesis_profiler.frames);
    const int m68k_avg_ms100 = gwenesis_profiler_centi_ms(m68k_avg_us);
    const int z80_avg_ms100 = gwenesis_profiler_centi_ms(z80_avg_us);
    const int vdp_avg_ms100 = gwenesis_profiler_centi_ms(vdp_avg_us);
    const int ym_main_avg_ms100 = gwenesis_profiler_centi_ms(ym_main_avg_us);
    gwenesis_perf_overlay.m68k_ms100 = m68k_avg_ms100;
    gwenesis_perf_overlay.z80_ms100 = z80_avg_ms100;
    gwenesis_perf_overlay.vdp_ms100 = vdp_avg_ms100;
    gwenesis_perf_overlay.ym_ms100 = ym_main_avg_ms100;
    gwenesis_perf_overlay.frames = gwenesis_profiler.frames;
    gwenesis_perf_overlay.draw_frames = gwenesis_profiler.draw_frames;

    gwenesis_profiler_reset(now);
}
#endif

static void gwenesis_frameskip_reset(int64_t now)
{
    frame_pacer_next_us = now;
#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_FIXED_DRAW_SKIP
    frameskip_fixed_phase = 0;
#else
    frameskip_draw_streak = 0;
    frameskip_skip_streak = 0;
    frameskip_last_debt_us = 0;
#if defined(RG_TARGET_HOLO_DYNMOD)
    render_pacer_next_us = now;
#endif
#endif
    frame_yield_count = 0;
}

static bool gwenesis_frameskip_should_draw(bool audio_enabled, int64_t now)
{
    if (frame_pacer_next_us == 0)
        gwenesis_frameskip_reset(now);

#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_FIXED_DRAW_SKIP
    const bool draw = frameskip_fixed_phase == 0 ||
                      frameskip_fixed_phase == 2 ||
                      frameskip_fixed_phase == 3;
    if (++frameskip_fixed_phase == 5)
        frameskip_fixed_phase = 0;
    (void)audio_enabled;
    return draw;
#else
    int64_t debt_us = now - frame_pacer_next_us;
    if (debt_us > frame_target_us * 4)
    {
        gwenesis_frameskip_reset(now);
        debt_us = 0;
    }
    else if (debt_us < 0)
    {
        debt_us = 0;
    }

    if (debt_us > INT32_MAX)
        debt_us = INT32_MAX;
    frameskip_last_debt_us = (int)debt_us;

#if defined(RG_TARGET_HOLO_DYNMOD)
    (void)audio_enabled;
    if (now < render_pacer_next_us)
        return false;

    const int64_t lateness_us = now - render_pacer_next_us;
    render_pacer_next_us += render_target_us;
    if (lateness_us > render_target_us * 4 || render_pacer_next_us <= now)
        render_pacer_next_us = now + render_target_us;

    if (frameskip_last_debt_us >= GWENESIS_RENDER_SKIP_DEBT_US)
        return false;
    return true;
#else
    if (frameskip_skip_streak >= frameskip_max_consecutive_skips)
        return true;

    const int skip_debt_us = audio_enabled ? frameskip_audio_on_debt_us
                                           : frameskip_audio_off_debt_us;
    const int force_debt_us = audio_enabled ? frameskip_audio_on_force_debt_us
                                            : frameskip_audio_off_force_debt_us;
    const int min_draws = audio_enabled ? frameskip_audio_on_min_draws
                                        : frameskip_audio_off_min_draws;

    if (debt_us >= force_debt_us)
        return false;
    if (frameskip_draw_streak < min_draws)
        return true;
    return debt_us < skip_debt_us;
#endif
#endif
}

#if !(defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_FIXED_DRAW_SKIP)
static void gwenesis_frameskip_note_frame(bool drew_frame, int64_t now)
{
    if (drew_frame)
    {
        frameskip_draw_streak++;
        frameskip_skip_streak = 0;
    }
    else
    {
        frameskip_draw_streak = 0;
        frameskip_skip_streak++;
    }
    (void)now;
}
#endif

static void gwenesis_pace_frame(void)
{
    const int64_t now = rg_system_timer();

    if (frame_pacer_next_us == 0)
        gwenesis_frameskip_reset(now);

    frame_pacer_next_us += frame_target_us;

    if (now < frame_pacer_next_us)
    {
        rg_usleep((uint32_t)(frame_pacer_next_us - now));
        return;
    }

    if (now - frame_pacer_next_us > frame_target_us * 2)
        gwenesis_frameskip_reset(now);

    if (frame_min_yield_ms > 0 && frame_min_yield_interval_frames > 0)
    {
        frame_yield_count++;
        if (frame_yield_count >= frame_min_yield_interval_frames)
        {
            frame_yield_count = 0;
            rg_task_delay((uint32_t)frame_min_yield_ms);
            return;
        }
    }

}

static int16_t gwenesis_audio_clip(int32_t value)
{
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return (int16_t)value;
}

static inline int32_t gwenesis_audio_scale(int32_t value)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    return (value * 4) / 3;
#else
    return value;
#endif
}

#if GWENESIS_AUDIO_EQ_ENABLED
#define GWENESIS_AUDIO_EQ_BANDS 2
#define GWENESIS_AUDIO_EQ_PI 3.14159265358979323846f

typedef struct
{
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1;
    float z2;
} gwenesis_audio_biquad_t;

typedef struct
{
    float freq_hz;
    float gain_db;
    float q;
} gwenesis_audio_eq_band_t;

static const gwenesis_audio_eq_band_t gwenesis_audio_eq_default[GWENESIS_AUDIO_EQ_BANDS] = {
    /* 300Hz +5.5dB, Q=0.667 (150-600Hz), fs=11050 */
    {300.0f, 5.5f, 0.667f},
    /* 1.8kHz -3dB, Q=0.6 (1k-4kHz), fs=11050 */
    {1800.0f, -3.0f, 0.6f},
};
static gwenesis_audio_biquad_t gwenesis_audio_eq[GWENESIS_AUDIO_EQ_BANDS];
static bool gwenesis_audio_eq_configured;

static bool gwenesis_audio_eq_design_peaking(gwenesis_audio_biquad_t *out,
                                             float freq_hz,
                                             float gain_db,
                                             float q)
{
    const float sample_rate = (float)AUDIO_OUTPUT_SAMPLE_RATE;
    float w0;
    float alpha;
    float a;
    float a0;
    float inv_a0;
    float cos_w0;

    if (!out || !(freq_hz > 0.0f) || !(freq_hz < sample_rate * 0.49f) ||
        !(gain_db >= -24.0f) || !(gain_db <= 24.0f) ||
        !(q >= 0.05f) || !(q <= 10.0f))
        return false;

    a = powf(10.0f, gain_db / 40.0f);
    if (!(a > 0.0f))
        return false;

    w0 = 2.0f * GWENESIS_AUDIO_EQ_PI * freq_hz / sample_rate;
    cos_w0 = cosf(w0);
    alpha = sinf(w0) / (2.0f * q);
    a0 = 1.0f + alpha / a;
    if (!(a0 > 0.0f))
        return false;

    inv_a0 = 1.0f / a0;
    out->b0 = (1.0f + alpha * a) * inv_a0;
    out->b1 = (-2.0f * cos_w0) * inv_a0;
    out->b2 = (1.0f - alpha * a) * inv_a0;
    out->a1 = (-2.0f * cos_w0) * inv_a0;
    out->a2 = (1.0f - alpha / a) * inv_a0;
    out->z1 = 0.0f;
    out->z2 = 0.0f;
    return true;
}

void gwenesis_audio_eq_restore_default(void)
{
    gwenesis_audio_biquad_t next[GWENESIS_AUDIO_EQ_BANDS];

    for (size_t i = 0; i < RG_COUNT(next); ++i)
    {
        if (!gwenesis_audio_eq_design_peaking(&next[i],
                                              gwenesis_audio_eq_default[i].freq_hz,
                                              gwenesis_audio_eq_default[i].gain_db,
                                              gwenesis_audio_eq_default[i].q))
            return;
    }
    memcpy(gwenesis_audio_eq, next, sizeof(gwenesis_audio_eq));
    gwenesis_audio_eq_configured = true;
}

bool gwenesis_audio_eq_set_peaking_bands(const float *freq_hz,
                                         const float *gain_db,
                                         const float *q,
                                         size_t count)
{
    gwenesis_audio_biquad_t next[GWENESIS_AUDIO_EQ_BANDS];

    if (!freq_hz || !gain_db || !q || count != GWENESIS_AUDIO_EQ_BANDS || gwenesis_audio_task_running)
        return false;

    for (size_t i = 0; i < RG_COUNT(next); ++i)
    {
        if (!gwenesis_audio_eq_design_peaking(&next[i], freq_hz[i], gain_db[i], q[i]))
            return false;
    }

    memcpy(gwenesis_audio_eq, next, sizeof(gwenesis_audio_eq));
    gwenesis_audio_eq_configured = true;
    return true;
}

static void gwenesis_audio_eq_reset(void)
{
    if (!gwenesis_audio_eq_configured)
        gwenesis_audio_eq_restore_default();
    for (size_t i = 0; i < RG_COUNT(gwenesis_audio_eq); ++i)
    {
        gwenesis_audio_eq[i].z1 = 0.0f;
        gwenesis_audio_eq[i].z2 = 0.0f;
    }
}

static inline float gwenesis_audio_biquad_process(gwenesis_audio_biquad_t *bq, float input)
{
    const float output = bq->b0 * input + bq->z1;
    bq->z1 = bq->b1 * input - bq->a1 * output + bq->z2;
    bq->z2 = bq->b2 * input - bq->a2 * output;
    return output;
}

static inline int32_t gwenesis_audio_eq_process_sample(int16_t sample)
{
    float output = (float)sample;
    for (size_t i = 0; i < RG_COUNT(gwenesis_audio_eq); ++i)
        output = gwenesis_audio_biquad_process(&gwenesis_audio_eq[i], output);
    return (int32_t)(output + (output >= 0.0f ? 0.5f : -0.5f));
}

static void gwenesis_audio_eq_process_frames(rg_audio_frame_t *frames, size_t count)
{
    if (!frames)
        return;

    for (size_t i = 0; i < count; ++i)
    {
        const int16_t mono = (int16_t)(((int32_t)frames[i].left + frames[i].right) / 2);
        const int16_t filtered = gwenesis_audio_clip(gwenesis_audio_eq_process_sample(mono));
        frames[i].left = filtered;
        frames[i].right = filtered;
    }
}
#else
void gwenesis_audio_eq_restore_default(void) {}
bool gwenesis_audio_eq_set_peaking_bands(const float *freq_hz,
                                         const float *gain_db,
                                         const float *q,
                                         size_t count)
{
    (void)freq_hz;
    (void)gain_db;
    (void)q;
    (void)count;
    return false;
}
static void gwenesis_audio_eq_reset(void) {}
static void gwenesis_audio_eq_process_frames(rg_audio_frame_t *frames, size_t count)
{
    (void)frames;
    (void)count;
}
#endif

static bool gwenesis_audio_queue_push(const rg_audio_frame_t *frames, size_t count)
{
    if (!gwenesis_audio_queue || !frames || count == 0)
        return false;

    if (count > AUDIO_BUFFER_LENGTH)
        count = AUDIO_BUFFER_LENGTH;

    while (gwenesis_audio_task_running)
    {
        const uint32_t read = __atomic_load_n(&gwenesis_audio_queue_read, __ATOMIC_ACQUIRE);
        const uint32_t write = __atomic_load_n(&gwenesis_audio_queue_write, __ATOMIC_ACQUIRE);
        const uint32_t used = write - read;

        if (used < GWENESIS_AUDIO_QUEUE_PACKETS)
        {
            gwenesis_audio_packet_t *packet = &gwenesis_audio_queue[write % GWENESIS_AUDIO_QUEUE_PACKETS];
            packet->count = count;
            memcpy(packet->frames, frames, count * sizeof(packet->frames[0]));
            __atomic_store_n(&gwenesis_audio_queue_write, write + 1, __ATOMIC_RELEASE);
            return true;
        }

        rg_task_delay(1);
    }

    return false;
}

static void gwenesis_audio_make_fade_out(rg_audio_frame_t *frames, size_t count, rg_audio_frame_t start)
{
    if (!frames || count == 0)
        return;

    if (count == 1)
    {
        frames[0].left = 0;
        frames[0].right = 0;
        return;
    }

    const int32_t denom = (int32_t)(count - 1);
    for (size_t i = 0; i < count; ++i)
    {
        const int32_t scale = (int32_t)(count - 1 - i);
        frames[i].left = (int16_t)(((int32_t)start.left * scale) / denom);
        frames[i].right = (int16_t)(((int32_t)start.right * scale) / denom);
    }
}

static void gwenesis_audio_apply_fade_in(rg_audio_frame_t *frames, size_t count)
{
    if (!frames || count == 0)
        return;

    const size_t fade_count = RG_MIN(count, (size_t)GWENESIS_AUDIO_UNDERFLOW_CHUNK_FRAMES);
    for (size_t i = 0; i < fade_count; ++i)
    {
        const int32_t scale = (int32_t)(i + 1);
        const int32_t denom = (int32_t)fade_count;
        frames[i].left = (int16_t)(((int32_t)frames[i].left * scale) / denom);
        frames[i].right = (int16_t)(((int32_t)frames[i].right * scale) / denom);
    }
}
static void gwenesis_audio_task(void *arg)
{
    rg_audio_frame_t generated[GWENESIS_AUDIO_UNDERFLOW_CHUNK_FRAMES];
    rg_audio_frame_t last_frame = {0, 0};
    bool have_last_frame = false;
    bool prebuffering = true;
    bool was_underflowing = false;
    (void)arg;

    while (gwenesis_audio_task_running ||
           __atomic_load_n(&gwenesis_audio_queue_read, __ATOMIC_ACQUIRE) !=
               __atomic_load_n(&gwenesis_audio_queue_write, __ATOMIC_ACQUIRE))
    {
        uint32_t read = __atomic_load_n(&gwenesis_audio_queue_read, __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&gwenesis_audio_queue_write, __ATOMIC_ACQUIRE);
        const uint32_t used = write - read;

        if (gwenesis_audio_task_running && prebuffering && used < GWENESIS_AUDIO_PREBUFFER_PACKETS)
        {
            rg_task_delay(1);
            continue;
        }
        prebuffering = false;

        if (read == write)
        {
            if (have_last_frame)
            {
                printf("[warn] Genesis audio underflow queue empty\n");
                gwenesis_audio_make_fade_out(generated, RG_COUNT(generated), last_frame);
                have_last_frame = false;
                rg_audio_submit(generated, RG_COUNT(generated));
                was_underflowing = true;
            }
            prebuffering = true;
            rg_task_delay(1);
            continue;
        }

        gwenesis_audio_packet_t *packet = &gwenesis_audio_queue[read % GWENESIS_AUDIO_QUEUE_PACKETS];
        const size_t count = RG_MIN(packet->count, (size_t)AUDIO_BUFFER_LENGTH);

        if (count > 0)
        {
            gwenesis_audio_eq_process_frames(packet->frames, count);
            if (was_underflowing)
            {
                gwenesis_audio_apply_fade_in(packet->frames, count);
                was_underflowing = false;
            }
            rg_audio_submit(packet->frames, count);
            last_frame = packet->frames[count - 1];
            have_last_frame = true;
        }

        read++;
        __atomic_store_n(&gwenesis_audio_queue_read, read, __ATOMIC_RELEASE);
    }

    gwenesis_audio_task_handle = NULL;
}

static bool gwenesis_audio_start(void)
{
    gwenesis_audio_mix_buffer = rg_alloc(sizeof(*gwenesis_audio_mix_buffer) * AUDIO_BUFFER_LENGTH,
                                         MEM_FAST | MEM_NOPANIC);
    gwenesis_audio_queue = rg_alloc(sizeof(*gwenesis_audio_queue) * GWENESIS_AUDIO_QUEUE_PACKETS,
                                    GWENESIS_AUDIO_QUEUE_MEM | MEM_NOPANIC);
    if (!gwenesis_audio_mix_buffer || !gwenesis_audio_queue)
        goto fail;

    __atomic_store_n(&gwenesis_audio_queue_read, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gwenesis_audio_queue_write, 0, __ATOMIC_RELEASE);
    gwenesis_audio_eq_reset();

    gwenesis_audio_task_running = true;
    gwenesis_audio_task_handle = rg_task_create("gwen_audio", gwenesis_audio_task, NULL,
                                                GWENESIS_AUDIO_TASK_STACK, RG_TASK_PRIORITY_3,
                                                GWENESIS_AUDIO_TASK_CORE);
    if (!gwenesis_audio_task_handle)
    {
        gwenesis_audio_task_running = false;
        free(gwenesis_audio_queue);
        gwenesis_audio_queue = NULL;
        RG_LOGW("Genesis audio task unavailable, using direct audio\n");
    }
    else
    {
        RG_LOGI("Genesis audio task started on core %d, queue=%d packets\n",
                GWENESIS_AUDIO_TASK_CORE, GWENESIS_AUDIO_QUEUE_PACKETS);
    }

    return true;

fail:
    free(gwenesis_audio_mix_buffer);
    gwenesis_audio_mix_buffer = NULL;
    free(gwenesis_audio_queue);
    gwenesis_audio_queue = NULL;
    return false;
}

static void gwenesis_audio_stop(void)
{
    if (gwenesis_audio_task_handle)
    {
        const uint32_t write = __atomic_load_n(&gwenesis_audio_queue_write, __ATOMIC_ACQUIRE);
        __atomic_store_n(&gwenesis_audio_queue_read, write, __ATOMIC_RELEASE);
        gwenesis_audio_task_running = false;

        int waited = 0;
        while (rg_task_find("gwen_audio"))
        {
            if (waited++ == 500)
                RG_LOGW("Waiting for Genesis audio task to stop\n");
            rg_task_delay(1);
        }
    }

    gwenesis_audio_task_handle = NULL;
    gwenesis_audio_task_running = false;
    free(gwenesis_audio_queue);
    gwenesis_audio_queue = NULL;
    free(gwenesis_audio_mix_buffer);
    gwenesis_audio_mix_buffer = NULL;
}

static void gwenesis_audio_submit_frames(rg_audio_frame_t *frames, size_t count)
{
    if (!frames || count == 0)
        return;

    if (gwenesis_audio_task_handle && gwenesis_audio_queue_push(frames, count))
        return;

    gwenesis_audio_eq_process_frames(frames, count);
    rg_audio_submit(frames, count);
}

static void gwenesis_audio_submit_frame(size_t count)
{
    if (rg_audio_get_mute())
        return;
    if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE || !gwenesis_audio_mix_buffer || count == 0)
        return;

    if (count > AUDIO_BUFFER_LENGTH)
        count = AUDIO_BUFFER_LENGTH;

    for (size_t i = 0; i < count; ++i)
    {
        const int32_t sample = gwenesis_audio_scale(gwenesis_ym2612_buffer[i]);
        const int16_t clipped = gwenesis_audio_clip(sample);
        gwenesis_audio_mix_buffer[i].left = clipped;
        gwenesis_audio_mix_buffer[i].right = clipped;
    }

    gwenesis_audio_submit_frames(gwenesis_audio_mix_buffer, count);
}

#if defined(RG_TARGET_HOLO_DYNMOD)
static void gwenesis_clear_surface(void)
{
    for (size_t i = 0; i < RG_COUNT(updates); ++i)
    {
        if (updates[i] && updates[i]->data)
            memset(updates[i]->data, 0, updates[i]->stride * GWENESIS_SURFACE_HEIGHT);
    }
}

static inline bool gwenesis_display_double_buffered(void)
{
    return updates[1] != NULL;
}

static inline rg_surface_t *gwenesis_display_visible_surface(void)
{
    return displayUpdate ? displayUpdate : currentUpdate;
}

static void gwenesis_display_flip_surface(void)
{
    if (gwenesis_display_double_buffered())
        currentUpdate = (currentUpdate == updates[0]) ? updates[1] : updates[0];
}

static void gwenesis_force_native_display(void)
{
    rg_display_set_scaling(RG_DISPLAY_SCALING_OFF);
    rg_display_set_filter(RG_DISPLAY_FILTER_OFF);
}
#endif

#if GWENESIS_PROFILER_DETAILED
#define GWENESIS_PERF_OVERLAY_SCALE 2
#define GWENESIS_PERF_OVERLAY_CHAR_W (3 * GWENESIS_PERF_OVERLAY_SCALE)
#define GWENESIS_PERF_OVERLAY_CHAR_STEP (4 * GWENESIS_PERF_OVERLAY_SCALE)
#define GWENESIS_PERF_OVERLAY_LINE_H (6 * GWENESIS_PERF_OVERLAY_SCALE)
#define GWENESIS_PERF_OVERLAY_MAX_CHARS 9
#define GWENESIS_PERF_OVERLAY_LINES 5

static uint16_t gwenesis_overlay_surface_color(const rg_surface_t *surface, uint16_t color)
{
    if (surface && surface->format == RG_PIXEL_PAL565_BE)
        return (color << 8) | (color >> 8);
    return color;
}

static int gwenesis_overlay_luma(uint16_t color)
{
    const int r = (color >> 11) & 0x1f;
    const int g = (color >> 5) & 0x3f;
    const int b = color & 0x1f;
    return r * 54 + g * 183 + b * 18;
}

static void gwenesis_overlay_select_colors(rg_surface_t *surface, uint8_t *bg, uint8_t *fg)
{
    int darkest = 0;
    int brightest = 0;
    int darkest_luma = 0x7fffffff;
    int brightest_luma = -1;

    *bg = 0;
    *fg = 0;
    if (!surface || !surface->palette)
        return;

    for (int i = 0; i < 256; ++i)
    {
        uint16_t color = surface->palette[i];
        if (surface->format == RG_PIXEL_PAL565_BE)
            color = (color << 8) | (color >> 8);
        const int luma = gwenesis_overlay_luma(color);
        if (luma < darkest_luma)
        {
            darkest_luma = luma;
            darkest = i;
        }
        if (luma > brightest_luma)
        {
            brightest_luma = luma;
            brightest = i;
        }
    }

    if (brightest_luma - darkest_luma < 256)
    {
        darkest = 254;
        brightest = 255;
        surface->palette[darkest] = gwenesis_overlay_surface_color(surface, 0x0000);
        surface->palette[brightest] = gwenesis_overlay_surface_color(surface, 0xffff);
    }

    *bg = (uint8_t)darkest;
    *fg = (uint8_t)brightest;
}

static uint8_t gwenesis_overlay_digit_row(char c, int row)
{
    static const uint8_t digits[10][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
        {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
    };

    return digits[c - '0'][row];
}

static uint8_t gwenesis_overlay_glyph_row(char c, int row)
{
    if (c >= '0' && c <= '9')
        return gwenesis_overlay_digit_row(c, row);

    switch (c)
    {
    case 'D':
        return (uint8_t[]){6, 5, 5, 5, 6}[row];
    case 'F':
        return (uint8_t[]){7, 4, 6, 4, 4}[row];
    case 'K':
        return (uint8_t[]){5, 5, 6, 5, 5}[row];
    case 'M':
        return (uint8_t[]){5, 7, 7, 5, 5}[row];
    case 'V':
        return (uint8_t[]){5, 5, 5, 5, 2}[row];
    case 'Y':
        return (uint8_t[]){5, 5, 2, 2, 2}[row];
    case 'Z':
        return (uint8_t[]){7, 1, 2, 4, 7}[row];
    case '.':
        return (uint8_t[]){0, 0, 0, 0, 2}[row];
    case ' ':
    default:
        return 0;
    }
}

static void gwenesis_overlay_draw_char(uint8_t *base, int stride, int x, int y, char c, uint8_t color)
{
    for (int row = 0; row < 5; ++row)
    {
        const uint8_t bits = gwenesis_overlay_glyph_row(c, row);
        for (int col = 0; col < 3; ++col)
        {
            if (!(bits & (1 << (2 - col))))
                continue;

            const int px = x + col * GWENESIS_PERF_OVERLAY_SCALE;
            const int py = y + row * GWENESIS_PERF_OVERLAY_SCALE;
            for (int sy = 0; sy < GWENESIS_PERF_OVERLAY_SCALE; ++sy)
            {
                uint8_t *dst = base + (py + sy) * stride + px;
                for (int sx = 0; sx < GWENESIS_PERF_OVERLAY_SCALE; ++sx)
                    dst[sx] = color;
            }
        }
    }
}

static void gwenesis_overlay_draw_text(uint8_t *base, int stride, int x, int y,
                                       const char *text, uint8_t color)
{
    for (int i = 0; text[i] && i < GWENESIS_PERF_OVERLAY_MAX_CHARS; ++i)
        gwenesis_overlay_draw_char(base, stride, x + i * GWENESIS_PERF_OVERLAY_CHAR_STEP, y, text[i], color);
}

static void gwenesis_overlay_fill_rect(uint8_t *base, int stride, int x, int y,
                                       int width, int height, uint8_t color)
{
    for (int row = 0; row < height; ++row)
        memset(base + (y + row) * stride + x, color, width);
}

static void gwenesis_perf_overlay_format(char *out, size_t out_size,
                                         const char *label, int ms100)
{
    if (!out || out_size < 10)
        return;

    if (ms100 < 0)
        ms100 = 0;
    if (ms100 > 9999)
        ms100 = 9999;

    const int whole = ms100 / 100;
    const int frac = ms100 % 100;
    out[0] = label[0] ? label[0] : ' ';
    out[1] = label[1] ? label[1] : ' ';
    out[2] = label[2] ? label[2] : ' ';
    out[3] = label[3] ? label[3] : ' ';
    out[4] = whole >= 10 ? (char)('0' + ((whole / 10) % 10)) : ' ';
    out[5] = (char)('0' + (whole % 10));
    out[6] = '.';
    out[7] = (char)('0' + (frac / 10));
    out[8] = (char)('0' + (frac % 10));
    out[9] = 0;
}

static void gwenesis_perf_overlay_draw(rg_surface_t *surface)
{
    if (!gwenesis_perf_overlay_enabled || !surface || !surface->data ||
        !surface->palette || !(surface->format & RG_PIXEL_PALETTE))
        return;

    const int overlay_width = GWENESIS_PERF_OVERLAY_MAX_CHARS * GWENESIS_PERF_OVERLAY_CHAR_STEP +
                              GWENESIS_PERF_OVERLAY_SCALE * 2;
    const int overlay_height = GWENESIS_PERF_OVERLAY_LINES * GWENESIS_PERF_OVERLAY_LINE_H +
                               GWENESIS_PERF_OVERLAY_SCALE * 2;
    if (surface->width < overlay_width || surface->height < overlay_height)
        return;

    uint8_t bg;
    uint8_t fg;
    char line[16];
    uint8_t *base = (uint8_t *)surface->data + surface->offset;
    const int x = surface->width - overlay_width - 2;
    const int y = 2;
    const int text_x = x + GWENESIS_PERF_OVERLAY_SCALE;
    const int text_y = y + GWENESIS_PERF_OVERLAY_SCALE;

    gwenesis_overlay_select_colors(surface, &bg, &fg);
    gwenesis_overlay_fill_rect(base, surface->stride, x, y, overlay_width, overlay_height, bg);

    gwenesis_perf_overlay_format(line, sizeof(line), "M68K", gwenesis_perf_overlay.m68k_ms100);
    gwenesis_overlay_draw_text(base, surface->stride, text_x, text_y, line, fg);
    gwenesis_perf_overlay_format(line, sizeof(line), "Z80 ", gwenesis_perf_overlay.z80_ms100);
    gwenesis_overlay_draw_text(base, surface->stride, text_x, text_y + GWENESIS_PERF_OVERLAY_LINE_H, line, fg);
    gwenesis_perf_overlay_format(line, sizeof(line), "VDP ", gwenesis_perf_overlay.vdp_ms100);
    gwenesis_overlay_draw_text(base, surface->stride, text_x, text_y + GWENESIS_PERF_OVERLAY_LINE_H * 2, line, fg);
    gwenesis_perf_overlay_format(line, sizeof(line), "YM0 ", gwenesis_perf_overlay.ym_ms100);
    gwenesis_overlay_draw_text(base, surface->stride, text_x, text_y + GWENESIS_PERF_OVERLAY_LINE_H * 3, line, fg);
    snprintf(line, sizeof(line), "F%02u D%02u",
             (unsigned)(gwenesis_perf_overlay.frames > 99 ? 99 : gwenesis_perf_overlay.frames),
             (unsigned)(gwenesis_perf_overlay.draw_frames > 99 ? 99 : gwenesis_perf_overlay.draw_frames));
    gwenesis_overlay_draw_text(base, surface->stride, text_x, text_y + GWENESIS_PERF_OVERLAY_LINE_H * 4, line, fg);
}
#else
static void gwenesis_perf_overlay_draw(rg_surface_t *surface)
{
    (void)surface;
}
#endif

#if GWENESIS_VDP_ASYNC_ENABLED
typedef enum
{
    GWENESIS_VDP_JOB_FREE = 0,
    GWENESIS_VDP_JOB_PREPARING,
    GWENESIS_VDP_JOB_FILLING,
    GWENESIS_VDP_JOB_READY,
    GWENESIS_VDP_JOB_RENDERING,
    GWENESIS_VDP_JOB_DISPLAYING,
    GWENESIS_VDP_JOB_DONE,
    GWENESIS_VDP_JOB_HELD,
} gwenesis_vdp_async_state_t;

typedef struct
{
    volatile uint32_t state;
    volatile uint32_t discard;
    uint32_t frame_id;
    unsigned char *vram;
    unsigned short *vsram;
    unsigned short *cram565;
    unsigned char *sat_cache;
    unsigned char *regs;
    uint32_t pending_vram_dirty[GWENESIS_VDP_SNAPSHOT_DIRTY_WORDS];
    int screen_width;
    int screen_height;
    rg_surface_t *surface;
} gwenesis_vdp_async_job_t;

#if defined(RG_TARGET_HOLO_DYNMOD)
static gwenesis_vdp_async_job_t *gwenesis_vdp_async_jobs;
#else
static gwenesis_vdp_async_job_t gwenesis_vdp_async_jobs[GWENESIS_VDP_ASYNC_JOBS];
#endif
#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_VDP_ASYNC_VRAM_MEM == MEM_FAST
static unsigned char *gwenesis_vdp_async_reserved_vram[GWENESIS_VDP_ASYNC_JOBS];
#endif
static void gwenesis_vdp_async_stop(void);
static void gwenesis_vdp_async_wait_idle_for_sync(void);

#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_VDP_ASYNC_VRAM_MEM == MEM_FAST
static void *gwenesis_vdp_async_internal_calloc(size_t size)
{
    const module_host_api_v2 *host = holo_port_host();
    const size_t need_heap = offsetof(module_host_api_v2, heap) + sizeof(module_heap_api_t);
    if (!host || host->size < need_heap ||
        host->heap.size < sizeof(module_heap_api_t) ||
        !host->heap.calloc)
    {
        return NULL;
    }
    return host->heap.calloc(1, size, MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
}

static void gwenesis_vdp_async_release_reserved_vram(void)
{
    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        free(gwenesis_vdp_async_reserved_vram[i]);
        gwenesis_vdp_async_reserved_vram[i] = NULL;
    }
}

bool gwenesis_vdp_async_reserve_vram_early(void)
{
    bool ok = true;

    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        if (gwenesis_vdp_async_reserved_vram[i])
            continue;

#if defined(ESP_PLATFORM)
        const size_t internal_before =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t spiram_before =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        unsigned char *ptr = gwenesis_vdp_async_internal_calloc(VRAM_MAX_SIZE);
#if defined(ESP_PLATFORM)
        const size_t internal_after =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t spiram_after =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif

        if (!ptr)
        {
            ok = false;
#if defined(ESP_PLATFORM)
            printf("[warn] Genesis VDP async early VRAM reserve FAILED job=%d size=%u "
                   "caps=INTERNAL largest_free internal %u->%u spiram %u->%u\n",
                   i, (unsigned)VRAM_MAX_SIZE,
                   (unsigned)internal_before, (unsigned)internal_after,
                   (unsigned)spiram_before, (unsigned)spiram_after);
            RG_LOGW("Genesis VDP async early VRAM reserve FAILED job=%d size=%u "
                    "caps=INTERNAL largest_free internal %u->%u spiram %u->%u",
                    i, (unsigned)VRAM_MAX_SIZE,
                    (unsigned)internal_before, (unsigned)internal_after,
                    (unsigned)spiram_before, (unsigned)spiram_after);
#else
            printf("[warn] Genesis VDP async early VRAM reserve FAILED job=%d size=%u caps=INTERNAL\n",
                   i, (unsigned)VRAM_MAX_SIZE);
#endif
            continue;
        }

        gwenesis_vdp_async_reserved_vram[i] = ptr;
#if defined(ESP_PLATFORM)
        printf("[info] Genesis VDP async early VRAM reserve job=%d ptr=%p size=%u "
               "caps=INTERNAL addr_guess=%s largest_free internal %u->%u spiram %u->%u\n",
               i, ptr, (unsigned)VRAM_MAX_SIZE,
               PTR_IN_SPIRAM(ptr) ? "SPIRAM" : "not-SPIRAM",
               (unsigned)internal_before, (unsigned)internal_after,
               (unsigned)spiram_before, (unsigned)spiram_after);
        RG_LOGI("Genesis VDP async early VRAM reserve job=%d ptr=%p size=%u "
                "caps=INTERNAL addr_guess=%s largest_free internal %u->%u spiram %u->%u",
                i, ptr, (unsigned)VRAM_MAX_SIZE,
                PTR_IN_SPIRAM(ptr) ? "SPIRAM" : "not-SPIRAM",
                (unsigned)internal_before, (unsigned)internal_after,
                (unsigned)spiram_before, (unsigned)spiram_after);
#else
        printf("[info] Genesis VDP async early VRAM reserve job=%d ptr=%p size=%u caps=INTERNAL\n",
               i, ptr, (unsigned)VRAM_MAX_SIZE);
#endif
    }

    return ok;
}

static unsigned char *gwenesis_vdp_async_take_reserved_vram(int index)
{
    if (index < 0 || index >= GWENESIS_VDP_ASYNC_JOBS)
        return NULL;

    unsigned char *ptr = gwenesis_vdp_async_reserved_vram[index];
    gwenesis_vdp_async_reserved_vram[index] = NULL;
    return ptr;
}
#else
bool gwenesis_vdp_async_reserve_vram_early(void)
{
    return true;
}
#endif

static void gwenesis_vdp_async_discard_frame(uint32_t frame_id)
{
    if (frame_id == 0)
        return;
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!gwenesis_vdp_async_jobs)
        return;
#endif

    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
        if (job->frame_id == frame_id)
            __atomic_store_n(&job->discard, 1, __ATOMIC_RELEASE);
    }
}

void gwenesis_vdp_async_mark_midframe_write(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    return;
#else
    const int visible_height = (int)screen_height;
    if (visible_height <= 0 || scan_line < 0 || scan_line >= visible_height)
        return;

    __atomic_store_n(&gwenesis_vdp_async_unsafe_frames,
                     GWENESIS_VDP_ASYNC_UNSAFE_FRAMES,
                     __ATOMIC_RELEASE);
    gwenesis_vdp_async_discard_frame(
        __atomic_load_n(&gwenesis_vdp_async_current_frame_id, __ATOMIC_ACQUIRE));
#endif
}

static void gwenesis_vdp_async_begin_frame(uint32_t frame_id)
{
    __atomic_store_n(&gwenesis_vdp_async_current_frame_id, frame_id, __ATOMIC_RELEASE);
}

static bool gwenesis_vdp_async_can_submit_frame(void)
{
    if (!gwenesis_vdp_async_task_handle ||
        !__atomic_load_n(&gwenesis_vdp_async_task_running, __ATOMIC_ACQUIRE))
        return false;

#if defined(RG_TARGET_HOLO_DYNMOD)
    if (__atomic_load_n(&gwenesis_vdp_async_paused, __ATOMIC_ACQUIRE))
        return false;
#endif

#if !defined(RG_TARGET_HOLO_DYNMOD)
    const uint32_t unsafe_frames = __atomic_load_n(&gwenesis_vdp_async_unsafe_frames, __ATOMIC_ACQUIRE);
    if (unsafe_frames > 0)
    {
        __atomic_store_n(&gwenesis_vdp_async_unsafe_frames, unsafe_frames - 1, __ATOMIC_RELEASE);
        return false;
    }
#endif

#if !defined(RG_TARGET_HOLO_DYNMOD)
    if (REG0_LINE_INTERRUPT || MODE_SHI)
    {
        return false;
    }
#endif

    return true;
}

static inline __attribute__((always_inline)) void gwenesis_vdp_async_release_held_jobs(void)
{
    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
        if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == GWENESIS_VDP_JOB_HELD)
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
    }
}

static void gwenesis_vdp_async_release_displayed_jobs(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!gwenesis_vdp_async_jobs)
        return;
#endif
    if (!rg_display_sync(false))
        return;

    gwenesis_vdp_async_release_held_jobs();
}

static gwenesis_vdp_async_job_t *gwenesis_vdp_async_acquire_job(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!gwenesis_vdp_async_jobs)
        return NULL;
#endif
    gwenesis_vdp_async_release_displayed_jobs();

    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
        if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == GWENESIS_VDP_JOB_FREE)
        {
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_PREPARING, __ATOMIC_RELEASE);
            return job;
        }
    }
    return NULL;
}

static gwenesis_vdp_async_job_t *gwenesis_vdp_async_take_latest_ready_job(void)
{
    gwenesis_vdp_async_job_t *best = NULL;
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!gwenesis_vdp_async_jobs)
        return NULL;
#endif

    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
        if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) != GWENESIS_VDP_JOB_READY)
            continue;
        if (__atomic_load_n(&job->discard, __ATOMIC_ACQUIRE))
        {
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
            continue;
        }
        if (!best || job->frame_id > best->frame_id)
            best = job;
    }

    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
        if (job != best &&
            __atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == GWENESIS_VDP_JOB_READY)
        {
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
        }
    }

    return best;
}

static void gwenesis_vdp_snapshot_merge_frame_dirty(void)
{
    uint32_t frame_dirty[GWENESIS_VDP_SNAPSHOT_DIRTY_WORDS];
    gwenesis_vdp_snapshot_dirty_take(frame_dirty);

    for (unsigned int word = 0; word < GWENESIS_VDP_SNAPSHOT_DIRTY_WORDS; ++word)
    {
        const uint32_t dirty = frame_dirty[word];
        if (!dirty)
            continue;
        for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
            gwenesis_vdp_async_jobs[i].pending_vram_dirty[word] |= dirty;
    }
}

static void gwenesis_vdp_snapshot_copy_vram(gwenesis_vdp_async_job_t *job)
{
    unsigned int pages = 0;
    for (unsigned int word = 0; word < GWENESIS_VDP_SNAPSHOT_DIRTY_WORDS; ++word)
        pages += (unsigned int)__builtin_popcount(job->pending_vram_dirty[word]);

    if (pages == 0)
        return;

    if (pages >= GWENESIS_VDP_SNAPSHOT_FULL_THRESHOLD_PAGES)
    {
        memcpy(job->vram, VRAM, VRAM_MAX_SIZE);
        memset(job->pending_vram_dirty, 0, sizeof(job->pending_vram_dirty));
        return;
    }

    unsigned int page = 0;
    while (page < GWENESIS_VDP_SNAPSHOT_PAGE_COUNT)
    {
        const uint32_t mask = 1U << (page & 31U);
        if (!(job->pending_vram_dirty[page >> 5] & mask))
        {
            ++page;
            continue;
        }

        const unsigned int first = page;
        do
        {
            job->pending_vram_dirty[page >> 5] &= ~(1U << (page & 31U));
            ++page;
        } while (page < GWENESIS_VDP_SNAPSHOT_PAGE_COUNT &&
                 (job->pending_vram_dirty[page >> 5] & (1U << (page & 31U))));

        const size_t offset = (size_t)first << GWENESIS_VDP_SNAPSHOT_PAGE_SHIFT;
        const size_t length = (size_t)(page - first) << GWENESIS_VDP_SNAPSHOT_PAGE_SHIFT;
        memcpy(job->vram + offset, VRAM + offset, length);
    }

}

static bool gwenesis_vdp_async_fill_job(gwenesis_vdp_async_job_t *job)
{
    if (!job || !job->vram || !job->vsram || !job->cram565 || !job->sat_cache || !job->surface)
        return false;

    gwenesis_vdp_snapshot_merge_frame_dirty();
    gwenesis_vdp_snapshot_copy_vram(job);
    memcpy(job->vsram, VSRAM, sizeof(*VSRAM) * VSRAM_MAX_SIZE);
    memcpy(job->cram565, CRAM565, sizeof(*CRAM565) * CRAM_MAX_SIZE * 4);
    memcpy(job->sat_cache, SAT_CACHE, SAT_CACHE_MAX_SIZE);
    memcpy(job->regs, gwenesis_vdp_regs, REG_SIZE);

    return true;
}

static void gwenesis_vdp_async_wake_worker(void)
{
    /* Worker uses fixed polling with rg_task_delay(2). */
}

#if defined(RG_TARGET_HOLO_DYNMOD)
static void gwenesis_vdp_async_pause_core(void)
{
    __atomic_store_n(&gwenesis_vdp_async_paused, true, __ATOMIC_RELEASE);
    if (!gwenesis_vdp_async_jobs)
        return;
    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
        __atomic_store_n(&gwenesis_vdp_async_jobs[i].discard, 1, __ATOMIC_RELEASE);
    gwenesis_vdp_async_wait_idle_for_sync();
}

static void gwenesis_vdp_async_resume_core(void)
{
    __atomic_store_n(&gwenesis_vdp_async_paused, false, __ATOMIC_RELEASE);
}
#endif

static bool gwenesis_vdp_async_submit_snapshot(uint32_t frame_id, int frame_width, int frame_height)
{
    gwenesis_vdp_async_job_t *job = gwenesis_vdp_async_acquire_job();
    if (!job)
        return false;

    job->frame_id = frame_id;
    job->screen_width = frame_width;
    job->screen_height = frame_height;
    __atomic_store_n(&job->discard, 0, __ATOMIC_RELEASE);

    if (!gwenesis_vdp_async_fill_job(job))
    {
        __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
        return false;
    }

    __atomic_store_n(&job->state, GWENESIS_VDP_JOB_READY, __ATOMIC_RELEASE);
    gwenesis_vdp_async_wake_worker();
    return true;
}

static bool gwenesis_vdp_async_prepare_surface(gwenesis_vdp_async_job_t *job)
{
    if (!job || !job->surface || !job->cram565)
        return false;

    rg_surface_t *surface = job->surface;
    memcpy(surface->palette, job->cram565, sizeof(*job->cram565) * CRAM_MAX_SIZE * 4);
    surface->width = job->screen_width;
    surface->height = job->screen_height;
    surface->offset = (((GWENESIS_SURFACE_HEIGHT - job->screen_height) / 2) * surface->stride) +
                      (((GWENESIS_SURFACE_WIDTH - job->screen_width) / 2) *
                       RG_PIXEL_GET_SIZE(surface->format));
    gwenesis_perf_overlay_draw(surface);
    displayUpdate = surface;
    return true;
}

#if defined(RG_TARGET_HOLO_DYNMOD)
static bool gwenesis_vdp_async_display_job_nonblocking(gwenesis_vdp_async_job_t *job)
{
    if (!job || __atomic_load_n(&job->discard, __ATOMIC_ACQUIRE))
        return false;

    if (!rg_display_sync(false))
        return false;
    gwenesis_vdp_async_release_held_jobs();

    if (__atomic_load_n(&job->discard, __ATOMIC_ACQUIRE))
        return false;
    if (!gwenesis_vdp_async_prepare_surface(job))
        return false;

    if (__atomic_load_n(&job->discard, __ATOMIC_ACQUIRE))
        return false;

    rg_display_submit(displayUpdate, 0);
    return true;
}
#endif

static void gwenesis_vdp_async_worker_task(void *arg)
{
    (void)arg;

    while (__atomic_load_n(&gwenesis_vdp_async_task_running, __ATOMIC_ACQUIRE))
    {
        gwenesis_vdp_async_job_t *job = gwenesis_vdp_async_take_latest_ready_job();
        if (!job)
        {
            rg_task_delay(2);
            continue;
        }

        if (__atomic_load_n(&job->discard, __ATOMIC_ACQUIRE))
        {
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
            continue;
        }

        gwenesis_vdp_render_context_t ctx = {
            .vram = job->vram,
            .vsram = job->vsram,
            .cram565 = job->cram565,
            .sat_cache = job->sat_cache,
            .regs = job->regs,
            .screen_width = job->screen_width,
            .screen_height = job->screen_height,
        };

        __atomic_store_n(&job->state, GWENESIS_VDP_JOB_RENDERING, __ATOMIC_RELEASE);
        gwenesis_vdp_gfx_set_render_context(&ctx);
        gwenesis_vdp_set_buffer((unsigned short *)job->surface->data);

        int64_t render_us = 0;
#if GWENESIS_PROFILER_DETAILED
        GWENESIS_PROFILER_TIME_START(slice_start_us);
#endif
        gwenesis_vdp_render_config();
#if GWENESIS_PROFILER_DETAILED
        if (gwenesis_profiler_active() && slice_start_us > 0)
            render_us += rg_system_timer() - slice_start_us;
#endif
        for (int line = 0; line < job->screen_height; ++line)
        {
            if (__atomic_load_n(&job->discard, __ATOMIC_ACQUIRE))
                break;
#if GWENESIS_PROFILER_DETAILED
            GWENESIS_PROFILER_TIME_RESTART(slice_start_us);
#endif
            gwenesis_vdp_render_line(line);
#if GWENESIS_PROFILER_DETAILED
            if (gwenesis_profiler_active() && slice_start_us > 0)
                render_us += rg_system_timer() - slice_start_us;
#endif
        }
        gwenesis_vdp_gfx_set_render_context(NULL);

#if GWENESIS_PROFILER_DETAILED
        if (gwenesis_profiler_active())
        {
            gwenesis_vdp_async_render_us += (uint32_t)render_us;
            gwenesis_vdp_async_render_count++;
        }
#endif
        if (__atomic_load_n(&job->discard, __ATOMIC_ACQUIRE))
        {
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
        }
        else
        {
#if defined(RG_TARGET_HOLO_DYNMOD)
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_DISPLAYING, __ATOMIC_RELEASE);
            if (gwenesis_vdp_async_display_job_nonblocking(job))
            {
                __atomic_store_n(&job->state, GWENESIS_VDP_JOB_HELD, __ATOMIC_RELEASE);
            }
            else
            {
                __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
            }
#else
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_DONE, __ATOMIC_RELEASE);
#endif
        }
#if GWENESIS_VDP_ASYNC_RENDER_FRAME_DELAY_MS > 0
        rg_task_delay(GWENESIS_VDP_ASYNC_RENDER_FRAME_DELAY_MS);
#endif
    }

    gwenesis_vdp_gfx_set_render_context(NULL);
    gwenesis_vdp_async_task_handle = NULL;
}

static void gwenesis_vdp_async_wait_idle_for_sync(void)
{
    if (!gwenesis_vdp_async_task_handle)
        return;
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!gwenesis_vdp_async_jobs)
        return;
#endif

    bool need_display_sync = false;
    int waited = 0;
    while (true)
    {
        bool busy = false;
        for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
        {
            gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
            const uint32_t state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
            if (state == GWENESIS_VDP_JOB_FILLING ||
                state == GWENESIS_VDP_JOB_RENDERING ||
                state == GWENESIS_VDP_JOB_DISPLAYING ||
                state == GWENESIS_VDP_JOB_READY)
            {
                __atomic_store_n(&job->discard, 1, __ATOMIC_RELEASE);
                busy = true;
            }
            else if (state == GWENESIS_VDP_JOB_HELD)
            {
                need_display_sync = true;
            }
            else if (state == GWENESIS_VDP_JOB_DONE ||
                     state == GWENESIS_VDP_JOB_PREPARING)
            {
                __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
            }
        }

        if (!busy)
            break;
        if (waited++ == 500)
            RG_LOGW("Waiting for Genesis VDP async worker to go idle\n");
        rg_task_delay(1);
    }

    if (need_display_sync)
    {
        rg_display_sync(true);
        for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
        {
            gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
            if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == GWENESIS_VDP_JOB_HELD)
                __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
        }
    }
}

#if defined(GWENESIS_VDP_ASYNC_SERIAL_TEST) && GWENESIS_VDP_ASYNC_SERIAL_TEST
static bool gwenesis_vdp_async_wait_frame_done(uint32_t frame_id)
{
    if (!frame_id || !gwenesis_vdp_async_task_handle)
        return false;
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!gwenesis_vdp_async_jobs)
        return false;
#endif

    int waited = 0;
    while (__atomic_load_n(&gwenesis_vdp_async_task_running, __ATOMIC_ACQUIRE))
    {
        bool found = false;
        for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
        {
            gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
            if (job->frame_id != frame_id)
                continue;

            found = true;
            const uint32_t state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
            if (state == GWENESIS_VDP_JOB_DONE || state == GWENESIS_VDP_JOB_HELD)
                return true;
            if (state == GWENESIS_VDP_JOB_FREE)
                return false;
        }

        if (!found)
            return false;
        if (waited++ == 500)
            RG_LOGW("Waiting for Genesis VDP async serial frame %u\n",
                    (unsigned)frame_id);
        rg_task_delay(1);
    }
    return false;
}
#endif

static gwenesis_vdp_async_job_t *gwenesis_vdp_async_take_latest_done_job(void)
{
    gwenesis_vdp_async_job_t *best = NULL;
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!gwenesis_vdp_async_jobs)
        return NULL;
#endif

    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
        if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) != GWENESIS_VDP_JOB_DONE)
            continue;
        if (__atomic_load_n(&job->discard, __ATOMIC_ACQUIRE))
        {
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
            continue;
        }
        if (!best || job->frame_id > best->frame_id)
            best = job;
    }

    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
        const uint32_t state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
        if (job != best && state == GWENESIS_VDP_JOB_DONE)
            __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
    }

    return best;
}

#if !defined(RG_TARGET_HOLO_DYNMOD)
static bool gwenesis_vdp_async_display_latest(void)
{
    gwenesis_vdp_async_release_displayed_jobs();
    if (!rg_display_sync(false))
        return false;

    gwenesis_vdp_async_job_t *job = gwenesis_vdp_async_take_latest_done_job();
    if (!job)
        return false;

    if (!gwenesis_vdp_async_prepare_surface(job))
        return false;

    rg_display_submit(displayUpdate, 0);
    __atomic_store_n(&job->state, GWENESIS_VDP_JOB_HELD, __ATOMIC_RELEASE);
    return true;
}
#endif

#if defined(RG_TARGET_HOLO_DYNMOD)
static bool gwenesis_vdp_async_display_latest_serial(void)
{
    gwenesis_vdp_async_release_displayed_jobs();
    if (!rg_display_sync(false))
        return false;

    gwenesis_vdp_async_job_t *job = gwenesis_vdp_async_take_latest_done_job();
    if (!job)
        return false;
    if (!gwenesis_vdp_async_prepare_surface(job))
    {
        __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
        return false;
    }

    rg_display_submit(displayUpdate, 0);
    __atomic_store_n(&job->state, GWENESIS_VDP_JOB_HELD, __ATOMIC_RELEASE);
    return true;
}
#endif

static bool gwenesis_vdp_async_start(void)
{
    if (gwenesis_vdp_async_task_handle)
        return true;

    if (GWENESIS_VDP_ASYNC_JOBS > GWENESIS_SURFACE_COUNT)
        return false;

    bool vram_fallback_used = false;

#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!gwenesis_vdp_async_jobs)
    {
        gwenesis_vdp_async_jobs =
            rg_alloc(sizeof(*gwenesis_vdp_async_jobs) * GWENESIS_VDP_ASYNC_JOBS,
                     MEM_FAST | MEM_NOPANIC);
        if (gwenesis_vdp_async_jobs && PTR_IN_SPIRAM(gwenesis_vdp_async_jobs))
        {
            free(gwenesis_vdp_async_jobs);
            gwenesis_vdp_async_jobs = NULL;
        }
        if (!gwenesis_vdp_async_jobs)
        {
            RG_LOGW("Genesis VDP async disabled: job state allocation failed\n");
            return false;
        }
        RG_LOGI("Genesis VDP async job state: ptr=%p size=%u requested=MEM_FAST addr_guess=%s",
                gwenesis_vdp_async_jobs,
                (unsigned)(sizeof(*gwenesis_vdp_async_jobs) * GWENESIS_VDP_ASYNC_JOBS),
                PTR_IN_SPIRAM(gwenesis_vdp_async_jobs) ? "SPIRAM" : "not-SPIRAM");
    }
#endif

    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
        memset(job, 0, sizeof(*job));
        memset(job->pending_vram_dirty, 0xFF, sizeof(job->pending_vram_dirty));
        job->surface = updates[i];
        if (!job->surface)
        {
            RG_LOGW("Genesis VDP async disabled: missing surface %d\n", i);
            gwenesis_vdp_async_stop();
            return false;
        }

#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_VDP_ASYNC_VRAM_MEM == MEM_FAST
        job->vram = gwenesis_vdp_async_take_reserved_vram(i);
        if (!job->vram)
        {
            job->vram = rg_alloc(VRAM_MAX_SIZE,
                                 GWENESIS_VDP_ASYNC_VRAM_FALLBACK_MEM | MEM_NOPANIC);
            vram_fallback_used = job->vram != NULL;
            RG_LOGW("Genesis VDP async snapshot job=%d falling back to %s ptr=%p addr_guess=%s",
                    i,
                    GWENESIS_VDP_ASYNC_VRAM_FALLBACK_MEM_NAME,
                    job->vram,
                    PTR_IN_SPIRAM(job->vram) ? "SPIRAM" : "not-SPIRAM");
        }
        else
        {
            RG_LOGI("Genesis VDP async snapshot job=%d using early VRAM ptr=%p addr_guess=%s",
                    i, job->vram, PTR_IN_SPIRAM(job->vram) ? "SPIRAM" : "not-SPIRAM");
        }
#else
        job->vram = rg_alloc(VRAM_MAX_SIZE, GWENESIS_VDP_ASYNC_VRAM_MEM | MEM_NOPANIC);
#endif
        job->vsram = rg_alloc(sizeof(*job->vsram) * VSRAM_MAX_SIZE, MEM_FAST | MEM_NOPANIC);
        job->cram565 = rg_alloc(sizeof(*job->cram565) * CRAM_MAX_SIZE * 4, MEM_FAST | MEM_NOPANIC);
        job->sat_cache = rg_alloc(SAT_CACHE_MAX_SIZE, MEM_FAST | MEM_NOPANIC);
        job->regs = rg_alloc(REG_SIZE, MEM_FAST | MEM_NOPANIC);
        if (!job->vram || !job->vsram || !job->cram565 || !job->sat_cache || !job->regs)
        {
            RG_LOGW("Genesis VDP async disabled: snapshot allocation failed\n");
            gwenesis_vdp_async_stop();
            return false;
        }
        __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
    }

    __atomic_store_n(&gwenesis_vdp_async_current_frame_id, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gwenesis_vdp_async_unsafe_frames, 0, __ATOMIC_RELEASE);
#if defined(RG_TARGET_HOLO_DYNMOD)
    __atomic_store_n(&gwenesis_vdp_async_paused, false, __ATOMIC_RELEASE);
#endif
    __atomic_store_n(&gwenesis_vdp_async_task_running, true, __ATOMIC_RELEASE);
    gwenesis_vdp_async_task_handle = rg_task_create("gwen_vdp",
                                                    gwenesis_vdp_async_worker_task,
                                                    NULL,
                                                    GWENESIS_VDP_ASYNC_TASK_STACK,
                                                    GWENESIS_VDP_ASYNC_TASK_PRIORITY,
                                                    GWENESIS_VDP_ASYNC_TASK_CORE);
    if (!gwenesis_vdp_async_task_handle)
    {
        __atomic_store_n(&gwenesis_vdp_async_task_running, false, __ATOMIC_RELEASE);
        gwenesis_vdp_async_stop();
        RG_LOGW("Genesis VDP async disabled: task creation failed\n");
        return false;
    }

    RG_LOGI("Genesis VDP async renderer started on core %d, prio=%d, jobs=%d serial=%d activeptr=%d vram=%s fallback=%d snapshots=%uKB\n",
            GWENESIS_VDP_ASYNC_TASK_CORE, GWENESIS_VDP_ASYNC_TASK_PRIORITY, GWENESIS_VDP_ASYNC_JOBS,
#if defined(GWENESIS_VDP_ASYNC_SERIAL_TEST) && GWENESIS_VDP_ASYNC_SERIAL_TEST
            1,
#else
            0,
#endif
#if GWENESIS_VDP_GFX_ACTIVE_PTR_TEST
            1,
#else
            0,
#endif
            GWENESIS_VDP_ASYNC_VRAM_MEM_NAME,
            vram_fallback_used ? 1 : 0,
            (unsigned)((VRAM_MAX_SIZE + SAT_CACHE_MAX_SIZE +
                        REG_SIZE +
                        sizeof(unsigned short) * (VSRAM_MAX_SIZE + CRAM_MAX_SIZE * 4)) *
                       GWENESIS_VDP_ASYNC_JOBS / 1024));
    return true;
}

static void gwenesis_vdp_async_stop(void)
{
    if (gwenesis_vdp_async_task_handle)
    {
#if defined(RG_TARGET_HOLO_DYNMOD)
        if (gwenesis_vdp_async_jobs)
#endif
        for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
            __atomic_store_n(&gwenesis_vdp_async_jobs[i].discard, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&gwenesis_vdp_async_task_running, false, __ATOMIC_RELEASE);
        gwenesis_vdp_async_wake_worker();

        int waited = 0;
        while (rg_task_find("gwen_vdp"))
        {
            if (waited++ == 500)
                RG_LOGW("Waiting for Genesis VDP async task to stop\n");
            rg_task_delay(1);
        }
    }

    gwenesis_vdp_async_task_handle = NULL;
    __atomic_store_n(&gwenesis_vdp_async_task_running, false, __ATOMIC_RELEASE);
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (gwenesis_vdp_async_jobs)
    {
#endif
    for (int i = 0; i < GWENESIS_VDP_ASYNC_JOBS; ++i)
    {
        gwenesis_vdp_async_job_t *job = &gwenesis_vdp_async_jobs[i];
        free(job->vram);
        free(job->vsram);
        free(job->cram565);
        free(job->sat_cache);
        free(job->regs);
        job->vram = NULL;
        job->vsram = NULL;
        job->cram565 = NULL;
        job->sat_cache = NULL;
        job->regs = NULL;
        job->surface = NULL;
        __atomic_store_n(&job->state, GWENESIS_VDP_JOB_FREE, __ATOMIC_RELEASE);
    }
#if defined(RG_TARGET_HOLO_DYNMOD)
        free(gwenesis_vdp_async_jobs);
        gwenesis_vdp_async_jobs = NULL;
    }
#endif
#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_VDP_ASYNC_VRAM_MEM == MEM_FAST
    gwenesis_vdp_async_release_reserved_vram();
#endif
}
#else
void gwenesis_vdp_async_mark_midframe_write(void)
{
}

bool gwenesis_vdp_async_reserve_vram_early(void)
{
    return true;
}
#endif

bool gwenesis_alloc_vram_fast(void)
{
    if (VRAM)
        return true;

#if defined(ESP_PLATFORM)
    size_t vram_internal_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t vram_spiram_before = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    const char *vram_mem_name = GWENESIS_VRAM_MEM_NAME;
    VRAM = rg_alloc(VRAM_MAX_SIZE, GWENESIS_VRAM_MEM | MEM_NOPANIC);
    if (!VRAM && GWENESIS_VRAM_FALLBACK_MEM != GWENESIS_VRAM_MEM)
    {
        vram_mem_name = GWENESIS_VRAM_FALLBACK_MEM_NAME;
        VRAM = rg_alloc(VRAM_MAX_SIZE, GWENESIS_VRAM_FALLBACK_MEM | MEM_NOPANIC);
    }
    if (!VRAM)
    {
        RG_LOGE("Genesis VRAM allocation failed!");
        return false;
    }
#if defined(ESP_PLATFORM)
    size_t vram_internal_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t vram_spiram_after = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    printf("[warn] %s: Genesis VRAM: ptr=%p size=%u requested=%s addr_guess=%s "
           "largest_free internal %u->%u spiram %u->%u\n",
           __func__, VRAM, (unsigned)VRAM_MAX_SIZE, vram_mem_name,
           PTR_IN_SPIRAM(VRAM) ? "SPIRAM" : "not-SPIRAM",
           (unsigned)vram_internal_before, (unsigned)vram_internal_after,
           (unsigned)vram_spiram_before, (unsigned)vram_spiram_after);
    RG_LOGW("Genesis VRAM: ptr=%p size=%u requested=%s addr_guess=%s "
            "largest_free internal %u->%u spiram %u->%u\n",
            VRAM, (unsigned)VRAM_MAX_SIZE, vram_mem_name,
            PTR_IN_SPIRAM(VRAM) ? "SPIRAM" : "not-SPIRAM",
            (unsigned)vram_internal_before, (unsigned)vram_internal_after,
            (unsigned)vram_spiram_before, (unsigned)vram_spiram_after);
#endif
    return true;
}

#if defined(RG_TARGET_HOLO_DYNMOD)
void gwenesis_release_early_resources_for_holo(void)
{
#if GWENESIS_VDP_ASYNC_ENABLED
    gwenesis_vdp_async_stop();
#endif
    free(VRAM);
    VRAM = NULL;
}
#endif

static void gwenesis_cleanup(void)
{
    if (gwenesis_cleaned_up)
        return;
    gwenesis_cleaned_up = true;

    rg_system_set_monitor_extra(NULL);
#if GWENESIS_VDP_ASYNC_ENABLED
    gwenesis_vdp_async_stop();
#endif
    gwenesis_audio_stop();

    gwenesis_close_savestate();

    for (size_t i = 0; i < RG_COUNT(updates); ++i)
    {
        if (updates[i])
        {
            if (update_data_base[i])
                updates[i]->data = update_data_base[i];
            if (update_height_base[i])
                updates[i]->height = update_height_base[i];
            rg_surface_free(updates[i]);
            updates[i] = NULL;
            update_data_base[i] = NULL;
            update_height_base[i] = 0;
        }
    }
    currentUpdate = NULL;
    displayUpdate = NULL;

    free(VRAM);
    VRAM = NULL;

#if GWENESIS_SN76489_RUN_ENABLED
    free(gwenesis_sn76489_buffer);
    gwenesis_sn76489_buffer = NULL;
#endif
    free(gwenesis_ym2612_buffer);
    gwenesis_ym2612_buffer = NULL;

    gwenesis_vdp_gfx_deinit_fast_ram();
    gwenesis_vdp_mem_deinit_fast_ram();
#if GWENESIS_SN76489_RUN_ENABLED
    gwenesis_sn76489_deinit_fast_ram();
#endif
    gwenesis_ym2612_deinit_fast_ram();
    z80_deinit_fast_ram();
#if defined(RG_TARGET_HOLO_DYNMOD)
    gwenesis_m68k_core_deinit_fast_ram();
#endif

#if GNW_TARGET_MARIO == 0 && GNW_TARGET_ZELDA == 0
    unload_cartridge();
#endif
    gwenesis_bus_deinit_fast_ram();
}

static void gwenesis_startup_error(const char *log_message, void **rom_data)
{
    if (rom_data && *rom_data)
    {
        free(*rom_data);
        *rom_data = NULL;
    }

#if defined(RG_TARGET_HOLO_DYNMOD)
    if (log_message)
        RG_LOGE("%s", log_message);
    gwenesis_cleanup();
    rg_display_clear(C_BLACK);
    rg_gui_draw_message("%s",
                        "Not enough internal RAM!\n"
                        "Please restart the device\n"
                        "and run this emulator again.");
    rg_display_sync(true);
    rg_task_delay(5000);
    holo_runtime_request_switch("desktop", "", 0);
#else
    RG_PANIC(log_message ? log_message : "Genesis startup failed!");
#endif
}

typedef struct
{
    char key[28];
    uint32_t length;
} svar_t;

SaveState *saveGwenesisStateOpenForRead(const char *fileName)
{
    return (void *)1;
}

SaveState *saveGwenesisStateOpenForWrite(const char *fileName)
{
    return (void *)1;
}

int saveGwenesisStateGet(SaveState *state, const char *tagName)
{
    int value = 0;
    saveGwenesisStateGetBuffer(state, tagName, &value, sizeof(int));
    return value;
}

void saveGwenesisStateSet(SaveState *state, const char *tagName, int value)
{
    saveGwenesisStateSetBuffer(state, tagName, &value, sizeof(int));
}

void saveGwenesisStateGetBuffer(SaveState *state, const char *tagName, void *buffer, int length)
{
    size_t initial_pos = ftell(savestate_fp);
    bool from_start = false;
    svar_t var;

    // Odds are that calls to this func will be in order, so try searching from current file position.
    while (!from_start || ftell(savestate_fp) < initial_pos)
    {
        if (!fread(&var, sizeof(svar_t), 1, savestate_fp))
        {
            if (!from_start)
            {
                fseek(savestate_fp, 0, SEEK_SET);
                from_start = true;
                continue;
            }
            break;
        }
        if (strncmp(var.key, tagName, sizeof(var.key)) == 0)
        {
            fread(buffer, RG_MIN(var.length, length), 1, savestate_fp);
            RG_LOGI("Loaded key '%s'\n", tagName);
            return;
        }
        fseek(savestate_fp, var.length, SEEK_CUR);
    }
    RG_LOGW("Key %s NOT FOUND!\n", tagName);
    savestate_errors++;
}

void saveGwenesisStateSetBuffer(SaveState *state, const char *tagName, void *buffer, int length)
{
    // TO DO: seek the file to find if the key already exists. It's possible it could be written twice.
    svar_t var = {{0}, length};
    strncpy(var.key, tagName, sizeof(var.key) - 1);
    fwrite(&var, sizeof(var), 1, savestate_fp);
    fwrite(buffer, length, 1, savestate_fp);
    RG_LOGI("Saved key '%s'\n", tagName);
}

void gwenesis_io_get_buttons()
{
}

static rg_gui_event_t audio_mode_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
#if !GWENESIS_AUDIO_EMULATION
    gwenesis_audio_mode = GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE;
    strcpy(option->value, "Mute");
    return RG_DIALOG_VOID;
#endif
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (event == RG_DIALOG_NEXT)
        {
            if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_FAST)
                gwenesis_audio_mode = GWENESIS_AUDIO_MODE_BALANCE;
            else if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_BALANCE)
                gwenesis_audio_mode = GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE;
            else
                gwenesis_audio_mode = GWENESIS_AUDIO_MODE_FAST;
        }
        else
        {
            if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_FAST)
                gwenesis_audio_mode = GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE;
            else if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE)
                gwenesis_audio_mode = GWENESIS_AUDIO_MODE_BALANCE;
            else
                gwenesis_audio_mode = GWENESIS_AUDIO_MODE_FAST;
        }
        rg_settings_set_number(NS_APP, SETTING_AUDIO_MODE, gwenesis_audio_mode);
        ym2612_set_lite_mode(gwenesis_audio_mode == GWENESIS_AUDIO_MODE_FAST);
    }
    strcpy(option->value, gwenesis_audio_mode_name(gwenesis_audio_mode));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t z80_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        gwenesis_z80_enabled = !gwenesis_z80_enabled;
        rg_settings_set_number(NS_APP, SETTING_Z80_ENABLE, gwenesis_z80_enabled ? 1 : 0);
        z80_set_enabled(gwenesis_z80_enabled);
    }
    strcpy(option->value, gwenesis_z80_enabled ? "On" : "Off");

    return RG_DIALOG_VOID;
}

static rg_gui_event_t perf_overlay_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        gwenesis_perf_overlay_enabled = !gwenesis_perf_overlay_enabled;
        rg_settings_set_number(NS_APP, SETTING_PERF_OVERLAY, gwenesis_perf_overlay_enabled ? 1 : 0);
    }
    strcpy(option->value, gwenesis_perf_overlay_enabled ? "On" : "Off");

    return RG_DIALOG_VOID;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    rg_surface_t *surface = displayUpdate ? displayUpdate : currentUpdate;
    return rg_surface_save_image_file(surface, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    if ((savestate_fp = fopen(filename, "wb")))
    {
        savestate_errors = 0;
        gwenesis_save_state();
        gwenesis_close_savestate();
        return savestate_errors == 0;
    }
    return false;
}

static bool load_state_handler(const char *filename)
{
    if ((savestate_fp = fopen(filename, "rb")))
    {
        savestate_errors = 0;
        gwenesis_load_state();
        gwenesis_close_savestate();
        if (savestate_errors == 0)
            return true;
    }
    reset_emulation();
    return false;
}

static bool reset_handler(bool hard)
{
    reset_emulation();
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_surface_t *surface = displayUpdate ? displayUpdate : currentUpdate;
        rg_display_submit(surface, 0);
    }
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, "Genesis audio", "-", RG_DIALOG_FLAG_NORMAL, &audio_mode_update_cb};
    *dest++ = (rg_gui_option_t){0, "Genesis Z80", "-", RG_DIALOG_FLAG_NORMAL, &z80_update_cb};
    *dest++ = (rg_gui_option_t){0, "Genesis perf", "-", RG_DIALOG_FLAG_NORMAL, &perf_overlay_update_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

void app_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .options = &options_handler,
    };

    gwenesis_cleaned_up = false;

#if defined(RG_TARGET_HOLO_DYNMOD)
#if GWENESIS_VDP_ASYNC_ENABLED && GWENESIS_VDP_ASYNC_VRAM_MEM == MEM_FAST
    gwenesis_vdp_async_reserve_vram_early();
#endif
    if (!holo_display_acquire(RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT))
        printf("[warn] gwenesis app_main: early display acquire failed\n");
    if (!VRAM && !gwenesis_alloc_vram_fast())
        printf("[warn] gwenesis app_main: early VRAM allocation failed\n");
    app = rg_system_reinit(AUDIO_OUTPUT_SAMPLE_RATE, &handlers, NULL);
#else
    app = rg_system_init(AUDIO_OUTPUT_SAMPLE_RATE, &handlers, NULL);
#endif

#if GWENESIS_AUDIO_EMULATION
    gwenesis_audio_mode = rg_settings_get_number(NS_APP, SETTING_AUDIO_MODE, GWENESIS_AUDIO_MODE_BALANCE);
    if (gwenesis_audio_mode < GWENESIS_AUDIO_MODE_FAST ||
        gwenesis_audio_mode > GWENESIS_AUDIO_MODE_BALANCE)
    {
        gwenesis_audio_mode = GWENESIS_AUDIO_MODE_BALANCE;
    }
#else
    gwenesis_audio_mode = GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE;
#endif
    gwenesis_z80_enabled = rg_settings_get_number(NS_APP, SETTING_Z80_ENABLE, 1) != 0;
    z80_set_enabled(gwenesis_z80_enabled);
    ym2612_set_lite_mode(gwenesis_audio_mode == GWENESIS_AUDIO_MODE_FAST);
    gwenesis_perf_overlay_enabled = rg_settings_get_number(NS_APP, SETTING_PERF_OVERLAY, 0) != 0;

#if defined(RG_TARGET_HOLO_DYNMOD)
    gwenesis_force_native_display();
#endif

    RG_LOGI("Genesis start\n");
#if !defined(RG_TARGET_HOLO_DYNMOD)
    gwenesis_alloc_vram_fast();
#else
    if (!VRAM && !gwenesis_alloc_vram_fast())
    {
        gwenesis_startup_error("Genesis VRAM allocation failed!", NULL);
        return;
    }
#endif

    size_t rom_size = 0;
    void *rom_data = NULL;

    if (rg_extension_match(app->romPath, "zip"))
    {
        if (!rg_storage_unzip_file(app->romPath, NULL, &rom_data, &rom_size, RG_FILE_ALIGN_64KB))
        {
            gwenesis_startup_error("ROM file unzipping failed!", &rom_data);
            return;
        }
    }
    else if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size, RG_FILE_ALIGN_64KB))
    {
        gwenesis_startup_error("ROM load failed!", &rom_data);
        return;
    }

#if defined(RG_TARGET_HOLO_DYNMOD)
    for (size_t i = 0; i < RG_COUNT(updates); ++i)
    {
        updates[i] = rg_surface_create(GWENESIS_SURFACE_WIDTH, GWENESIS_SURFACE_HEIGHT,
                                       GWENESIS_SURFACE_FORMAT, GWENESIS_SURFACE_MEM | MEM_NOPANIC);
    }
#else
    updates[0] = rg_surface_create(GWENESIS_SURFACE_WIDTH, GWENESIS_SURFACE_HEIGHT,
                                   GWENESIS_SURFACE_FORMAT, GWENESIS_SURFACE_MEM | MEM_NOPANIC);
#endif
    currentUpdate = updates[0];
    if (!currentUpdate)
    {
        gwenesis_startup_error("Genesis video surface allocation failed!", &rom_data);
        return;
    }

#if defined(RG_TARGET_HOLO_DYNMOD)
    gwenesis_clear_surface();
#else
    // This is a hack because our new surface format doesn't yet support overdraw space easily
    update_data_base[0] = updates[0]->data;
    update_height_base[0] = updates[0]->height;
    updates[0]->data += 160;
    updates[0]->height = 240;
#endif
    // updates[1]->data += 160;
    // updates[1]->height = 240;

    if (!gwenesis_bus_init_fast_ram())
    {
        gwenesis_startup_error("Genesis fast RAM allocation failed!", &rom_data);
        return;
    }
#if GWENESIS_AUDIO_EMULATION
    if (!gwenesis_ym2612_init_fast_ram())
    {
        gwenesis_startup_error("Genesis sound state fast memory allocation failed!", &rom_data);
        return;
    }
#if GWENESIS_SN76489_RUN_ENABLED
    gwenesis_sn76489_buffer = rg_alloc(sizeof(*gwenesis_sn76489_buffer) * AUDIO_BUFFER_LENGTH, MEM_FAST | MEM_NOPANIC);
#endif
    gwenesis_ym2612_buffer = rg_alloc(sizeof(*gwenesis_ym2612_buffer) * AUDIO_BUFFER_LENGTH, MEM_FAST | MEM_NOPANIC);
    if (!gwenesis_ym2612_buffer
#if GWENESIS_SN76489_RUN_ENABLED
        || !gwenesis_sn76489_buffer
#endif
    )
    {
        gwenesis_startup_error("Genesis audio buffer allocation failed!", &rom_data);
        return;
    }
#endif

    if (!gwenesis_vdp_mem_init_fast_ram())
    {
        gwenesis_startup_error("Genesis VDP fast memory allocation failed!", &rom_data);
        return;
    }
    if (!gwenesis_vdp_gfx_init_fast_ram())
    {
        gwenesis_startup_error("Genesis VDP gfx fast memory allocation failed!", &rom_data);
        return;
    }
#if GWENESIS_AUDIO_EMULATION
    if (!gwenesis_audio_start())
    {
        gwenesis_startup_error("Genesis audio queue allocation failed!", &rom_data);
        return;
    }
#endif

    RG_LOGI("load_cartridge(%p, %d)\n", rom_data, rom_size);
    load_cartridge(rom_data, rom_size);
    rom_data = NULL; // load_cartridge takes ownership

    RG_LOGI("power_on()\n");
#if defined(RG_TARGET_HOLO_DYNMOD)
    if (!gwenesis_m68k_core_init_fast_ram())
    {
        gwenesis_startup_error("Genesis M68K core allocation failed!", NULL);
        return;
    }
#endif
    power_on();

    RG_LOGI("reset_emulation()\n");
    reset_emulation();

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        rg_emu_load_state(app->saveSlot);
    }

#if defined(RG_TARGET_HOLO_DYNMOD)
    bool audio_pal_mode = REG1_PAL != 0;
    ym2612_set_divisor(gwenesis_audio_divisor(audio_pal_mode));
#endif

#if GWENESIS_VDP_ASYNC_ENABLED
    gwenesis_vdp_async_start();
#endif

    rg_system_set_tick_rate(GWENESIS_FRAME_TARGET_FPS);
    app->frameskip = 0;
    app->maxFrameskip = 0;
    gwenesis_frameskip_reset(rg_system_timer());
    RG_LOGI("mod frameskip: debt target=%dfps render=%dfps off=%dus/%dus on=%dus/%dus maxskip=%d dbuf=%d\n",
            GWENESIS_FRAME_TARGET_FPS,
#if defined(RG_TARGET_HOLO_DYNMOD)
            GWENESIS_RENDER_TARGET_FPS,
#else
            GWENESIS_FRAME_TARGET_FPS,
#endif
            frameskip_audio_off_debt_us,
            frameskip_audio_off_force_debt_us,
            frameskip_audio_on_debt_us,
            frameskip_audio_on_force_debt_us,
            frameskip_max_consecutive_skips,
            updates[1] != NULL);
    RG_LOGI("mod audio: synth=%dHz output=%dHz buffer=%d queue=%d prebuffer=%d\n",
            AUDIO_SYNTH_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_BUFFER_LENGTH,
            GWENESIS_AUDIO_QUEUE_PACKETS,
            GWENESIS_AUDIO_PREBUFFER_PACKETS);
    RG_LOGI("mod audio sync: Fast ym=%d z80=%d Balance ym=%d z80=%d Mute z80=%d\n",
            GWENESIS_YM_BATCH_LINES_FAST,
            GWENESIS_Z80_BATCH_LINES_FAST,
            GWENESIS_YM_BATCH_LINES_BALANCE,
            GWENESIS_Z80_BATCH_LINES_BALANCE,
            GWENESIS_Z80_BATCH_LINES_MUTED);

    extern unsigned char *gwenesis_vdp_regs;
    extern unsigned short gwenesis_vdp_status;
    extern unsigned short *CRAM565;
    extern int screen_width, screen_height;
    extern int hint_pending;

    // Gwenesis button bits are U,D,L,R,C,B,A,Start. Keep MD B/C aligned
    // with package/main.lua: retrogo BTN_X => MD B, retrogo BTN_B => MD C.
    uint32_t keymap[8] = {RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT, RG_KEY_B, RG_KEY_X, RG_KEY_A, RG_KEY_START};
    uint32_t joystick = 0, joystick_old;
#if defined(RG_TARGET_HOLO_DYNMOD)
    int last_screen_width = 0;
    int last_screen_height = 0;
#endif

    RG_LOGI("emulation loop\n");
    while (true)
    {
#if defined(RG_TARGET_HOLO_DYNMOD)
        if (holo_runtime_switch_requested() || holo_runtime_stop_requested())
        {
#if GWENESIS_VDP_ASYNC_ENABLED
            gwenesis_vdp_async_pause_core();
#endif
            gwenesis_cleanup();
            return;
        }
#endif
        joystick_old = joystick;
        joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_VDP_ASYNC_ENABLED
            gwenesis_vdp_async_pause_core();
#elif GWENESIS_VDP_ASYNC_ENABLED
            gwenesis_vdp_async_wait_idle_for_sync();
#endif
            rg_display_sync(true);
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
#if defined(RG_TARGET_HOLO_DYNMOD)
            gwenesis_force_native_display();
            audio_pal_mode = REG1_PAL != 0;
            ym2612_set_divisor(gwenesis_audio_divisor(audio_pal_mode));
            if (holo_runtime_switch_requested() || holo_runtime_stop_requested())
            {
                gwenesis_cleanup();
                return;
            }
#endif
#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_VDP_ASYNC_ENABLED
            gwenesis_vdp_async_resume_core();
#endif
        }
        else if (joystick != joystick_old)
        {
            for (int i = 0; i < 8; i++)
            {
                if ((joystick & keymap[i]) == keymap[i])
                    gwenesis_io_pad_press_button(0, i);
                else
                    gwenesis_io_pad_release_button(0, i);
            }
        }

        int64_t startTime = rg_system_timer();
        const bool audio_muted = rg_audio_get_mute();
        const bool yfm_run_enabled = (gwenesis_audio_mode != GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE) && !audio_muted;
#if GWENESIS_SN76489_RUN_ENABLED
        const bool sn76489_run_enabled = yfm_run_enabled && gwenesis_sn76489_buffer != NULL;
#endif
        const bool z80_run_enabled = gwenesis_z80_enabled;
        bool scheduled_draw = gwenesis_frameskip_should_draw(yfm_run_enabled, startTime);

        bool display_ready = true;
#if defined(RG_TARGET_HOLO_DYNMOD)
        if (scheduled_draw && !gwenesis_display_double_buffered())
#else
        if (scheduled_draw)
#endif
        {
            display_ready = rg_display_sync(false);
        }
        bool drawFrame = scheduled_draw && display_ready;
#if GWENESIS_VDP_ASYNC_ENABLED
        bool async_vdp_path = false;
#if defined(RG_TARGET_HOLO_DYNMOD)
        bool sync_vdp_frame = false;
#else
        bool sync_vdp_frame = drawFrame;
#endif
#else
        const bool sync_vdp_frame = drawFrame;
#endif
        const bool pal_mode = REG1_PAL != 0;
        int lines_per_frame = pal_mode ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        int hint_counter = gwenesis_vdp_regs[10];

        screen_width = REG12_MODE_H40 ? 320 : 256;
        screen_height = pal_mode ? 240 : 224;
#if defined(RG_TARGET_HOLO_DYNMOD)
        if (pal_mode != audio_pal_mode)
        {
            audio_pal_mode = pal_mode;
            ym2612_set_divisor(gwenesis_audio_divisor(pal_mode));
        }
#endif

#if defined(RG_TARGET_HOLO_DYNMOD)
        if (screen_width != last_screen_width || screen_height != last_screen_height)
        {
#if GWENESIS_VDP_ASYNC_ENABLED
            gwenesis_vdp_async_wait_idle_for_sync();
#endif
            gwenesis_clear_surface();
            last_screen_width = screen_width;
            last_screen_height = screen_height;
        }
#endif

#if GWENESIS_VDP_ASYNC_ENABLED
        static uint32_t async_vdp_frame_seq;
        const uint32_t async_vdp_frame_id = ++async_vdp_frame_seq;
        gwenesis_vdp_async_begin_frame(async_vdp_frame_id);
        if (drawFrame)
        {
#if defined(RG_TARGET_HOLO_DYNMOD)
            if (gwenesis_vdp_async_can_submit_frame())
            {
                async_vdp_path = gwenesis_vdp_async_submit_snapshot(async_vdp_frame_id,
                                                                    screen_width,
                                                                    screen_height);
                if (!async_vdp_path)
                    drawFrame = false;
            }
            else
            {
                drawFrame = false;
            }
#else
            if (gwenesis_vdp_async_can_submit_frame())
            {
                sync_vdp_frame = false;
                async_vdp_path = true;
                const bool async_vdp_submitted =
                    gwenesis_vdp_async_submit_snapshot(async_vdp_frame_id,
                                                       screen_width,
                                                       screen_height);
#if defined(GWENESIS_VDP_ASYNC_SERIAL_TEST) && GWENESIS_VDP_ASYNC_SERIAL_TEST
                if (async_vdp_submitted)
                    gwenesis_vdp_async_wait_frame_done(async_vdp_frame_id);
#endif
            }
            else
            {
                gwenesis_vdp_async_wait_idle_for_sync();
            }
#endif
        }
#endif

#if !(defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_FIXED_DRAW_SKIP)
        gwenesis_frameskip_note_frame(drawFrame, startTime);
#endif

        if (sync_vdp_frame)
        {
#if GWENESIS_VDP_GFX_ACTIVE_PTR_TEST
            gwenesis_vdp_gfx_set_render_context(NULL);
#endif
            gwenesis_vdp_set_buffer(currentUpdate->data);
            gwenesis_vdp_render_config();
#if GWENESIS_PROFILER_DETAILED
            GWENESIS_PROFILER_INC(vdp_sync_frames);
#endif
        }

        /* Reset the difference clocks and audio index */
        system_clock = 0;
        zclk = z80_run_enabled ? 0 : 0x1000000;

        ym2612_clock = yfm_run_enabled ? 0 : 0x1000000;
        ym2612_index = 0;
        if (yfm_run_enabled && gwenesis_ym2612_buffer)
            memset(gwenesis_ym2612_buffer, 0, sizeof(*gwenesis_ym2612_buffer) * AUDIO_BUFFER_LENGTH);

#if GWENESIS_SN76489_RUN_ENABLED
        sn76489_clock = sn76489_run_enabled ? 0 : 0x1000000;
        sn76489_index = 0;
        if (sn76489_run_enabled)
            memset(gwenesis_sn76489_buffer, 0, sizeof(*gwenesis_sn76489_buffer) * AUDIO_BUFFER_LENGTH);
#endif

        scan_line = 0;
        const int active_z80_batch_lines = gwenesis_z80_batch_lines_current(yfm_run_enabled);
        const int active_ym_batch_lines = gwenesis_ym_batch_lines_current();

        while (scan_line < lines_per_frame)
        {
            const int line_target = system_clock + VDP_CYCLES_PER_LINE;
            const bool z80_sync_line =
                active_z80_batch_lines <= 1 ||
                ((scan_line + 1) % active_z80_batch_lines) == 0 ||
                scan_line == screen_height - 1 ||
                scan_line == screen_height ||
                scan_line == lines_per_frame - 1;
            const bool ym_sync_line =
                active_ym_batch_lines <= 1 ||
                ((scan_line + 1) % active_ym_batch_lines) == 0 ||
                scan_line == lines_per_frame - 1;

            GWENESIS_PROFILER_TIME_START(prof_start);
            m68k_run(line_target);
            GWENESIS_PROFILER_TIME_ADD(m68k_us, prof_start);
            if (z80_run_enabled && z80_sync_line)
            {
                GWENESIS_PROFILER_TIME_RESTART(prof_start);
                z80_run(line_target);
                GWENESIS_PROFILER_TIME_ADD(z80_us, prof_start);
            }

            /* Audio */
            /*  GWENESIS_AUDIO_ACCURATE:
             *    =1 : cycle accurate mode. audio is refreshed when CPUs are performing a R/W access
             *    =0 : line  accurate mode. audio is refreshed every lines.
             */
            if (GWENESIS_AUDIO_ACCURATE == 0)
            {
#if GWENESIS_SN76489_RUN_ENABLED
                if (sn76489_run_enabled)
                    gwenesis_SN76489_run(line_target);
#endif
                if (yfm_run_enabled)
                {
                    if (ym_sync_line)
                    {
                        GWENESIS_PROFILER_TIME_RESTART(prof_start);
                        ym2612_run(line_target);
                        GWENESIS_PROFILER_TIME_ADD(ym_us, prof_start);
                    }
                }
            }

            /* Video */
            if (sync_vdp_frame && scan_line < screen_height)
            {
                GWENESIS_PROFILER_TIME_RESTART(prof_start);
                gwenesis_vdp_render_line(scan_line); /* render scan_line */
                GWENESIS_PROFILER_TIME_ADD(vdp_us, prof_start);
            }

            // On these lines, the line counter interrupt is reloaded
            if ((scan_line == 0) || (scan_line > screen_height))
            {
                //  if (REG0_LINE_INTERRUPT != 0)
                //    printf("HINTERRUPT counter reloaded: (scan_line: %d, new
                //    counter: %d)\n", scan_line, REG10_LINE_COUNTER);
                hint_counter = REG10_LINE_COUNTER;
            }

            // interrupt line counter
            if (--hint_counter < 0)
            {
                if ((REG0_LINE_INTERRUPT != 0) && (scan_line <= screen_height))
                {
                    hint_pending = 1;
                    // printf("Line int pending %d\n",scan_line);
                    if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                        m68k_update_irq(4);
                }
                hint_counter = REG10_LINE_COUNTER;
            }

            scan_line++;

            // vblank begin at the end of last rendered line
            if (scan_line == screen_height)
            {
                if (REG1_VBLANK_INTERRUPT != 0)
                {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    m68k_set_irq(6);
                }
                if (z80_run_enabled)
                    z80_irq_line(1);
            }
            if (scan_line == (screen_height + 1))
            {
                if (z80_run_enabled)
                    z80_irq_line(0);
            }

            system_clock += VDP_CYCLES_PER_LINE;
        }

        /* Audio
         * synchronize enabled audio chips to system_clock
         * it completes the missing audio sample for accurate audio mode
         */
        if (GWENESIS_AUDIO_ACCURATE == 1)
        {
#if GWENESIS_SN76489_RUN_ENABLED
            if (sn76489_run_enabled)
                gwenesis_SN76489_run(system_clock);
#endif
            if (yfm_run_enabled)
                ym2612_run(system_clock);
        }

        // reset m68k cycles to the begin of next frame cycle
        m68k.cycles -= system_clock;

#if defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_VDP_ASYNC_ENABLED
        {
            (void)async_vdp_path;
            gwenesis_vdp_async_release_displayed_jobs();
        }
#else
        if (drawFrame)
        {
#if !defined(RG_TARGET_HOLO_DYNMOD) && GWENESIS_VDP_ASYNC_ENABLED
            if (async_vdp_path)
            {
                gwenesis_vdp_async_display_latest();
            }
            else
#endif
            {
#if defined(RG_TARGET_HOLO_DYNMOD)
                for (int i = 0; i < 256; ++i)
                    currentUpdate->palette[i] = CRAM565[i];
                currentUpdate->width = screen_width;
                currentUpdate->height = screen_height;
                currentUpdate->offset = (((GWENESIS_SURFACE_HEIGHT - screen_height) / 2) * currentUpdate->stride) +
                                        (((GWENESIS_SURFACE_WIDTH - screen_width) / 2) *
                                         RG_PIXEL_GET_SIZE(currentUpdate->format));
                gwenesis_perf_overlay_draw(currentUpdate);
                displayUpdate = currentUpdate;
                rg_display_submit(displayUpdate, 0);
                gwenesis_display_flip_surface();
#else
                for (int i = 0; i < 256; ++i)
                    currentUpdate->palette[i] = (CRAM565[i] << 8) | (CRAM565[i] >> 8);
                currentUpdate->width = screen_width;
                currentUpdate->height = screen_height;
                currentUpdate->offset = 0;
                gwenesis_perf_overlay_draw(currentUpdate);
                displayUpdate = currentUpdate;
                rg_display_submit(displayUpdate, 0);
#endif
            }
        }
#endif

        size_t audio_count = 0;
        if (yfm_run_enabled && ym2612_index > (int)audio_count)
            audio_count = ym2612_index;
#if GWENESIS_SN76489_RUN_ENABLED
        if (sn76489_run_enabled && sn76489_index > (int)audio_count)
            audio_count = sn76489_index;
#endif
        gwenesis_audio_submit_frame(audio_count);

        const int64_t frame_work_total_us = rg_system_timer() - startTime;
        rg_system_tick(frame_work_total_us);
        gwenesis_pace_frame();
#if GWENESIS_MONITOR_EXTRA_ENABLED
        char monitor_extra[32];
        const int ym_core = yfm_run_enabled ? gwenesis_current_core_id() : -1;
        snprintf(monitor_extra, sizeof(monitor_extra), "avg:%dus ymc:%d",
                 (int)frame_work_total_us, ym_core);
        rg_system_set_monitor_extra(monitor_extra);
#endif
        GWENESIS_PROFILER_INC(frames);
        if (drawFrame)
            GWENESIS_PROFILER_INC(draw_frames);
#if GWENESIS_PROFILER_DETAILED
        gwenesis_profiler_maybe_log();
#endif
    }

    gwenesis_cleanup();
}
