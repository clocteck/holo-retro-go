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
extern unsigned char *gwenesis_vdp_regs;
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
} gwenesis_audio_mode_t;

static gwenesis_audio_mode_t gwenesis_audio_mode = GWENESIS_AUDIO_MODE_FAST;

static const char *gwenesis_audio_mode_name(gwenesis_audio_mode_t mode)
{
    switch (mode)
    {
    case GWENESIS_AUDIO_MODE_FAST:
        return "Fast";
    case GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE:
    default:
        return "Mute";
    }
}

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_app_t *app;
static void *update_data_base[2];
static int update_height_base[2];
static rg_audio_frame_t *gwenesis_audio_mix_buffer;

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
#define GWENESIS_AUDIO_QUEUE_PACKETS 5
#define GWENESIS_AUDIO_PREBUFFER_PACKETS 4
#define GWENESIS_AUDIO_UNDERFLOW_CHUNK_FRAMES 64
#define GWENESIS_YM_AUDIO_FLUSH_FRAMES 32
#if defined(RG_TARGET_HOLO_DYNMOD)
#define GWENESIS_AUDIO_TASK_STACK (3 * 1024)
#else
#define GWENESIS_AUDIO_TASK_STACK (4 * 1024 - 256)
#endif
#if defined(RG_TARGET_HOLO_DYNMOD)
#define GWENESIS_YM_ASYNC_CORE0 1
#define GWENESIS_YM_QUEUE_PACKETS 8
#define GWENESIS_YM_QUEUE_MEM MEM_SLOW
#define GWENESIS_YM_EVENTS_PER_PACKET 128
#define GWENESIS_PROFILER_DETAILED 0
#else
#define GWENESIS_YM_ASYNC_CORE0 0
#define GWENESIS_YM_QUEUE_PACKETS GWENESIS_AUDIO_QUEUE_PACKETS
#define GWENESIS_YM_QUEUE_MEM MEM_FAST
#define GWENESIS_PROFILER_DETAILED 1
#endif
// --- MAIN

#define GWENESIS_AUTO_FRAMESKIP_TARGET_FPS 50
static const int auto_frameskip_target_us = 1000000 / GWENESIS_AUTO_FRAMESKIP_TARGET_FPS;
#if defined(RG_TARGET_HOLO_DYNMOD)
static const int auto_frameskip_min = 2; /* draw 1, skip 1 */
static const int auto_frameskip_start = 2;
#else
static const int auto_frameskip_min = 1; /* draw every frame */
static const int auto_frameskip_start = 3;
#endif
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
static const int ym_batch_lines = 2;
#else
static const int z80_batch_lines = 1;
static const int ym_batch_lines = 1;
#endif
typedef struct
{
    size_t count;
    rg_audio_frame_t frames[AUDIO_BUFFER_LENGTH];
} gwenesis_audio_packet_t;

#if GWENESIS_YM_ASYNC_CORE0
typedef struct
{
    int target;
    uint8_t a;
    uint8_t v;
    uint8_t reserved[2];
} gwenesis_ym_event_t;

typedef struct
{
    int target;
    int divisor;
    uint16_t count;
    uint8_t reset;
    uint8_t final;
    uint8_t needs_sort;
    gwenesis_ym_event_t events[GWENESIS_YM_EVENTS_PER_PACKET];
} gwenesis_ym_frame_packet_t;
#endif

static gwenesis_audio_packet_t *gwenesis_audio_queue;
static rg_task_t *gwenesis_audio_task_handle;
static volatile bool gwenesis_audio_task_running;
static uint32_t gwenesis_audio_queue_read;
static uint32_t gwenesis_audio_queue_write;
#if GWENESIS_PROFILER_DETAILED
static volatile uint32_t gwenesis_audio_queue_min_fill;
static volatile uint32_t gwenesis_audio_queue_max_fill;
static volatile uint32_t gwenesis_audio_queue_full_waits;
static volatile uint32_t gwenesis_audio_queue_empty_waits;
static volatile uint32_t gwenesis_audio_silence_samples;
static uint32_t gwenesis_audio_clip_count;
static int32_t gwenesis_audio_peak;
#endif
static bool gwenesis_cleaned_up;
#if GWENESIS_YM_ASYNC_CORE0
static gwenesis_ym_frame_packet_t *gwenesis_ym_frame_queue;
static gwenesis_ym_frame_packet_t *gwenesis_ym_pending_frame;
static uint32_t gwenesis_ym_frame_queue_read;
static uint32_t gwenesis_ym_frame_queue_write;
static size_t gwenesis_ym_audio_submitted;
static volatile bool gwenesis_ym_async_capture_active;
#if GWENESIS_PROFILER_DETAILED
static volatile uint32_t gwenesis_ym_async_us;
static volatile uint32_t gwenesis_ym_async_calls;
static volatile uint32_t gwenesis_ym_async_samples;
static volatile uint32_t gwenesis_ym_async_audio_samples;
static volatile uint32_t gwenesis_ym_async_events;
static volatile uint32_t gwenesis_ym_async_event_overflows;
static volatile uint32_t gwenesis_ym_async_reads;
#endif
static uint32_t gwenesis_ym_async_status_mirror;
#endif

