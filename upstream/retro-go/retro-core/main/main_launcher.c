#include "shared.h"

void launcher_main(void)
{
    extern void retrogo_launcher_app_main(void);
    retrogo_launcher_app_main();
}
