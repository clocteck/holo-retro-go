#include <rg_system.h>
#include <rg_audio.h>
#include <rg_display.h>
#include <rg_utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gwenesis.h>
#if defined(RG_TARGET_HOLO_DYNMOD)
#include "holo_port.h"
#endif

#define AUDIO_OUTPUT_SAMPLE_RATE (GWENESIS_AUDIO_OUTPUT_RATE)
#define AUDIO_BUFFER_LENGTH (GWENESIS_AUDIO_BUFFER_LENGTH_PAL + 8)

extern unsigned char *VRAM;
extern int zclk;
int system_clock;
int scan_line;

int16_t *gwenesis_sn76489_buffer;
int sn76489_index;
int sn76489_clock;
int16_t *gwenesis_ym2612_buffer;
int ym2612_index;
int ym2612_clock;

static FILE *savestate_fp = NULL;
static int savestate_errors = 0;

typedef enum
{
    GWENESIS_AUDIO_MODE_FAST = 0,
    GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE = 1,
    GWENESIS_AUDIO_MODE_LITE_YM = 2,
} gwenesis_audio_mode_t;

static gwenesis_audio_mode_t gwenesis_audio_mode = GWENESIS_AUDIO_MODE_FAST;

static const char *gwenesis_audio_mode_name(gwenesis_audio_mode_t mode)
{
    switch (mode)
    {
    case GWENESIS_AUDIO_MODE_FAST:
        return "Fast";
    case GWENESIS_AUDIO_MODE_LITE_YM:
        return "Lite YM";
    case GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE:
    default:
        return "Muted Performance";
    }
}

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_app_t *app;
static void *update_data_base[2];
static int update_height_base[2];
static rg_audio_frame_t *gwenesis_audio_mix_buffer;
static rg_audio_frame_t *gwenesis_audio_stretch_buffer;

static const char *SETTING_AUDIO_MODE = "audio_mode";
static const char *SETTING_Z80_ENABLE = "z80_enable";
static bool gwenesis_z80_enabled = true;

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
#define GWENESIS_AUDIO_RING_FRAMES 2048
#define GWENESIS_AUDIO_TASK_CHUNK_FRAMES 64
#define GWENESIS_AUDIO_RING_TARGET_LOW 512
#define GWENESIS_AUDIO_RING_TARGET_HIGH 768
#define GWENESIS_AUDIO_RING_CRITICAL_HIGH 1536
#define GWENESIS_AUDIO_TASK_STACK (4 * 1024 - 256)
#define GWENESIS_AUDIO_STRETCH_MAX_FRAMES (AUDIO_BUFFER_LENGTH * 4)
// --- MAIN

#define GWENESIS_AUTO_FRAMESKIP_TARGET_FPS 50
static const int auto_frameskip_target_us = 1000000 / GWENESIS_AUTO_FRAMESKIP_TARGET_FPS;
static const int auto_frameskip_min = 1; /* draw every frame */
static const int auto_frameskip_max = 4; /* draw 1, skip 3 */
static const int auto_frameskip_window = 30;
static const int auto_frameskip_margin_us = 800;
static int auto_frameskip = 3;
static int64_t auto_frameskip_accum_us;
static int auto_frameskip_count;
static int64_t frame_pacer_next_us;
static uint32_t frame_counter;
#if defined(RG_TARGET_HOLO_DYNMOD)
static const int z80_batch_lines = 8;
static const int z80_lite_batch_lines = 8;
static const int ym_batch_lines = 1;
static const int ym_lite_batch_lines = 1;
#else
static const int z80_batch_lines = 1;
static const int ym_batch_lines = 1;
#endif
static rg_audio_frame_t *gwenesis_audio_ring;
static rg_task_t *gwenesis_audio_task_handle;
static volatile bool gwenesis_audio_task_running;
static uint32_t gwenesis_audio_ring_read;
static uint32_t gwenesis_audio_ring_write;
static volatile uint32_t gwenesis_audio_ring_min_fill;
static volatile uint32_t gwenesis_audio_ring_max_fill;
static volatile uint32_t gwenesis_audio_ring_dropped;
static volatile uint32_t gwenesis_audio_ring_empty_waits;
static uint32_t gwenesis_audio_output_remainder;
static uint32_t gwenesis_audio_clip_count;
static uint32_t gwenesis_audio_boost_samples;
static int32_t gwenesis_audio_peak;
static rg_audio_frame_t gwenesis_audio_last_frame;
static bool gwenesis_audio_fading;
static bool gwenesis_cleaned_up;

static inline int gwenesis_z80_batch_lines_current(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    return gwenesis_audio_mode == GWENESIS_AUDIO_MODE_LITE_YM ? z80_lite_batch_lines : z80_batch_lines;
#else
    return z80_batch_lines;
#endif
}