static inline int gwenesis_z80_batch_lines_current(void)
{
    return z80_batch_lines;
}

static inline int gwenesis_ym_batch_lines_current(void)
{
    return ym_batch_lines;
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

#if GWENESIS_PROFILER_DETAILED
static inline int gwenesis_profiler_pct(int64_t part, int64_t total)
{
    return total > 0 ? (int)((part * 100 + (total / 2)) / total) : 0;
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
#define GWENESIS_PROFILER_INC(field) (gwenesis_profiler.field++)
#define GWENESIS_PROFILER_ADD_VALUE(field, value) (gwenesis_profiler.field += (value))
#else
#define GWENESIS_PROFILER_INC(field) ((void)0)
#define GWENESIS_PROFILER_ADD_VALUE(field, value) ((void)(value))
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

#if GWENESIS_YM_ASYNC_CORE0
    const uint32_t async_ym_us = gwenesis_ym_async_us;
    const uint32_t async_ym_calls = gwenesis_ym_async_calls;
    const uint32_t async_ym_samples = gwenesis_ym_async_samples;
    const uint32_t async_audio_samples = gwenesis_ym_async_audio_samples;
    const uint32_t async_ym_events = gwenesis_ym_async_events;
    const uint32_t async_ym_overflows = gwenesis_ym_async_event_overflows;
    const uint32_t async_ym_reads = gwenesis_ym_async_reads;
    const bool ym_sync_mode_active = false;
    gwenesis_ym_async_us = 0;
    gwenesis_ym_async_calls = 0;
    gwenesis_ym_async_samples = 0;
    gwenesis_ym_async_audio_samples = 0;
    gwenesis_ym_async_events = 0;
    gwenesis_ym_async_event_overflows = 0;
    gwenesis_ym_async_reads = 0;
#else
    const uint32_t async_ym_us = 0;
    const uint32_t async_ym_calls = 0;
    const uint32_t async_ym_samples = 0;
    const uint32_t async_audio_samples = 0;
    const uint32_t async_ym_events = 0;
    const uint32_t async_ym_overflows = 0;
    const uint32_t async_ym_reads = 0;
    const bool ym_sync_mode_active = false;
#endif
    const int64_t ym_total_us = gwenesis_profiler.ym_us + async_ym_us;
    const uint32_t ym_total_calls = gwenesis_profiler.ym_calls + async_ym_calls;
    const uint32_t ym_total_samples = gwenesis_profiler.ym_samples + async_ym_samples;
    const uint32_t audio_total_samples = gwenesis_profiler.audio_samples + async_audio_samples;
    const int64_t known_us = gwenesis_profiler.m68k_us + gwenesis_profiler.z80_us +
                             ym_total_us + gwenesis_profiler.sn_us +
                             gwenesis_profiler.vdp_us + gwenesis_profiler.display_us +
                             gwenesis_profiler.audio_us + gwenesis_profiler.throttle_us;
    const int64_t other_us = gwenesis_profiler.total_us > known_us
                                 ? gwenesis_profiler.total_us - known_us
                                 : 0;
    uint32_t audio_queue_min = gwenesis_audio_queue_min_fill;
    uint32_t audio_queue_max = gwenesis_audio_queue_max_fill;
    uint32_t audio_queue_full = gwenesis_audio_queue_full_waits;
    uint32_t audio_queue_empty = gwenesis_audio_queue_empty_waits;
    uint32_t audio_silence_samples = gwenesis_audio_silence_samples;
    uint32_t audio_clip_count = gwenesis_audio_clip_count;
    int32_t audio_peak = gwenesis_audio_peak;
#if GWENESIS_PROFILER_DETAILED
    gwenesis_audio_queue_min_fill = (uint32_t)-1;
    gwenesis_audio_queue_max_fill = 0;
    gwenesis_audio_queue_full_waits = 0;
    gwenesis_audio_queue_empty_waits = 0;
    gwenesis_audio_silence_samples = 0;
    gwenesis_audio_clip_count = 0;
    gwenesis_audio_peak = 0;
#endif
    if (audio_queue_min == (uint32_t)-1)
        audio_queue_min = 0;

    RG_LOGI("gwen prof: mode=%s frames=%u draw=%u avg=%dus askip=%d "
            "m68k=%d%% z80=%d%% ym=%d%% sn=%d%% vdp=%d%% disp=%d%% aud=%d%% idle=%d%% other=%d%% dskip=%u "
            "ym_samp=%u out_samp=%u z80_calls=%u ym_calls=%u ymev=%u ymov=%u ymrd=%u ysync=%u aq=%u-%u aqwait=%u aempty=%u asilence=%u aclip=%u apeak=%d",
            gwenesis_audio_mode_name(gwenesis_audio_mode),
            (unsigned)gwenesis_profiler.frames,
            (unsigned)gwenesis_profiler.draw_frames,
            (int)(gwenesis_profiler.total_us / gwenesis_profiler.frames),
            auto_frameskip,
            gwenesis_profiler_pct(gwenesis_profiler.m68k_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.z80_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(ym_total_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.sn_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.vdp_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.display_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.audio_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(gwenesis_profiler.throttle_us, gwenesis_profiler.total_us),
            gwenesis_profiler_pct(other_us, gwenesis_profiler.total_us),
            (unsigned)gwenesis_profiler.display_skips,
            (unsigned)ym_total_samples,
            (unsigned)audio_total_samples,
            (unsigned)gwenesis_profiler.z80_calls,
            (unsigned)ym_total_calls,
            (unsigned)async_ym_events,
            (unsigned)async_ym_overflows,
            (unsigned)async_ym_reads,
            ym_sync_mode_active ? 1U : 0U,
            (unsigned)audio_queue_min,
            (unsigned)audio_queue_max,
            (unsigned)audio_queue_full,
            (unsigned)audio_queue_empty,
            (unsigned)audio_silence_samples,
            (unsigned)audio_clip_count,
            (int)audio_peak);

    memset(&gwenesis_profiler, 0, sizeof(gwenesis_profiler));
    gwenesis_profiler.last_log_us = now;
}
#endif

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
#if GWENESIS_PROFILER_DETAILED
    const int32_t magnitude = value < 0 ? -value : value;
    if (magnitude > gwenesis_audio_peak)
        gwenesis_audio_peak = magnitude;
#endif
    if (value > 32767)
    {
#if GWENESIS_PROFILER_DETAILED
        gwenesis_audio_clip_count++;
#endif
        return 32767;
    }
    if (value < -32768)
    {
#if GWENESIS_PROFILER_DETAILED
        gwenesis_audio_clip_count++;
#endif
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
    /* 260Hz +6dB, Q=0.85, fs=11025 */
    {1.057644912f, -1.863515053f, 0.826516458f, -1.863515053f, 0.884161370f, 0.0f, 0.0f},
    /* 1.2kHz -4dB, Q=1.0, fs=11025 */
    {0.894993041f, -1.109143068f, 0.535929332f, -1.109143068f, 0.430922373f, 0.0f, 0.0f},
    /* 2.4kHz -4dB, Q=0.7, fs=11025 */
    {0.827177030f, -0.214419562f, 0.236221375f, -0.214419562f, 0.063398405f, 0.0f, 0.0f},
    /* 4.2kHz -4dB, Q=0.7, fs=11025 */
    {0.859944072f, 0.909700527f, 0.381032948f, 0.909700527f, 0.240977020f, 0.0f, 0.0f},
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

static void gwenesis_audio_queue_stat_min(uint32_t value)
{
#if GWENESIS_PROFILER_DETAILED
    if (value < gwenesis_audio_queue_min_fill)
        gwenesis_audio_queue_min_fill = value;
#else
    (void)value;
#endif
}

static void gwenesis_audio_queue_stat_max(uint32_t value)
{
#if GWENESIS_PROFILER_DETAILED
    if (value > gwenesis_audio_queue_max_fill)
        gwenesis_audio_queue_max_fill = value;
#else
    (void)value;
#endif
}

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

        gwenesis_audio_queue_stat_max(used);
        if (used < GWENESIS_AUDIO_QUEUE_PACKETS)
        {
            gwenesis_audio_packet_t *packet = &gwenesis_audio_queue[write % GWENESIS_AUDIO_QUEUE_PACKETS];
            packet->count = count;
            memcpy(packet->frames, frames, count * sizeof(packet->frames[0]));
            __atomic_store_n(&gwenesis_audio_queue_write, write + 1, __ATOMIC_RELEASE);
            gwenesis_audio_queue_stat_max(used + 1);
            return true;
        }

#if GWENESIS_PROFILER_DETAILED
        gwenesis_audio_queue_full_waits++;
#endif
        rg_task_delay(1);
    }

    return false;
}

#if !GWENESIS_YM_ASYNC_CORE0
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
#endif

#if GWENESIS_YM_ASYNC_CORE0
bool gwenesis_ym2612_async_enabled(void)
{
    return gwenesis_ym_async_capture_active && gwenesis_ym_pending_frame != NULL;
}

static bool gwenesis_ym_async_submit_pending(int target, bool final, bool block);

static void gwenesis_ym_async_copy_packet(gwenesis_ym_frame_packet_t *dst,
                                          const gwenesis_ym_frame_packet_t *src)
{
    dst->target = src->target;
    dst->divisor = src->divisor;
    dst->count = src->count;
    dst->reset = src->reset;
    dst->final = src->final;
    dst->needs_sort = src->needs_sort;
    if (src->count > 0)
        memcpy(dst->events, src->events, src->count * sizeof(src->events[0]));
}

bool gwenesis_ym2612_async_write(unsigned int a, unsigned int v, int target)
{
    gwenesis_ym_frame_packet_t *packet = gwenesis_ym_pending_frame;
    if (!gwenesis_ym2612_async_enabled() || !packet)
        return false;

    if (target < 0)
        target = 0;

    if (packet->count >= GWENESIS_YM_EVENTS_PER_PACKET)
    {
        if (!gwenesis_ym_async_submit_pending(target, false, false))
        {
#if GWENESIS_PROFILER_DETAILED
            gwenesis_ym_async_event_overflows++;
#endif
            return true;
        }
        packet = gwenesis_ym_pending_frame;
    }

    if (packet->count < GWENESIS_YM_EVENTS_PER_PACKET)
    {
        if (packet->count > 0 && target < packet->events[packet->count - 1].target)
            packet->needs_sort = 1;
        gwenesis_ym_event_t *event = &packet->events[packet->count++];
        event->target = target;
        event->a = (uint8_t)(a & 0x03);
        event->v = (uint8_t)(v & 0xff);
#if GWENESIS_PROFILER_DETAILED
        gwenesis_ym_async_events++;
#endif
    }
    else
    {
#if GWENESIS_PROFILER_DETAILED
        gwenesis_ym_async_event_overflows++;
#endif
    }

    return true;
}

static bool gwenesis_ym_async_submit_pending(int target, bool final, bool block)
{
    if (target < 0)
        target = 0;

    if (!gwenesis_ym2612_async_enabled())
        return false;

    gwenesis_ym_frame_packet_t *pending = gwenesis_ym_pending_frame;
    pending->target = target;
    pending->divisor = REG1_PAL ? AUDIO_FREQ_DIVISOR_PAL : AUDIO_FREQ_DIVISOR_NTSC;
    pending->final = final ? 1 : 0;

    while (gwenesis_audio_task_running)
    {
        const uint32_t read = __atomic_load_n(&gwenesis_ym_frame_queue_read, __ATOMIC_ACQUIRE);
        const uint32_t write = __atomic_load_n(&gwenesis_ym_frame_queue_write, __ATOMIC_ACQUIRE);
        const uint32_t used = write - read;

        gwenesis_audio_queue_stat_max(used);
        if (used < GWENESIS_YM_QUEUE_PACKETS)
        {
            gwenesis_ym_frame_packet_t *packet = &gwenesis_ym_frame_queue[write % GWENESIS_YM_QUEUE_PACKETS];
            gwenesis_ym_async_copy_packet(packet, pending);
            __atomic_store_n(&gwenesis_ym_frame_queue_write, write + 1, __ATOMIC_RELEASE);
            gwenesis_audio_queue_stat_max(used + 1);

            pending->target = target;
            pending->divisor = REG1_PAL ? AUDIO_FREQ_DIVISOR_PAL : AUDIO_FREQ_DIVISOR_NTSC;
            pending->count = 0;
            pending->reset = 0;
            pending->final = 0;
            pending->needs_sort = 0;
            if (final)
                gwenesis_ym_async_capture_active = false;
            return true;
        }

        if (!block)
            return false;

#if GWENESIS_PROFILER_DETAILED
        gwenesis_audio_queue_full_waits++;
#endif
        rg_task_delay(1);
    }

    if (final)
        gwenesis_ym_async_capture_active = false;
    return false;
}

unsigned int gwenesis_ym2612_async_read(int target)
{
    (void)target;
#if GWENESIS_PROFILER_DETAILED
    gwenesis_ym_async_reads++;
#endif
    return __atomic_load_n(&gwenesis_ym_async_status_mirror, __ATOMIC_ACQUIRE) & 0x7f;
}

static bool gwenesis_ym_async_begin_frame(void)
{
    if (!gwenesis_ym_frame_queue || !gwenesis_ym_pending_frame || !gwenesis_audio_task_handle)
    {
        gwenesis_ym_async_capture_active = false;
        RG_PANIC("Genesis YM async audio task not available!");
        return false;
    }

    gwenesis_ym_pending_frame->target = 0;
    gwenesis_ym_pending_frame->divisor = REG1_PAL ? AUDIO_FREQ_DIVISOR_PAL : AUDIO_FREQ_DIVISOR_NTSC;
    gwenesis_ym_pending_frame->count = 0;
    gwenesis_ym_pending_frame->reset = 1;
    gwenesis_ym_pending_frame->final = 0;
    gwenesis_ym_pending_frame->needs_sort = 0;
    gwenesis_ym_async_capture_active = true;
    return true;
}

static bool gwenesis_ym_async_submit_chunk(int target)
{
    return gwenesis_ym_async_submit_pending(target, false, false);
}

static bool gwenesis_ym_async_submit_frame(int target)
{
    return gwenesis_ym_async_submit_pending(target, true, true);
}

static void gwenesis_ym_async_sort_events(gwenesis_ym_event_t *events, uint16_t count)
{
    for (uint16_t i = 1; i < count; ++i)
    {
        const gwenesis_ym_event_t key = events[i];
        uint16_t j = i;

        while (j > 0 && events[j - 1].target > key.target)
        {
            events[j] = events[j - 1];
            --j;
        }

        events[j] = key;
    }
}

static size_t gwenesis_ym_async_render_packet(gwenesis_ym_frame_packet_t *packet)
{
    if (!packet || !gwenesis_ym2612_buffer || !gwenesis_audio_mix_buffer)
        return 0;

    const int64_t prof_start = gwenesis_profiler_now();
    int frame_target = packet->target;
    if (frame_target < 0)
        frame_target = 0;

    if (packet->reset)
    {
        memset(gwenesis_ym2612_buffer, 0, sizeof(*gwenesis_ym2612_buffer) * AUDIO_BUFFER_LENGTH);
        ym2612_set_lite_mode(false);
        ym2612_set_divisor(packet->divisor);
        ym2612_clock = 0;
        ym2612_index = 0;
        gwenesis_ym_audio_submitted = 0;
    }

    if (packet->needs_sort)
        gwenesis_ym_async_sort_events(packet->events, packet->count);

    for (uint16_t i = 0; i < packet->count; ++i)
    {
        int event_target = packet->events[i].target;
        if (event_target < 0)
            event_target = 0;
        if (event_target > frame_target)
            event_target = frame_target;
        YM2612WriteDirect(packet->events[i].a, packet->events[i].v, event_target);
        __atomic_store_n(&gwenesis_ym_async_status_mirror, YM2612ReadStatusDirect(), __ATOMIC_RELEASE);
    }

    ym2612_run(frame_target);
    __atomic_store_n(&gwenesis_ym_async_status_mirror, YM2612ReadStatusDirect(), __ATOMIC_RELEASE);
#if GWENESIS_PROFILER_DETAILED
    gwenesis_ym_async_us += (uint32_t)(rg_system_timer() - prof_start);
#else
    (void)prof_start;
#endif
#if GWENESIS_PROFILER_DETAILED
    gwenesis_ym_async_calls += (uint32_t)packet->count + 1;
#endif

    size_t count = ym2612_index > 0 ? (size_t)ym2612_index : 0;
    if (count > AUDIO_BUFFER_LENGTH)
        count = AUDIO_BUFFER_LENGTH;
    if (gwenesis_ym_audio_submitted > count)
        gwenesis_ym_audio_submitted = count;
    const size_t pending_count = count - gwenesis_ym_audio_submitted;
    if (!packet->final && pending_count < GWENESIS_YM_AUDIO_FLUSH_FRAMES)
        return 0;
#if GWENESIS_PROFILER_DETAILED
    gwenesis_ym_async_samples += (uint32_t)pending_count;
#endif

    if (pending_count == 0)
        return 0;

    for (size_t i = 0; i < pending_count; ++i)
    {
        const int32_t sample = gwenesis_audio_scale(gwenesis_ym2612_buffer[gwenesis_ym_audio_submitted + i]);
        const int16_t clipped = gwenesis_audio_clip(sample);
        gwenesis_audio_mix_buffer[i].left = clipped;
        gwenesis_audio_mix_buffer[i].right = clipped;
    }
    gwenesis_ym_audio_submitted = count;

#if GWENESIS_PROFILER_DETAILED
    gwenesis_ym_async_audio_samples += (uint32_t)pending_count;
#endif
    return pending_count;
}

static void gwenesis_audio_task(void *arg)
{
    (void)arg;

    while (gwenesis_audio_task_running ||
           __atomic_load_n(&gwenesis_ym_frame_queue_read, __ATOMIC_ACQUIRE) !=
               __atomic_load_n(&gwenesis_ym_frame_queue_write, __ATOMIC_ACQUIRE))
    {
        uint32_t read = __atomic_load_n(&gwenesis_ym_frame_queue_read, __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&gwenesis_ym_frame_queue_write, __ATOMIC_ACQUIRE);

        if (read == write)
        {
#if GWENESIS_PROFILER_DETAILED
            gwenesis_audio_queue_empty_waits++;
#endif
            gwenesis_audio_queue_stat_min(0);
            rg_task_delay(1);
            continue;
        }

        gwenesis_ym_frame_packet_t *packet = &gwenesis_ym_frame_queue[read % GWENESIS_YM_QUEUE_PACKETS];
        const size_t count = gwenesis_ym_async_render_packet(packet);

        if (count > 0)
        {
            gwenesis_audio_eq_process_frames(gwenesis_audio_mix_buffer, count);
            rg_audio_submit(gwenesis_audio_mix_buffer, count);
        }

        read++;
        __atomic_store_n(&gwenesis_ym_frame_queue_read, read, __ATOMIC_RELEASE);
        gwenesis_audio_queue_stat_min(write - read);
    }

    gwenesis_audio_task_handle = NULL;
}
#else
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
            gwenesis_audio_queue_stat_min(used);
            if (used == 0)
            {
#if GWENESIS_PROFILER_DETAILED
                gwenesis_audio_queue_empty_waits++;
#endif
            }
            rg_task_delay(1);
            continue;
        }
        prebuffering = false;

        if (read == write)
        {
#if GWENESIS_PROFILER_DETAILED
            gwenesis_audio_queue_empty_waits++;
#endif
            gwenesis_audio_queue_stat_min(0);
            if (have_last_frame)
            {
                gwenesis_audio_make_fade_out(generated, RG_COUNT(generated), last_frame);
                have_last_frame = false;
                rg_audio_submit(generated, RG_COUNT(generated));
#if GWENESIS_PROFILER_DETAILED
                gwenesis_audio_silence_samples += RG_COUNT(generated);
#endif
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
        gwenesis_audio_queue_stat_min(write - read);
    }

    gwenesis_audio_task_handle = NULL;
}
#endif

static bool gwenesis_audio_start(void)
{
    gwenesis_audio_mix_buffer = rg_alloc(sizeof(*gwenesis_audio_mix_buffer) * AUDIO_BUFFER_LENGTH, MEM_FAST);
#if GWENESIS_YM_ASYNC_CORE0
    gwenesis_ym_frame_queue = rg_alloc(sizeof(*gwenesis_ym_frame_queue) * GWENESIS_YM_QUEUE_PACKETS, GWENESIS_YM_QUEUE_MEM);
    gwenesis_ym_pending_frame = rg_alloc(sizeof(*gwenesis_ym_pending_frame), MEM_FAST);
    if (!gwenesis_audio_mix_buffer || !gwenesis_ym_frame_queue || !gwenesis_ym_pending_frame)
        goto fail;
#else
    gwenesis_audio_queue = rg_alloc(sizeof(*gwenesis_audio_queue) * GWENESIS_AUDIO_QUEUE_PACKETS, MEM_FAST);
    if (!gwenesis_audio_mix_buffer || !gwenesis_audio_queue)
        goto fail;

    __atomic_store_n(&gwenesis_audio_queue_read, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gwenesis_audio_queue_write, 0, __ATOMIC_RELEASE);
#endif
#if GWENESIS_YM_ASYNC_CORE0
    __atomic_store_n(&gwenesis_ym_frame_queue_read, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gwenesis_ym_frame_queue_write, 0, __ATOMIC_RELEASE);
    gwenesis_ym_async_capture_active = false;
#if GWENESIS_PROFILER_DETAILED
    gwenesis_ym_async_us = 0;
    gwenesis_ym_async_calls = 0;
    gwenesis_ym_async_samples = 0;
    gwenesis_ym_async_audio_samples = 0;
    gwenesis_ym_async_events = 0;
    gwenesis_ym_async_event_overflows = 0;
    gwenesis_ym_async_reads = 0;
#endif
    __atomic_store_n(&gwenesis_ym_async_status_mirror, 0, __ATOMIC_RELEASE);
#endif
#if GWENESIS_PROFILER_DETAILED
    gwenesis_audio_queue_min_fill = (uint32_t)-1;
    gwenesis_audio_queue_max_fill = 0;
    gwenesis_audio_queue_full_waits = 0;
    gwenesis_audio_queue_empty_waits = 0;
    gwenesis_audio_silence_samples = 0;
    gwenesis_audio_clip_count = 0;
    gwenesis_audio_peak = 0;
#endif
    gwenesis_audio_eq_reset();
    gwenesis_audio_task_running = true;
    gwenesis_audio_task_handle = rg_task_create("gwen_audio", gwenesis_audio_task, NULL,
                                                GWENESIS_AUDIO_TASK_STACK, RG_TASK_PRIORITY_3, 0);
    if (!gwenesis_audio_task_handle)
    {
        gwenesis_audio_task_running = false;
#if !GWENESIS_YM_ASYNC_CORE0
        free(gwenesis_audio_queue);
        gwenesis_audio_queue = NULL;
#endif
#if GWENESIS_YM_ASYNC_CORE0
        gwenesis_ym_async_capture_active = false;
        free(gwenesis_ym_frame_queue);
        gwenesis_ym_frame_queue = NULL;
        free(gwenesis_ym_pending_frame);
        gwenesis_ym_pending_frame = NULL;
        free(gwenesis_audio_mix_buffer);
        gwenesis_audio_mix_buffer = NULL;
        RG_LOGE("Genesis YM async audio task unavailable");
        return false;
#endif
        RG_LOGW("Genesis audio task unavailable, using synchronous audio\n");
    }
    else
    {
#if GWENESIS_YM_ASYNC_CORE0
        RG_LOGI("Genesis YM async audio task started on core 0, queue=%d packets, events=%d/packet, mem=%s\n",
                GWENESIS_YM_QUEUE_PACKETS, GWENESIS_YM_EVENTS_PER_PACKET,
                GWENESIS_YM_QUEUE_MEM == MEM_SLOW ? "slow" : "fast");
#else
        RG_LOGI("Genesis audio task started on core 0, queue=%d packets\n", GWENESIS_AUDIO_QUEUE_PACKETS);
#endif
    }

    return true;

fail:
    free(gwenesis_audio_mix_buffer);
    gwenesis_audio_mix_buffer = NULL;
    free(gwenesis_audio_queue);
    gwenesis_audio_queue = NULL;
#if GWENESIS_YM_ASYNC_CORE0
    free(gwenesis_ym_frame_queue);
    gwenesis_ym_frame_queue = NULL;
    free(gwenesis_ym_pending_frame);
    gwenesis_ym_pending_frame = NULL;
#endif
    return false;
}

static void gwenesis_audio_stop(void)
{
    if (gwenesis_audio_task_handle)
    {
#if GWENESIS_YM_ASYNC_CORE0
        const uint32_t write = __atomic_load_n(&gwenesis_ym_frame_queue_write, __ATOMIC_ACQUIRE);
        __atomic_store_n(&gwenesis_ym_frame_queue_read, write, __ATOMIC_RELEASE);
        gwenesis_ym_async_capture_active = false;
#else
        const uint32_t write = __atomic_load_n(&gwenesis_audio_queue_write, __ATOMIC_ACQUIRE);
        __atomic_store_n(&gwenesis_audio_queue_read, write, __ATOMIC_RELEASE);
#endif
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
#if !GWENESIS_YM_ASYNC_CORE0
    free(gwenesis_audio_queue);
    gwenesis_audio_queue = NULL;
#endif
#if GWENESIS_YM_ASYNC_CORE0
    gwenesis_ym_async_capture_active = false;
    free(gwenesis_ym_frame_queue);
    gwenesis_ym_frame_queue = NULL;
    free(gwenesis_ym_pending_frame);
    gwenesis_ym_pending_frame = NULL;
#endif
    free(gwenesis_audio_mix_buffer);
    gwenesis_audio_mix_buffer = NULL;
}

static void gwenesis_audio_submit_frames(rg_audio_frame_t *frames, size_t count)
{
    if (!frames || count == 0)
        return;

    if (gwenesis_audio_task_handle && gwenesis_audio_queue_push(frames, count))
    {
        return;
    }

    gwenesis_audio_eq_process_frames(frames, count);
    rg_audio_submit(frames, count);
}

static size_t gwenesis_audio_submit_frame(size_t count)
{
    if (rg_audio_get_mute())
        return 0;
    if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE || !gwenesis_audio_mix_buffer || count == 0)
        return 0;

    if (count > AUDIO_BUFFER_LENGTH)
        count = AUDIO_BUFFER_LENGTH;

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

    rg_system_set_monitor_extra(NULL);
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
    strcpy(option->value, "Mute");
    return RG_DIALOG_VOID;
#endif
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_FAST)
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
        gwenesis_audio_mode > GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE)
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

    RG_LOGI("Genesis start\n");

    size_t rom_size = 0;
    void *rom_data = NULL;

    if (rg_extension_match(app->romPath, "zip"))
    {
        if (!rg_storage_unzip_file(app->romPath, NULL, &rom_data, &rom_size, RG_FILE_ALIGN_64KB))
            RG_PANIC("ROM file unzipping failed!");
    }
    else if (!rg_storage_read_file(app->romPath, &rom_data, &rom_size, RG_FILE_ALIGN_64KB))
    {
        RG_PANIC("ROM load failed!");
    }

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
        RG_PANIC("Genesis audio queue allocation failed!");
