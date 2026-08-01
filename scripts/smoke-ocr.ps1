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
$required = @(
    "rapidocr_runner.exe",
    "onnxruntime.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    ".airshot-manifest.json",
    ".airshot-manifest.sig"
)
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
$originalDataDirectory = $env:AIRSHOT_DATA_DIR
$temporaryDirectory = Join-Path (
    [IO.Path]::GetTempPath()
) ("airshot-ocr-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
$image = Join-Path $temporaryDirectory "input.png"
$expectedText = "AirOCR123中文测试"
$imageWidth = 1000
$imageHeight = 240

function Assert-OcrProtocol {
    param(
        [Parameter(Mandatory = $true)]$Protocol,
        [Parameter(Mandatory = $true)][string]$Profile,
        [Parameter(Mandatory = $true)][string]$RawText
    )

    $blocks = @($Protocol.blocks)
    if ($Protocol.schemaVersion -ne 1 -or
        $Protocol.profile -cne $Profile -or
        $Protocol.preprocess.sourceWidth -ne $imageWidth -or
        $Protocol.preprocess.sourceHeight -ne $imageHeight -or
        $Protocol.preprocess.inputWidth -ne $imageWidth -or
        $Protocol.preprocess.inputHeight -ne $imageHeight -or
        $Protocol.preprocess.scaleX -ne 1.0 -or
        $Protocol.preprocess.scaleY -ne 1.0 -or
        $Protocol.preprocess.resample -cne "none" -or
        $Protocol.preprocess.coordinateSpace -cne "input-pixels" -or
        $Protocol.preprocess.tileCount -lt 1 -or
        $Protocol.timings.totalMs -lt 0 -or
        $blocks.Count -lt 1 -or
        $blocks.Count -gt 16384) {
        throw "$Profile OCR 协议元数据无效：$RawText"
    }
    foreach ($block in $blocks) {
        if ([string]::IsNullOrWhiteSpace([string]$block.text) -or
            $block.score -lt 0.0 -or $block.score -gt 1.0 -or
            @($block.quad).Count -ne 4) {
            throw "$Profile OCR 文本块无效：$($block | ConvertTo-Json -Compress -Depth 4)"
        }
    }

    $recognizedText = (@($blocks | ForEach-Object text) -join " ")
    $normalized = [Regex]::Replace($recognizedText, "[^\p{L}\p{Nd}]", "")
    if ($normalized -cne $expectedText) {
        throw "$Profile OCR 输出不匹配。期望：$expectedText；实际：$normalized；原始：$RawText"
    }
    return $recognizedText
}

function Invoke-OcrSmokeProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$StandardOutputPath,
        [Parameter(Mandatory = $true)][string]$StandardErrorPath,
        [Parameter(Mandatory = $true)][string]$Label,
        [int]$TimeoutMilliseconds = 120000
    )

    $process = $null
    $timedOut = $false
    $exitCode = $null
    $cleanupError = ""
    try {
        $process = Start-Process `
            -FilePath $FilePath `
            -ArgumentList $ArgumentList `
            -PassThru `
            -WindowStyle Hidden `
            -RedirectStandardOutput $StandardOutputPath `
            -RedirectStandardError $StandardErrorPath
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $timedOut = $true
        }
        else {
            # Ensure redirected output has been fully flushed after process exit.
            $process.WaitForExit()
            $exitCode = $process.ExitCode
        }
    }
    finally {
        if ($process) {
            try {
                if (-not $process.HasExited) {
                    $process.Kill($true)
                    if (-not $process.WaitForExit(5000)) {
                        $cleanupError = "process remained alive after termination"
                    }
                }
            }
            catch [InvalidOperationException] {
                # The process exited between HasExited and Kill.
            }
            catch {
                $cleanupError = $_.Exception.Message
            }
            finally {
                $process.Dispose()
            }
        }
    }

    if ($timedOut) {
        $cleanupDetail = if ([string]::IsNullOrWhiteSpace($cleanupError)) {
            ""
        } else {
            " Cleanup: $cleanupError"
        }
        throw "$Label timed out after $TimeoutMilliseconds ms.$cleanupDetail"
    }
    if (-not [string]::IsNullOrWhiteSpace($cleanupError)) {
        throw "$Label process cleanup failed: $cleanupError"
    }
    $outText = if (Test-Path -LiteralPath $StandardOutputPath) {
        Get-Content -LiteralPath $StandardOutputPath -Raw -Encoding utf8
    } else {
        ""
    }
    $errText = if (Test-Path -LiteralPath $StandardErrorPath) {
        Get-Content -LiteralPath $StandardErrorPath -Raw -Encoding utf8
    } else {
        ""
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $outText
        Error = $errText
    }
}