static inline int gwenesis_ym_batch_lines_current(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    return gwenesis_audio_mode == GWENESIS_AUDIO_MODE_LITE_YM ? ym_lite_batch_lines : ym_batch_lines;
#else
    return ym_batch_lines;
#endif
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
    int64_t total_us;
    int64_t m68k_us;
    int64_t z80_us;
    int64_t ym_us;
    int64_t sn_us;
    int64_t vdp_us;
    int64_t display_us;
    int64_t audio_us;
    int64_t throttle_us;
    uint32_t frames;
    uint32_t draw_frames;
    uint32_t z80_calls;
    uint32_t ym_calls;
    uint32_t ym_samples;
    uint32_t audio_samples;
    uint32_t display_skips;
} gwenesis_profiler_t;

static gwenesis_profiler_t gwenesis_profiler;

static inline int gwenesis_profiler_pct(int64_t part, int64_t total)
{
    return total > 0 ? (int)((part * 100 + (total / 2)) / total) : 0;
}

static inline void gwenesis_profiler_add(int64_t *field, int64_t start_us)
{
    *field += rg_system_timer() - start_us;
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

    const int64_t known_us = gwenesis_profiler.m68k_us + gwenesis_profiler.z80_us +
                             gwenesis_profiler.ym_us + gwenesis_profiler.sn_us +
                             gwenesis_profiler.vdp_us + gwenesis_profiler.display_us +
                             gwenesis_profiler.audio_us + gwenesis_profiler.throttle_us;
    const int64_t other_us = gwenesis_profiler.total_us > known_us
                                 ? gwenesis_profiler.total_us - known_us
                                 : 0;
    uint32_t audio_ring_min = gwenesis_audio_ring_min_fill;
    uint32_t audio_ring_max = gwenesis_audio_ring_max_fill;
    uint32_t audio_ring_dropped = gwenesis_audio_ring_dropped;
    uint32_t audio_ring_empty = gwenesis_audio_ring_empty_waits;
    uint32_t audio_clip_count = gwenesis_audio_clip_count;
    uint32_t audio_boost_samples = gwenesis_audio_boost_samples;
    int32_t audio_peak = gwenesis_audio_peak;
    gwenesis_audio_ring_min_fill = (uint32_t)-1;
    gwenesis_audio_ring_max_fill = 0;
    gwenesis_audio_ring_dropped = 0;
    gwenesis_audio_ring_empty_waits = 0;
    gwenesis_audio_clip_count = 0;
    gwenesis_audio_boost_samples = 0;
    gwenesis_audio_peak = 0;
    if (audio_ring_min == (uint32_t)-1)
        audio_ring_min = 0;

    RG_LOGI("gwen prof: mode=%s frames=%u draw=%u avg=%dus askip=%d "
            "m68k=%d%% z80=%d%% ym=%d%% sn=%d%% vdp=%d%% disp=%d%% aud=%d%% idle=%d%% other=%d%% dskip=%u "
            "ym_samp=%u out_samp=%u z80_calls=%u ym_calls=%u aring=%u-%u adrop=%u aempty=%u aclip=%u apeak=%d aboost=%u",
            gwenesis_audio_mode_name(gwenesis_audio_mode),
            (unsigned)gwenesis_profiler.frames,
            (unsigned)gwenesis_profiler.draw_frames,
            (int)(gwenesis_profiler.total_us / gwenesis_profiler.frames),
            auto_frameskip,
            gwenesis_profiler_pct(gwenesis_profiler.m68k_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.z80_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.ym_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.sn_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.vdp_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.display_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.audio_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.throttle_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(other_us, gwenesis_profiler.total_us),
            (unsigned)gwenesis_profiler.display_skips,
            (unsigned)gwenesis_profiler.ym_samples,
            (unsigned)gwenesis_profiler.audio_samples,
            (unsigned)gwenesis_profiler.z80_calls,
            (unsigned)gwenesis_profiler.ym_calls,
            (unsigned)audio_ring_min,
            (unsigned)audio_ring_max,
            (unsigned)audio_ring_dropped,
            (unsigned)audio_ring_empty,
            (unsigned)audio_clip_count,
            (int)audio_peak,
            (unsigned)audio_boost_samples);

    memset(&gwenesis_profiler, 0, sizeof(gwenesis_profiler));
    gwenesis_profiler.last_log_us = now;
}

static void gwenesis_auto_frameskip_update(int64_t frame_us)
{
    auto_frameskip_accum_us += frame_us;
    auto_frameskip_count++;

    if (auto_frameskip_count < auto_frameskip_window)
        return;

    const int avg_us = (int)(auto_frameskip_accum_us / auto_frameskip_count);
    if (avg_us > auto_frameskip_target_us + auto_frameskip_margin_us)
    {
        if (auto_frameskip < auto_frameskip_max)
            auto_frameskip++;
    }
    else if (avg_us < auto_frameskip_target_us - auto_frameskip_margin_us)
    {
        if (auto_frameskip > auto_frameskip_min)
            auto_frameskip--;
    }

    auto_frameskip_accum_us = 0;
    auto_frameskip_count = 0;
}

static int64_t gwenesis_pace_frame(void)
{
    const int64_t now = rg_system_timer();

    if (frame_pacer_next_us == 0)
        frame_pacer_next_us = now;

    frame_pacer_next_us += auto_frameskip_target_us;

    if (now < frame_pacer_next_us)
    {
        rg_usleep((uint32_t)(frame_pacer_next_us - now));
        return rg_system_timer() - now;
    }

    if (now - frame_pacer_next_us > auto_frameskip_target_us * 2)
        frame_pacer_next_us = now;

    return 0;
}

static int16_t gwenesis_audio_clip(int32_t value)
{
    const int32_t magnitude = value < 0 ? -value : value;
    if (magnitude > gwenesis_audio_peak)
        gwenesis_audio_peak = magnitude;
    if (value > 32767)
    {
        gwenesis_audio_clip_count++;
        return 32767;
    }
    if (value < -32768)
    {
        gwenesis_audio_clip_count++;
        return -32768;
    }
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

#if defined(RG_TARGET_HOLO_DYNMOD)
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

static gwenesis_audio_biquad_t gwenesis_audio_eq[] = {
    /* 200Hz +5dB, Q=0.9, fs=11025 */
    {1.035208556f, -1.897131619f, 0.874313510f, -1.897131619f, 0.909522067f, 0.0f, 0.0f},
    /* 1.2kHz -5dB, Q=1.0, fs=11025 */
    {0.870277324f, -1.090752989f, 0.536919728f, -1.090752989f, 0.407197052f, 0.0f, 0.0f},
    /* 2.4kHz -3dB, Q=1.0, fs=11025 */
    {0.892551232f, -0.254905631f, 0.371634920f, -0.254905631f, 0.264186152f, 0.0f, 0.0f},
};

static void gwenesis_audio_eq_reset(void)
{
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
static void gwenesis_audio_eq_reset(void) {}
static void gwenesis_audio_eq_process_frames(rg_audio_frame_t *frames, size_t count)
{
    (void)frames;
    (void)count;
}
#endif

static void gwenesis_audio_make_fade_chunk(rg_audio_frame_t *frames, size_t count)
{
    if (!frames || count == 0)
        return;

    const int32_t start_left = gwenesis_audio_fading ? gwenesis_audio_last_frame.left : 0;
    const int32_t start_right = gwenesis_audio_fading ? gwenesis_audio_last_frame.right : 0;

    for (size_t i = 0; i < count; ++i)
    {
        const int32_t remaining = (int32_t)(count - i);
        frames[i].left = (int16_t)((start_left * remaining) / (int32_t)count);
        frames[i].right = (int16_t)((start_right * remaining) / (int32_t)count);
    }

    gwenesis_audio_last_frame.left = 0;
    gwenesis_audio_last_frame.right = 0;
    gwenesis_audio_fading = false;
}

static void gwenesis_audio_ring_stat_min(uint32_t value)
{
    if (value < gwenesis_audio_ring_min_fill)
        gwenesis_audio_ring_min_fill = value;
}

static void gwenesis_audio_ring_stat_max(uint32_t value)
{
    if (value > gwenesis_audio_ring_max_fill)
        gwenesis_audio_ring_max_fill = value;
}

static void gwenesis_audio_ring_push(const rg_audio_frame_t *frames, size_t count)
{
    uint32_t read = __atomic_load_n(&gwenesis_audio_ring_read, __ATOMIC_ACQUIRE);
    uint32_t write = __atomic_load_n(&gwenesis_audio_ring_write, __ATOMIC_RELAXED);
    uint32_t used = write - read;
    size_t written = 0;

    if (!gwenesis_audio_ring || !frames || count == 0)
        return;

    if (count > GWENESIS_AUDIO_RING_FRAMES)
    {
        frames += count - GWENESIS_AUDIO_RING_FRAMES;
        count = GWENESIS_AUDIO_RING_FRAMES;
    }

    if (used + count > GWENESIS_AUDIO_RING_FRAMES)
    {
        const uint32_t dropped = (uint32_t)(used + count - GWENESIS_AUDIO_RING_FRAMES);
        gwenesis_audio_ring_dropped += dropped;
        read += dropped;
        __atomic_store_n(&gwenesis_audio_ring_read, read, __ATOMIC_RELEASE);
    }

    while (written < count && (uint32_t)(write - read) < GWENESIS_AUDIO_RING_FRAMES)
    {
        gwenesis_audio_ring[write & (GWENESIS_AUDIO_RING_FRAMES - 1)] = frames[written++];
        write++;
    }

    __atomic_store_n(&gwenesis_audio_ring_write, write, __ATOMIC_RELEASE);
    gwenesis_audio_ring_stat_max(write - read);
}

static uint32_t gwenesis_audio_prefill_frames(void);
static void gwenesis_audio_task_pace(size_t count, int64_t *next_us);

static void gwenesis_audio_task(void *arg)
{
    rg_audio_frame_t chunk[GWENESIS_AUDIO_TASK_CHUNK_FRAMES];
    bool primed = false;
    int64_t next_audio_us = 0;
    (void)arg;

    while (gwenesis_audio_task_running ||
           __atomic_load_n(&gwenesis_audio_ring_read, __ATOMIC_ACQUIRE) !=
               __atomic_load_n(&gwenesis_audio_ring_write, __ATOMIC_ACQUIRE))
    {
        uint32_t read = __atomic_load_n(&gwenesis_audio_ring_read, __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&gwenesis_audio_ring_write, __ATOMIC_ACQUIRE);
        const uint32_t used = write - read;
        size_t count = 0;

        if (!primed && gwenesis_audio_task_running)
        {
            const uint32_t prefill = gwenesis_audio_prefill_frames();
            if (used < prefill)
            {
                if (used == 0)
                {
                    gwenesis_audio_ring_empty_waits++;
                    gwenesis_audio_ring_stat_min(0);
                }
                else
                {
                    gwenesis_audio_ring_stat_min(used);
                }
                rg_task_delay(1);
                continue;
            }

            primed = true;
            next_audio_us = rg_system_timer();
        }

        while (count < RG_COUNT(chunk) && read != write)
        {
            chunk[count++] = gwenesis_audio_ring[read & (GWENESIS_AUDIO_RING_FRAMES - 1)];
            read++;
        }

        if (count > 0)
        {
            __atomic_store_n(&gwenesis_audio_ring_read, read, __ATOMIC_RELEASE);
            gwenesis_audio_ring_stat_min(write - read);
            gwenesis_audio_eq_process_frames(chunk, count);
            gwenesis_audio_last_frame = chunk[count - 1];
            gwenesis_audio_fading = true;
            rg_audio_submit(chunk, count);
            gwenesis_audio_task_pace(count, &next_audio_us);
        }
        else
        {
            gwenesis_audio_ring_empty_waits++;
            gwenesis_audio_ring_stat_min(0);
            primed = false;
            gwenesis_audio_make_fade_chunk(chunk, RG_COUNT(chunk));
            rg_audio_submit(chunk, RG_COUNT(chunk));
            gwenesis_audio_task_pace(RG_COUNT(chunk), &next_audio_us);
        }
    }

    gwenesis_audio_task_handle = NULL;
}

static bool gwenesis_audio_start(void)
{
    gwenesis_audio_mix_buffer = rg_alloc(sizeof(*gwenesis_audio_mix_buffer) * AUDIO_BUFFER_LENGTH, MEM_FAST);
    gwenesis_audio_stretch_buffer = rg_alloc(sizeof(*gwenesis_audio_stretch_buffer) * GWENESIS_AUDIO_STRETCH_MAX_FRAMES, MEM_FAST);
    gwenesis_audio_ring = rg_alloc(sizeof(*gwenesis_audio_ring) * GWENESIS_AUDIO_RING_FRAMES, MEM_FAST);
    if (!gwenesis_audio_mix_buffer || !gwenesis_audio_stretch_buffer || !gwenesis_audio_ring)
        return false;

    __atomic_store_n(&gwenesis_audio_ring_read, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gwenesis_audio_ring_write, 0, __ATOMIC_RELEASE);
    gwenesis_audio_ring_min_fill = (uint32_t)-1;
    gwenesis_audio_ring_max_fill = 0;
    gwenesis_audio_ring_dropped = 0;
    gwenesis_audio_ring_empty_waits = 0;
    gwenesis_audio_output_remainder = 0;
    gwenesis_audio_clip_count = 0;
    gwenesis_audio_boost_samples = 0;
    gwenesis_audio_peak = 0;
    gwenesis_audio_last_frame.left = 0;
    gwenesis_audio_last_frame.right = 0;
    gwenesis_audio_fading = false;
    gwenesis_audio_eq_reset();
    gwenesis_audio_task_running = true;
    gwenesis_audio_task_handle = rg_task_create("gwen_audio", gwenesis_audio_task, NULL,
                                                GWENESIS_AUDIO_TASK_STACK, RG_TASK_PRIORITY_3, 0);
    if (!gwenesis_audio_task_handle)
    {
        gwenesis_audio_task_running = false;
        free(gwenesis_audio_ring);
        gwenesis_audio_ring = NULL;
        RG_LOGW("Genesis audio task unavailable, using synchronous audio\n");
    }
    else
    {
        RG_LOGI("Genesis audio task started on core 0, ring=%d frames\n", GWENESIS_AUDIO_RING_FRAMES);
    }

    return true;
}

static void gwenesis_audio_stop(void)
{
    if (gwenesis_audio_task_handle)
    {
        const uint32_t write = __atomic_load_n(&gwenesis_audio_ring_write, __ATOMIC_ACQUIRE);
        __atomic_store_n(&gwenesis_audio_ring_read, write, __ATOMIC_RELEASE);
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
    free(gwenesis_audio_ring);
    gwenesis_audio_ring = NULL;
    free(gwenesis_audio_mix_buffer);
    gwenesis_audio_mix_buffer = NULL;
    free(gwenesis_audio_stretch_buffer);
    gwenesis_audio_stretch_buffer = NULL;
}

static void gwenesis_audio_submit_frames(const rg_audio_frame_t *frames, size_t count)
{
    if (gwenesis_audio_ring)
        gwenesis_audio_ring_push(frames, count);
    else
        rg_audio_submit(frames, count);
}

static uint32_t gwenesis_audio_prefill_frames(void)
{
    return GWENESIS_AUDIO_RING_TARGET_LOW;
}

static uint32_t gwenesis_audio_ring_fill(void)
{
    if (!gwenesis_audio_ring)
        return 0;

    const uint32_t read = __atomic_load_n(&gwenesis_audio_ring_read, __ATOMIC_ACQUIRE);
    const uint32_t write = __atomic_load_n(&gwenesis_audio_ring_write, __ATOMIC_ACQUIRE);
    return write - read;
}

static void gwenesis_audio_task_pace(size_t count, int64_t *next_us)
{
    if (count == 0 || !next_us)
        return;

    const int64_t now = rg_system_timer();
    if (*next_us == 0)
        *next_us = now;

    *next_us += ((int64_t)count * 1000000 + AUDIO_OUTPUT_SAMPLE_RATE - 1) /
                AUDIO_OUTPUT_SAMPLE_RATE;

    if (now < *next_us)
    {
        rg_usleep((uint32_t)(*next_us - now));
    }
    else if (now - *next_us > 100000)
    {
        *next_us = now;
    }
}

#if defined(RG_TARGET_HOLO_DYNMOD)
static size_t gwenesis_audio_target_frame_samples(void)
{
    gwenesis_audio_output_remainder += AUDIO_OUTPUT_SAMPLE_RATE;
    const size_t fixed_count = gwenesis_audio_output_remainder / GWENESIS_AUTO_FRAMESKIP_TARGET_FPS;
    size_t target_count = fixed_count;
    gwenesis_audio_output_remainder -= fixed_count * GWENESIS_AUTO_FRAMESKIP_TARGET_FPS;
    const uint32_t fill = gwenesis_audio_ring_fill();
    const uint32_t target_high = GWENESIS_AUDIO_RING_TARGET_HIGH;

    if (fill > target_high)
    {
        size_t reduce = RG_MIN(target_count, (size_t)(fill - target_high));
        if (fill > GWENESIS_AUDIO_RING_CRITICAL_HIGH)
            reduce = RG_MIN(target_count, reduce + (size_t)(fill - GWENESIS_AUDIO_RING_CRITICAL_HIGH));
        target_count -= reduce;
    }

    if (gwenesis_audio_ring)
    {
        const size_t free_frames = fill < GWENESIS_AUDIO_RING_FRAMES
                                       ? (GWENESIS_AUDIO_RING_FRAMES - fill)
                                       : 0;
        target_count = RG_MIN(target_count, free_frames);
    }

    return target_count;
}
#endif

static size_t gwenesis_audio_submit_frame(size_t count)
{
    if (rg_audio_get_mute())
        return 0;
    if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE || !gwenesis_audio_mix_buffer || count == 0)
        return 0;

    if (count > AUDIO_BUFFER_LENGTH)
        count = AUDIO_BUFFER_LENGTH;

    size_t output_count = count;
#if defined(RG_TARGET_HOLO_DYNMOD)
    output_count = RG_MIN(gwenesis_audio_target_frame_samples(), (size_t)GWENESIS_AUDIO_STRETCH_MAX_FRAMES);
#endif

    if (output_count == 0)
        return 0;

    if (output_count != count && gwenesis_audio_stretch_buffer)
    {
        rg_audio_frame_t *out = gwenesis_audio_stretch_buffer;
        uint32_t pos = 0;
        const uint32_t step = (count > 1 && output_count > 1)
                                  ? (uint32_t)(((uint64_t)(count - 1) << 16) / (output_count - 1))
                                  : 0;

        for (size_t i = 0; i < output_count; ++i)
        {
            const size_t src = pos >> 16;
            const size_t next = RG_MIN(src + 1, count - 1);
            const int32_t frac = pos & 0xFFFF;
            const int32_t a = gwenesis_ym2612_buffer[src];
            const int32_t b = gwenesis_ym2612_buffer[next];
            const int16_t sample = gwenesis_audio_clip(gwenesis_audio_scale(a + (((b - a) * frac) >> 16)));

            out[i].left = sample;
            out[i].right = sample;
            pos += step;
        }

        gwenesis_audio_submit_frames(out, output_count);
        return output_count;
    }

    for (size_t i = 0; i < count; ++i)
    {
        int32_t left = 0;
        int32_t right = 0;

        left += gwenesis_audio_scale(gwenesis_ym2612_buffer[i]);
        right += gwenesis_audio_scale(gwenesis_ym2612_buffer[i]);

        gwenesis_audio_mix_buffer[i].left = gwenesis_audio_clip(left);
        gwenesis_audio_mix_buffer[i].right = gwenesis_audio_clip(right);
    }

    gwenesis_audio_submit_frames(gwenesis_audio_mix_buffer, count);
    return count;
}

#if defined(RG_TARGET_HOLO_DYNMOD)
static void gwenesis_clear_surface(void)
{
    if (currentUpdate && currentUpdate->data)
    {
        memset(currentUpdate->data, 0, currentUpdate->stride * GWENESIS_SURFACE_HEIGHT);
    }
}

static void gwenesis_force_native_display(void)
{
    rg_display_set_scaling(RG_DISPLAY_SCALING_OFF);
    rg_display_set_filter(RG_DISPLAY_FILTER_OFF);
}
#endif

static void gwenesis_cleanup(void)
{
    if (gwenesis_cleaned_up)
        return;
    gwenesis_cleaned_up = true;

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

    free(VRAM);
    VRAM = NULL;

    free(gwenesis_sn76489_buffer);
    gwenesis_sn76489_buffer = NULL;
    free(gwenesis_ym2612_buffer);
    gwenesis_ym2612_buffer = NULL;

    gwenesis_vdp_gfx_deinit_fast_ram();
    gwenesis_vdp_mem_deinit_fast_ram();
    gwenesis_sn76489_deinit_fast_ram();
    gwenesis_ym2612_deinit_fast_ram();

#if GNW_TARGET_MARIO == 0 && GNW_TARGET_ZELDA == 0
    unload_cartridge();
#endif
    gwenesis_bus_deinit_fast_ram();
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
    strcpy(option->value, "Muted Performance");
    return RG_DIALOG_VOID;
#endif
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_FAST)
            gwenesis_audio_mode = GWENESIS_AUDIO_MODE_LITE_YM;
        else if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_LITE_YM)
            gwenesis_audio_mode = GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE;
        else
            gwenesis_audio_mode = GWENESIS_AUDIO_MODE_FAST;
        rg_settings_set_number(NS_APP, SETTING_AUDIO_MODE, gwenesis_audio_mode);
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

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
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
        rg_display_submit(currentUpdate, 0);
    }
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, "Genesis audio", "-", RG_DIALOG_FLAG_NORMAL, &audio_mode_update_cb};
    *dest++ = (rg_gui_option_t){0, "Genesis Z80", "-", RG_DIALOG_FLAG_NORMAL, &z80_update_cb};
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

