# Lyren

Lyren（弦间）是面向 Wayland 桌面的 C++26 音乐播放器，使用 Vulkan 绘制窗口和界面。音频通过 PipeWire 输出。

## 功能

平台适配模块支持网易云音乐、QQ 音乐，通过共享界面提供搜索、播放和音乐资料浏览。

支持 MP3、FLAC、AAC 和 Vorbis 解码、渐进下载、进度跳转及音量控制。播放地址与可用音质由平台响应决定。

通过 MPRIS 接入桌面媒体控制和媒体键。桌面歌词使用独立的 Wayland 表面，可调整位置、锁定和切换显示模式。

## 运行环境

需要支持 C++26 的 GCC 16 工具链、CMake 4.0 或更新版本、Ninja 和 `glslc`。运行环境需提供 Wayland 会话和 Vulkan 驱动。播放需要 PipeWire 服务。

系统库由 `pkg-config` 检测；完整依赖和固定下载版本见 [CMakeLists.txt](CMakeLists.txt) 与 [cmake/Deps.cmake](cmake/Deps.cmake)。

## 构建和运行

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lyren
```

CMake 可选参数：

| 参数 | 作用 |
|---|---|
| `-DLYREN_TEST_GRAPHICS=ON` | 启用 Wayland / Vulkan 集成测试，需要 `kwin_wayland` |

执行已启用的测试：

```sh
ctest --test-dir build --output-on-failure
```

## 配置与数据

首次运行会生成 `~/.config/lyren/config.toml`。配置项及默认值见 [default-config.toml](assets/default-config.toml)；配置目录遵循 `XDG_CONFIG_HOME`。

播放状态保存在 `~/.local/state/lyren/state.json`，状态目录遵循 `XDG_STATE_HOME`。

账号凭据通过 systemd 用户凭据接口加密保存。各平台会话独立管理。

## 开发

`./build/lyren --debug` 启用调试日志。

`--script FILE` 重放应用内输入并支持截图。回放使用独立的会话总线及 XDG 目录：

```sh
dbus-run-session -- env \
  XDG_CONFIG_HOME="$PWD/.scratch/config" \
  XDG_STATE_HOME="$PWD/.scratch/state" \
  XDG_CACHE_HOME="$PWD/.scratch/cache" \
  ./build/lyren --script .scratch/session.txt
```

`lyren-probe` 提供接口诊断和音频检查；命令及参数见 [lyren-probe.cpp](tools/lyren-probe.cpp)。

## 项目结构

| 目录 | 职责 |
|---|---|
| [src/core/](src/core/) | 事件循环、协程和基础类型 |
| [src/wl/](src/wl/) | Wayland 窗口与输入 |
| [src/gfx/](src/gfx/) | Vulkan 渲染与图像处理 |
| [src/text/](src/text/) | 字体、文字整形和字形缓存 |
| [src/ui/](src/ui/) | 界面组件、主题与动效 |
| [src/net/](src/net/) | HTTP 请求与下载 |
| [src/audio/](src/audio/) | 缓冲、音频解码与 PipeWire 输出 |
| [src/sys/](src/sys/) | 配置、凭据与桌面服务 |
| [src/app/](src/app/) | 应用状态、导航与页面 |

## 文档

- [QQ 音乐接口规范](docs/qqmusic-api.md)
