set(COMPONENT_SRCDIRS "src/bus src/cpus/M68K src/cpus/Z80 src/io src/savestate src/sound src/vdp")
set(COMPONENT_ADD_INCLUDEDIRS ". src/bus src/cpus/M68K src/cpus/Z80 src/io src/savestate src/sound src/vdp")
set(COMPONENT_REQUIRES "retro-go")
register_component()
rg_setup_compile_options(
    -O3
    -fjump-tables
    -ftree-switch-conversion
    -fbuiltin
    -fomit-frame-pointer
    -fno-math-errno
    -Wno-incompatible-pointer-types
    -DROM_DATA=gwenesis_ROM_DATA
)
