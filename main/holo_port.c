#include "holo_port.h"

#include <string.h>

#include "../include/module_abi.h"

static const module_host_api_v1 *s_host;
static uint32_t s_input_source_mask;
static uint32_t s_input_mask;
static holo_launch_t s_launch = {
    .config_ns = "launcher",
    .rom_path = "",
    .boot_flags = 0,
};
static volatile int s_switch_requested;
static volatile int s_stop_requested;
static void *s_display_surface;
static void *s_audio_stream;
static volatile uint8_t *s_runtime_running;
static void **s_runtime_task;

#define HOLO_RG_KEY_UP      (1u << 0)
#define HOLO_RG_KEY_RIGHT   (1u << 1)
#define HOLO_RG_KEY_DOWN    (1u << 2)
#define HOLO_RG_KEY_LEFT    (1u << 3)
#define HOLO_RG_KEY_SELECT  (1u << 4)
#define HOLO_RG_KEY_START   (1u << 5)
#define HOLO_RG_KEY_MENU    (1u << 6)
#define HOLO_RG_KEY_OPTION  (1u << 7)
#define HOLO_RG_KEY_A       (1u << 8)
#define HOLO_RG_KEY_B       (1u << 9)
#define HOLO_RG_KEY_X       (1u << 10)
#define HOLO_RG_KEY_Y       (1u << 11)
#define HOLO_RG_KEY_L       (1u << 12)
#define HOLO_RG_KEY_R       (1u << 13)

static uint32_t nes_mask_to_rg_mask(uint32_t mask)
{
    uint32_t out = 0;

    if (mask & MODULE_GAMEPAD_A) {
        out |= HOLO_RG_KEY_A;
    }
    if (mask & MODULE_GAMEPAD_B) {
        out |= HOLO_RG_KEY_B;
    }
    if (mask & MODULE_GAMEPAD_SELECT) {
        out |= HOLO_RG_KEY_SELECT;
    }
    if (mask & MODULE_GAMEPAD_START) {
        out |= HOLO_RG_KEY_START;
    }
    if (mask & MODULE_GAMEPAD_UP) {
        out |= HOLO_RG_KEY_UP;
    }
    if (mask & MODULE_GAMEPAD_DOWN) {
        out |= HOLO_RG_KEY_DOWN;
    }
    if (mask & MODULE_GAMEPAD_LEFT) {
        out |= HOLO_RG_KEY_LEFT;
    }
    if (mask & MODULE_GAMEPAD_RIGHT) {
        out |= HOLO_RG_KEY_RIGHT;
    }
    if (mask & MODULE_GAMEPAD_X) {
        out |= HOLO_RG_KEY_X;
    }
    if (mask & MODULE_GAMEPAD_Y) {
        out |= HOLO_RG_KEY_Y;
    }
    if (mask & MODULE_GAMEPAD_L) {
        out |= HOLO_RG_KEY_L;
    }
    if (mask & MODULE_GAMEPAD_R) {
        out |= HOLO_RG_KEY_R;
    }
    if (mask & MODULE_GAMEPAD_MENU) {
        out |= HOLO_RG_KEY_MENU;
    }
    if (mask & MODULE_GAMEPAD_HOME) {
        out |= HOLO_RG_KEY_OPTION;
    }

    return out;
}

void holo_port_set_host(const module_host_api_v1 *host)
{
    s_host = host;
}

const module_host_api_v1 *holo_port_host(void)
{
    return s_host;
}

void holo_port_log(const char *text)
{
    if (s_host && s_host->serial.println) {
        s_host->serial.println(text ? text : "");
    }
}

void holo_input_set_mask(uint32_t mask)
{
    s_input_source_mask = mask;
    s_input_mask = nes_mask_to_rg_mask(mask);
}

uint32_t holo_input_get_mask(void)
{
    return s_input_mask;
}

uint32_t holo_input_get_raw_mask(void)
{
    return s_input_source_mask;
}

void holo_launch_set(const char *config_ns, const char *rom_path, uint32_t boot_flags)
{
    size_t i;

    if (!config_ns || !config_ns[0]) {
        config_ns = "launcher";
    }
    if (!rom_path) {
        rom_path = "";
    }

    for (i = 0; i + 1 < sizeof(s_launch.config_ns) && config_ns[i]; ++i) {
        s_launch.config_ns[i] = config_ns[i];
    }
    s_launch.config_ns[i] = '\0';

    for (i = 0; i + 1 < sizeof(s_launch.rom_path) && rom_path[i]; ++i) {
        s_launch.rom_path[i] = rom_path[i];
    }
    s_launch.rom_path[i] = '\0';

    s_launch.boot_flags = boot_flags;
    s_stop_requested = 0;
}