try {
    $isolatedDataDirectory = Join-Path $temporaryDirectory "data"
    New-Item -ItemType Directory -Path $isolatedDataDirectory | Out-Null
    $env:AIRSHOT_DATA_DIR = $isolatedDataDirectory

    $bitmap = [Drawing.Bitmap]::new($imageWidth, $imageHeight)
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
            "--ort-threads", "2",
            "--source-width", "$imageWidth",
            "--source-height", "$imageHeight",
            "--input-width", "$imageWidth",
            "--input-height", "$imageHeight",
            "--scale-x", "1",
            "--scale-y", "1",
            "--preprocess-mode", "none"
        )
        $result = Invoke-OcrSmokeProcess `
            -FilePath $helper `
            -ArgumentList $arguments `
            -StandardOutputPath $stdout `
            -StandardErrorPath $stderr `
            -Label "$profile OCR smoke"
        $outText = $result.Output
        if ($result.ExitCode -ne 0) {
            throw "$profile OCR 失败：$($result.Error)"
        }

        try {
            $protocol = $outText | ConvertFrom-Json -Depth 16
        }
        catch {
            throw "$profile OCR 返回了无效 JSON：$($_.Exception.Message)；原始：$outText"
        }
        $recognizedText = Assert-OcrProtocol `
            -Protocol $protocol `
            -Profile $profile `
            -RawText $outText
        Write-Host (
            "$profile OCR smoke passed: $recognizedText " +
            "($($protocol.timings.totalMs) ms, $(@($protocol.blocks).Count) blocks)"
        )
    }

    $workerProfile = "rapidocr-v5-fast"
    $packagedOcrRoot = [IO.Path]::GetFullPath(
        (Join-Path (Split-Path -Parent $helper) "ocr\rapidocr-onnx")
    )
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
            $OcrRoot,
            $packagedOcrRoot
        )) {
        throw (
            "Production warm OCR smoke requires the selected dependency root " +
            "to be packaged beside the helper: $packagedOcrRoot"
        )
    }
    $warmStdout = Join-Path $temporaryDirectory "warm-worker.out"
    $warmStderr = Join-Path $temporaryDirectory "warm-worker.err"
    $warmResult = Invoke-OcrSmokeProcess `
        -FilePath $helper `
        -ArgumentList @(
            "--ocr-warm-smoke",
            "--image", "`"$image`"",
            "--ocr-profile", $workerProfile
        ) `
        -StandardOutputPath $warmStdout `
        -StandardErrorPath $warmStderr `
        -Label "production warm OCR smoke"
    if ($warmResult.ExitCode -ne 0) {
        throw "Production warm OCR smoke failed: $($warmResult.Error)"
    }
    $warmNormalized = [Regex]::Replace(
        $warmResult.Output,
        "[^\p{L}\p{Nd}]",
        ""
    )
    if ($warmNormalized -cne $expectedText) {
        throw "Production warm OCR output mismatch: $($warmResult.Output)"
    }
    Write-Host "production warm OCR smoke passed: $($warmResult.Output.Trim())"
}
finally {
    if ($null -eq $originalDataDirectory) {
        Remove-Item Env:\AIRSHOT_DATA_DIR -ErrorAction SilentlyContinue
    } else {
        $env:AIRSHOT_DATA_DIR = $originalDataDirectory
    }
    Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
