# Air Screenshot

Air Screenshot 是一个面向 Windows 10 2004+ x64 的轻量原生截图工具。

## 能力

- 区域、活动窗口、显示器、全虚拟桌面截图和长截图
- 剪贴板与 PNG 输出
- 可关闭的轻量标注、贴图和可切换 OCR 引擎
- 托盘、开机自启、全局快捷键与便携 CLI
- 便携 EXE 静默下载更新，退出或下次启动时应用

常驻宿主只负责托盘、快捷键和命名管道；截图缓冲、Direct2D 资源和 OCR 引擎都在使用时创建。

## 下载与使用

从 [公开下载页](https://mmm1h.github.io/air-screenshot/) 下载 `AirScreenshot.exe` 和 `airshot_ocr.exe`，放到同一个普通可写目录后双击运行。无需安装、管理员权限或证书脚本。

首次启动默认注册当前用户开机启动项，可在设置中关闭。移动 EXE 后再次启动会自动修正启动项路径。

Windows SmartScreen 可能提示未知发布者。这是因为当前使用自签名代码签名证书；请确认下载来源为本项目后再选择继续运行。

## 构建与验证

```powershell
.\scripts\build.ps1
.\scripts\test.ps1
.\scripts\smoke-portable.ps1
.\scripts\measure-performance.ps1
```

生成本地便携包：

```powershell
.\scripts\package.ps1 -Version 0.2.2
```

## 自动更新

程序启动后会读取 GitHub Pages 上的 `latest.json`。发现新版本时：

1. 静默下载新版 EXE 到 `%LOCALAPPDATA%\AirScreenshot\updates`。
2. 校验文件大小、SHA256、Authenticode 完整性和内置发布证书指纹。
3. 用户退出程序时完成替换；若程序仍在运行，则下次启动先更新再继续运行。

当前 EXE 所在目录不可写时不会请求提权，也不会覆盖原文件；程序会提示将 EXE 移到普通可写目录。

## 发布

推送格式为 `vX.Y.Z` 的 tag 会自动运行 [release.yml](.github/workflows/release.yml)：

```powershell
git tag v0.2.2
git push origin v0.2.2
```

工作流会运行测试、准备 OCR 依赖、生成并验证签名 EXE、执行便携与 OCR 烟测、创建 GitHub Release，并更新 GitHub Pages 下载页、`latest.json`、`ocr-dependencies.json` 和 OCR 依赖文件。

发布工作流使用以下仓库配置；为兼容旧配置，也会回退读取原 `MSIX_*` 名称：

| 类型 | 名称 | 用途 |
| --- | --- | --- |
| Variable | `RELEASE_PUBLISHER` | 发布签名证书主题 |
| Secret | `CODE_SIGNING_PFX_BASE64` | Base64 编码的代码签名 PFX |
| Secret | `CODE_SIGNING_PFX_PASSWORD` | 代码签名 PFX 密码 |

`scripts/create-release-cert.ps1` 可创建自签名发布证书并打印 SHA256 指纹；证书变化时必须同步更新程序内置指纹。

## CLI

主程序同时提供 GUI 与 CLI：

```powershell
.\AirScreenshot.exe capture region
.\AirScreenshot.exe capture screen --monitor all --output clipboard
.\AirScreenshot.exe ocr region --copy
.\AirScreenshot.exe module list
.\AirScreenshot.exe app settings
.\AirScreenshot.exe --help
```

无参数双击时启动托盘宿主。CLI 主要面向 PowerShell 与 Windows Terminal；程序保持 GUI 子系统，因此双击不会弹出黑色控制台窗口。

## OCR

OCR 使用本地 RapidOCR / PP-OCRv5 / ONNX Runtime CPU 推理，设置中可切换三档：

- 极速 OCR：默认档，使用 PP-OCRv5 mobile 模型，适合日常截图识字。
- 高精度 OCR：使用 PP-OCRv5 server 模型，适合小字、大图和复杂背景。
- 兼容 OCR：使用 PP-OCRv4 mobile 模型，作为稳定兼容档。

首次使用前在设置中点击“下载依赖”。依赖按清单校验 SHA256 后安装到 `%LOCALAPPDATA%\AirScreenshot\ocr\rapidocr-onnx`。OCR 识别在 `airshot_ocr.exe` 子进程中完成；模型和 ONNX Runtime 不会常驻托盘进程，单次识别超时会停止子进程。源码发布前可运行 `.\scripts\prepare-ocr-dependencies.ps1` 准备 `dist\ocr-dependencies\rapidocr-onnx`，再由 `.\scripts\package.ps1` 基于真实文件生成下载清单。离线环境可提前把依赖目录放到程序同目录下的 `ocr\rapidocr-onnx`。

## 限制

- GDI `BitBlt` 无法捕获受保护内容和部分硬件覆盖层。
- 不包含录屏、历史记录或通用第三方插件系统。
- 本地 OCR 依赖需要单独下载；未安装依赖时会提示到设置中下载。
- 旧 MSIX 版本不会自动迁移配置，需要用户自行卸载。

项目采用 `LGPL-3.0-only`。完整许可证见 [LICENSE](LICENSE)，第三方声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
