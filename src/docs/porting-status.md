# Porting Status

The port is built from upstream Retro-Go source, not a rewritten emulator core.

Holo dynmod compiled modules:

- Classic core module (`retrogo.so`): NES `nofrendo`, GB/GBC `gnuboy`, SMS/GG/COL `smsplus`, PCE `pce-go`, GW `gw-emulator`, Lynx `handy`, SNES `snes9x`
- MD module (`gwenesis.so`): `gwenesis`

Official Retro-Go launcher sources are included in the module build:

- `launcher/main/main.c`
- `applications.c`
- `bookmarks.c`
- `browser.c`
- `gui.c`
- `images.c`

For the Holo dynmod build, `retro-core/main/main_launcher.c` enters the official launcher main loop. The `retrogo.so` module registers the classic Retro-Go core package, while `gwenesis.so` registers only MD.

The Holo dynmod build scans ROM and save directories directly through the host firmware's exported POSIX `opendir`, `readdir`, and `closedir` symbols. Lua no longer pre-scans the ROM tree or injects an in-memory catalog. The Lua WebUI keeps a separate ROM list cache and refreshes it only when its list endpoint is requested.

The current build intentionally retains unresolved `opendir`, `readdir`, and `closedir` references at the module boundary; the dynmod firmware exports those ESP-IDF VFS functions. For the Holo target, networking/netplay sources and `esp_wifi`/`esp_http_client` build dependencies are disabled.
