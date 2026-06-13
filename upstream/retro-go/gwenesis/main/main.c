#include <rg_system.h>
#include <rg_audio.h>
#include <rg_display.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gwenesis.h>
#if defined(RG_TARGET_HOLO_DYNMOD)
#include "holo_port.h"
#endif

#define AUDIO_OUTPUT_SAMPLE_RATE (GWENESIS_AUDIO_OUTPUT_RATE)
#define AUDIO_BUFFER_LENGTH (GWENESIS_AUDIO_BUFFER_LENGTH_PAL + 8)

extern unsigned char* VRAM;
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

typedef enum {
    GWENESIS_AUDIO_MODE_FAST = 0,
    GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE = 1,
} gwenesis_audio_mode_t;

static gwenesis_audio_mode_t gwenesis_audio_mode = GWENESIS_AUDIO_MODE_FAST;

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_app_t *app;
static void *update_data_base[2];
static int update_height_base[2];
static rg_audio_frame_t *gwenesis_audio_mix_buffer;

static const char *SETTING_AUDIO_MODE = "audio_mode";

#if defined(RG_TARGET_HOLO_DYNMOD)
#define GWENESIS_SURFACE_MEM MEM_SLOW
#define GWENESIS_SURFACE_WIDTH 320
#define GWENESIS_SURFACE_HEIGHT 240
#define GWENESIS_SURFACE_FORMAT RG_PIXEL_565_LE
#else
#define GWENESIS_SURFACE_MEM MEM_FAST
#define GWENESIS_SURFACE_WIDTH 320
#define GWENESIS_SURFACE_HEIGHT 241
#define GWENESIS_SURFACE_FORMAT RG_PIXEL_PAL565_BE
#endif
#define GWENESIS_AUDIO_RING_FRAMES 1024
#define GWENESIS_AUDIO_TASK_STACK (4 * 1024)
// --- MAIN

static const int full_frameskip = 3;
static const int muted_frameskip = 2;
static uint32_t frame_counter;
static rg_audio_frame_t *gwenesis_audio_ring;
static rg_task_t *gwenesis_audio_task_handle;
static volatile bool gwenesis_audio_task_running;
static uint32_t gwenesis_audio_ring_read;
static uint32_t gwenesis_audio_ring_write;

static int16_t gwenesis_audio_clip(int32_t value)
{
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return (int16_t)value;
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
        read += used + count - GWENESIS_AUDIO_RING_FRAMES;
        __atomic_store_n(&gwenesis_audio_ring_read, read, __ATOMIC_RELEASE);
    }

    while (written < count && (uint32_t)(write - read) < GWENESIS_AUDIO_RING_FRAMES)
    {
        gwenesis_audio_ring[write & (GWENESIS_AUDIO_RING_FRAMES - 1)] = frames[written++];
        write++;
    }

    __atomic_store_n(&gwenesis_audio_ring_write, write, __ATOMIC_RELEASE);
}

static void gwenesis_audio_task(void *arg)
{
    rg_audio_frame_t chunk[128];
    (void)arg;

    while (gwenesis_audio_task_running ||
           __atomic_load_n(&gwenesis_audio_ring_read, __ATOMIC_ACQUIRE) !=
           __atomic_load_n(&gwenesis_audio_ring_write, __ATOMIC_ACQUIRE))
    {
        uint32_t read = __atomic_load_n(&gwenesis_audio_ring_read, __ATOMIC_RELAXED);
        const uint32_t write = __atomic_load_n(&gwenesis_audio_ring_write, __ATOMIC_ACQUIRE);
        size_t count = 0;

        while (count < RG_COUNT(chunk) && read != write)
        {
            chunk[count++] = gwenesis_audio_ring[read & (GWENESIS_AUDIO_RING_FRAMES - 1)];
            read++;
        }

        if (count > 0)
        {
            __atomic_store_n(&gwenesis_audio_ring_read, read, __ATOMIC_RELEASE);
            rg_audio_submit(chunk, count);
        }
        else
        {
            rg_task_delay(1);
        }
    }

    gwenesis_audio_task_handle = NULL;
}

static bool gwenesis_audio_start(void)
{
    gwenesis_audio_mix_buffer = rg_alloc(sizeof(*gwenesis_audio_mix_buffer) * AUDIO_BUFFER_LENGTH, MEM_FAST);
    gwenesis_audio_ring = rg_alloc(sizeof(*gwenesis_audio_ring) * GWENESIS_AUDIO_RING_FRAMES, MEM_FAST);
    if (!gwenesis_audio_mix_buffer || !gwenesis_audio_ring)
        return false;

    __atomic_store_n(&gwenesis_audio_ring_read, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gwenesis_audio_ring_write, 0, __ATOMIC_RELEASE);
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
}

