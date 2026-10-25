# Lyren

Lyren（弦间）是面向 Wayland 桌面的 C++26 音乐播放器，使用 Vulkan 绘制窗口和界面。

## 运行环境

需要支持 C++26 的 GCC 16 工具链、CMake 4.0 或更新版本、Ninja 和 `glslc`。运行环境需提供 Wayland 会话和 Vulkan 驱动。

系统库由 `pkg-config` 检测；完整依赖和固定下载版本见 [CMakeLists.txt](CMakeLists.txt) 与 [cmake/Deps.cmake](cmake/Deps.cmake)。

## 构建和运行

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lyren
```

执行已启用的测试：

```sh
ctest --test-dir build --output-on-failure
```

## 配置与数据

首次运行会生成 `~/.config/lyren/config.toml`。配置项及默认值见 [default-config.toml](assets/default-config.toml)；配置目录遵循 `XDG_CONFIG_HOME`。

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

## 项目结构

| 目录 | 职责 |
|---|---|
| [src/core/](src/core/) | 事件循环、协程和基础类型 |
| [src/wl/](src/wl/) | Wayland 窗口与输入 |
| [src/gfx/](src/gfx/) | Vulkan 渲染与图像处理 |
| [src/text/](src/text/) | 字体、文字整形和字形缓存 |
| [src/ui/](src/ui/) | 界面组件、主题与动效 |
| [src/sys/](src/sys/) | 配置、凭据与桌面服务 |
| [src/app/](src/app/) | 应用状态、导航与页面 |

## 文档

- [QQ 音乐接口规范](docs/qqmusic-api.md)
