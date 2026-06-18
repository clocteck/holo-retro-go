#include "shared.h"
#include "holo_port.h"


void app_main(void)
{
    while (true)
    {
        rg_app_t *app = rg_system_reinit(AUDIO_SAMPLE_RATE, NULL, NULL);

        RG_LOGI("configNs=%s", app->configNs);

#if HOLO_RETRO_GWENESIS_ONLY
        if (strcmp(app->configNs, "md") == 0)
            gwenesis_main();
        else
            launcher_main();
#else
        if (strcmp(app->configNs, "nes") == 0)
            nes_main();
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
        else if (strcmp(app->configNs, "lnx") == 0)
            lynx_main();
        else
            launcher_main();
#endif

        if (!holo_runtime_switch_requested())
            break;
        holo_runtime_clear_switch_requested();
    }
    return;
}
