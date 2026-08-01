<p align="center">
  <img src="src/host/logo.svg" width="88" alt="Air Screenshot">
</p>

<h1 align="center">Air Screenshot</h1>

<p align="center">轻量、原生、可靠的 Windows 截图工具。</p>

<p align="center">
  <a href="https://mmm1h.github.io/air-screenshot/">下载</a> ·
  <a href="docs/user-guide.md">使用指南</a> ·
  <a href="docs/feature-parity.md">功能对照</a>
</p>

<p align="center">Windows 10 2004+ · x64 · 便携单文件</p>

## 核心能力

| | 能力 |
| --- | --- |
| 截图 | 区域、窗口、控件、显示器、全桌面与滚动截图；支持多屏和混合 DPI |
| 标注 | 形状、箭头、画笔、文字、序号、高亮、马赛克、模糊、实心遮挡与双模式橡皮 |
| OCR | Rust + ONNX 本地识别，无 Python worker；支持极速、高精度和兼容模式 |
| 贴图 | 从截图、剪贴板或本地图片创建贴图，支持缩放、透明度、旋转、翻转和滤镜 |
| 输出 | 复制到剪贴板或保存 PNG；圆角截图保留透明像素 |
| 系统集成 | 全局快捷键、托盘、开机启动、浅色/深色/高对比度和安全自动更新 |

Air Screenshot 以原生 Win32、Direct2D 和 DirectWrite 构建。截图、OCR 或输出失败时，当前选区和标注会保留，可直接重试。

## 快速开始

1. 从[下载页](https://mmm1h.github.io/air-screenshot/)获取 `AirScreenshot.exe`，放到普通可写目录后双击运行。
2. 按 `Ctrl + Alt + A`，拖动选择区域；悬停窗口或控件时会自动识别边界。
3. 使用工具栏标注、滚动截图、贴图或 OCR。
4. 按 `Enter` 完成，或用 `Ctrl + C` 复制、`Ctrl + S` 保存。

程序无需安装和管理员权限。首次运行若出现 SmartScreen 提示，请确认文件来自本项目下载页。

## 常用快捷键

| 快捷键 | 操作 |
| --- | --- |
| `Ctrl + Alt + A` | 开始区域截图 |
| `Enter` / `Ctrl + C` | 完成并复制 |
| `Ctrl + S` | 保存 PNG |
| `Esc` | 退出当前操作；再次按下取消截图 |
| `F2` | 精确输入 `X / Y / W / H`、比例和圆角 |
| `F5` | 刷新底图，保留选区与标注 |
| `Shift + C` | 识别当前选区文字 |
| `Ctrl + Z` / `Ctrl + Y` | 撤销 / 重做 |
| 方向键 | 像素级移动；配合 `Ctrl` / `Shift` 调整选区 |

快捷键、工具顺序和可见性都可在设置中修改。完整鼠标与键盘操作见[使用指南](docs/user-guide.md)。

## 本地 OCR

OCR 首次使用时会自动下载并验证所需组件，完成后可离线使用。识别在独立进程中运行，可随时按 `Esc` 取消，不会让模型常驻托盘进程。

| 模式 | 适合场景 |
| --- | --- |
| 极速 | 日常横排文字，优先响应速度 |
| 高精度 | 小字、复杂背景或较低置信度结果 |
| 兼容 | 需要旧模型行为的图片 |

截图内 OCR 会打开可选择、可滚动的结果面板，并可直接切换模式重试。

## 命令行

主程序同时提供 CLI：

```powershell
AirScreenshot.exe capture region
AirScreenshot.exe capture repeat
AirScreenshot.exe capture screen --monitor all --output clipboard
AirScreenshot.exe pin clipboard
AirScreenshot.exe pin file "D:\Pictures\sample.png"
AirScreenshot.exe ocr region --copy
AirScreenshot.exe app settings
AirScreenshot.exe --help
```

隐藏托盘图标后，后台快捷键仍然有效；可运行 `AirScreenshot.exe app tray show` 恢复托盘入口。

## 构建

需要 PowerShell 7、CMake 3.25+、Ninja、Rust 1.88.0、Visual Studio 2022 17.8+ 和 Windows SDK 10.0.19041+。

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\test.ps1
.\scripts\package.ps1
```

Rust 仅用于构建原生 OCR worker，最终用户无需安装 Rust、Python 或 ONNX Runtime。

## 更新与限制

默认会静默下载并校验更新，在连续 15 分钟无人操作且没有截图、贴图、全屏或外部任务时快速重启完成；托盘可立即更新或暂缓自动重启 24 小时。该行为可在“设置 → 更新与安全”调整，程序不会为更新自动提权。

- DRM 或系统保护内容可能无法捕获。
- HDR 目前按普通 BGRA 图像输出。
- 滚动截图由用户滚动目标窗口，程序负责采样、质量判断和拼接。
- 当前不包含录屏、贴图历史/分组或通用第三方插件系统。

## 文档与许可

- [使用指南](docs/user-guide.md)：完整操作、OCR、贴图和故障恢复
- [功能对照](docs/feature-parity.md)：设计取舍、参考产品与验收边界
- [第三方声明](THIRD_PARTY_NOTICES.md)：改编来源与 OCR 依赖许可

项目采用 [LGPL-3.0-only](LICENSE) 许可。
