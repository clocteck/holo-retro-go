set(COMPONENT_SRCDIRS ". drivers/audio fonts libs/lodepng")
set(COMPONENT_ADD_INCLUDEDIRS ". libs/lodepng")

if(RG_ENABLE_NETPLAY)
    list(APPEND COMPONENT_SRCDIRS "libs/netplay")
    list(APPEND COMPONENT_ADD_INCLUDEDIRS "libs/netplay")
endif()

# The Holo dynmod still compiles Retro-Go's platform files; keep the IDF
# components that provide their headers even when host APIs handle runtime I/O.
set(COMPONENT_REQUIRES
    "spi_flash"
    "fatfs"
    "app_update"
    "json"
    "nvs_flash"
    "esp_adc"
    "esp_timer"
    "esp_psram"
    "esp_driver_gpio"
    "esp_driver_i2c"
    "esp_driver_i2s"
    "esp_driver_ledc"
    "esp_driver_sdmmc"
    "esp_driver_sdspi"
    "esp_driver_spi"
    "driver"
)

if(RG_ENABLE_NETWORKING OR RG_ENABLE_NETPLAY)
    list(APPEND COMPONENT_REQUIRES esp_wifi esp_http_client)
endif()

register_component()

# Small size is preferred because of the small cache and most things here aren't performance sensitive!
# rg_display and rg_audio benefit from higher optimization (which of -O2 or -O3 is better depends...)

component_compile_options(
    -DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    -DLODEPNG_NO_COMPILE_ERROR_TEXT
    -fPIC
    -fvisibility=hidden
    -Os
    -Wno-unused-function
)

if(HOLO_RETRO_MAIN_GWENESIS_ONLY OR HOLO_RETRO_GWENESIS_ONLY)
    component_compile_options(
        -flto=auto
        -ffat-lto-objects
        -flto-compression-level=9
    )
endif()

set_source_files_properties(
    rg_audio.c rg_display.c rg_utils.c rg_surface.c libs/lodepng/lodepng.c
    PROPERTIES COMPILE_FLAGS
    -O2
)

if(RG_ENABLE_NETPLAY)
    component_compile_options(-DRG_ENABLE_NETWORKING -DRG_ENABLE_NETPLAY)
elseif(RG_ENABLE_NETWORKING)
    component_compile_options(-DRG_ENABLE_NETWORKING)
endif()

if(RG_ENABLE_PROFILING)
    component_compile_options(-DRG_ENABLE_PROFILING)
endif()

if(RG_PROJECT_VER)
    component_compile_options(-DRG_PROJECT_VER="${RG_PROJECT_VER}")
endif()

if(RG_BUILD_RELEASE)
    component_compile_options(-DRG_BUILD_RELEASE=1)
endif()

set(RG_BUILD_INFO "${RG_BUILD_TARGET}-${RG_BUILD_RELEASE} ${PROJECT_VER} SDK:${IDF_VER} ${IDF_TARGET}")
string(TIMESTAMP RG_BUILD_TIME "%s")

component_compile_options(-D${RG_BUILD_TARGET}=1)
component_compile_options(-DRG_PROJECT_APP="${RG_PROJECT_APP}")
component_compile_options(-DRG_BUILD_INFO="${RG_BUILD_INFO}")
component_compile_options(-DRG_BUILD_TIME=${RG_BUILD_TIME})

message(STATUS "RG_PROJECT_APP: ${RG_PROJECT_APP}")
message(STATUS "RG_PROJECT_VER: ${RG_PROJECT_VER}")
message(STATUS "RG_BUILD_INFO: ${RG_BUILD_INFO}")
