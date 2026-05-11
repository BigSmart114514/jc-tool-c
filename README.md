# jc-tool-c

一个基于 C++/Qt 的远程桌面工具，支持屏幕传输与基础远程控制能力。

## 功能特性

- 支持基于 P2P 网络进行远程连接（依赖 EasyTier）。
- 支持屏幕画面传输。
- 支持鼠标操作。
- 支持基础键盘操作。
- 使用 H.265（HEVC）进行视频编码，在较低带宽下具备较好的传输效率。

## 依赖环境

- Windows（主要开发与构建目标平台）
- CMake 3.16+
- Qt6（Core / Gui / Widgets / Network）
- FFmpeg（需提供 include / lib / bin）
- EasyTier FFI 动态库（`easytier_ffi`）
- MSVC（GitHub Actions 使用 Visual Studio 2022 工具链）

## 构建说明

### 方式一：本地 `build.bat`

1. 安装并配置 Qt6。
2. 准备 FFmpeg 目录（默认 `../ffmpeg`）。
3. 准备 EasyTier 目录（默认 `../easytier`）。
4. 设置环境变量 `QT6_DIR` 指向 Qt6 安装目录。
5. 在项目根目录运行 `build.bat`。

### 方式二：GitHub Actions

仓库已提供 Windows 构建工作流，可自动下载依赖并完成编译打包：

- `.github/workflows/build.yml`

此外还提供 EasyTier FFI 产物构建工作流：

- `.github/workflows/easytier.yml`

## 目录结构

- `app/`：程序入口
- `client/`：客户端界面与解码相关逻辑
- `server/`：服务端采集、编码与服务逻辑
- `common/`：传输与公共模块

## 免责声明

本项目仅用于技术学习与研究，请勿用于任何非法用途。