#endif

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
    auto_frameskip = auto_frameskip_start;
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
        bool scheduled_draw = (frame_counter++ % frameskip) == 0;

        bool display_ready = true;
        if (scheduled_draw)
        {
            int64_t prof_start = gwenesis_profiler_now();
            display_ready = rg_display_sync(false);
            gwenesis_profiler_add(&gwenesis_profiler.display_us, prof_start);
            if (!display_ready)
                GWENESIS_PROFILER_INC(display_skips);
        }
        const bool drawFrame = scheduled_draw && display_ready;
        z80_set_enabled(z80_run_enabled);
#if GWENESIS_YM_ASYNC_CORE0
        const bool ym_async_frame = yfm_run_enabled && gwenesis_ym_async_begin_frame();
#else
        const bool ym_async_frame = false;
        if (!ym_async_frame)
            ym2612_set_lite_mode(false);
#endif

        int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        int hint_counter = gwenesis_vdp_regs[10];

        screen_width = REG12_MODE_H40 ? 320 : 256;
        screen_height = REG1_PAL ? 240 : 224;
#if defined(RG_TARGET_HOLO_DYNMOD)
        if (!ym_async_frame)
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

        if (!ym_async_frame)
        {
            ym2612_clock = yfm_run_enabled ? 0 : 0x1000000;
            ym2612_index = 0;
            if (yfm_run_enabled && gwenesis_ym2612_buffer)
                memset(gwenesis_ym2612_buffer, 0, sizeof(*gwenesis_ym2612_buffer) * AUDIO_BUFFER_LENGTH);
        }

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

            int64_t prof_start = gwenesis_profiler_now();
            m68k_run(line_target);
            gwenesis_profiler_add(&gwenesis_profiler.m68k_us, prof_start);
            if (z80_run_enabled && z80_sync_line)
            {
                prof_start = gwenesis_profiler_now();
                z80_run(line_target);
                gwenesis_profiler_add(&gwenesis_profiler.z80_us, prof_start);
                GWENESIS_PROFILER_INC(z80_calls);
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
                    prof_start = gwenesis_profiler_now();
                    gwenesis_SN76489_run(line_target);
                    gwenesis_profiler_add(&gwenesis_profiler.sn_us, prof_start);
                }
                if (yfm_run_enabled && !ym_async_frame)
                {
                    if (ym_sync_line)
                    {
                        prof_start = gwenesis_profiler_now();
                        ym2612_run(line_target);
                        gwenesis_profiler_add(&gwenesis_profiler.ym_us, prof_start);
                        GWENESIS_PROFILER_INC(ym_calls);
                    }
                }
