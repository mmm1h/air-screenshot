[CmdletBinding()]
param(
    [string]$Version = "0.1.0.0",
    [string]$PackageName = "AirScreenshot.Dev",
    [string]$Publisher = "CN=AirScreenshot Dev",
    [switch]$Sign,
    [string]$CertPath,
    [string]$CertPassword = "air-screenshot-dev",
    [string]$AppInstallerUri = "https://mmm1h.github.io/air-screenshot/AirScreenshot.appinstaller",
    [string]$PackageUri
)

$ErrorActionPreference = "Stop"
if ($Version -notmatch "^\d+\.\d+\.\d+\.\d+$") {
    throw "MSIX 版本必须是四段数字，例如 1.2.3.0。"
}
$versionParts = $Version -split "\." | ForEach-Object { [int]$_ }
if ($versionParts | Where-Object { $_ -lt 0 -or $_ -gt 65535 }) {
    throw "MSIX 每段版本号必须在 0 到 65535 之间。"
}
if ([string]::IsNullOrWhiteSpace($PackageName) -or [string]::IsNullOrWhiteSpace($Publisher)) {
    throw "PackageName 和 Publisher 不能为空。"
}

$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "build.ps1") -Configuration Release

$stage = Join-Path $root "dist\stage"
$site = Join-Path $root "dist\site"
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path $site) { Remove-Item -LiteralPath $site -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stage "Assets") -Force | Out-Null
New-Item -ItemType Directory -Path $site -Force | Out-Null

Copy-Item (Join-Path $root "build\AirScreenshot.exe") $stage
Copy-Item (Join-Path $root "build\airshot.exe") $stage
Copy-Item (Join-Path $root "LICENSE") $stage
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") $stage

Add-Type -AssemblyName System.Drawing
foreach ($item in @(
    @{ Name = "StoreLogo.png"; Size = 50 },
    @{ Name = "Square44x44Logo.png"; Size = 44 },
    @{ Name = "Square150x150Logo.png"; Size = 150 }
)) {
    $bitmap = [System.Drawing.Bitmap]::new($item.Size, $item.Size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::FromArgb(22, 119, 255))
    $pen = [System.Drawing.Pen]::new([System.Drawing.Color]::White, [Math]::Max(2, $item.Size / 16))
    $margin = [Math]::Max(6, $item.Size / 5)
    $graphics.DrawRectangle($pen, $margin, $margin, $item.Size - 2 * $margin, $item.Size - 2 * $margin)
    $bitmap.Save((Join-Path $stage "Assets\$($item.Name)"), [System.Drawing.Imaging.ImageFormat]::Png)
    $pen.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}

$manifest = Get-Content (Join-Path $root "packaging\AppxManifest.xml.in") -Raw
$manifest = $manifest.Replace("__VERSION__", $Version).Replace("__PACKAGE_NAME__", $PackageName).Replace("__PUBLISHER__", $Publisher)
Set-Content -Path (Join-Path $stage "AppxManifest.xml") -Value $manifest -Encoding utf8NoBOM

$semver = ($Version -split "\.")[0..2] -join "."
if (-not $PackageUri) {
    $PackageUri = "https://mmm1h.github.io/air-screenshot/AirScreenshot-$semver.msix"
}
$appinstaller = Get-Content (Join-Path $root "packaging\AirScreenshot.appinstaller.in") -Raw
$appinstaller = $appinstaller.Replace("__VERSION__", $Version)
$appinstaller = $appinstaller.Replace("__PACKAGE_NAME__", $PackageName)
$appinstaller = $appinstaller.Replace("__PUBLISHER__", $Publisher)
$appinstaller = $appinstaller.Replace("__APPINSTALLER_URI__", $AppInstallerUri)
$appinstaller = $appinstaller.Replace("__PACKAGE_URI__", $PackageUri)
Set-Content -Path (Join-Path $site "AirScreenshot.appinstaller") -Value $appinstaller -Encoding utf8NoBOM

$certificateUri = [Uri]::new([Uri]$AppInstallerUri, "AirScreenshot.cer").AbsoluteUri
$installScript = Get-Content (Join-Path $root "packaging\Install-AirScreenshot.ps1.in") -Raw
$installScript = $installScript.Replace("__APPINSTALLER_URI__", $AppInstallerUri).Replace("__CERTIFICATE_URI__", $certificateUri)
Set-Content -Path (Join-Path $site "Install-AirScreenshot.ps1") -Value $installScript -Encoding utf8BOM

$index = Get-Content (Join-Path $root "packaging\index.html.in") -Raw
$index = $index.Replace("__SEMVER__", $semver)
Set-Content -Path (Join-Path $site "index.html") -Value $index -Encoding utf8NoBOM
Set-Content -Path (Join-Path $site ".nojekyll") -Value "" -Encoding ascii

$kitsRoot = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots").KitsRoot10
$sdkBin = Get-ChildItem (Join-Path $kitsRoot "bin") -Directory | Where-Object Name -Match "^\d+\.\d+\.\d+\.\d+$" | Sort-Object Name -Descending | Select-Object -First 1
$makeAppx = Join-Path $sdkBin.FullName "x64\makeappx.exe"
$signTool = Join-Path $sdkBin.FullName "x64\signtool.exe"
$msix = Join-Path $site "AirScreenshot-$semver.msix"
if (Test-Path $msix) { Remove-Item -LiteralPath $msix -Force }
& $makeAppx pack /d $stage /p $msix /o
if ($LASTEXITCODE -ne 0) { throw "MakeAppx 打包失败。" }

if ($Sign) {
    if (-not $CertPath) { $CertPath = Join-Path $root "dist\cert\AirScreenshot.Dev.pfx" }
    if (-not (Test-Path -LiteralPath $CertPath)) { throw "未找到签名证书：$CertPath" }
    & $signTool sign /fd SHA256 /f $CertPath /p $CertPassword $msix
    if ($LASTEXITCODE -ne 0) { throw "SignTool 签名失败。" }
}

Write-Host "MSIX 已生成：$msix"
