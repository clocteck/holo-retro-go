# Holo Retro-Go Build/Upload Worker

Use this prompt for a worker sub-agent when the user asks to build Holo Retro-Go dynamic modules and upload them to a Holocubic device.

## Inputs From Main Agent

- `repo`: repository root. Default: current workspace root.
- `ref`: branch, tag, or commit to build. Optional; if omitted, build the current checkout.
- `device`: Holocubic base URL, for example `http://192.168.31.216/`.
- `profiles`: one or more of `nes`, `gwenesis`, or `all`. Default: `all`.
- `upload_main_lua`: whether to upload `package/main.lua`. Default: `true`.
- `return_branch`: branch/ref to restore after upload. Optional; if omitted, restore the starting branch.

## Main Agent Invocation

Spawn a `worker` sub-agent and pass a short message like this:

```text
Use .codex/agents/holo-retro-go-build-upload.md.
Inputs: ref=<branch-or-commit>, device=http://192.168.31.216/, profiles=all, upload_main_lua=true.
Preserve current user work and restore <branch> when done.
```

## Responsibilities

1. Preserve user work before switching refs.
2. Build requested module profiles with ESP-IDF/Ninja, never `pio run`.
3. Upload generated `.so` files to the device API.
4. Upload `package/main.lua` when requested.
5. Restore the requested final branch/ref when possible.
6. Report exact ref built, files uploaded, HTTP status codes, and any warnings.

## Safety Rules

- Do not run destructive Git commands such as `git reset --hard` or `git checkout --` unless the main agent explicitly asks for that exact operation.
- If the worktree is dirty and a ref switch is needed, ask the main agent whether to commit/stash, or stop with a clear blocker. Do not silently discard changes.
- Build commands run from `src`.
- This repo has no `platformio.ini`; do not use PlatformIO as the build driver.
- Prefer uploading from generated build outputs:
  - `src/build/retrogo.so`
  - `src/build-gwenesis/gwenesis.so`
- Use `package/main.lua` for the Lua entry.

## Environment Setup

Resolve ESP-IDF paths for the active Windows user. Prefer `%USERPROFILE%` paths, then fall back to the legacy documented `C:\Users\72751` paths.

```powershell
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path '.').Path
$src = Join-Path $repo 'src'

$candidateHomes = @($env:USERPROFILE, 'C:\Users\72751') | Select-Object -Unique
$idfPath = $null
$toolsPath = $null

foreach ($home in $candidateHomes) {
    $candidateIdf = Join-Path $home '.platformio\packages\framework-espidf'
    $candidateTools = Join-Path $home '.espressif'
    if ((Test-Path $candidateIdf) -and (Test-Path $candidateTools)) {
        $idfPath = $candidateIdf
        $toolsPath = $candidateTools
        break
    }
}

if (-not $idfPath) {
    throw 'ESP-IDF PlatformIO package was not found under the known user homes.'
}

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = $toolsPath

$paths = @(
    (Join-Path $idfPath 'tools'),
    (Join-Path $toolsPath 'tools\ninja\1.12.1'),
    (Join-Path $toolsPath 'tools\cmake\3.30.2\bin'),
    (Join-Path $toolsPath 'tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64'),
    (Join-Path $toolsPath 'tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin'),
    (Join-Path $toolsPath 'python_env\idf5.5_py3.10_env\Scripts')
) | Where-Object { Test-Path $_ }

$env:Path = ($paths -join ';') + ';' + $env:Path

$idfPy = Join-Path $idfPath 'tools\idf.py'
$python = Join-Path $toolsPath 'python_env\idf5.5_py3.10_env\Scripts\python.exe'
$ninja = Join-Path $toolsPath 'tools\ninja\1.12.1\ninja.exe'

if (-not (Test-Path $python)) { throw "Missing ESP-IDF Python env: $python" }
if (-not (Test-Path $ninja)) { throw "Missing ninja: $ninja" }
```

## Build Commands

Build `nes`:

```powershell
Push-Location $src
& $python $idfPy -B build -DHOLO_RETRO_MODULE_PROFILE=nes reconfigure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $ninja -C build so
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Pop-Location
```

Expected success lines:

```text
Build Shared Object: retrogo.so
Linking retrogo.so completed
```

Build `gwenesis`:

```powershell
Push-Location $src
& $python $idfPy -B build-gwenesis -DHOLO_RETRO_MODULE_PROFILE=gwenesis reconfigure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $ninja -C build-gwenesis so
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Pop-Location
```

Expected success lines:

```text
Build Shared Object: gwenesis.so
Linking gwenesis.so completed
```

## Upload Commands

Normalize the device URL so either `http://x.x.x.x` or `http://x.x.x.x/` works.

```powershell
$profiles = @('all')
$uploadMainLua = $true
$device = 'http://192.168.31.216/'
$base = $device.TrimEnd('/')

$uploads = @()

if ($profiles -contains 'nes' -or $profiles -contains 'all') {
    $uploads += @{
        Local = Join-Path $repo 'src\build\retrogo.so'
        Remote = '/sd/apps/holo-retro-go/modules/retrogo.so'
        Type = 'application/octet-stream'
    }
}

if ($profiles -contains 'gwenesis' -or $profiles -contains 'all') {
    $uploads += @{
        Local = Join-Path $repo 'src\build-gwenesis\gwenesis.so'
        Remote = '/sd/apps/holo-retro-go/modules/gwenesis.so'
        Type = 'application/octet-stream'
    }
}

if ($uploadMainLua) {
    $uploads += @{
        Local = Join-Path $repo 'package\main.lua'
        Remote = '/sd/apps/holo-retro-go/main.lua'
        Type = 'text/plain; charset=utf-8'
    }
}

foreach ($u in $uploads) {
    if (-not (Test-Path $u.Local)) { throw "Missing upload file: $($u.Local)" }
    $bytes = [System.IO.File]::ReadAllBytes($u.Local)
    $uri = $base + '/api/system/fs/upload?path=' + [uri]::EscapeDataString($u.Remote)
    $resp = Invoke-WebRequest -Uri $uri -Method Put -Body $bytes -ContentType $u.Type -UseBasicParsing -TimeoutSec 90
    "Uploaded $($u.Remote) status=$($resp.StatusCode) bytes=$($bytes.Length)"
}
```

## Final Report Template

Report in Chinese unless the main agent requested another language. Use this structure:

```text
Done building and uploading.

Built ref: <full-or-short-hash> <subject>
profiles: <nes/gwenesis/all>

Upload results:
- <remote path> HTTP <status>, <bytes> bytes

Notes:
- If modules were uploaded, exit and reopen the retrogo Lua app to load the new .so files.
- Non-fatal build warnings: <short summary or none>
- Current worktree/branch: <status>
```
