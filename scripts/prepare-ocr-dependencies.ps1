[CmdletBinding()]
param(
    [string]$OcrRoot,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OcrRoot)) {
    $OcrRoot = Join-Path $root "dist\ocr-dependencies\rapidocr-onnx"
}

$cache = Join-Path $root "dist\ocr-cache"
$staging = Join-Path $cache "staging"
New-Item -ItemType Directory -Force -Path $cache, $staging, $OcrRoot | Out-Null

function Get-DependencyFile {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Destination,
        [string]$Sha256
    )

    if ((Test-Path -LiteralPath $Destination) -and -not $Force) {
        if ([string]::IsNullOrWhiteSpace($Sha256)) {
            return
        }
        $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -eq $Sha256.ToLowerInvariant()) {
            return
        }
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Destination

    if (-not [string]::IsNullOrWhiteSpace($Sha256)) {
        $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $Sha256.ToLowerInvariant()) {
            throw "SHA256 校验失败：$Destination`n期望：$Sha256`n实际：$actual"
        }
    }
}

function Copy-PreparedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        throw "缺少准备好的依赖文件：$Source"
    }
    $destination = Join-Path $OcrRoot $RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination -Force
}

$rapidOcrArchive = Join-Path $cache "windows-clib-vs2022-mt.7z"
Get-DependencyFile `
    -Url "https://github.com/RapidAI/RapidOcrOnnx/releases/download/1.2.3/windows-clib-vs2022-mt.7z" `
    -Destination $rapidOcrArchive

