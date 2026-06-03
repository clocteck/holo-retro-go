#include "shared.h"

void launcher_main(void)
{
#if defined(RG_TARGET_HOLO_DYNMOD)
    extern void retrogo_launcher_app_main(void);
    retrogo_launcher_app_main();
#else
    // app->configNs = "launcher";
    // app->isLauncher = true;
    // Currently a separate app, see launcher in project's root
    rg_system_exit();
#endif
}
