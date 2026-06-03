#include "holo_port.h"

static uint16_t lcd_buffer[LCD_BUFFER_LENGTH];
static int s_holo_window_left;
static int s_holo_window_top;
static int s_holo_window_width;
static int s_holo_window_height;
static size_t s_holo_window_offset;

static void lcd_init(void)
{
    if (!holo_display_acquire(RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT)) {
        holo_port_log("[retrogo.so] display acquire failed");
    }
}

static void lcd_deinit(void)
{
    holo_display_release();
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
    s_holo_window_left = left;
    s_holo_window_top = top;
    s_holo_window_width = width;
    s_holo_window_height = height;
    s_holo_window_offset = 0;
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    (void)length;
    return lcd_buffer;
}

static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    if (!buffer || length == 0 || s_holo_window_width <= 0 || s_holo_window_height <= 0) {
        return;
    }

    const size_t window_width = (size_t)s_holo_window_width;
    const size_t window_height = (size_t)s_holo_window_height;
    const size_t total_pixels = window_width * window_height;

    while (length > 0 && s_holo_window_offset < total_pixels) {
        const size_t row = s_holo_window_offset / window_width;
        const size_t col = s_holo_window_offset % window_width;
        const size_t row_space = window_width - col;
        size_t sent = 0;

        if (col == 0 && length >= window_width) {
            size_t rows = length / window_width;
            if (row + rows > window_height) {
                rows = window_height - row;
            }

            if (rows == 0) {
                break;
            }

            if (!holo_display_push_image((int16_t)s_holo_window_left,
                                         (int16_t)(s_holo_window_top + (int)row),
                                         (uint16_t)window_width,
                                         (uint16_t)rows,
                                         buffer)) {
                break;
            }
            sent = rows * window_width;
        } else {
            size_t pixels = length < row_space ? length : row_space;
            if (!holo_display_push_image((int16_t)(s_holo_window_left + (int)col),
                                         (int16_t)(s_holo_window_top + (int)row),
                                         (uint16_t)pixels,
                                         1,
                                         buffer)) {
                break;
            }
            sent = pixels;
        }

        buffer += sent;
        length -= sent;
        s_holo_window_offset += sent;
    }
}

static void lcd_sync(void)
{
}

const rg_display_driver_t rg_display_driver_holo = {
    .name = "holo",
};