static void gwenesis_audio_submit_frames(const rg_audio_frame_t *frames, size_t count)
{
    if (gwenesis_audio_ring)
        gwenesis_audio_ring_push(frames, count);
    else
        rg_audio_submit(frames, count);
}

static void gwenesis_audio_submit_frame(size_t count)
{
    if (rg_audio_get_mute())
        return;
    if (gwenesis_audio_mode != GWENESIS_AUDIO_MODE_FAST || !gwenesis_audio_mix_buffer || count == 0)
        return;

    if (count > AUDIO_BUFFER_LENGTH)
        count = AUDIO_BUFFER_LENGTH;

    for (size_t i = 0; i < count; ++i)
    {
        int32_t left = 0;
        int32_t right = 0;

        left += gwenesis_ym2612_buffer[i];
        right += gwenesis_ym2612_buffer[i];

        gwenesis_audio_mix_buffer[i].left = gwenesis_audio_clip(left);
        gwenesis_audio_mix_buffer[i].right = gwenesis_audio_clip(right);
    }

    gwenesis_audio_submit_frames(gwenesis_audio_mix_buffer, count);
}

#if defined(RG_TARGET_HOLO_DYNMOD)
static void gwenesis_clear_surface(void)
{
    if (currentUpdate && currentUpdate->data) {
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
    gwenesis_audio_stop();

    if (savestate_fp)
    {
        fclose(savestate_fp);
        savestate_fp = NULL;
    }

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

typedef struct {
    char key[28];
    uint32_t length;
} svar_t;

SaveState* saveGwenesisStateOpenForRead(const char* fileName)
{
    return (void*)1;
}

SaveState* saveGwenesisStateOpenForWrite(const char* fileName)
{
    return (void*)1;
}

int saveGwenesisStateGet(SaveState* state, const char* tagName)
{
    int value = 0;
    saveGwenesisStateGetBuffer(state, tagName, &value, sizeof(int));
    return value;
}

void saveGwenesisStateSet(SaveState* state, const char* tagName, int value)
{
    saveGwenesisStateSetBuffer(state, tagName, &value, sizeof(int));
}

void saveGwenesisStateGetBuffer(SaveState* state, const char* tagName, void* buffer, int length)
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

void saveGwenesisStateSetBuffer(SaveState* state, const char* tagName, void* buffer, int length)
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
        gwenesis_audio_mode = (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_FAST)
            ? GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE
            : GWENESIS_AUDIO_MODE_FAST;
        rg_settings_set_number(NS_APP, SETTING_AUDIO_MODE, gwenesis_audio_mode);
    }
    strcpy(option->value, gwenesis_audio_mode == GWENESIS_AUDIO_MODE_FAST
        ? "Fast"
        : "Muted Performance");

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
        fclose(savestate_fp);
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
        fclose(savestate_fp);
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

#if GWENESIS_AUDIO_EMULATION
    gwenesis_audio_mode = rg_settings_get_number(NS_APP, SETTING_AUDIO_MODE, GWENESIS_AUDIO_MODE_FAST);
    if (gwenesis_audio_mode < GWENESIS_AUDIO_MODE_FAST ||
        gwenesis_audio_mode > GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE) {
        gwenesis_audio_mode = GWENESIS_AUDIO_MODE_FAST;
    }
#else
    gwenesis_audio_mode = GWENESIS_AUDIO_MODE_MUTED_PERFORMANCE;
#endif

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
    RG_LOGI("mod frameskip: audio=%d (~20fps), muted=%d (~30fps)\n", full_frameskip, muted_frameskip);

    extern unsigned char *gwenesis_vdp_regs;
    extern unsigned int gwenesis_vdp_status;
#if !defined(RG_TARGET_HOLO_DYNMOD)
    extern unsigned short *CRAM565;
#endif
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
        const bool yfm_run_enabled = (gwenesis_audio_mode == GWENESIS_AUDIO_MODE_FAST) && !audio_muted;
        const bool sn76489_run_enabled = false;
        const int frameskip = yfm_run_enabled ? full_frameskip : muted_frameskip;
        const bool drawFrame = (frame_counter++ % frameskip) == 0;

        int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        int hint_counter = gwenesis_vdp_regs[10];

        screen_width = REG12_MODE_H40 ? 320 : 256;
        screen_height = REG1_PAL ? 240 : 224;

#if defined(RG_TARGET_HOLO_DYNMOD)
        if (screen_width != last_screen_width || screen_height != last_screen_height) {
            gwenesis_clear_surface();
            last_screen_width = screen_width;
            last_screen_height = screen_height;
        }
#endif

        gwenesis_vdp_set_buffer(currentUpdate->data);
        gwenesis_vdp_render_config();

        /* Reset the difference clocks and audio index */
        system_clock = 0;
        zclk = 0;

        ym2612_clock = yfm_run_enabled ? 0 : 0x1000000;
        ym2612_index = 0;
        if (yfm_run_enabled && gwenesis_ym2612_buffer)
            memset(gwenesis_ym2612_buffer, 0, sizeof(*gwenesis_ym2612_buffer) * AUDIO_BUFFER_LENGTH);

        sn76489_clock = sn76489_run_enabled ? 0 : 0x1000000;
        sn76489_index = 0;
        if (sn76489_run_enabled)
            memset(gwenesis_sn76489_buffer, 0, sizeof(*gwenesis_sn76489_buffer) * AUDIO_BUFFER_LENGTH);

        scan_line = 0;

        while (scan_line < lines_per_frame)
        {
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);
            z80_run(system_clock + VDP_CYCLES_PER_LINE);

            /* Audio */
            /*  GWENESIS_AUDIO_ACCURATE:
            *    =1 : cycle accurate mode. audio is refreshed when CPUs are performing a R/W access
            *    =0 : line  accurate mode. audio is refreshed every lines.
            */
            if (GWENESIS_AUDIO_ACCURATE == 0) {
                if (sn76489_run_enabled)
                    gwenesis_SN76489_run(system_clock + VDP_CYCLES_PER_LINE);
                if (yfm_run_enabled)
                    ym2612_run(system_clock + VDP_CYCLES_PER_LINE);
            }

            /* Video */
            if (drawFrame && scan_line < screen_height)
                gwenesis_vdp_render_line(scan_line); /* render scan_line */

            // On these lines, the line counter interrupt is reloaded
            if ((scan_line == 0) || (scan_line > screen_height)) {
                //  if (REG0_LINE_INTERRUPT != 0)
                //    printf("HINTERRUPT counter reloaded: (scan_line: %d, new
                //    counter: %d)\n", scan_line, REG10_LINE_COUNTER);
                hint_counter = REG10_LINE_COUNTER;
            }

            // interrupt line counter
            if (--hint_counter < 0) {
                if ((REG0_LINE_INTERRUPT != 0) && (scan_line <= screen_height)) {
                    hint_pending = 1;
                    // printf("Line int pending %d\n",scan_line);
                    if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                    m68k_update_irq(4);
                }
                hint_counter = REG10_LINE_COUNTER;
            }

            scan_line++;

            // vblank begin at the end of last rendered line
            if (scan_line == screen_height) {
                if (REG1_VBLANK_INTERRUPT != 0) {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    m68k_set_irq(6);
                }
                z80_irq_line(1);
            }
            if (scan_line == (screen_height + 1)) {
                z80_irq_line(0);
            }

            system_clock += VDP_CYCLES_PER_LINE;
        }

        /* Audio
        * synchronize YM2612 and SN76489 to system_clock
        * it completes the missing audio sample for accurate audio mode
        */
        if (GWENESIS_AUDIO_ACCURATE == 1) {
            if (sn76489_run_enabled)
                gwenesis_SN76489_run(system_clock);
            if (yfm_run_enabled)
                ym2612_run(system_clock);
        }

        // reset m68k cycles to the begin of next frame cycle
        m68k.cycles -= system_clock;

        if (drawFrame)
        {
#if defined(RG_TARGET_HOLO_DYNMOD)
            if (rg_display_sync(false))
            {
                currentUpdate->width = screen_width;
                currentUpdate->height = screen_height;
                currentUpdate->offset = (((GWENESIS_SURFACE_HEIGHT - screen_height) / 2) * currentUpdate->stride) +
                                        (((GWENESIS_SURFACE_WIDTH - screen_width) / 2) * sizeof(uint16_t));
                rg_display_submit(currentUpdate, 0);
            }
#else
            for (int i = 0; i < 256; ++i)
                currentUpdate->palette[i] = (CRAM565[i] << 8) | (CRAM565[i] >> 8);
            if (rg_display_sync(false))
            {
                currentUpdate->width = screen_width;
                currentUpdate->height = screen_height;
                rg_display_submit(currentUpdate, 0);
            }
#endif
        }

        rg_system_tick(rg_system_timer() - startTime);

        size_t audio_count = 0;
        if (yfm_run_enabled && ym2612_index > (int)audio_count)
            audio_count = ym2612_index;
        if (sn76489_run_enabled && sn76489_index > (int)audio_count)
            audio_count = sn76489_index;
        gwenesis_audio_submit_frame(audio_count);

#if defined(RG_TARGET_HOLO_DYNMOD)
        rg_task_delay(1);
#endif
    }

    gwenesis_cleanup();
}
