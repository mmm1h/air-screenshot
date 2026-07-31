# Air Screenshot

Air Screenshot 是一个面向 Windows 10 2004+ x64 的轻量原生截图工具。

## 能力

- 区域、活动窗口、显示器、全虚拟桌面截图和长截图
- 优先使用 Windows 合成捕获，受限环境自动回退到 GDI
- 剪贴板与 PNG 输出
- 可二次选择编辑的轻量标注，以及截图、剪贴板图像、图片文件、颜色或文本贴图
- 可独立隐藏的托盘图标、开机自启、全局快捷键与便携 CLI
- 跟随浅色/深色及系统高对比度的原生 Direct2D 设置界面
- 便携 EXE 定时静默下载更新，空闲时可立即重启应用

常驻宿主只负责托盘、快捷键和命名管道；截图缓冲、Direct2D 资源和 OCR 引擎都在使用时创建。

保存、复制或 OCR 失败时，当前选区和标注不会关闭，可以直接重试。OCR 在后台执行，识别过程中按 `Esc` 可取消。长截图会锁定开始时的目标窗口；切换到其他窗口时自动暂停，保存或复制失败时保留已拼接内容。

标注完成后切到“选择”工具，可拖动对象；矩形、椭圆和路径类对象可用控制柄缩放，线段和箭头可直接拖动端点，按住 `Shift` 拖角点保持宽高比。`Ctrl + D` 或 `Ctrl + 拖动` 可克隆，方向键微调，`Shift + 方向键` 每次移动 10 像素，`Delete` 删除；这些操作均支持撤销/重做。为避免底图与标注错位，存在标注时截图选区会保持锁定，撤销或清空标注后可再次调整选区。

托盘菜单的“贴出剪贴板”可将剪贴板图像、复制的本地图片文件、`#RRGGBB` / `rgb(...)` 颜色或普通文本直接变成桌面贴图；也可以在设置的“快捷键”页为它单独设置全局热键，默认留空以避免占用其他软件的粘贴快捷键。已有贴图在下一次截图时会临时隐藏，截图结束或取消后恢复。

贴图支持滚轮缩放、`Ctrl + 滚轮` 调整透明度、`0` 适应屏幕、`1` 恢复 100%、`T` 切换置顶、`R` / `L` 旋转、`H` / `V` 翻转、`Ctrl + C` 复制和右键菜单。开启“鼠标穿透”后，可从托盘菜单选择“恢复所有贴图交互”，或运行 `AirScreenshot.exe pin restore`；隐藏托盘图标时会禁止进入不可恢复的穿透状态。显示器断开、分辨率或工作区变化时，跑到屏幕外的贴图会自动移回可操作范围。

## 下载与使用