#if defined(RG_TARGET_HOLO_DYNMOD)
    app = rg_system_reinit(AUDIO_OUTPUT_SAMPLE_RATE, &handlers, NULL);
#else
    app = rg_system_init(AUDIO_OUTPUT_SAMPLE_RATE, &handlers, NULL);
#endif
    gwenesis_cleaned_up = false;

#if GWENESIS_AUDIO_EMULATION
    gwenesis_audio_mode = rg_settings_get_number(NS_APP, SETTING_AUDIO_MODE, GWENESIS_AUDIO_MODE_FAST);
    if (gwenesis_audio_mode < GWENESIS_AUDIO_MODE_FAST ||
        gwenesis_audio_mode > GWENESIS_AUDIO_MODE_LITE_YM)
    {
        gwenesis_audio_mode = GWENESIS_AUDIO_MODE_FAST;
    }
#else
    gwenesis_audio_mode = GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE;
#endif
    gwenesis_z80_enabled = rg_settings_get_number(NS_APP, SETTING_Z80_ENABLE, 1) != 0;
    z80_set_enabled(gwenesis_z80_enabled);

#if defined(RG_TARGET_HOLO_DYNMOD)
    gwenesis_force_native_display();
#endif

    updates[0] = rg_surface_create(GWENESIS_SURFACE_WIDTH, GWENESIS_SURFACE_HEIGHT,
                                   GWENESIS_SURFACE_FORMAT, GWENESIS_SURFACE_MEM);
    // updates[1] = rg_surface_create(320, 241, RG_PIXEL_PAL565_BE, MEM_FAST);
    currentUpdate = updates[0];
    if (!currentUpdate)
        RG_PANIC("Genesis video surface allocation failed!");

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
        RG_PANIC("Genesis fast RAM allocation failed!");
