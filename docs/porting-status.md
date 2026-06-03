# Porting Status

The port is built from upstream Retro-Go source, not a rewritten emulator core.

Default compiled cores:

- NES: `nofrendo`
- GB/GBC: `gnuboy`
- SMS/GG/COL: `smsplus`
- PCE: `pce-go`
- GW: `gw-emulator`

Lynx and SNES are intentionally left out of the first default build.

Official Retro-Go launcher sources are included in the module build:

- `launcher/main/main.c`
- `applications.c`
- `bookmarks.c`
- `browser.c`
- `gui.c`
- `images.c`

For `RG_TARGET_HOLO_DYNMOD`, `retro-core/main/main_launcher.c` enters the official launcher main loop and enables `browser_init()`.

The first ABI workaround is a Lua-provided catalog. The host scans `/sd/roms`, serializes a flat list, and passes it to `retrogo.set_catalog(...)`. Retro-Go launcher/core code still calls `rg_storage_scandir()`, but for `RG_TARGET_HOLO_DYNMOD` that function reads the injected catalog.

The current build is compile-safe and does not retain unresolved `opendir`, `readdir`, `closedir`, ROM I2C clock-control, WiFi, HTTP, or Retro-Go internal symbols at the module boundary. For the Holo target, networking/netplay sources and `esp_wifi`/`esp_http_client` build dependencies are disabled. Display and audio are still target stubs; host RGB565 DMA and int16 stereo audio drivers remain to be wired.