void holo_launch_get(holo_launch_t *out)
{
    if (out) {
        *out = s_launch;
    }
}

void holo_runtime_request_switch(const char *config_ns, const char *rom_path, uint32_t boot_flags)
{
    holo_launch_set(config_ns, rom_path, boot_flags);
    s_switch_requested = 1;
}

int holo_runtime_switch_requested(void)
{
    return s_switch_requested != 0;
}

void holo_runtime_clear_switch_requested(void)
{
    s_switch_requested = 0;
}

void holo_runtime_request_stop(void)
{
    s_stop_requested = 1;
}

int holo_runtime_stop_requested(void)
{
    return s_stop_requested != 0;
}

void holo_runtime_bind_task(volatile uint8_t *running, void **task)
{
    s_runtime_running = running;
    s_runtime_task = task;
}

void holo_runtime_unbind_task(void)
{
    s_runtime_running = 0;
    s_runtime_task = 0;
}

void holo_runtime_exit_now(void)
{
    if (s_runtime_running) {
        *s_runtime_running = 0;
    }
    if (s_runtime_task) {
        *s_runtime_task = 0;
    }
    holo_runtime_unbind_task();
    holo_audio_end();
    holo_display_release();
    if (s_host && s_host->task.remove) {
        s_host->task.remove(0);
    }
    while (1) {
        if (s_host && s_host->task.delay) {
            s_host->task.delay(1000);
        }
    }
}

int holo_display_acquire(uint16_t width, uint16_t height)
{
    if (s_display_surface) {
        return 1;
    }
    if (!s_host || !s_host->display.acquire) {
        return 0;
    }

    module_display_desc_t desc = {
        .size = sizeof(desc),
        .width = width,
        .height = height,
        .pixel_format = MODULE_PIXEL_RGB565,
        .flags = 0,
    };

    return s_host->display.acquire("retrogo", &desc, &s_display_surface) == MODULE_OK && s_display_surface;
}

void holo_display_release(void)
{
    if (s_display_surface && s_host && s_host->display.release) {
        s_host->display.release(s_display_surface);
    }
    s_display_surface = NULL;
}

int holo_display_push_image(int16_t x, int16_t y, uint16_t width, uint16_t height, const uint16_t *pixels)
{
    if (!pixels || width == 0 || height == 0) {
        return 0;
    }
    if (!s_display_surface) {
        return 0;
    }
    if (!s_host || !s_host->display.pushImageDMA) {
        return 0;
    }

    return s_host->display.pushImageDMA(s_display_surface, x, y, width, height, pixels) == MODULE_OK;
}

void *holo_dma_alloc(size_t size)
{
    if (!s_host || !s_host->heap.malloc || size == 0) {
        return NULL;
    }
    return s_host->heap.malloc(size, MODULE_HEAP_INTERNAL | MODULE_HEAP_DMA | MODULE_HEAP_8BIT);
}

void holo_dma_free(void *ptr)
{
    if (ptr && s_host && s_host->heap.free) {
        s_host->heap.free(ptr);
    }
}

int holo_audio_begin(uint32_t sample_rate, uint16_t bits_per_sample, uint16_t channels)
{
    if (s_audio_stream) {
        return 1;
    }
    if (!s_host || !s_host->audio.begin) {
        return 0;
    }

    module_audio_desc_t desc = {
        .size = sizeof(desc),
        .sample_rate = sample_rate,
        .bits_per_sample = bits_per_sample,
        .channels = channels,
        .flags = 0,
    };

    return s_host->audio.begin(&desc, &s_audio_stream) == MODULE_OK && s_audio_stream;
}

void holo_audio_end(void)
{
    if (s_audio_stream && s_host && s_host->audio.end) {
        s_host->audio.end(s_audio_stream);
    }
    s_audio_stream = NULL;
}

int holo_audio_write(const void *samples, size_t bytes, size_t *out_written)
{
    if (out_written) {
        *out_written = 0;
    }
    if (!samples || bytes == 0) {
        return 1;
    }
    if (!s_audio_stream || !s_host || !s_host->audio.write) {
        return 0;
    }

    return s_host->audio.write(s_audio_stream, samples, bytes, out_written) == MODULE_OK;
}

int holo_audio_available(size_t *out_bytes)
{
    if (out_bytes) {
        *out_bytes = 0;
    }
    if (!s_audio_stream || !s_host || !s_host->audio.available) {
        return 0;
    }

    return s_host->audio.available(s_audio_stream, out_bytes) == MODULE_OK;
}
