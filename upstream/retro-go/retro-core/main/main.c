#include "shared.h"
#if defined(RG_TARGET_HOLO_DYNMOD)
#include "holo_port.h"
#endif


void app_main(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    while (true)
    {
        rg_app_t *app = rg_system_reinit(AUDIO_SAMPLE_RATE, NULL, NULL);

        RG_LOGI("configNs=%s", app->configNs);

        if (strcmp(app->configNs, "nes") == 0)
            nes_main();
        else
            launcher_main();

        if (!holo_runtime_switch_requested())
            break;
        holo_runtime_clear_switch_requested();
    }
    return;
#else
    rg_app_t *app = rg_system_init(AUDIO_SAMPLE_RATE, NULL, NULL);

    RG_LOGI("configNs=%s", app->configNs);

    if (strcmp(app->configNs, "nes") == 0)
        nes_main();
#if !defined(RG_TARGET_HOLO_DYNMOD)
    else if (strcmp(app->configNs, "gbc") == 0 || strcmp(app->configNs, "gb") == 0)
        gbc_main();
    else if (strcmp(app->configNs, "pce") == 0)
        pce_main();
    else if (strcmp(app->configNs, "sms") == 0)
        sms_main();
    else if (strcmp(app->configNs, "gg") == 0)
        sms_main();
    else if (strcmp(app->configNs, "col") == 0)
        sms_main();
    else if (strcmp(app->configNs, "gw") == 0)
        gw_main();
    else if (strcmp(app->configNs, "snes") == 0)
        snes_main();
#ifndef __TINYC__
    else if (strcmp(app->configNs, "lnx") == 0)
        lynx_main();
#endif
#endif
    else
        launcher_main();

    RG_PANIC("Never reached");
#endif
}
