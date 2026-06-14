set(COMPONENT_SRCDIRS "src/bus src/cpus/M68K src/cpus/Z80 src/io src/savestate src/sound src/vdp")
set(COMPONENT_ADD_INCLUDEDIRS ". src/bus src/cpus/M68K src/cpus/Z80 src/io src/savestate src/sound src/vdp")
set(COMPONENT_REQUIRES "retro-go")
register_component()
rg_setup_compile_options(
    -O3
    -fbuiltin
    -fomit-frame-pointer
    -fno-math-errno
    -Wno-incompatible-pointer-types
    -DBUILD_TABLES
    -DROM_DATA=gwenesis_ROM_DATA
)

set_source_files_properties("src/cpus/Z80/Z80.c"
    PROPERTIES COMPILE_OPTIONS "-Ofast;-fjump-tables;-ftree-switch-conversion")
set_source_files_properties("src/cpus/M68K/m68kcpu.c"
    PROPERTIES COMPILE_OPTIONS "-Ofast")

if(HOLO_RETRO_GWENESIS_ONLY)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -flto=auto
        -ffat-lto-objects
        -flto-compression-level=9
    )
endif()
