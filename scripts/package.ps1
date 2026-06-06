[CmdletBinding()]
param(
    [string]$Version = "0.2.2",
    [switch]$Sign,
    [string]$CertPath,
    [string]$CertPassword,
    [string]$DownloadUrl = "https://mmm1h.github.io/air-screenshot/AirScreenshot.exe"
)

$ErrorActionPreference = "Stop"
if ($Version -notmatch "^\d+\.\d+\.\d+$") {
    throw "发布版本必须使用 X.Y.Z 格式。"
}

$root = Split-Path -Parent $PSScriptRoot
$site = Join-Path $root "dist\site"
& (Join-Path $PSScriptRoot "build.ps1") -Configuration Release -Version $Version

if (Test-Path $site) {
    Remove-Item -LiteralPath $site -Recurse -Force
}
New-Item -ItemType Directory -Path $site -Force | Out-Null

$executable = Join-Path $site "AirScreenshot.exe"
Copy-Item (Join-Path $root "build\AirScreenshot.exe") $executable
$ocrHelper = Join-Path $site "airshot_ocr.exe"
Copy-Item (Join-Path $root "build\airshot_ocr.exe") $ocrHelper

if ($Sign) {
    if ([string]::IsNullOrWhiteSpace($CertPassword)) {
        throw "签名时必须显式提供 CertPassword。"
    }
    if (-not $CertPath) {
        $CertPath = Join-Path $root "dist\cert\AirScreenshot.Release.pfx"
    }
    if (-not (Test-Path -LiteralPath $CertPath)) {
        throw "未找到签名证书：$CertPath"
    }
    $kitsRoot = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots").KitsRoot10
    $sdkBin = Get-ChildItem (Join-Path $kitsRoot "bin") -Directory |
        Where-Object Name -Match "^\d+\.\d+\.\d+\.\d+$" |
        Sort-Object Name -Descending |
        Select-Object -First 1
    $signTool = Join-Path $sdkBin.FullName "x64\signtool.exe"
    foreach ($binary in @($executable, $ocrHelper)) {
        & $signTool sign /fd SHA256 /f $CertPath /p $CertPassword $binary
        if ($LASTEXITCODE -ne 0) {
            throw "SignTool 签名失败：$binary"
        }
    }
}

$file = Get-Item -LiteralPath $executable
$latest = [ordered]@{
    version = $Version
    url = $DownloadUrl
    sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    size = $file.Length
} | ConvertTo-Json
Set-Content -LiteralPath (Join-Path $site "latest.json") -Value $latest -Encoding utf8NoBOM

$index = Get-Content (Join-Path $root "packaging\index.html.in") -Raw
$index = $index.Replace("__SEMVER__", $Version).Replace("__DOWNLOAD_URL__", $DownloadUrl)
Set-Content -LiteralPath (Join-Path $site "index.html") -Value $index -Encoding utf8NoBOM
Set-Content -LiteralPath (Join-Path $site ".nojekyll") -Value "" -Encoding ascii

$ocrPackageId = "rapidocr-onnx"
$ocrSource = Join-Path $root "dist\ocr-dependencies\$ocrPackageId"
if (Test-Path -LiteralPath $ocrSource) {
    $ocrTarget = Join-Path $site "ocr\$ocrPackageId"
    New-Item -ItemType Directory -Path $ocrTarget -Force | Out-Null
    Copy-Item -Path (Join-Path $ocrSource "*") -Destination $ocrTarget -Recurse -Force

    $files = Get-ChildItem -LiteralPath $ocrTarget -File -Recurse | Sort-Object FullName | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($ocrTarget, $_.FullName).Replace("\", "/")
        [ordered]@{
            path = $relative
            url = "https://mmm1h.github.io/air-screenshot/ocr/$ocrPackageId/$relative"
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            size = $_.Length
        }
    }

    $ocrManifest = [ordered]@{
        packageId = $ocrPackageId
        files = @($files)
    } | ConvertTo-Json -Depth 4
    Set-Content -LiteralPath (Join-Path $site "ocr-dependencies.json") -Value $ocrManifest -Encoding utf8NoBOM
}

Write-Host "便携版已生成：$executable"
