// Target definition
#define RG_TARGET_NAME             "HOLO-DYNMOD"

// Storage
#define RG_STORAGE_ROOT            "/sd"
#define RG_BASE_PATH               RG_STORAGE_ROOT "/apps/holo-retro-go"
#define RG_BASE_PATH_BIOS          RG_BASE_PATH "/bios"
#define RG_BASE_PATH_CACHE         RG_BASE_PATH "/cache"
#define RG_BASE_PATH_CONFIG        RG_BASE_PATH "/config"
#define RG_BASE_PATH_COVERS        RG_BASE_PATH "/covers"
#define RG_BASE_PATH_MUSIC         RG_BASE_PATH "/music"
#define RG_BASE_PATH_ROMS          RG_STORAGE_ROOT "/roms"
#define RG_BASE_PATH_SAVES         RG_BASE_PATH "/saves"
#define RG_BASE_PATH_THEMES        RG_BASE_PATH "/themes"
#define RG_BASE_PATH_BORDERS       RG_BASE_PATH "/borders"

// Audio
#define RG_AUDIO_USE_HOLO_HOST     1
#define RG_AUDIO_USE_INT_DAC       0
#define RG_AUDIO_USE_EXT_DAC       0
#define RG_AUDIO_USE_SDL2          0
#define RG_AUDIO_USE_BUZZER_PIN    0

// Video
#define RG_SCREEN_DRIVER           100
#define RG_SCREEN_HOST             0
#define RG_SCREEN_SPEED            0
#define RG_SCREEN_BACKLIGHT        1
#define RG_SCREEN_WIDTH            320
#define RG_SCREEN_HEIGHT           240
#define RG_SCREEN_ROTATE           0
#define RG_SCREEN_VISIBLE_AREA     {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA        {0, 0, 0, 0}
#define RG_SCREEN_INIT()

// Input is supplied by Lua/host through holo_input_set_mask().
#define RG_RECOVERY_BTN            0

// Runtime
#define RG_UPDATER_ENABLE          0
#define RG_ZIP_SUPPORT             0
#define RG_LOG_COLORS              0
#define RG_PATH_MAX                255
#define RG_BATTERY_DRIVER          0

// Use a compact CJK font so UTF-8 Chinese ROM names render in the launcher.
#define RG_CHINESE_SUPPORT         1
#define RG_FONT_DEFAULT            RG_FONT_FUSIONPIXEL_12
#define RG_FONT_CHINESE            RG_FONT_FUSIONPIXEL_12