#if GWENESIS_AUDIO_EMULATION
    if (!gwenesis_ym2612_init_fast_ram() || !gwenesis_sn76489_init_fast_ram())
        RG_PANIC("Genesis sound state fast memory allocation failed!");
    gwenesis_sn76489_buffer = rg_alloc(sizeof(*gwenesis_sn76489_buffer) * AUDIO_BUFFER_LENGTH, MEM_FAST);
    gwenesis_ym2612_buffer = rg_alloc(sizeof(*gwenesis_ym2612_buffer) * AUDIO_BUFFER_LENGTH, MEM_FAST);
    if (!gwenesis_sn76489_buffer || !gwenesis_ym2612_buffer)
        RG_PANIC("Genesis audio buffer allocation failed!");
#endif

    VRAM = rg_alloc(VRAM_MAX_SIZE, MEM_FAST);
    if (!VRAM)
        RG_PANIC("Genesis VRAM allocation failed!");
    if (!gwenesis_vdp_mem_init_fast_ram())
        RG_PANIC("Genesis VDP fast memory allocation failed!");
    if (!gwenesis_vdp_gfx_init_fast_ram())
        RG_PANIC("Genesis VDP gfx fast memory allocation failed!");
#if GWENESIS_AUDIO_EMULATION
    if (!gwenesis_audio_start())
        RG_PANIC("Genesis audio ring allocation failed!");
