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

set(RETRO_CORE_MAIN_SRCS
    "${RG_UPSTREAM_DIR}/retro-core/main/main.c"
    "${RG_UPSTREAM_DIR}/retro-core/main/main_launcher.c"
    "${RG_UPSTREAM_DIR}/retro-core/main/main_nes.c"
)

set(LAUNCHER_MAIN_SRCS
    "${RG_UPSTREAM_DIR}/launcher/main/applications.c"
    "${RG_UPSTREAM_DIR}/launcher/main/bookmarks.c"
    "${RG_UPSTREAM_DIR}/launcher/main/browser.c"
    "${RG_UPSTREAM_DIR}/launcher/main/gui.c"
    "${RG_UPSTREAM_DIR}/launcher/main/images.c"
    "${RG_UPSTREAM_DIR}/launcher/main/main.c"
)

idf_component_register(
    SRCS
        "retrogo_module.c"
        "holo_catalog.c"
        "holo_port.c"
        "holo_compat.c"
        "optional_core_stubs.c"
        ${RETRO_CORE_MAIN_SRCS}
        ${LAUNCHER_MAIN_SRCS}
    INCLUDE_DIRS
        "."
        "${MODULE_ABI_DIR}"
        "${RG_UPSTREAM_DIR}/launcher/main"
        "${RG_UPSTREAM_DIR}/retro-core/main"
        "${RG_UPSTREAM_DIR}/components/retro-go"
        "${RG_UPSTREAM_DIR}/retro-core/components/nofrendo"
    REQUIRES
        retro-go
        nofrendo
)

set_source_files_properties("${RG_UPSTREAM_DIR}/retro-core/main/main.c"
    PROPERTIES COMPILE_DEFINITIONS app_main=retrogo_core_app_main)
set_source_files_properties("${RG_UPSTREAM_DIR}/launcher/main/main.c"
    PROPERTIES COMPILE_DEFINITIONS app_main=retrogo_launcher_app_main)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -fPIC
    -fvisibility=hidden
    -fdata-sections
    -ffunction-sections
    -DRG_TARGET_HOLO_DYNMOD=1
    -DRETRO_GO=1
    -DRG_PROJECT_APP="retrogo"
    -DRG_PROJECT_VER="0.1.0"
    -DRG_BUILD_RELEASE=0
    -Wno-error=format
)
