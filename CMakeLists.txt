cmake_minimum_required(VERSION 3.16)

set(RG_UPSTREAM_DIR "${CMAKE_CURRENT_LIST_DIR}/upstream/retro-go")
set(EXTRA_COMPONENT_DIRS
    "${RG_UPSTREAM_DIR}/components"
    "${RG_UPSTREAM_DIR}/retro-core/components"
)

set(COMPONENTS
    main
    retro-go
    nofrendo
    espressif__elf_loader
    lwip
)

set(RG_BUILD_TARGET RG_TARGET_HOLO_DYNMOD)
set(RG_PROJECT_APP "retrogo")
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
project(retrogo)

idf_build_set_property(COMPILE_DEFINITIONS "RG_TARGET_HOLO_DYNMOD=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "RETRO_GO=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "RG_PROJECT_APP=\"retrogo\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "RG_PROJECT_VER=\"0.1.0\"" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "RG_BUILD_RELEASE=0" APPEND)

execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
    OUTPUT_VARIABLE RETROGO_LIBGCC
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(ELF_LIBS
    "${CMAKE_BINARY_DIR}/esp-idf/main/libmain.a"
    "${CMAKE_BINARY_DIR}/esp-idf/retro-go/libretro-go.a"
    "${CMAKE_BINARY_DIR}/esp-idf/nofrendo/libnofrendo.a"
    "$ENV{IDF_PATH}/components/json/cJSON/cJSON.c"
    "${RETROGO_LIBGCC}"
)

set(ELF_SO_LINK_FLAGS
    "-Wl,-Bsymbolic"
    "-Wl,--exclude-libs,ALL"
    "-Wl,-z,notext"
    "-Wl,--no-check-sections"
    "-Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/exports.map"
    "-Wl,-Map,${CMAKE_BINARY_DIR}/retrogo_so.map"
)

set(ELF_SO_OBJCOPY_FLAGS
    "--rename-section" ".iram1.literal=.mod_iram.literal"
    "--rename-section" ".iram1=.mod_iram"
    "--rename-section" ".iram1.0.literal=.mod_iram.0.literal"
    "--rename-section" ".iram1.0=.mod_iram.0"
    "--rename-section" ".iram1.1.literal=.mod_iram.1.literal"
    "--rename-section" ".iram1.1=.mod_iram.1"
    "--rename-section" ".iram1.2.literal=.mod_iram.2.literal"
    "--rename-section" ".iram1.2=.mod_iram.2"
    "--rename-section" ".iram1.3.literal=.mod_iram.3.literal"
    "--rename-section" ".iram1.3=.mod_iram.3"
)

include(elf_loader)
project_so(retrogo)