#endif

    RG_LOGI("Genesis start\n");

    size_t rom_size;
    void *rom_data;

    if (rg_extension_match(app->romPath, "zip"))
    {
        if (!rg_storage_unzip_file(app->romPath, NULL, &rom_data, &rom_size, RG_FILE_ALIGN_64KB))
            RG_PANIC("ROM file unzipping failed!");
    }
    else if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size, RG_FILE_ALIGN_64KB))
    {
        RG_PANIC("ROM load failed!");
    }

    RG_LOGI("load_cartridge(%p, %d)\n", rom_data, rom_size);
    load_cartridge(rom_data, rom_size);
    // free(rom_data); // load_cartridge takes ownership

    RG_LOGI("power_on()\n");
    power_on();

    RG_LOGI("reset_emulation()\n");
    reset_emulation();

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        rg_emu_load_state(app->saveSlot);
    }

    rg_system_set_tick_rate(60);
    app->frameskip = 0;
    app->maxFrameskip = 0;
    frame_counter = 0;
    auto_frameskip = 3;
    auto_frameskip_accum_us = 0;
    auto_frameskip_count = 0;
    frame_pacer_next_us = rg_system_timer();
    RG_LOGI("mod frameskip: auto target=%dfps range=%d..%d start=%d\n",
            GWENESIS_AUTO_FRAMESKIP_TARGET_FPS,
            auto_frameskip_min,
            auto_frameskip_max,
            auto_frameskip);

    extern unsigned char *gwenesis_vdp_regs;
    extern unsigned short gwenesis_vdp_status;
    extern unsigned short *CRAM565;
    extern unsigned int screen_width, screen_height;
    extern int hint_pending;

    uint32_t keymap[8] = {RG_KEY_UP, RG_KEY_DOWN, RG_KEY_LEFT, RG_KEY_RIGHT, RG_KEY_A, RG_KEY_B, RG_KEY_SELECT, RG_KEY_START};
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
            gwenesis_cleanup();
            return;
        }
