# Holo Retro-Go

这是一个把 [retro-go](https://github.com/ducalex/retro-go) 移植到 Clocteck Holocubic / cubic Lua 固件动态模块环境的项目。它是在 Retro-Go 源码基础上增加 Holocubic 宿主 ABI、ESP-ELFLoader 模块入口和少量适配层，最终构建为可由 Lua 加载的 `retrogo.so`。

## 项目用途

本项目用于在支持 cubic Lua 动态模块 ABI 的 Holocubic 固件中运行 Retro-Go launcher 和部分 Retro-Go 模拟器核心。

当前默认移植目标：

- 产物：`src/build/retrogo.so`
- 仓库内设备运行包：`package/`
- 当前运行包模块快照：`package/modules/retrogo.so`
- 模块加载路径：当前 app 目录下的 `modules`，例如 `/sd/apps/retro-go/modules/retrogo.so`
- ROM 根目录：当前 app 目录下的 `roms`，例如 `/sd/apps/retro-go/roms`
- ROM 上传页面：当前 app 的 WebUI 路由，例如 `/retro-go/`


默认编译的 Retro-Go core 包括 NES、GB/GBC、SMS/GG/COL、PCE 和 Game & Watch。Lynx、SNES、网络、netplay、WiFi、HTTP 相关功能暂未作为第一版默认目标。

## 编译流程

需要先准备 ESP-IDF，并确保目标固件对应的 `module_abi.h` 可用 (现在只能用于clocteck holocubic产品)。仓库内已带一份当前 ABI。

PowerShell 示例：

```powershell
# 在 holo-retro-go/src 目录执行
$env:CUBICLUA_ROOT="..\cubic_lua\cubic_arduino\cubic-develop"
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

不使用 `CUBICLUA_ROOT` 时可以直接传入 ABI 目录：

```powershell
idf.py "-DMODULE_ABI_DIR=..\cubic_lua\cubic_arduino\cubic-develop\src\dynmod" build
```

需要确认 ESP-ELFLoader 的 shared object 动态加载选项已启用：

```text
CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT=y
```

成功后生成：

```text
src/build/retrogo.so
```

仓库中也保留了一份当前编译好的模块：

```text
package/modules/retrogo.so
```

## 实现说明

- `src/upstream/retro-go` 保存官方 Retro-Go 源码。
- `src/main/retrogo_module.c` 提供动态模块导出入口：`module_query_v1`、`module_create_v1`、`module_luaopen_v1`、`module_destroy_v1`。
- `src/main/holo_*.c` 提供 Holocubic 宿主 API 到 Retro-Go 运行环境的兼容层。
- Lua app 负责扫描当前 app 目录下的 `roms`（例如 `/sd/apps/retro-go/roms`）并把 catalog 注入模块，模块侧的 Retro-Go storage 接口再读取这个虚拟目录列表。Lua app 还注册了一个简单 WebUI，可通过浏览器上传 ROM 到该目录。
- 当前显示和音频仍是第一阶段可编译 stub，后续需要接入宿主 RGB565 DMA 推屏和 int16 stereo 音频。

## 致谢

感谢 [ducalex/retro-go](https://github.com/ducalex/retro-go) 提供优秀的 ESP32 Retro-Go launcher、模拟器核心和移植基础。本项目是在 Retro-Go 基础上做的 Holocubic / cubic Lua 动态模块移植，核心代码和上游组件保留在 `upstream/retro-go`。

## License

本项目作为 Retro-Go 移植项目按 GPLv2 开源，见 [LICENSE](LICENSE)。`src/upstream/retro-go` 内的上游源码和第三方组件保留各自原始版权声明和许可证说明。
 
