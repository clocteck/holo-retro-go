#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <rg_system.h>
#include "holo_port.h"

#define AUDIO_SAMPLE_RATE   (32000)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 50 + 1)

extern uint8_t shared_memory_block_64K[0x10000];

static inline int holo_should_exit(void)
{
    return holo_runtime_switch_requested() || holo_runtime_stop_requested();
}

void launcher_main();
void gbc_main();
void nes_main();
void pce_main();
void sms_main();
void gw_main();
void lynx_main();
void snes_main();
#if HOLO_RETRO_GWENESIS_ONLY
void gwenesis_main();
#endif