#endif
        joystick_old = joystick;
        joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
#if defined(RG_TARGET_HOLO_DYNMOD)
            gwenesis_force_native_display();
            if (holo_runtime_switch_requested() || holo_runtime_stop_requested())
            {
                gwenesis_cleanup();
                return;
            }
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
        const bool sn76489_run_enabled = false;
        const bool z80_run_enabled = gwenesis_z80_enabled;
        const int frameskip = auto_frameskip;
        const bool scheduled_draw = (frame_counter++ % frameskip) == 0;
        bool display_ready = true;
        if (scheduled_draw)
        {
            int64_t prof_start = rg_system_timer();
            display_ready = rg_display_sync(false);
            gwenesis_profiler_add(&gwenesis_profiler.display_us, prof_start);
            if (!display_ready)
                gwenesis_profiler.display_skips++;
        }
        const bool drawFrame = scheduled_draw && display_ready;
        z80_set_enabled(z80_run_enabled);
        ym2612_set_lite_mode(gwenesis_audio_mode == GWENESIS_AUDIO_MODE_LITE_YM);

        int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        int hint_counter = gwenesis_vdp_regs[10];

        screen_width = REG12_MODE_H40 ? 320 : 256;
        screen_height = REG1_PAL ? 240 : 224;
#if defined(RG_TARGET_HOLO_DYNMOD)
        ym2612_set_divisor(REG1_PAL ? AUDIO_FREQ_DIVISOR_PAL : AUDIO_FREQ_DIVISOR_NTSC);
