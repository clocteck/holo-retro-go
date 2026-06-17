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
- Workspace path: `E:\cubicsrc\holo-retro-go`
- PlatformIO Core: `C:\Users\72751\.platformio\penv\Scripts\pio.exe`
- ESP-IDF path: `C:\Users\72751\.platformio\packages\framework-espidf`
- ESP-IDF tools path: `C:\Users\72751\.espressif`
- `idf.py` path: `C:\Users\72751\.platformio\packages\framework-espidf\tools`
- `ninja` path: `C:\Users\72751\.espressif\tools\ninja\1.12.1`
- `ccache` path: `C:\Users\72751\.espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64`
- `cmake` path: `C:\Users\72751\.espressif\tools\cmake\3.30.2\bin`
- Xtensa toolchain path: `C:\Users\72751\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin`
- Default Retro-Go build dir: `build`
- Gwenesis-only build dir: `build-gwenesis`
- Shared object target: `so`

This repository is not a standalone PlatformIO project and has no
`platformio.ini`; do not use `pio run` from this directory. PlatformIO provides
the ESP-IDF package and toolchain, but the module build is still driven by
`idf.py`/`ninja`.

The current user environment should include these paths in `Path`, plus
`IDF_PATH=C:\Users\72751\.platformio\packages\framework-espidf` and
`IDF_TOOLS_PATH=C:\Users\72751\.espressif`. If a fresh shell cannot find
`idf.py`, `ninja`, `ccache`, `cmake`, or `xtensa-esp32s3-elf-gcc`, add the
paths above to `Path` for that shell.

## Build retro-core

Reconfigure when CMake files or profile selection changed:

```powershell
powershell -NoProfile -Command "$env:IDF_PATH='C:\Users\72751\.platformio\packages\framework-espidf'; $env:IDF_TOOLS_PATH='C:\Users\72751\.espressif'; idf.py -B build -DHOLO_RETRO_MODULE_PROFILE=retro-core reconfigure; ninja -C build so"
```

For incremental rebuilds after the build dir is already configured:

```powershell
powershell -NoProfile -Command "ninja -C build so"
```

Expected output:

```text
Build Shared Object: retrogo.so
Linking retrogo.so completed
```

## Build gwenesis

```powershell
powershell -NoProfile -Command "$env:IDF_PATH='C:\Users\72751\.platformio\packages\framework-espidf'; $env:IDF_TOOLS_PATH='C:\Users\72751\.espressif'; idf.py -B build-gwenesis -DHOLO_RETRO_MODULE_PROFILE=gwenesis reconfigure; ninja -C build-gwenesis so"
```

For incremental rebuilds after `build-gwenesis` is already configured:

```powershell
powershell -NoProfile -Command "ninja -C build-gwenesis so"
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
$path='E:\cubicsrc\holo-retro-go\build\retrogo.so'
$bytes=[System.IO.File]::ReadAllBytes($path)
$uri='http://192.168.31.197/api/system/fs/upload?path=' + [uri]::EscapeDataString('/sd/modules/retrogo.so')
Invoke-WebRequest -Uri $uri -Method Put -Body $bytes -ContentType 'application/octet-stream' -UseBasicParsing -TimeoutSec 90
```

Upload `gwenesis.so`:

```powershell
$path='E:\cubicsrc\holo-retro-go\build-gwenesis\gwenesis.so'
$bytes=[System.IO.File]::ReadAllBytes($path)
$uri='http://192.168.31.197/api/system/fs/upload?path=' + [uri]::EscapeDataString('/sd/modules/gwenesis.so')
Invoke-WebRequest -Uri $uri -Method Put -Body $bytes -ContentType 'application/octet-stream' -UseBasicParsing -TimeoutSec 90
```

Upload the Lua app entry:

```powershell
$path='E:\cubicsrc\holo-retro-go\retrogo_main.lua'
$bytes=[System.IO.File]::ReadAllBytes($path)
$uri='http://192.168.31.197/api/system/fs/upload?path=' + [uri]::EscapeDataString('/sd/apps/retrogo/main.lua')
Invoke-WebRequest -Uri $uri -Method Put -Body $bytes -ContentType 'text/plain; charset=utf-8' -UseBasicParsing -TimeoutSec 90
```

After uploading, exit and reopen the retrogo Lua app so the host loads the new module files.

## Dynamic Module IRAM

The dynmod host loader only treats executable sections named `.mod_iram` or
`.mod_iram.*` as module hot code. Plain ESP-IDF `IRAM_ATTR` usually emits
`.iram1*`, which is not enough by itself.

For non-gwenesis experiments, a tiny leaf function can be placed in
`.mod_iram` with an explicit section attribute, then verified with `objdump`:

```c
#define HOLO_MOD_IRAM __attribute__((section(".mod_iram"), noinline, noclone, used, aligned(4)))

HOLO_MOD_IRAM void small_hot_function(void)
{
    ...
}
```

Existing function sections can also be collected with a linker script passed
through `ELF_SO_LINK_FLAGS`, but only after proving the call graph and literal
layout are safe. Use `INSERT BEFORE .text` when collecting existing `.text.*`
sections so the default `.text` rule does not consume them first:

```ld
SECTIONS
{
  .mod_iram ALIGN(4) :
  {
    *(.mod_iram .mod_iram.*)
    *(.literal.small_hot_function*)
    *(.text.small_hot_function*)
  }
}
INSERT BEFORE .text;
```

For the linker-script method, Xtensa `.literal.*` pools must stay reachable from
their matching `.text.*` code. Multiple `.mod_iram.*` input sections must end up
contiguous in the final shared object, so prefer collecting them into one
`.mod_iram` output section.

Do not use the linker-script collection method for gwenesis hot CPU/bus/Z80/YM
helpers in the current loader. A tested build moved `RdZ80`, `WrZ80`,
`z80_write_ctrl`, M68K memory helpers, and `YM2612Write` into `.mod_iram`; the
device crashed with `IllegalInstruction`, with the PC landing in the `.mod_iram`
literal pool. `-mlongcalls` did not make the build safe.

The root hazard is that the host loader can copy `.mod_iram` and relocate
data-held addresses, but it does not rewrite already-linked Xtensa direct call
instructions. Moving a callee to `.mod_iram` while callers stay in `.text` can
therefore jump to the wrong runtime address. For gwenesis, keep code in `.text`
and spend internal RAM on hot data buffers unless the loader is changed to
handle these relocations or the moved code is a fully isolated, proven-safe call
island. Do not blindly redefine `GWENESIS_HOT` to `.mod_iram`.

After changing CMake, linker scripts, or section placement, reconfigure and
verify the final shared object:

```powershell
xtensa-esp32s3-elf-objdump -h build-gwenesis/gwenesis.so | Select-String mod_iram
rg "\.mod_iram|small_hot_function" build-gwenesis/gwenesis_so.map
```

## Notes

- `sdkconfig` and `sdkconfig.defaults` currently target ESP32-S3 at 240 MHz, but a dynamic module follows the host firmware's actual runtime CPU frequency.
- CMake writes `build*/holo_retro_profile.cmake` during configure. It is generated state for component-scope profile selection; do not edit it by hand.
- Dynamic modules currently keep code out of `.mod_iram`. The host ELF loader can map small `.mod_iram` functions, but it does not rewrite already-linked Xtensa direct call instructions; moving gwenesis CPU/bus/Z80/audio/VDP code there caused `IllegalInstruction` crashes. Prefer internal RAM for data buffers, not dynamic-module code, unless the call path is proven relocation-safe.
- Gwenesis also allocates hot VDP state/buffers with `MEM_FAST`: CRAM, CRAM565, VSRAM, SAT cache, VDP registers/FIFO, and the per-line plane/sprite render buffers. These are released by the MD cleanup path.
- Gwenesis Z80 RAM is dynamically allocated as `ZRAM` in `MEM_FAST`; `gwenesis_bus_init_fast_ram()` rebinds it with `z80_set_memory()`, and `RdZ80/WrZ80` guard against a missing pointer to avoid null+offset crashes.
- Gwenesis keeps the 64KB M68K RAM in `MEM_SLOW` because it is too large for the remaining internal RAM budget on Holo dynmod builds. Prefer keeping smaller hot blocks in `MEM_FAST`: Z80 RAM, VDP registers/CRAM/SAT/fifo/line buffers, YM/SN source buffers, the mixed audio buffer, and the audio ring.
- Gwenesis audio uses `MEM_FAST` for the YM2612/SN76489 source buffers, a mixed stereo buffer, and a 2048-frame audio ring. Current Holo dynmod experiment pins async VDP, `rg_display`, `gwen_audio`/YM compute, and `gwen_aout` host-audio drain to Core 0 (`ymc:0`) to keep main emulation on Core 1. VDP async uses `GWENESIS_VDP_ASYNC_JOBS=2` and a latest-frame/drop policy: no READY/RENDERING backlog, no hot-path sync fallback, and no waiting for CPU0 to go idle. SN76489 stays off by default and the YM-only path bypasses the mixer for performance; enable SN76489 only when PSG accuracy matters more than speed.
- Gwenesis currently uses fixed MD frameskip: draw 1 frame, then skip 2 frames. The Retro-Go global system-monitor frameskip is disabled for this core so it cannot overwrite the local policy.
- Prefer compatibility fixes in upstream cores. Avoid ROM-specific hacks unless explicitly requested.
- 不用管非 Holo dynmod 构建
