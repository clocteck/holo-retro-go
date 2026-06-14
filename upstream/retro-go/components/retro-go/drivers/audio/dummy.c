#include "rg_system.h"
#include "rg_audio.h"

static int64_t busyUntil = 0;

static bool driver_init(int device, int sampleRate)
{
    busyUntil = 0;
    return true;
}

static bool driver_deinit(void)
{
    return true;
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    // Wait until the previous submission is done "playing"
    if (busyUntil > rg_system_timer())
        rg_usleep(busyUntil - rg_system_timer());
    busyUntil = rg_system_timer() + (count * (1000000.f / rg_audio_get_sample_rate()));
    return true;
}

const rg_audio_driver_t rg_audio_driver_dummy = {
    .name = "dummy",
    .init = driver_init,
    .deinit = driver_deinit,
    .submit = driver_submit,
};

#if RG_AUDIO_USE_HOLO_HOST

#include "holo_port.h"

#define HOLO_AUDIO_BUFFER_FRAMES 256

static struct {
    const char *last_error;
    int volume;
    bool muted;
} holo_state;

static bool holo_driver_init(int device, int sample_rate)
{
    (void)device;
    holo_state.last_error = NULL;
    holo_state.volume = 50;
    holo_state.muted = false;

    if (!holo_audio_begin((uint32_t)sample_rate, 16, 2)) {
        holo_state.last_error = "host audio begin failed";
        return false;
    }

    return true;
}

static bool holo_driver_deinit(void)
{
    holo_audio_end();
    return true;
}

static bool holo_driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    rg_audio_frame_t buffer[HOLO_AUDIO_BUFFER_FRAMES];
    size_t pos = 0;

    if (!frames || count == 0) {
        return true;
    }
    if (holo_state.muted || holo_state.volume <= 0) {
        return true;
    }

    const float volume = holo_state.volume * 0.01f;

    for (size_t i = 0; i < count; ++i) {
        buffer[pos].left = (int16_t)(frames[i].left * volume);
        buffer[pos].right = (int16_t)(frames[i].right * volume);
        pos++;

        if (i == count - 1 || pos == RG_COUNT(buffer)) {
            size_t written = 0;
            const size_t bytes = pos * sizeof(buffer[0]);
            if (!holo_audio_write(buffer, bytes, &written) || written != bytes) {
                holo_state.last_error = "host audio write failed";
                return false;
            }
            pos = 0;
        }
    }

    return true;
}

static bool holo_driver_set_mute(bool mute)
{
    holo_state.muted = mute;
    return true;
}

static bool holo_driver_set_volume(int volume)
{
    holo_state.volume = volume;
    return true;
}

static const char *holo_driver_get_error(void)
{
    return holo_state.last_error;
}

const rg_audio_driver_t rg_audio_driver_holo = {
    .name = "holo",
    .init = holo_driver_init,
    .deinit = holo_driver_deinit,
    .submit = holo_driver_submit,
    .set_mute = holo_driver_set_mute,
    .set_volume = holo_driver_set_volume,
    .get_error = holo_driver_get_error,
};

#endif