#if GWENESIS_YM_ASYNC_CORE0
                else if (ym_async_frame && ym_sync_line)
                {
                    gwenesis_ym_async_submit_chunk(line_target);
                }
#endif
            }

            /* Video */
            if (drawFrame && scan_line < screen_height)
            {
                prof_start = gwenesis_profiler_now();
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
            if (yfm_run_enabled && !ym_async_frame)
                ym2612_run(system_clock);
        }

        // reset m68k cycles to the begin of next frame cycle
        m68k.cycles -= system_clock;

        if (drawFrame)
        {
            int64_t prof_start = gwenesis_profiler_now();
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
        size_t audio_output_count = 0;
        const int64_t prof_audio_start = gwenesis_profiler_now();
        if (ym_async_frame)
        {
#if GWENESIS_YM_ASYNC_CORE0
            gwenesis_ym_async_submit_frame(system_clock);
#endif
        }
        else
        {
            if (yfm_run_enabled && ym2612_index > (int)audio_count)
                audio_count = ym2612_index;
            if (sn76489_run_enabled && sn76489_index > (int)audio_count)
                audio_count = sn76489_index;
            audio_output_count = gwenesis_audio_submit_frame(audio_count);
        }
        gwenesis_profiler_add(&gwenesis_profiler.audio_us, prof_audio_start);

        const int64_t frame_work_total_us = rg_system_timer() - startTime;
        rg_system_tick(frame_work_total_us);
        gwenesis_auto_frameskip_update(frame_work_total_us);
#if GWENESIS_PROFILER_DETAILED
        gwenesis_profiler.throttle_us += gwenesis_pace_frame();
#else
        gwenesis_pace_frame();
#endif
        const int64_t frame_total_us = rg_system_timer() - startTime;
        char monitor_extra[32];
        const int ym_core =
#if GWENESIS_YM_ASYNC_CORE0
            ym_async_frame ? 0 :
#endif
            gwenesis_current_core_id();
        snprintf(monitor_extra, sizeof(monitor_extra), "avg:%dus askip:%d ymc:%d",
                 (int)frame_work_total_us, auto_frameskip, ym_core);
        rg_system_set_monitor_extra(monitor_extra);
        GWENESIS_PROFILER_ADD_VALUE(total_us, frame_total_us);
        GWENESIS_PROFILER_INC(frames);
        if (drawFrame)
            GWENESIS_PROFILER_INC(draw_frames);
        GWENESIS_PROFILER_ADD_VALUE(ym_samples, (uint32_t)audio_count);
        GWENESIS_PROFILER_ADD_VALUE(audio_samples, (uint32_t)audio_output_count);
#if GWENESIS_PROFILER_DETAILED
        gwenesis_profiler_maybe_log();
#endif
    }

    gwenesis_cleanup();
}
