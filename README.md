# Air Screenshot

Air Screenshot 是一个面向 Windows 10 2004+ x64 的轻量原生截图工具。

## 能力

- 区域、活动窗口、显示器、全虚拟桌面截图和长截图
- 优先使用 Windows 合成捕获，受限环境自动回退到 GDI
- 剪贴板与 PNG 输出，支持直角/圆角截图及透明圆角
- 可二次选择编辑的轻量标注，以及截图、剪贴板图像、图片文件、颜色或文本贴图
- 可独立隐藏的托盘图标、开机自启、全局快捷键与便携 CLI
- 跟随浅色/深色及系统高对比度的原生 Direct2D 设置界面
- 便携 EXE 定时静默下载更新，空闲时可立即重启应用

常驻宿主只负责托盘、快捷键和命名管道；截图缓冲、Direct2D 资源和 OCR 引擎都在使用时创建。

保存、复制或 OCR 失败时，当前选区和标注不会关闭，可以直接重试。图像和文本会先完整准备全部新格式，再通过 OLE 一次提交；旧剪贴板采用尽力而为的稳定快照，复杂或延迟渲染格式无法快照时不会阻断正常复制。提交或刷新失败时，只有本次对象仍拥有剪贴板才会回滚，绝不会覆盖其他应用刚写入的新内容；仅当恢复与固化都成功时才提示“已恢复”，否则明确提示当前状态不确定。读取旧内容超过约 5 秒会取消本次准备、禁止后台晚写并允许立即重试；若已进入提交阶段，则由后台安全收尾并暂时拒绝重入。

OCR 首次使用时会自动进入本地依赖的安全准备流程，显示检查、下载、验证与安装进度；可取消，网络中断或用户取消后可原地重试，已验证的本地依赖可离线复用。识别同样在后台执行，按 `Esc` 可取消完整任务。长截图会锁定开始时的目标窗口，控制条持续显示拼接尺寸、帧数和匹配质量；切换目标、连续低置信或匹配失败时会安全暂停，可按 `P`/空格或点击控制条继续，`Enter` 完成、`Esc` 取消。继续前会重新验证目标、选区尺寸和拼接基线，保存或复制失败时保留已拼接内容。

标注完成后切到“选择”工具，可拖动对象；矩形、椭圆和路径类对象可用控制柄缩放，线段和箭头可直接拖动端点，按住 `Shift` 保持宽高比，按住 `Alt` 以中心缩放。`Ctrl + D` 或 `Ctrl + 拖动` 可克隆，方向键微调，`Shift + 方向键` 每次移动 10 像素，`Delete` 删除；这些操作均支持撤销/重做。矩形支持圆角、填充和虚线，箭头支持正向、反向和双向，高亮支持直线约束，文字可设置字体、字号、粗斜体和背景样式；马赛克与模糊会实时预览强度，水印支持自定义文字、浓度、替换和清除，橡皮只删除真实命中的对象并把一次连续擦除合并为一个历史步骤。序号在每次新截图中从 1 重新开始，并会自动适配三位数以上编号。为避免底图与标注错位，存在标注时截图选区会保持锁定，撤销或清空标注后可再次调整选区。

截图工具栏依据 Stitch 中统一的线性图标规范重绘：共用光学网格、线宽、圆角和交互尺寸，并为选择、马赛克、模糊、OCR、滚动截图、贴图、复制与保存保留明确不混淆的轮廓。主工具和二级控件使用一致的指针命中目标，同时保留高对比度和每显示器 DPI 适配。

托盘菜单的“贴出剪贴板”可将剪贴板图像、复制的本地图片文件、`#RRGGBB` / `rgb(...)` 颜色或普通文本直接变成桌面贴图；“从文件贴图…”和 `pin file` 可显式选择本地图片，也可以把图片文件拖到已有贴图上进行事务式替换。创建贴图前必须先得到有效图像；截图通道产生的“全零保留 alpha”会按不透明图像处理，真实的图像透明度仍保留，避免出现已创建却透明空白的贴图。可在设置的“快捷键”页为剪贴板贴图单独设置全局热键，默认留空以避免占用其他软件的粘贴快捷键。已有贴图在下一次截图时会临时隐藏，截图结束或取消后恢复；用户主动隐藏的贴图不会被误恢复。

