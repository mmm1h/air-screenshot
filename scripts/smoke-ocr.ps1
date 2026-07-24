#requires -Version 7.0

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$OcrRoot,
    [string]$HelperPath
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true
$root = Split-Path -Parent $PSScriptRoot
$helper = if ([string]::IsNullOrWhiteSpace($HelperPath)) {
    Join-Path $root "build\$($Configuration.ToLowerInvariant())\bin\AirScreenshot.exe"
} else {
    $HelperPath
}
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration
}
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    throw "未找到 OCR helper：$helper"
}

if ([string]::IsNullOrWhiteSpace($OcrRoot)) {
    $OcrRoot = Join-Path $root "dist\ocr-dependencies\rapidocr-onnx"
}
if (-not (Test-Path -LiteralPath $OcrRoot -PathType Container)) {
    throw "未找到 OCR 依赖目录：$OcrRoot"
}
$OcrRoot = (Resolve-Path -LiteralPath $OcrRoot).Path
$helper = (Resolve-Path -LiteralPath $helper).Path

$profiles = @(
    "rapidocr-v5-fast",
    "rapidocr-v5-accurate",
    "rapidocr-v4-compat"
)
$required = @("rapidocr_runner.exe")
foreach ($profile in $profiles) {
    foreach ($file in @("det.onnx", "rec.onnx", "cls.onnx", "dict.txt")) {
        $required += "models\$profile\$file"
    }
}

$missing = @()
foreach ($relative in $required) {
    $path = Join-Path $OcrRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-Item -LiteralPath $path).Length -le 0) {
        $missing += $relative
    }
}
if ($missing.Count -gt 0) {
    throw "OCR 依赖不完整：$($missing -join ', ')"
}

Add-Type -AssemblyName System.Drawing
$temporaryDirectory = Join-Path (
    [IO.Path]::GetTempPath()
) ("airshot-ocr-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
$image = Join-Path $temporaryDirectory "input.png"
$expectedText = "AirOCR123中文测试"

try {
    $bitmap = [Drawing.Bitmap]::new(1000, 240)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $font = [Drawing.Font]::new(
        "Microsoft YaHei",
        52,
        [Drawing.FontStyle]::Regular,
        [Drawing.GraphicsUnit]::Pixel
    )
    try {
        $graphics.Clear([Drawing.Color]::White)
        $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $graphics.DrawString(
            "Air OCR 123 中文测试",
            $font,
            [Drawing.Brushes]::Black,
            32,
            72
        )
        $bitmap.Save($image, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $graphics.Dispose()
        $font.Dispose()
        $bitmap.Dispose()
    }

    foreach ($profile in $profiles) {
        $stdout = Join-Path $temporaryDirectory "$profile.out"
        $stderr = Join-Path $temporaryDirectory "$profile.err"
        $modelDir = Join-Path $OcrRoot "models\$profile"
        $arguments = @(
            "--ocr-internal",
            "--engine", "onnx",
            "--image", "`"$image`"",
            "--dependency-dir", "`"$OcrRoot`"",
            "--model-dir", "`"$modelDir`"",
            "--ocr-profile", $profile,
            "--ort-threads", "2"
        )
        $process = Start-Process `
            -FilePath $helper `
            -ArgumentList $arguments `
            -PassThru `
            -WindowStyle Hidden `
            -RedirectStandardOutput $stdout `
            -RedirectStandardError $stderr
        if (-not $process.WaitForExit(120000)) {
            $process.Kill($true)
            throw "$profile OCR smoke timed out after 120 seconds."
        }
        # Ensure redirected output has been fully flushed after process exit.
        $process.WaitForExit()

        $outText = if (Test-Path -LiteralPath $stdout) {
            Get-Content -LiteralPath $stdout -Raw -Encoding utf8
        } else {
            ""
        }
        $errText = if (Test-Path -LiteralPath $stderr) {
            Get-Content -LiteralPath $stderr -Raw -Encoding utf8
        } else {
            ""
        }
        if ($process.ExitCode -ne 0) {
            throw "$profile OCR 失败：$errText"
        }

        $normalized = [Regex]::Replace($outText, "[^\p{L}\p{Nd}]", "")
        if ($normalized -cne $expectedText) {
            throw "$profile OCR 输出不匹配。期望：$expectedText；实际：$normalized；原始：$outText"
        }
        Write-Host "$profile OCR smoke passed: $($outText.Trim())"
    }
}
finally {
    Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
