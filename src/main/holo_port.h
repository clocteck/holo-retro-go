#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct module_host_api_v2 module_host_api_v2;

#ifndef HOLO_MODULE_PATH_MAX
#define HOLO_MODULE_PATH_MAX 160u
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct holo_launch_t {
    char config_ns[16];
    char rom_path[HOLO_MODULE_PATH_MAX];
    char language[8];
    uint32_t boot_flags;
} holo_launch_t;

void holo_port_set_host(const module_host_api_v2 *host);
const module_host_api_v2 *holo_port_host(void);
void holo_port_log(const char *text);

void holo_input_set_mask(uint32_t mask);
uint32_t holo_input_get_mask(void);
uint32_t holo_input_get_raw_mask(void);

void holo_launch_set(const char *config_ns, const char *rom_path, uint32_t boot_flags);
void holo_launch_set_language(const char *language);
void holo_launch_get(holo_launch_t *out);

void holo_runtime_request_switch(const char *config_ns, const char *rom_path, uint32_t boot_flags);
int holo_runtime_switch_requested(void);
void holo_runtime_clear_switch_requested(void);
void holo_runtime_request_stop(void);
int holo_runtime_stop_requested(void);
void holo_runtime_bind_task(volatile uint8_t *running, void **task);
void holo_runtime_unbind_task(void);
void holo_runtime_exit_now(void) __attribute__((noreturn));

int holo_display_acquire(uint16_t width, uint16_t height);
int holo_display_release(void);
int holo_display_release_retry(uint32_t attempts, uint32_t delay_ms);
int holo_display_start_write(void);
int holo_display_push_image(int16_t x, int16_t y, uint16_t width, uint16_t height, const uint16_t *pixels);
int holo_display_set_addr_window(int32_t x, int32_t y, int32_t width, int32_t height);
int holo_display_push_pixels(const uint16_t *pixels, size_t len);
int holo_display_end_write(void);
void *holo_dma_alloc(size_t size);
void holo_dma_free(void *ptr);

int holo_audio_begin(uint32_t sample_rate, uint16_t bits_per_sample, uint16_t channels);
void holo_audio_end(void);
int holo_audio_write(const void *samples, size_t bytes, size_t *out_written);
int holo_audio_available(size_t *out_bytes);


#ifdef __cplusplus
}
#endif