#endif

#if defined(RG_TARGET_HOLO_DYNMOD)
        if (screen_width != last_screen_width || screen_height != last_screen_height)
        {
            gwenesis_clear_surface();
            last_screen_width = screen_width;
            last_screen_height = screen_height;
        }
#endif

        if (drawFrame)
        {
            gwenesis_vdp_set_buffer(currentUpdate->data);
            gwenesis_vdp_render_config();
        }

        /* Reset the difference clocks and audio index */
        system_clock = 0;
        zclk = z80_run_enabled ? 0 : 0x1000000;

        ym2612_clock = yfm_run_enabled ? 0 : 0x1000000;
        ym2612_index = 0;
        if (yfm_run_enabled && gwenesis_ym2612_buffer)
            memset(gwenesis_ym2612_buffer, 0, sizeof(*gwenesis_ym2612_buffer) * AUDIO_BUFFER_LENGTH);

        sn76489_clock = sn76489_run_enabled ? 0 : 0x1000000;
        sn76489_index = 0;
        if (sn76489_run_enabled)
            memset(gwenesis_sn76489_buffer, 0, sizeof(*gwenesis_sn76489_buffer) * AUDIO_BUFFER_LENGTH);

        scan_line = 0;
        const int active_z80_batch_lines = gwenesis_z80_batch_lines_current();
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

            int64_t prof_start = rg_system_timer();
            m68k_run(line_target);
            gwenesis_profiler_add(&gwenesis_profiler.m68k_us, prof_start);
            if (z80_run_enabled && z80_sync_line)
            {
                prof_start = rg_system_timer();
                z80_run(line_target);
                gwenesis_profiler_add(&gwenesis_profiler.z80_us, prof_start);
                gwenesis_profiler.z80_calls++;
            }

            /* Audio */
            /*  GWENESIS_AUDIO_ACCURATE:
             *    =1 : cycle accurate mode. audio is refreshed when CPUs are performing a R/W access
             *    =0 : line  accurate mode. audio is refreshed every lines.
             */
            if (GWENESIS_AUDIO_ACCURATE == 0)
            {
                if (sn76489_run_enabled)
                {
                    prof_start = rg_system_timer();
                    gwenesis_SN76489_run(line_target);
                    gwenesis_profiler_add(&gwenesis_profiler.sn_us, prof_start);
                }
                if (yfm_run_enabled)
                {
                    if (ym_sync_line)
                    {
                        prof_start = rg_system_timer();
                        ym2612_run(line_target);
                        gwenesis_profiler_add(&gwenesis_profiler.ym_us, prof_start);
                        gwenesis_profiler.ym_calls++;
                    }
                }
            }

            /* Video */
            if (drawFrame && scan_line < screen_height)
            {
                prof_start = rg_system_timer();
                gwenesis_vdp_render_line(scan_line); /* render scan_line */
                gwenesis_profiler_add(&gwenesis_profiler.vdp_us, prof_start);
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
         * synchronize YM2612 and SN76489 to system_clock
         * it completes the missing audio sample for accurate audio mode
         */
        if (GWENESIS_AUDIO_ACCURATE == 1)
        {
            if (sn76489_run_enabled)
                gwenesis_SN76489_run(system_clock);
            if (yfm_run_enabled)
                ym2612_run(system_clock);
        }

        // reset m68k cycles to the begin of next frame cycle
        m68k.cycles -= system_clock;

        if (drawFrame)
        {
            int64_t prof_start = rg_system_timer();
#if defined(RG_TARGET_HOLO_DYNMOD)
            for (int i = 0; i < 256; ++i)
                currentUpdate->palette[i] = CRAM565[i];
            currentUpdate->width = screen_width;
            currentUpdate->height = screen_height;
            currentUpdate->offset = (((GWENESIS_SURFACE_HEIGHT - screen_height) / 2) * currentUpdate->stride) +
                                    (((GWENESIS_SURFACE_WIDTH - screen_width) / 2) *
                                     RG_PIXEL_GET_SIZE(currentUpdate->format));
            rg_display_submit(currentUpdate, 0);
#else
            for (int i = 0; i < 256; ++i)
                currentUpdate->palette[i] = (CRAM565[i] << 8) | (CRAM565[i] >> 8);
            currentUpdate->width = screen_width;
            currentUpdate->height = screen_height;
            rg_display_submit(currentUpdate, 0);
#endif
            gwenesis_profiler_add(&gwenesis_profiler.display_us, prof_start);
        }

        size_t audio_count = 0;
        if (yfm_run_enabled && ym2612_index > (int)audio_count)
            audio_count = ym2612_index;
        if (sn76489_run_enabled && sn76489_index > (int)audio_count)
            audio_count = sn76489_index;
        const int64_t prof_audio_start = rg_system_timer();
        const size_t audio_output_count = gwenesis_audio_submit_frame(audio_count);
        gwenesis_profiler_add(&gwenesis_profiler.audio_us, prof_audio_start);

        const int64_t frame_work_total_us = rg_system_timer() - startTime;
        rg_system_tick(frame_work_total_us);
        gwenesis_auto_frameskip_update(frame_work_total_us);
        gwenesis_profiler.throttle_us += gwenesis_pace_frame();
        const int64_t frame_total_us = rg_system_timer() - startTime;
        gwenesis_profiler.total_us += frame_total_us;
        gwenesis_profiler.frames++;
        if (drawFrame)
            gwenesis_profiler.draw_frames++;
        gwenesis_profiler.ym_samples += (uint32_t)audio_count;
        gwenesis_profiler.audio_samples += (uint32_t)audio_output_count;
        gwenesis_profiler_maybe_log();
    }

    gwenesis_cleanup();
}
