set(HOLO_RETRO_GO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(RG_UPSTREAM_DIR "${HOLO_RETRO_GO_ROOT}/upstream/retro-go")
set(MODULE_ABI_DIR "" CACHE PATH "Directory containing module_abi.h")

if(NOT MODULE_ABI_DIR AND DEFINED ENV{CUBICLUA_ROOT} AND EXISTS "$ENV{CUBICLUA_ROOT}/src/dynmod/module_abi.h")
    set(MODULE_ABI_DIR "$ENV{CUBICLUA_ROOT}/src/dynmod")
endif()

if(NOT MODULE_ABI_DIR AND EXISTS "${HOLO_RETRO_GO_ROOT}/include/module_abi.h")
    set(MODULE_ABI_DIR "${HOLO_RETRO_GO_ROOT}/include")
endif()

if(NOT EXISTS "${MODULE_ABI_DIR}/module_abi.h")
    message(FATAL_ERROR "module_abi.h not found. Set -DMODULE_ABI_DIR=/path/to/src/dynmod or CUBICLUA_ROOT.")
endif()

set(HOLO_RETRO_PROFILE_FILE "${CMAKE_BINARY_DIR}/holo_retro_profile.cmake")
if(EXISTS "${HOLO_RETRO_PROFILE_FILE}")
    include("${HOLO_RETRO_PROFILE_FILE}")
endif()

if(NOT DEFINED HOLO_RETRO_MAIN_GWENESIS_ONLY)
    set(HOLO_RETRO_MAIN_PROFILE "nes")
    set(HOLO_RETRO_MAIN_MODULE_NAME "retrogo")
    set(HOLO_RETRO_MAIN_GWENESIS_ONLY 0)
endif()

set(RETRO_CORE_MAIN_SRCS
    "${RG_UPSTREAM_DIR}/retro-core/main/main.c"
    "${RG_UPSTREAM_DIR}/retro-core/main/main_launcher.c"
)

if(HOLO_RETRO_MAIN_GWENESIS_ONLY)
    list(APPEND RETRO_CORE_MAIN_SRCS
        "${RG_UPSTREAM_DIR}/gwenesis/main/main.c"
    )
else()
    list(APPEND RETRO_CORE_MAIN_SRCS
        "${RG_UPSTREAM_DIR}/retro-core/main/main_gbc.c"
        "${RG_UPSTREAM_DIR}/retro-core/main/main_gw.c"
        "${RG_UPSTREAM_DIR}/retro-core/main/main_lynx.cpp"
        "${RG_UPSTREAM_DIR}/retro-core/main/main_nes.c"
        "${RG_UPSTREAM_DIR}/retro-core/main/main_pce.c"
        "${RG_UPSTREAM_DIR}/retro-core/main/main_sms.c"
        "${RG_UPSTREAM_DIR}/retro-core/main/main_snes.c"
    )
endif()

message(STATUS "main component profile: ${HOLO_RETRO_MAIN_PROFILE}; gwenesis-only: ${HOLO_RETRO_MAIN_GWENESIS_ONLY}")

set(MAIN_COMPONENT_INCLUDE_DIRS
    "."
    "${MODULE_ABI_DIR}"
    "${RG_UPSTREAM_DIR}/launcher/main"
    "${RG_UPSTREAM_DIR}/retro-core/main"
    "${RG_UPSTREAM_DIR}/components/retro-go"
)

if(HOLO_RETRO_MAIN_GWENESIS_ONLY)
    list(APPEND MAIN_COMPONENT_INCLUDE_DIRS
        "${RG_UPSTREAM_DIR}/gwenesis/components/gwenesis"
        "${RG_UPSTREAM_DIR}/gwenesis/components/gwenesis/src/bus"
        "${RG_UPSTREAM_DIR}/gwenesis/components/gwenesis/src/cpus/M68K"
        "${RG_UPSTREAM_DIR}/gwenesis/components/gwenesis/src/cpus/Z80"
        "${RG_UPSTREAM_DIR}/gwenesis/components/gwenesis/src/io"
        "${RG_UPSTREAM_DIR}/gwenesis/components/gwenesis/src/savestate"
        "${RG_UPSTREAM_DIR}/gwenesis/components/gwenesis/src/sound"
        "${RG_UPSTREAM_DIR}/gwenesis/components/gwenesis/src/vdp"
    )
else()
    list(APPEND MAIN_COMPONENT_INCLUDE_DIRS
        "${RG_UPSTREAM_DIR}/retro-core/components/gnuboy"
        "${RG_UPSTREAM_DIR}/retro-core/components/gw-emulator/src"
        "${RG_UPSTREAM_DIR}/retro-core/components/gw-emulator/src/cpus"
        "${RG_UPSTREAM_DIR}/retro-core/components/gw-emulator/src/gw_sys"
        "${RG_UPSTREAM_DIR}/retro-core/components/handy"
        "${RG_UPSTREAM_DIR}/retro-core/components/nofrendo"
        "${RG_UPSTREAM_DIR}/retro-core/components/pce-go"
        "${RG_UPSTREAM_DIR}/retro-core/components/smsplus"
        "${RG_UPSTREAM_DIR}/retro-core/components/smsplus/cpu"
        "${RG_UPSTREAM_DIR}/retro-core/components/smsplus/sound"
        "${RG_UPSTREAM_DIR}/retro-core/components/snes9x"
    )
endif()

set(LAUNCHER_MAIN_SRCS
    "${RG_UPSTREAM_DIR}/launcher/main/applications.c"
    "${RG_UPSTREAM_DIR}/launcher/main/bookmarks.c"
    "${RG_UPSTREAM_DIR}/launcher/main/browser.c"
    "${RG_UPSTREAM_DIR}/launcher/main/gui.c"
    "${RG_UPSTREAM_DIR}/launcher/main/images.c"
    "${RG_UPSTREAM_DIR}/launcher/main/main.c"
)

set(MAIN_COMPONENT_REQUIRES
    retro-go
)

if(HOLO_RETRO_MAIN_GWENESIS_ONLY)
    list(APPEND MAIN_COMPONENT_REQUIRES
        gwenesis
    )
else()
    list(APPEND MAIN_COMPONENT_REQUIRES
        gnuboy
        gw-emulator
        handy
        nofrendo
        pce-go
        smsplus
        snes9x
    )
endif()

idf_component_register(
    SRCS
        "retrogo_module.c"
        "holo_catalog.c"
        "holo_port.c"
        "holo_compat.c"
        "cxx_compat.c"
        "optional_core_stubs.c"
        ${RETRO_CORE_MAIN_SRCS}
        ${LAUNCHER_MAIN_SRCS}
    INCLUDE_DIRS
        ${MAIN_COMPONENT_INCLUDE_DIRS}
    REQUIRES
        ${MAIN_COMPONENT_REQUIRES}
)

set_source_files_properties("${RG_UPSTREAM_DIR}/retro-core/main/main.c"
    PROPERTIES COMPILE_DEFINITIONS app_main=retrogo_core_app_main)
set_source_files_properties("${RG_UPSTREAM_DIR}/launcher/main/main.c"
    PROPERTIES COMPILE_DEFINITIONS app_main=retrogo_launcher_app_main)
if(HOLO_RETRO_MAIN_GWENESIS_ONLY)
    set_source_files_properties("${RG_UPSTREAM_DIR}/gwenesis/main/main.c"
        PROPERTIES COMPILE_DEFINITIONS app_main=gwenesis_main)
endif()

if(HOLO_RETRO_MAIN_GWENESIS_ONLY)
    set(HOLO_RETRO_MAIN_OPTIMIZATION -O2)
else()
    set(HOLO_RETRO_MAIN_OPTIMIZATION -O2)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    ${HOLO_RETRO_MAIN_OPTIMIZATION}
    -fPIC
    -fvisibility=hidden
    -fdata-sections
    -ffunction-sections
    -fno-builtin-memcpy
    -fno-builtin-memmove
    -fno-builtin-memset
    -DRG_TARGET_HOLO_DYNMOD=1
    -DRETRO_GO=1
    -DRG_PROJECT_APP="${HOLO_RETRO_MAIN_MODULE_NAME}"
    -DRG_PROJECT_VER="0.1.0"
    -DRG_BUILD_RELEASE=0
    -DHOLO_RETRO_GWENESIS_ONLY=${HOLO_RETRO_MAIN_GWENESIS_ONLY}
    -Wno-error=format
    -Wno-error=nonnull-compare
)

if(HOLO_RETRO_MAIN_GWENESIS_ONLY)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -flto=auto
        -ffat-lto-objects
        -flto-compression-level=9
    )
endif()