$rapidOcrExtract = Join-Path $staging "rapidocr-clib"
if ($Force -or -not (Test-Path -LiteralPath (Join-Path $rapidOcrExtract "windows-clib-vs2022-mt\win-CLIB-CPU-x64\bin\RapidOcrOnnx.dll"))) {
    if (Test-Path -LiteralPath $rapidOcrExtract) {
        Remove-Item -LiteralPath $rapidOcrExtract -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $rapidOcrExtract | Out-Null
    tar -xf $rapidOcrArchive -C $rapidOcrExtract
}
Copy-PreparedFile `
    -Source (Join-Path $rapidOcrExtract "windows-clib-vs2022-mt\win-CLIB-CPU-x64\bin\RapidOcrOnnx.dll") `
    -RelativePath "rapidocr_api.dll"

$onnxRuntimePackage = Join-Path $cache "Microsoft.ML.OnnxRuntime.1.20.1.nupkg"
Get-DependencyFile `
    -Url "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/1.20.1" `
    -Destination $onnxRuntimePackage

$onnxRuntimeExtract = Join-Path $staging "onnxruntime"
if ($Force -or -not (Test-Path -LiteralPath (Join-Path $onnxRuntimeExtract "runtimes\win-x64\native\onnxruntime.dll"))) {
    if (Test-Path -LiteralPath $onnxRuntimeExtract) {
        Remove-Item -LiteralPath $onnxRuntimeExtract -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $onnxRuntimeExtract | Out-Null
    Expand-Archive -LiteralPath $onnxRuntimePackage -DestinationPath $onnxRuntimeExtract -Force
}
Copy-PreparedFile `
    -Source (Join-Path $onnxRuntimeExtract "runtimes\win-x64\native\onnxruntime.dll") `
    -RelativePath "onnxruntime.dll"

$runnerExe = Join-Path $OcrRoot "rapidocr_runner.exe"
$runnerInternal = Join-Path $OcrRoot "_internal"
if ($Force -or -not (Test-Path -LiteralPath $runnerExe) -or -not (Test-Path -LiteralPath $runnerInternal)) {
    $runnerVenv = Join-Path $cache "runner-venv"
    $runnerPython = Join-Path $runnerVenv "Scripts\python.exe"
    if ($Force -and (Test-Path -LiteralPath $runnerVenv)) {
        Remove-Item -LiteralPath $runnerVenv -Recurse -Force
    }
    if (-not (Test-Path -LiteralPath $runnerPython)) {
        python -m venv $runnerVenv
    }
    & $runnerPython -m pip install --upgrade pip
    & $runnerPython -m pip install `
        rapidocr==3.8.1 `
        onnxruntime==1.20.1 `
        pyinstaller==6.17.0

    $runnerDist = Join-Path $staging "rapidocr-runner-dist"
    $runnerBuild = Join-Path $staging "rapidocr-runner-build"
    if (Test-Path -LiteralPath $runnerDist) {
        Remove-Item -LiteralPath $runnerDist -Recurse -Force
    }
    if (Test-Path -LiteralPath $runnerBuild) {
        Remove-Item -LiteralPath $runnerBuild -Recurse -Force
    }
    & $runnerPython -m PyInstaller `
        --clean `
        --name rapidocr_runner `
        --distpath $runnerDist `
        --workpath $runnerBuild `
        --specpath $staging `
        --collect-all rapidocr `
        (Join-Path $PSScriptRoot "rapidocr_runner.py")
    Remove-Item -LiteralPath $runnerExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $runnerInternal -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -Path (Join-Path $runnerDist "rapidocr_runner\*") -Destination $OcrRoot -Recurse -Force
}

$models = @(
    @{
        Profile = "rapidocr-v5-fast"
        Files = @(
            @{ Name = "det.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_mobile.onnx"; Sha256 = "4d97c44a20d30a81aad087d6a396b08f786c4635742afc391f6621f5c6ae78ae" },
            @{ Name = "rec.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile.onnx"; Sha256 = "5825fc7ebf84ae7a412be049820b4d86d77620f204a041697b0494669b1742c5" },
            @{ Name = "cls.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/cls/ch_PP-LCNet_x0_25_textline_ori_cls_mobile.onnx"; Sha256 = "54379ae5174d026780215fc748a7f31910dee36818e63d49e17dc598ecc82df7" },
            @{ Name = "dict.txt"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/paddle/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile/ppocrv5_dict.txt" }
        )
    },
    @{
        Profile = "rapidocr-v5-accurate"
        Files = @(
            @{ Name = "det.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_server.onnx"; Sha256 = "0f8846b1d4bba223a2a2f9d9b44022fbc22cc019051a602b41a7fda9667e4cad" },
            @{ Name = "rec.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/rec/ch_PP-OCRv5_rec_server.onnx"; Sha256 = "e09385400eaaaef34ceff54aeb7c4f0f1fe014c27fa8b9905d4709b65746562a" },
            @{ Name = "cls.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/cls/ch_PP-LCNet_x1_0_textline_ori_cls_server.onnx"; Sha256 = "7d3c02ef6c7da8ae08b4347cc7695b2081aae68c325d64375724ecf39c99e743" },
            @{ Name = "dict.txt"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/paddle/PP-OCRv5/rec/ch_PP-OCRv5_rec_server/ppocrv5_dict.txt" }
        )
    },
    @{
        Profile = "rapidocr-v4-compat"
        Files = @(
            @{ Name = "det.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv4/det/ch_PP-OCRv4_det_mobile.onnx"; Sha256 = "d2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9" },
            @{ Name = "rec.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv4/rec/ch_PP-OCRv4_rec_mobile.onnx"; Sha256 = "48fc40f24f6d2a207a2b1091d3437eb3cc3eb6b676dc3ef9c37384005483683b" },
            @{ Name = "cls.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx"; Sha256 = "e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c" },
            @{ Name = "dict.txt"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/paddle/PP-OCRv4/rec/ch_PP-OCRv4_rec_mobile/ppocr_keys_v1.txt" }
        )
    }
)

foreach ($profile in $models) {
    foreach ($file in $profile.Files) {
        $destination = Join-Path $OcrRoot "models\$($profile.Profile)\$($file.Name)"
        Get-DependencyFile -Url $file.Url -Destination $destination -Sha256 $file.Sha256
    }
}

Get-ChildItem -LiteralPath $OcrRoot -File -Recurse |
    Where-Object Length -eq 0 |
    Remove-Item -Force

Write-Host "OCR dependencies ready: $OcrRoot"
