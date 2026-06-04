# Air Screenshot

Air Screenshot 是一个面向 Windows 10 2004+ x64 的轻量原生截图工具。

## 能力

- 区域、活动窗口、显示器和全虚拟桌面截图
- 剪贴板与 PNG 输出
- 可关闭的轻量标注、贴图和 Windows 系统 OCR
- 托盘、开机自启、全局快捷键与正式 CLI
- MSIX 安装与 App Installer 自动更新

常驻宿主只负责托盘、快捷键和命名管道；截图缓冲、Direct2D 资源和 OCR 引擎都在使用时创建。标注、OCR 与 Shell 使用内部延迟模块工厂，关闭后不注册对应能力。

## 构建

```powershell
.\scripts\build.ps1
.\scripts\test.ps1
.\scripts\measure-performance.ps1
```

生成开发 MSIX：

```powershell
.\scripts\create-dev-cert.ps1
# 下一条命令需要管理员 PowerShell；仅开发机首次运行
.\scripts\trust-dev-cert.ps1
.\scripts\package.ps1 -Sign
.\scripts\install-dev.ps1
```

`trust-dev-cert.ps1` 必须在管理员 PowerShell 中运行，因为 Windows 的 MSIX 部署服务只读取本地计算机的包签名信任。

## 安装与静默更新

发布版首次安装时，需要在管理员 PowerShell 中运行 Release 附带的 `Install-AirScreenshot.ps1`。脚本会信任发布证书，并通过 `.appinstaller` 建立更新关系。

首次安装完成后：

- 每次启动都会无界面检查新版本。
- Windows 每 8 小时进行一次后台检查。
- 新版本在合适时机静默安装，不需要用户重新下载安装。
- 必须保持相同的包身份和签名证书，版本号必须递增。

直接安装 `.msix` 不会建立 App Installer 自动更新关系。

## 发布

推送格式为 `vX.Y.Z` 的 tag 会自动运行 [release.yml](.github/workflows/release.yml)：

```powershell
git tag v0.1.0
git push origin v0.1.0
```

工作流会运行测试、生成并验证签名 MSIX、创建 GitHub Release，并将最新安装入口发布到 GitHub Pages。

Release 工作流依赖以下仓库配置：

| 类型 | 名称 | 用途 |
| --- | --- | --- |
| Variable | `MSIX_PACKAGE_NAME` | 稳定 MSIX 包身份 |
| Variable | `MSIX_PUBLISHER` | 必须与签名证书主题完全一致 |
| Secret | `MSIX_SIGNING_PFX_BASE64` | Base64 编码的发布签名 PFX |
| Secret | `MSIX_SIGNING_PFX_PASSWORD` | 发布签名 PFX 密码 |

可使用 `scripts/create-release-cert.ps1` 创建自签名发布证书。发布后不能随意更换包身份或证书，否则已安装版本无法静默升级。

## CLI

CLI 示例：

```powershell
airshot capture region
airshot capture screen --monitor all --output clipboard
airshot ocr region --copy
airshot module list
airshot app settings
```

运行 `airshot --help` 查看完整命令。宿主未运行时，CLI 会按命令需要启动常驻或临时宿主。

## 限制

- GDI `BitBlt` 无法捕获受保护内容和部分硬件覆盖层。
- 首版不包含长截图、录屏、历史记录或第三方 DLL 插件。
- OCR 只调用 Windows 系统 `Windows.Media.Ocr`，可用语言取决于系统语言包。

项目采用 `LGPL-3.0-only`，第三方来源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
