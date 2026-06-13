# Holo Retro-Go Agent Notes

This project builds Retro-Go based Holocubic Lua dynamic modules.

## Module Profiles

Two module profiles are built separately, matching the upstream Retro-Go split:

- `retro-core`: classic emulator collection. Output: `build/retrogo.so`.
  Includes NES, SNES, GB/GBC, Game & Watch, SMS/GG, ColecoVision, PCE, Lynx, and related Retro-Go cores. This profile excludes MD/gwenesis.
- `gwenesis`: Sega Mega Drive / Genesis only. Output: `build-gwenesis/gwenesis.so`.

Device module paths:

```text
/sd/modules/retrogo.so
/sd/modules/gwenesis.so
```

Lua app entry path on the device:

```text
/sd/apps/retrogo/main.lua
```

## Build Environment

- Target chip: ESP32-S3
- ESP-IDF path: `C:\Users\wzh\Documents\nodemcu-firmware\sdk\esp32-esp-idf`
- Default Retro-Go build dir: `build`
- Gwenesis-only build dir: `build-gwenesis`
- Shared object target: `so`

PowerShell may block `export.ps1`, so use `-ExecutionPolicy Bypass` for one-shot build commands.

## Build retro-core

Reconfigure when CMake files or profile selection changed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Users\wzh\Documents\nodemcu-firmware\sdk\esp32-esp-idf\export.ps1'; idf.py -B build -DHOLO_RETRO_MODULE_PROFILE=retro-core reconfigure; ninja -C build so"
```

For incremental rebuilds after the build dir is already configured:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Users\wzh\Documents\nodemcu-firmware\sdk\esp32-esp-idf\export.ps1'; ninja -C build so"
```

Expected output:

```text
Build Shared Object: retrogo.so
Linking retrogo.so completed
```

## Build gwenesis

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Users\wzh\Documents\nodemcu-firmware\sdk\esp32-esp-idf\export.ps1'; idf.py -B build-gwenesis -DHOLO_RETRO_MODULE_PROFILE=gwenesis reconfigure; ninja -C build-gwenesis so"
```

Expected output:

```text
Build Shared Object: gwenesis.so
Linking gwenesis.so completed
```

## Upload To Device

Current device address:

```text
http://192.168.31.197/
```

Upload `retrogo.so`:

```powershell
$path='C:\Users\wzh\Documents\PlatformIO\Projects\holo-retro-go\build\retrogo.so'
$bytes=[System.IO.File]::ReadAllBytes($path)
$uri='http://192.168.31.197/api/system/fs/upload?path=' + [uri]::EscapeDataString('/sd/modules/retrogo.so')
Invoke-WebRequest -Uri $uri -Method Put -Body $bytes -ContentType 'application/octet-stream' -UseBasicParsing -TimeoutSec 90
```

Upload `gwenesis.so`:

```powershell
$path='C:\Users\wzh\Documents\PlatformIO\Projects\holo-retro-go\build-gwenesis\gwenesis.so'
$bytes=[System.IO.File]::ReadAllBytes($path)
$uri='http://192.168.31.197/api/system/fs/upload?path=' + [uri]::EscapeDataString('/sd/modules/gwenesis.so')
Invoke-WebRequest -Uri $uri -Method Put -Body $bytes -ContentType 'application/octet-stream' -UseBasicParsing -TimeoutSec 90
```

Upload the Lua app entry:

```powershell
$path='C:\Users\wzh\Documents\PlatformIO\Projects\holo-retro-go\retrogo_main.lua'
$bytes=[System.IO.File]::ReadAllBytes($path)
$uri='http://192.168.31.197/api/system/fs/upload?path=' + [uri]::EscapeDataString('/sd/apps/retrogo/main.lua')
Invoke-WebRequest -Uri $uri -Method Put -Body $bytes -ContentType 'text/plain; charset=utf-8' -UseBasicParsing -TimeoutSec 90
```

After uploading, exit and reopen the retrogo Lua app so the host loads the new module files.

## Notes

- `sdkconfig` and `sdkconfig.defaults` currently target ESP32-S3 at 240 MHz, but a dynamic module follows the host firmware's actual runtime CPU frequency.
- CMake writes `build*/holo_retro_profile.cmake` during configure. It is generated state for component-scope profile selection; do not edit it by hand.
- Dynamic modules currently keep code out of `.mod_iram`. The host ELF loader can map small `.mod_iram` functions, but it does not rewrite already-linked Xtensa direct call instructions; moving gwenesis CPU/bus/Z80/audio/VDP code there caused `IllegalInstruction` crashes. Prefer internal RAM for data buffers, not dynamic-module code, unless the call path is proven relocation-safe.
- Gwenesis also allocates hot VDP state/buffers with `MEM_FAST`: CRAM, CRAM565, VSRAM, SAT cache, VDP registers/FIFO, and the per-line plane/sprite render buffers. These are released by the MD cleanup path.
- Gwenesis Z80 RAM is dynamically allocated as `ZRAM` in `MEM_FAST`; `gwenesis_bus_init_fast_ram()` rebinds it with `z80_set_memory()`, and `RdZ80/WrZ80` guard against a missing pointer to avoid null+offset crashes.
- Gwenesis keeps the 64KB M68K RAM in `MEM_SLOW` because it is too large for the remaining internal RAM budget on Holo dynmod builds. Prefer keeping smaller hot blocks in `MEM_FAST`: Z80 RAM, VDP registers/CRAM/SAT/fifo/line buffers, YM/SN source buffers, the mixed audio buffer, and the audio ring.
- Gwenesis audio uses `MEM_FAST` for the YM2612/SN76489 source buffers, a mixed stereo buffer, and a 2048-frame audio ring. A `gwen_audio` task pinned to Core 0 drains that ring through Retro-Go audio so host audio pacing does not block the MD emulation loop. SN76489 stays off by default and the YM-only path bypasses the mixer for performance; enable SN76489 only when PSG accuracy matters more than speed.
- Gwenesis currently uses fixed MD frameskip: draw 1 frame, then skip 2 frames. The Retro-Go global system-monitor frameskip is disabled for this core so it cannot overwrite the local policy.
- Prefer compatibility fixes in upstream cores. Avoid ROM-specific hacks unless explicitly requested.
