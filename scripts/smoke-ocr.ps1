[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$OcrRoot,
    [string]$HelperPath
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$helper = if ([string]::IsNullOrWhiteSpace($HelperPath)) {
    Join-Path $root "build\airshot_ocr.exe"
} else {
    $HelperPath
}
if (-not (Test-Path -LiteralPath $helper)) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration
}
if (-not (Test-Path -LiteralPath $helper)) {
    throw "未找到 OCR helper：$helper"
}

if ([string]::IsNullOrWhiteSpace($OcrRoot)) {
    $OcrRoot = Join-Path $root "dist\ocr-dependencies\rapidocr-onnx"
}
$OcrRoot = (Resolve-Path -LiteralPath $OcrRoot).Path
$helper = (Resolve-Path -LiteralPath $helper).Path

$profiles = @(
    "rapidocr-v5-fast",
    "rapidocr-v5-accurate",
    "rapidocr-v4-compat"
)
$required = @(
    "rapidocr_api.dll",
    "onnxruntime.dll",
    "rapidocr_runner.exe"
)
foreach ($profile in $profiles) {
    $required += "models\$profile\det.onnx"
    $required += "models\$profile\rec.onnx"
    $required += "models\$profile\cls.onnx"
    $required += "models\$profile\dict.txt"
}

$missing = @()
foreach ($relative in $required) {
    $path = Join-Path $OcrRoot $relative
    if (-not (Test-Path -LiteralPath $path)) {
        $missing += $relative
    }
}
if ($missing.Count -gt 0) {
    throw "OCR 依赖不完整：$($missing -join ', ')"
}

Add-Type -AssemblyName System.Drawing
$image = Join-Path ([IO.Path]::GetTempPath()) "airshot-ocr-smoke-$PID.png"
$stdout = Join-Path ([IO.Path]::GetTempPath()) "airshot-ocr-smoke-$PID.out"
$stderr = Join-Path ([IO.Path]::GetTempPath()) "airshot-ocr-smoke-$PID.err"

try {
    $bitmap = New-Object Drawing.Bitmap 640, 180
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([Drawing.Color]::White)
    $graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::ClearTypeGridFit
    $font = New-Object Drawing.Font "Microsoft YaHei", 34, ([Drawing.FontStyle]::Regular)
    $brush = [Drawing.Brushes]::Black
    $graphics.DrawString("Air OCR 123 中文测试", $font, $brush, 24, 52)
    $bitmap.Save($image, [Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $font.Dispose()
    $bitmap.Dispose()

    foreach ($profile in $profiles) {
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
        $modelDir = Join-Path $OcrRoot "models\$profile"
        $args = @(
            "--engine", "onnx",
            "--image", "`"$image`"",
            "--dependency-dir", "`"$OcrRoot`"",
            "--model-dir", "`"$modelDir`"",
            "--ocr-profile", $profile,
            "--ort-threads", "2"
        )
        $process = Start-Process -FilePath $helper `
            -ArgumentList $args `
            -Wait -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $stdout `
            -RedirectStandardError $stderr
        $outText = if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout -Raw -Encoding Unicode } else { "" }
        $errText = if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr -Raw -Encoding Unicode } else { "" }
        if ($process.ExitCode -ne 0) {
            throw "$profile OCR 失败：$errText"
        }
        if ($outText -notmatch "Air" -or $outText -notmatch "123") {
            throw "$profile OCR 输出未包含预期文本：$outText"
        }
        Write-Host "$profile OCR smoke passed: $($outText.Trim())"
    }
}
finally {
    Remove-Item -LiteralPath $image, $stdout, $stderr -Force -ErrorAction SilentlyContinue
}
