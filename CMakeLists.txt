cmake_minimum_required(VERSION 3.16)

set(RG_UPSTREAM_DIR "${CMAKE_CURRENT_LIST_DIR}/upstream/retro-go")
set(HOLO_RETRO_MODULE_PROFILE "nes" CACHE STRING "Module profile to build: nes or gwenesis")
set_property(CACHE HOLO_RETRO_MODULE_PROFILE PROPERTY STRINGS nes gwenesis)

if(HOLO_RETRO_MODULE_PROFILE STREQUAL "gwenesis")
    set(HOLO_RETRO_MODULE_PROFILE_CANONICAL "gwenesis")
    set(HOLO_RETRO_MODULE_NAME "gwenesis" CACHE INTERNAL "Holo Retro-Go module output name")
    set(HOLO_RETRO_GWENESIS_ONLY 1 CACHE INTERNAL "Build only the gwenesis core")
elseif(HOLO_RETRO_MODULE_PROFILE STREQUAL "nes")
    set(HOLO_RETRO_MODULE_PROFILE_CANONICAL "nes")
    set(HOLO_RETRO_MODULE_NAME "retrogo" CACHE INTERNAL "Holo Retro-Go module output name")
    set(HOLO_RETRO_GWENESIS_ONLY 0 CACHE INTERNAL "Build the Holo classic core module")
else()
    message(FATAL_ERROR "Unsupported HOLO_RETRO_MODULE_PROFILE='${HOLO_RETRO_MODULE_PROFILE}'")
endif()

message(STATUS "Holo Retro module profile: ${HOLO_RETRO_MODULE_PROFILE} (${HOLO_RETRO_MODULE_PROFILE_CANONICAL}) -> ${HOLO_RETRO_MODULE_NAME}")

file(WRITE "${CMAKE_BINARY_DIR}/holo_retro_profile.cmake"
    "set(HOLO_RETRO_MAIN_PROFILE \"${HOLO_RETRO_MODULE_PROFILE_CANONICAL}\")\n"
    "set(HOLO_RETRO_MAIN_MODULE_NAME \"${HOLO_RETRO_MODULE_NAME}\")\n"
    "set(HOLO_RETRO_MAIN_GWENESIS_ONLY ${HOLO_RETRO_GWENESIS_ONLY})\n"
)

set(EXTRA_COMPONENT_DIRS
    "${RG_UPSTREAM_DIR}/components"
)

if(HOLO_RETRO_GWENESIS_ONLY)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${RG_UPSTREAM_DIR}/gwenesis/components"
    )
    set(COMPONENTS
        main
        retro-go
        gwenesis
        espressif__elf_loader
        lwip
    )
else()
    list(APPEND EXTRA_COMPONENT_DIRS
        "${RG_UPSTREAM_DIR}/retro-core/components"
    )
    set(COMPONENTS
        main
        retro-go
        nofrendo
        gnuboy
        pce-go
        gw-emulator
        handy
        smsplus
        snes9x
        espressif__elf_loader
        lwip
    )
endif()

set(RG_BUILD_TARGET RG_TARGET_HOLO_DYNMOD)
set(RG_PROJECT_APP "${HOLO_RETRO_MODULE_NAME}")
set(RG_PROJECT_VER "0.1.0")
set(RG_ENABLE_NETWORKING 0)
set(RG_ENABLE_NETPLAY 0)
set(RG_ENABLE_PROFILING 0)
set(RG_BUILD_RELEASE 0)

macro(rg_setup_compile_options)
    component_compile_options(
        -D${RG_BUILD_TARGET}=1
        -DRETRO_GO=1
        -fPIC
        -fvisibility=hidden
        -fno-jump-tables
        -fno-tree-switch-conversion
        -fno-builtin-memcpy
        -fno-builtin-memmove
        -fno-builtin-memset
        -fno-builtin-strlen
        -fno-builtin-strcpy
        -fno-builtin-malloc
        -fno-builtin-free
        ${ARGV}
    )
endmacro()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${HOLO_RETRO_MODULE_NAME})

idf_build_set_property(COMPILE_DEFINITIONS "RG_TARGET_HOLO_DYNMOD=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "RETRO_GO=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "RG_PROJECT_APP=\"${RG_PROJECT_APP}\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "RG_PROJECT_VER=\"0.1.0\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "RG_BUILD_RELEASE=0" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "HOLO_RETRO_GWENESIS_ONLY=${HOLO_RETRO_GWENESIS_ONLY}" APPEND)

execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
    OUTPUT_VARIABLE RETROGO_LIBGCC
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libm.a
    OUTPUT_VARIABLE RETROGO_LIBM
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(ELF_LIBS
    "${CMAKE_BINARY_DIR}/esp-idf/main/libmain.a"
    "${CMAKE_BINARY_DIR}/esp-idf/retro-go/libretro-go.a"
)

if(HOLO_RETRO_GWENESIS_ONLY)
    list(APPEND ELF_LIBS
        "${CMAKE_BINARY_DIR}/esp-idf/gwenesis/libgwenesis.a"
    )
else()
    list(APPEND ELF_LIBS
    "${CMAKE_BINARY_DIR}/esp-idf/nofrendo/libnofrendo.a"
    "${CMAKE_BINARY_DIR}/esp-idf/gnuboy/libgnuboy.a"
    "${CMAKE_BINARY_DIR}/esp-idf/pce-go/libpce-go.a"
    "${CMAKE_BINARY_DIR}/esp-idf/gw-emulator/libgw-emulator.a"
    "${CMAKE_BINARY_DIR}/esp-idf/handy/libhandy.a"
    "${CMAKE_BINARY_DIR}/esp-idf/smsplus/libsmsplus.a"
    "${CMAKE_BINARY_DIR}/esp-idf/snes9x/libsnes9x.a"
    )
endif()

list(APPEND ELF_LIBS
    "$ENV{IDF_PATH}/components/json/cJSON/cJSON.c"
    "${RETROGO_LIBM}"
    "${RETROGO_LIBGCC}"
)

set(ELF_SO_LINK_FLAGS
    "-Wl,-Bsymbolic"
    "-Wl,--exclude-libs,ALL"
    "-Wl,-z,notext"
    "-Wl,--no-check-sections"
    "-Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/exports.map"
    "-Wl,-Map,${CMAKE_BINARY_DIR}/${HOLO_RETRO_MODULE_NAME}_so.map"
)

if(HOLO_RETRO_GWENESIS_ONLY)
    list(APPEND ELF_SO_LINK_FLAGS
        "-flto"
        "-fuse-linker-plugin"
        "-ffat-lto-objects"
        "-flto-partition=one"
    )
endif()

set(ELF_SO_OBJCOPY_FLAGS)

include(elf_loader)
project_so(${HOLO_RETRO_MODULE_NAME})