贴图支持滚轮或 `+` / `-` 缩放、10–1000% 精确缩放和 25/50/100/200% 预设；`Ctrl + 滚轮` / `Ctrl +/-` 调整透明度，`Ctrl+0` 重置透明度，`S` 切换平滑/像素缩放。`T` 切换置顶，`R` / `L` 旋转，`H` / `V` 翻转，`G` 灰度，`I` 反色；滤镜可逆且不会反复损伤原图，复制或保存导出当前可见效果。双击只隐藏贴图，`Esc` 或菜单才销毁；托盘可查看可见/隐藏数量并批量显示、隐藏或销毁。开启“鼠标穿透”后，可从托盘菜单选择“恢复所有贴图交互”，或运行 `AirScreenshot.exe pin restore`；`pin toggle` 可切换光标下贴图，无命中时恢复全部。隐藏托盘图标时会禁止进入不可恢复的穿透状态。显示器断开、分辨率、DPI 或工作区变化时，跑到屏幕外的贴图会自动移回可操作范围。

## 下载与使用

从 [公开下载页](https://mmm1h.github.io/air-screenshot/) 下载 `AirScreenshot.exe`，放到普通可写目录后双击运行。无需安装、管理员权限或证书脚本。

全新配置默认不开机启动，可在设置中开启；已有配置保留原值。移动 EXE 后再次启动会自动修正已启用启动项的路径。

Windows SmartScreen 可能提示未知发布者。这是因为当前使用自签名代码签名证书；请确认下载来源为本项目后再选择继续运行。

### 快速上手

1. 双击运行后按 `Ctrl+Alt+A`，拖动选择区域；悬停窗口或控件时会自动识别边界，滚轮可在父子层级间切换。
2. 选区完成后可直接按 `Enter` 复制，或使用工具栏标注、保存、贴图、OCR 和滚动截图。`Esc` 先退出当前工具或选择，再取消截图。
3. 选区无标注时，方向键移动 1 px，`Ctrl+方向键` 向对应方向扩大 1 px，`Shift+方向键` 缩小 1 px；拖动时 `Shift` 保持比例、`Alt` 从中心缩放。选中标注对象后，方向键移动 1 px，`Shift+方向键` 移动 10 px。
4. 选区没有标注时按 `F2`，可输入虚拟桌面坐标 `X / Y` 与尺寸 `W / H`，选择中心/左上锚点、锁定宽高比，并设置直角或圆角输出及圆角半径。
5. 托盘选择“重复上次区域”可直接复制同一区域；显示器布局变化时会按当前桌面平移或安全裁剪，无法落到任何实际屏幕时会明确提示而不是截取黑区。

圆角输出在最终截图上应用，不改变选区坐标或标注布局，并同样作用于直接创建的贴图。PNG 文件、剪贴板 PNG 和 `CF_DIBV5` 保留透明圆角；为兼容只识别旧剪贴板格式的应用，同时提供合成到白色背景的不透明回退格式。

完整操作说明见 [使用指南](docs/user-guide.md)，功能取舍和验收边界见 [功能对照](docs/feature-parity.md)。

## 构建与验证

要求 PowerShell 7、CMake 3.25+、Ninja，以及带“使用 C++ 的桌面开发”和 CMake 组件的 Visual Studio 2022 17.8+。只支持 MSVC x64，Windows SDK 必须为 10.0.19041 或更新版本；执行 RapidOCR runner 单测需要 Python 3.11 或更新版本，构建正式 OCR payload 则固定使用 GitHub Windows runner 可用的 CPython 3.11.9 x64。

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

常驻程序启动 90 秒后进行首次自动检查，成功后最多每天检查一次，临时失败时 6 小时后重试。可在“设置 → 更新与安全”或托盘菜单关闭自动检查，也可以随时从托盘手动检查；手动检查不受定时限制。

检查前会先确认当前 EXE 所在目录可安全替换，再读取 GitHub Pages 上的 `latest.json`。发现新版本时：

1. 静默下载新版 EXE 到 `%LOCALAPPDATA%\AirScreenshot\updates`。
2. 校验文件大小、SHA256、Authenticode 完整性和内置发布证书指纹。
3. 手动检查完成且程序空闲时，可从托盘选择“立即重启并更新”；手动意图会先落盘，短暂锁占用或截图、设置、贴图仍在处理时保留入口和当前工作。单实例仲裁、验签和更新助手握手不会让第二个程序实例绕过策略。
4. 未立即重启时，用户退出程序后完成替换；若替换需要等待，则下次启动先更新再继续运行。

关闭自动更新会向正在执行的自动检查/下载发送取消请求，但不会中断用户手动发起的检查；由自动检查暂存的版本也会暂停自动安装，重新开启后恢复。用户手动检查会保留明确的手动更新意图，因此仍可选择立即重启更新，或在退出时安装。

当前 EXE 所在目录不可写时不会联网下载、请求提权或覆盖原文件；自动检查只对同一路径提示一次，手动检查始终给出结果，并建议将 EXE 移到普通可写目录。

## 发布

推送格式为 `vX.Y.Z` 的 tag 会自动运行 [release.yml](.github/workflows/release.yml)：

```powershell
git tag v0.3.5
git push origin v0.3.5
```

工作流将职责分为四个 job：无 secrets 的构建与 Debug/Release 测试、只有签名权限的打包验证、无 PFX 的产物证明与 Pages 暂存、最终部署和 Release 发布。签名证书只写入签名 runner 的临时目录，并在打包步骤结束时删除。所有版本共享同一个发布并发组；发布前会将 lightweight/annotated tag 解引用到 commit，核对 `GITHUB_SHA`，并拒绝不高于现有最高正式版本的发布。失败重跑遇到同名正式 Release 时，会逐个比对不可变资产；内容完全一致才继续验证，绝不覆盖或悄悄替换资产。

GitHub Release 包含 EXE、PDB、OCR manifest 及其签名、SHA256 校验和、SPDX JSON SBOM、完整许可证和第三方声明；GitHub artifact attestation 保存签名 EXE 的构建来源。GitHub Pages 提供下载页、`latest.json`、`ocr-dependencies.json` 和 OCR 依赖文件。

发布工作流使用名为 `release-signing` 的 environment。为兼容现有仓库配置，证书 secrets 也会回退读取原 `MSIX_*` 名称：

| 类型 | 名称 | 用途 |
| --- | --- | --- |
| Variable | `RELEASE_PUBLISHER` | 发布签名证书主题 |
| Variable | `RELEASE_SIGNER_SHA256` | 可选；签名证书 SHA256 指纹，缺省时使用当前内置指纹 |
| Variable | `RELEASE_TIMESTAMP_URL` | 可选；HTTP(S) RFC 3161 时间戳服务（DigiCert 官方端点使用 HTTP，响应本身由 TSA 签名） |
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
.\AirScreenshot.exe capture repeat
.\AirScreenshot.exe capture repeat --output file --path ".\repeat-shot.png"
.\AirScreenshot.exe capture screen --monitor all --output clipboard
.\AirScreenshot.exe pin clipboard
.\AirScreenshot.exe pin file "D:\Pictures\sample.png"
.\AirScreenshot.exe pin toggle
.\AirScreenshot.exe pin restore
.\AirScreenshot.exe ocr region --copy
.\AirScreenshot.exe module list
.\AirScreenshot.exe app settings
.\AirScreenshot.exe app tray show
.\AirScreenshot.exe --help
```

无参数双击时启动后台宿主。设置中的“显示系统托盘图标”可以只隐藏图标而保留截图快捷键；隐藏后可运行 `.\AirScreenshot.exe app tray show` 直接恢复，也可用 `app settings` 打开设置。`app status --json` 会报告快捷键是否可用及最近的运行期错误，托盘创建失败也会有界重试。CLI 主要面向 PowerShell 与 Windows Terminal；程序保持 GUI 子系统，因此双击不会弹出黑色控制台窗口。

## OCR

OCR 使用本地 RapidOCR / PP-OCRv5 / ONNX Runtime CPU 推理，设置中可切换三档：

- 极速 OCR：默认档，使用 PP-OCRv5 mobile 模型，适合日常截图识字。
- 高精度 OCR：使用 PP-OCRv5 server 模型，适合小字、大图和复杂背景。
- 兼容 OCR：使用 PP-OCRv4 mobile 模型，作为稳定兼容档。

首次点击 OCR 时无需先跳转设置页：程序会自动检查本地依赖，必要时进入安全准备流程，并显示检查、下载、验证、安装及最终校验状态。用户可随时取消，取消或可恢复的网络故障不会污染已安装版本，可直接重试；已通过完整验证的依赖可离线复用。安装时先用内置公钥验证版本化清单的 ECDSA 签名、有效期和防回滚序列，再校验每个文件的大小和 SHA256，最后事务式安装到 `%LOCALAPPDATA%\AirScreenshot\ocr\rapidocr-onnx`；签名、校验或防回滚失败不会被“重试”绕过。

OCR 识别由后台任务启动受 Job 约束的独立子进程；同一依赖版本与识别档位可复用温热 worker，空闲 60 秒或宿主管道关闭后自动退出，因此模型和 ONNX Runtime 不会进入托盘主进程。worker 崩溃、超时或协议不匹配会被丢弃并安全回退到单次冷启动；用户取消会终止完整子进程树，不再继续冷启动。

小选区会使用高质量有限倍数放大，超长截图会分块识别、对重叠结果去重并还原全局坐标。结果协议保留文字块、四边形、置信度、排序、预处理方式及分阶段耗时；纯文本由结构化结果稳定生成。真实准确率和冷/热启动耗时必须使用正式签名 OCR payload 与固定语料测量，普通无 payload 的本地构建不会用模拟数据宣称性能提升。

源码发布前可运行 `.\scripts\prepare-ocr-dependencies.ps1` 准备 `dist\ocr-dependencies\rapidocr-onnx`，再由 `.\scripts\package.ps1` 基于真实文件生成签名下载清单；打包后的 `dist\site\ocr\rapidocr-onnx` 已内置与公开清单完全一致的隐藏签名元数据，可直接作为离线 payload。普通本地构建如需走完整 OCR 入口，必须通过 `-OcrManifestKeyId` 和 `-OcrManifestPublicKeyHex` 嵌入与清单匹配的公钥。手工组装离线目录时，仍需把 `ocr-dependencies.json` 保存为 `.airshot-manifest.json`，并把其 `.sig` sidecar 保存为 `.airshot-manifest.sig`；裸依赖目录会被拒绝。

## 限制

- 受 DRM 或系统保护的内容仍不能捕获。
- Windows 合成捕获被系统策略、远程会话或图形设备限制时会回退到 GDI；回退路径仍可能遗漏部分硬件覆盖层，HDR 内容会按普通 BGRA 图像输出。
- 不包含录屏、历史记录或通用第三方插件系统。
- 本地 OCR 依赖不嵌入单 EXE，首次识别时按需自动准备；未嵌入清单公钥的开发构建会明确显示不可用，并在发起网络请求前拒绝安装。
- 旧 MSIX 版本不会自动迁移配置，需要用户自行卸载。

项目采用 `LGPL-3.0-only`。标准许可证文本见 [LICENSE](LICENSE)，改编来源和可选 OCR payload 的许可证声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
