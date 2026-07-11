set(COMPONENT_SRCDIRS "src/bus src/cpus/M68K src/cpus/Z80 src/io src/savestate src/sound src/vdp")
set(COMPONENT_ADD_INCLUDEDIRS ". src/bus src/cpus/M68K src/cpus/Z80 src/io src/savestate src/sound src/vdp")
set(COMPONENT_REQUIRES "retro-go")
register_component()
rg_setup_compile_options(
    -O2
    -fbuiltin
    -fno-semantic-interposition
    -fomit-frame-pointer
    -fno-math-errno
    -Wno-incompatible-pointer-types
    -DBUILD_TABLES
    -DROM_DATA=gwenesis_ROM_DATA
    -DGWENESIS_SN76489_RUN_ENABLED=1
)

set_source_files_properties("src/cpus/Z80/Z80.c"
    PROPERTIES COMPILE_OPTIONS "-Ofast;-fjump-tables;-ftree-switch-conversion")
set_source_files_properties("src/cpus/M68K/m68kcpu.c"
    PROPERTIES COMPILE_OPTIONS "-Ofast")
set_source_files_properties(
    "src/vdp/gwenesis_vdp_gfx.c"
    "src/vdp/gwenesis_vdp_mem.c"
    PROPERTIES COMPILE_OPTIONS "-O3;-fjump-tables;-ftree-switch-conversion"
)
set_source_files_properties(
    "src/bus/gwenesis_bus.c"
    PROPERTIES COMPILE_OPTIONS "-O3;-fjump-tables;-ftree-switch-conversion"
)
set_source_files_properties(
    "src/sound/gwenesis_sn76489.c"
    "src/sound/ym2612.c"
    "src/sound/z80inst.c"
    PROPERTIES COMPILE_OPTIONS "-O3"
)

if(HOLO_RETRO_MAIN_GWENESIS_ONLY OR HOLO_RETRO_GWENESIS_ONLY)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -flto=auto
        -ffat-lto-objects
        -flto-compression-level=9
    )
endif()
