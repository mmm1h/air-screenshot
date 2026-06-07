[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Version,
    [Parameter(Mandatory)]
    [string]$Publisher,
    [string]$SignerSha256 = "CB864DBFC63D94625D2173DD9B5C0426A88A415A29F054D2936165FA793D7E1E",
    [switch]$RequireTrustedSignature
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$site = Join-Path $root "dist\site"
$executable = Join-Path $site "AirScreenshot.exe"
$latestPath = Join-Path $site "latest.json"
$indexPath = Join-Path $site "index.html"
$ocrManifestPath = Join-Path $site "ocr-dependencies.json"

foreach ($path in @($executable, $latestPath, $indexPath, $ocrManifestPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "缺少发布文件：$path"
    }
}
$unexpectedFiles = Get-ChildItem -LiteralPath $site -File |
    Where-Object Name -NotIn @("AirScreenshot.exe", "latest.json", "index.html", ".nojekyll", "ocr-dependencies.json")
if ($unexpectedFiles) {
    throw "发布目录包含多余文件：$($unexpectedFiles.Name -join ', ')"
}

$output = Join-Path ([IO.Path]::GetTempPath()) "airshot-version-$PID.txt"
try {
    $process = Start-Process -FilePath $executable -ArgumentList "--version" -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $output
    $reportedVersion = (Get-Content -LiteralPath $output -Raw).Trim()
    if ($process.ExitCode -ne 0 -or $reportedVersion -ne "AirScreenshot $Version") {
        throw "EXE 版本与发布版本不匹配：$reportedVersion"
    }
}
finally {
    Remove-Item -LiteralPath $output -Force -ErrorAction SilentlyContinue
}

$file = Get-Item -LiteralPath $executable
if ($file.Length -gt 15MB) {
    throw "AirScreenshot.exe 超过 15MB：$([Math]::Round($file.Length / 1MB, 2))MB"
}

$signature = Get-AuthenticodeSignature -FilePath $executable
if (-not $signature.SignerCertificate -or $signature.Status -in @("NotSigned", "HashMismatch")) {
    throw "AirScreenshot.exe 签名无效：$($signature.StatusMessage)"
}
if ($signature.SignerCertificate.Subject -ne $Publisher) {
    throw "签名证书主题与 Publisher 不匹配：$($signature.SignerCertificate.Subject)"
}
$actualSignerSha256 = $signature.SignerCertificate.GetCertHashString([Security.Cryptography.HashAlgorithmName]::SHA256)
if ($actualSignerSha256 -ne $SignerSha256) {
    throw "签名证书指纹与程序内置发布证书不匹配：$actualSignerSha256"
}
if ($RequireTrustedSignature -and $signature.Status -ne [Management.Automation.SignatureStatus]::Valid) {
    throw "EXE 签名未通过信任验证：$($signature.StatusMessage)"
}

$latest = Get-Content -LiteralPath $latestPath -Raw | ConvertFrom-Json
$expectedHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
if ($latest.version -ne $Version -or $latest.url -ne "https://mmm1h.github.io/air-screenshot/AirScreenshot.exe" -or
    $latest.sha256 -ne $expectedHash -or $latest.size -ne $file.Length) {
    throw "latest.json 与发布 EXE 不匹配。"
}

$runtimeVerification = Start-Process -FilePath $executable `
    -ArgumentList @("--verify-update", "`"$executable`"", "`"$latestPath`"") `
    -Wait -PassThru -WindowStyle Hidden
if ($runtimeVerification.ExitCode -ne 0) {
    throw "AirScreenshot.exe 自身未通过更新哈希与签名校验。"
}

$ocrPackageId = "rapidocr-onnx"
$requiredOcrFiles = @(
    "rapidocr_api.dll",
    "onnxruntime.dll",
    "rapidocr_runner.exe",
    "models/rapidocr-v5-fast/det.onnx",
    "models/rapidocr-v5-fast/rec.onnx",
    "models/rapidocr-v5-fast/cls.onnx",
    "models/rapidocr-v5-fast/dict.txt",
    "models/rapidocr-v5-accurate/det.onnx",
    "models/rapidocr-v5-accurate/rec.onnx",
    "models/rapidocr-v5-accurate/cls.onnx",
    "models/rapidocr-v5-accurate/dict.txt",
    "models/rapidocr-v4-compat/det.onnx",
    "models/rapidocr-v4-compat/rec.onnx",
    "models/rapidocr-v4-compat/cls.onnx",
    "models/rapidocr-v4-compat/dict.txt"
)
$ocrManifest = Get-Content -LiteralPath $ocrManifestPath -Raw | ConvertFrom-Json
if ($ocrManifest.packageId -ne $ocrPackageId) {
    throw "OCR 依赖清单 packageId 错误：$($ocrManifest.packageId)"
}
$manifestPaths = @($ocrManifest.files | ForEach-Object { $_.path })
foreach ($relative in $requiredOcrFiles) {
    if ($manifestPaths -notcontains $relative) {
        throw "OCR 依赖清单缺少文件：$relative"
    }
}
foreach ($entry in $ocrManifest.files) {
    if (-not $entry.path -or $entry.path -match "(^/|^\\|:|(^|/)\.\.($|/)|(^|/)\.($|/))") {
        throw "OCR 依赖清单包含不安全路径：$($entry.path)"
    }
    if ($entry.url -ne "https://mmm1h.github.io/air-screenshot/ocr/$ocrPackageId/$($entry.path)") {
        throw "OCR 依赖清单 URL 错误：$($entry.url)"
    }
    if ($entry.sha256 -notmatch "^[A-Fa-f0-9]{64}$" -or $entry.sha256 -match "^0{64}$") {
        throw "OCR 依赖清单 SHA256 无效：$($entry.path)"
    }
    $localPath = Join-Path (Join-Path $site "ocr\$ocrPackageId") ($entry.path -replace "/", "\")
    if (-not (Test-Path -LiteralPath $localPath)) {
        throw "OCR 发布目录缺少文件：$localPath"
    }
    $localFile = Get-Item -LiteralPath $localPath
    if ($entry.size -le 0 -or $entry.size -ne $localFile.Length) {
        throw "OCR 依赖文件大小不匹配：$($entry.path)"
    }
    $actualOcrHash = (Get-FileHash -LiteralPath $localPath -Algorithm SHA256).Hash
    if ($entry.sha256.ToUpperInvariant() -ne $actualOcrHash) {
        throw "OCR 依赖文件 SHA256 不匹配：$($entry.path)"
    }
}

$mismatchedLatestPath = Join-Path ([IO.Path]::GetTempPath()) "airshot-version-mismatch-$PID.json"
try {
    [ordered]@{
        version = "9.9.9"
        url = $latest.url
        sha256 = $latest.sha256
        size = $latest.size
    } | ConvertTo-Json | Set-Content -LiteralPath $mismatchedLatestPath -Encoding utf8NoBOM
    $mismatchedVerification = Start-Process -FilePath $executable `
        -ArgumentList @("--verify-update", "`"$executable`"", "`"$mismatchedLatestPath`"") `
        -Wait -PassThru -WindowStyle Hidden
    if ($mismatchedVerification.ExitCode -eq 0) {
        throw "AirScreenshot.exe 接受了与自身版本不匹配的更新清单。"
    }
}
finally {
    Remove-Item -LiteralPath $mismatchedLatestPath -Force -ErrorAction SilentlyContinue
}

$index = Get-Content -LiteralPath $indexPath -Raw
if ($index -notmatch [Regex]::Escape($Version) -or $index -notmatch "AirScreenshot.exe" -or
    $index -match "MSIX|AppInstaller|Install-AirScreenshot") {
    throw "下载页未指向当前便携版。"
}

Write-Host "便携发布包验证通过：$executable"
