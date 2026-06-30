#include "holo_port.h"

#define HOLO_LCD_DMA_BUFFER_COUNT 2

static uint16_t *lcd_buffers[HOLO_LCD_DMA_BUFFER_COUNT];
static uint8_t lcd_buffer_count;
static uint8_t lcd_buffer_index;
static int s_holo_window_width;
static int s_holo_window_height;
static size_t s_holo_window_offset;
static bool s_holo_write_active;
static bool s_holo_window_ready;

static void lcd_alloc_buffers(void)
{
    if (lcd_buffer_count > 0) {
        return;
    }

    for (uint8_t i = 0; i < HOLO_LCD_DMA_BUFFER_COUNT; ++i) {
        lcd_buffers[i] = (uint16_t *)holo_dma_alloc(LCD_BUFFER_LENGTH * sizeof(uint16_t));
        if (!lcd_buffers[i]) {
            if (i == 0) {
                holo_port_log("[retrogo.so] display dma buffer alloc failed; display updates disabled");
            } else {
                holo_port_log("[retrogo.so] second display dma buffer alloc failed; falling back to single buffer");
            }
            break;
        }
        lcd_buffer_count++;
    }
}

static void lcd_sync(void)
{
    if (s_holo_write_active) {
        holo_display_end_write();
        s_holo_write_active = false;
        s_holo_window_ready = false;
    }
}

static void lcd_init(void)
{
    if (!holo_display_acquire(RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT)) {
        holo_port_log("[retrogo.so] display acquire failed");
    }
}

static void lcd_deinit(void)
{
    lcd_sync();
    for (uint8_t i = 0; i < HOLO_LCD_DMA_BUFFER_COUNT; ++i) {
        if (lcd_buffers[i]) {
            holo_dma_free(lcd_buffers[i]);
            lcd_buffers[i] = NULL;
        }
    }
    lcd_buffer_count = 0;
    lcd_buffer_index = 0;
    if (!holo_display_release()) {
        holo_port_log("[retrogo.so] display release failed during lcd_deinit");
    }
}

static void lcd_set_rotation(int rotation)
{
    (void)rotation;
}

static void lcd_set_backlight(float percent)
{
    (void)percent;
}

static void lcd_set_window(int left, int top, int width, int height)
{
    s_holo_window_width = width;
    s_holo_window_height = height;
    s_holo_window_offset = 0;
    s_holo_window_ready = false;

    if (!s_holo_write_active) {
        s_holo_write_active = holo_display_start_write() != 0;
    }

    if (s_holo_write_active) {
        s_holo_window_ready = holo_display_set_addr_window(left, top, width, height) != 0;
    }
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    (void)length;
    if (lcd_buffer_count == 0) {
        lcd_alloc_buffers();
    }
    if (lcd_buffer_count == 0) {
        holo_port_log("[retrogo.so] display dma buffer unavailable");
        return NULL;
    }
    uint16_t *buffer = lcd_buffers[lcd_buffer_index];
    lcd_buffer_index = (lcd_buffer_index + 1) % lcd_buffer_count;
    return buffer;
}

static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    if (!buffer || length == 0 || !s_holo_window_ready ||
        s_holo_window_width <= 0 || s_holo_window_height <= 0) {
        return;
    }

    const size_t window_width = (size_t)s_holo_window_width;
    const size_t window_height = (size_t)s_holo_window_height;
    const size_t total_pixels = window_width * window_height;

    while (length > 0 && s_holo_window_offset < total_pixels) {
        size_t sent = length;
        const size_t remaining = total_pixels - s_holo_window_offset;
        if (sent > remaining) {
            sent = remaining;
        }

        if (!holo_display_push_pixels(buffer, sent)) {
            break;
        }
        buffer += sent;
        length -= sent;
        s_holo_window_offset += sent;
    }
}

const rg_display_driver_t rg_display_driver_holo = {
    .name = "holo",
};