从 [公开下载页](https://mmm1h.github.io/air-screenshot/) 下载 `AirScreenshot.exe`，放到普通可写目录后双击运行。无需安装、管理员权限或证书脚本。

全新配置默认不开机启动，可在设置中开启；已有配置保留原值。移动 EXE 后再次启动会自动修正已启用启动项的路径。

Windows SmartScreen 可能提示未知发布者。这是因为当前使用自签名代码签名证书；请确认下载来源为本项目后再选择继续运行。

## 构建与验证

要求 PowerShell 7、CMake 3.25+、Ninja，以及带“使用 C++ 的桌面开发”和 CMake 组件的 Visual Studio 2022 17.8+。只支持 MSVC x64，Windows SDK 必须为 10.0.19041 或更新版本。

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\test.ps1 # 默认依次构建并测试 Debug、Release
.\scripts\smoke-lifecycle.ps1
.\scripts\smoke-portable.ps1
.\scripts\measure-performance.ps1
```

构建目录分别为 `build/debug` 和 `build/release`；程序位于 `bin`，链接 PDB 位于 `symbols`。生成本地未签名便携包：

```powershell
.\scripts\package.ps1
```

默认版本来自仓库根目录的 `VERSION`。版本必须使用无前导零的规范 `X.Y.Z` 格式，不能是 `0.0.0`，且 major 不超过 9000、minor/patch 不超过 65535。`-Version X.Y.Z` 仅用于有意覆盖版本的本地验证；tag 发布要求 `vX.Y.Z` 与 `VERSION` 严格一致。

## 自动更新

常驻程序启动 90 秒后进行首次自动检查，成功后最多每天检查一次，临时失败时 6 小时后重试。托盘菜单可以关闭自动检查，也可以随时手动检查；手动检查不受定时限制。

检查前会先确认当前 EXE 所在目录可安全替换，再读取 GitHub Pages 上的 `latest.json`。发现新版本时：

1. 静默下载新版 EXE 到 `%LOCALAPPDATA%\AirScreenshot\updates`。
2. 校验文件大小、SHA256、Authenticode 完整性和内置发布证书指纹。
3. 手动检查完成且程序空闲时，可从托盘选择“立即重启并更新”；有截图、设置、贴图或请求正在处理时保留当前工作。
4. 未立即重启时，用户退出程序后完成替换；若替换需要等待，则下次启动先更新再继续运行。

当前 EXE 所在目录不可写时不会联网下载、请求提权或覆盖原文件；自动检查只对同一路径提示一次，手动检查始终给出结果，并建议将 EXE 移到普通可写目录。

## 发布

推送格式为 `vX.Y.Z` 的 tag 会自动运行 [release.yml](.github/workflows/release.yml)：

```powershell
git tag v0.2.3
git push origin v0.2.3
```

工作流将职责分为三个 job：无 secrets 的构建与 Debug/Release 测试、只有签名权限的打包验证、没有 PFX 的发布部署。签名证书只写入签名 runner 的临时目录，并在打包步骤结束时删除。所有版本共享同一个发布并发组；发布前会将 lightweight/annotated tag 解引用到 commit，核对 `GITHUB_SHA`，并拒绝不高于现有最高正式版本的发布。已有同名 GitHub Release 时流程会失败，不会覆盖资产。

GitHub Release 包含 EXE、PDB、OCR manifest 及其签名、SHA256 校验和、SPDX JSON SBOM、完整许可证和第三方声明；GitHub artifact attestation 保存签名 EXE 的构建来源。GitHub Pages 提供下载页、`latest.json`、`ocr-dependencies.json` 和 OCR 依赖文件。

发布工作流使用名为 `release-signing` 的 environment。为兼容现有仓库配置，证书 secrets 也会回退读取原 `MSIX_*` 名称：

| 类型 | 名称 | 用途 |
| --- | --- | --- |
| Variable | `RELEASE_PUBLISHER` | 发布签名证书主题 |
| Variable | `RELEASE_SIGNER_SHA256` | 可选；签名证书 SHA256 指纹，缺省时使用当前内置指纹 |
| Variable | `RELEASE_TIMESTAMP_URL` | 可选；HTTPS RFC 3161 时间戳服务 |
| Variable | `RELEASE_REQUIRE_TRUSTED_SIGNATURE` | 设为 `true` 时要求 Windows 信任链有效 |
| Variable | `OCR_MANIFEST_KEY_ID` | OCR 清单 ECDSA 签名密钥标识 |
| Variable | `OCR_MANIFEST_PUBLIC_KEY_HEX` | ECDSA P-256 公钥的 `x || y`，128 位十六进制 |
| Secret | `CODE_SIGNING_PFX_BASE64` | Base64 编码的代码签名 PFX |
| Secret | `CODE_SIGNING_PFX_PASSWORD` | 代码签名 PFX 密码 |
| Secret | `OCR_MANIFEST_PRIVATE_KEY_PEM` | 与公开变量匹配的 PKCS#8 ECDSA P-256 私钥 |

正式发布前应给 `release-signing` environment 配置审批保护，使用受信任 CA 签发且允许代码签名的证书，并将 `RELEASE_REQUIRE_TRUSTED_SIGNATURE` 设为 `true`。同时必须为 `refs/tags/v*` 配置 tag ruleset，禁止更新和删除已创建的发布 tag，并在仓库设置中启用 immutable releases。workflow 的发布前 tag 检查不是 tag 与 Release 创建之间的原子 CAS，不能替代这些仓库保护。OCR 私钥只配置在该 environment 中；公钥和 key id 是非秘密仓库变量，会在构建时嵌入 EXE。`scripts/create-release-cert.ps1` 生成的自签名证书只适合开发和流程验证。

更新器以程序内置证书指纹为最终信任锚，不依赖在线吊销检查。计划轮换证书时，必须在旧私钥仍安全时先发布一个由旧证书签名、同时信任新旧指纹的桥接版本；确认桥接版本已覆盖用户后，再切换发布签名并在后续版本移除旧指纹。当前单指纹版本若遇到旧私钥泄露，无法仅靠远端配置恢复自动更新，必须引导用户手工安装新信任锚版本。

## CLI

主程序同时提供 GUI 与 CLI：

```powershell
.\AirScreenshot.exe capture region
.\AirScreenshot.exe capture screen --monitor all --output clipboard
.\AirScreenshot.exe pin clipboard
.\AirScreenshot.exe pin restore
.\AirScreenshot.exe ocr region --copy
.\AirScreenshot.exe module list
.\AirScreenshot.exe app settings
.\AirScreenshot.exe --help
```

无参数双击时启动后台宿主。设置中的“显示系统托盘图标”可以只隐藏图标而保留截图快捷键；隐藏后可运行 `.\AirScreenshot.exe app settings` 重新打开设置并恢复。CLI 主要面向 PowerShell 与 Windows Terminal；程序保持 GUI 子系统，因此双击不会弹出黑色控制台窗口。

## OCR

OCR 使用本地 RapidOCR / PP-OCRv5 / ONNX Runtime CPU 推理，设置中可切换三档：

- 极速 OCR：默认档，使用 PP-OCRv5 mobile 模型，适合日常截图识字。
- 高精度 OCR：使用 PP-OCRv5 server 模型，适合小字、大图和复杂背景。
- 兼容 OCR：使用 PP-OCRv4 mobile 模型，作为稳定兼容档。

首次使用前在设置中点击“下载依赖”。程序先用内置公钥验证版本化清单的 ECDSA 签名、有效期和防回滚序列，再校验每个文件的大小和 SHA256，最后安装到 `%LOCALAPPDATA%\AirScreenshot\ocr\rapidocr-onnx`。OCR 识别由后台任务启动独立的自身子进程；模型和 ONNX Runtime 不会常驻托盘进程，单次识别超时或用户取消时会停止子进程。

源码发布前可运行 `.\scripts\prepare-ocr-dependencies.ps1` 准备 `dist\ocr-dependencies\rapidocr-onnx`，再由 `.\scripts\package.ps1` 基于真实文件生成签名下载清单。普通本地构建如需走完整 OCR 入口，必须通过 `-OcrManifestKeyId` 和 `-OcrManifestPublicKeyHex` 嵌入与清单匹配的公钥。离线部署可以把 payload 放到程序同目录的 `ocr\rapidocr-onnx`，但目录内还必须包含同一发布的签名元数据：将 `ocr-dependencies.json` 保存为 `.airshot-manifest.json`，将其 `.sig` sidecar 保存为 `.airshot-manifest.sig`；裸依赖目录会被拒绝。

## 限制

- 受 DRM 或系统保护的内容仍不能捕获。
- Windows 合成捕获被系统策略、远程会话或图形设备限制时会回退到 GDI；回退路径仍可能遗漏部分硬件覆盖层，HDR 内容会按普通 BGRA 图像输出。
- 不包含录屏、历史记录或通用第三方插件系统。
- 本地 OCR 依赖需要单独下载；未安装依赖时会提示到设置中下载。
- 旧 MSIX 版本不会自动迁移配置，需要用户自行卸载。

项目采用 `LGPL-3.0-only`。标准许可证文本见 [LICENSE](LICENSE)，改编来源和可选 OCR payload 的许可证声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
