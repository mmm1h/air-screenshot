#requires -Version 7.0

[CmdletBinding()]
param(
    [string]$OcrRoot,
    [string]$PythonExecutable,
    [string]$RequirementsLockPath,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "ocr-runner-payload.ps1")
if ([string]::IsNullOrWhiteSpace($OcrRoot)) {
    $OcrRoot = Join-Path $root "dist\ocr-dependencies\rapidocr-onnx"
}

$cache = Join-Path $root "dist\ocr-cache"
$staging = Join-Path $cache "staging"
New-Item -ItemType Directory -Force -Path $cache, $staging, $OcrRoot | Out-Null

function Invoke-CheckedNative {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Native command failed with exit code $LASTEXITCODE`: $FilePath $($ArgumentList -join ' ')"
    }
}

function Get-DependencyFile {
    param(
        [Parameter(Mandatory = $true)][ValidatePattern("^https://")][string]$Url,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][ValidatePattern("^[A-Fa-f0-9]{64}$")][string]$Sha256
    )

    $expectedHash = $Sha256.ToUpperInvariant()
    if (Test-Path -LiteralPath $Destination) {
        $actualHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
        if (-not $Force -and $actualHash -eq $expectedHash) {
            return
        }
        Remove-Item -LiteralPath $Destination -Force
    }

    $destinationParent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $destinationParent | Out-Null
    $temporary = Join-Path $destinationParent (
        ".$([IO.Path]::GetFileName($Destination)).$([guid]::NewGuid().ToString('N')).download"
    )
    try {
        Write-Host "Downloading $Url"
        Invoke-WebRequest `
            -Uri $Url `
            -OutFile $temporary `
            -MaximumRedirection 5 `
            -Headers @{ "User-Agent" = "AirScreenshot-OCR-Build/1" }
        $actualHash = (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash
        if ($actualHash -ne $expectedHash) {
            throw "SHA256 mismatch: $Url`nExpected: $expectedHash`nActual:   $actualHash"
        }
        Move-Item -LiteralPath $temporary -Destination $Destination -Force
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Resolve-Python311 {
    if (-not [string]::IsNullOrWhiteSpace($PythonExecutable)) {
        $command = Get-Command $PythonExecutable -ErrorAction Stop
        return @{
            Executable = $command.Source
            Prefix = @()
        }
    }

    $launcher = Get-Command "py" -ErrorAction SilentlyContinue
    if ($launcher) {
        try {
            $version = & $launcher.Source -3.11 -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}')"
            if ($LASTEXITCODE -eq 0 -and $version.Trim() -eq "3.11.9") {
                return @{
                    Executable = $launcher.Source
                    Prefix = @("-3.11")
                }
            }
        }
        catch {
            # Fall through to a directly discoverable python executable.
        }
    }

    $python = Get-Command "python" -ErrorAction Stop
    return @{
        Executable = $python.Source
        Prefix = @()
    }
}

function Copy-PreparedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Missing prepared dependency: $Source"
    }
    $destination = Join-Path $OcrRoot $RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination -Force
}

$inlineRequirementsLock = @'
altgraph==0.17.5 --hash=sha256:f3a22400bce1b0c701683820ac4f3b159cd301acab067c51c653e06961600597
certifi==2026.7.22 --hash=sha256:62f22742b58a1a33014a2b6b706588a8d7e2a88ae7bd1a6ebe8c992928483775
charset-normalizer==3.4.9 --hash=sha256:6366a16e1a25018694d6a5d784d09b046edc9eac40ea2b54065c3052672516a1
colorama==0.4.6 --hash=sha256:4f1d9991f5acc0ca119f9d443620b77f9d6b33703e51011c16baf57afb285fc6
coloredlogs==15.0.1 --hash=sha256:612ee75c546f53e92e70049c9dbfcc18c935a2b9a53b66085ce9ef6a6e5c0934
colorlog==6.11.0 --hash=sha256:f1e27d75aa2cb138f3f640c0e305b65b680ccbef6ecc034eba7e03494ffcd2a1
flatbuffers==25.12.19 --hash=sha256:7634f50c427838bb021c2d66a3d1168e9d199b0607e6329399f04846d42e20b4
humanfriendly==10.0 --hash=sha256:1697e1a8a8f550fd43c2865cd84542fc175a61dcb779b6fee18cf6b6ccba1477
idna==3.18 --hash=sha256:7f952cbe720b688055e3f87de14f5c3e5fdaa8bc3928985c4077ca689de849a2
mpmath==1.3.0 --hash=sha256:a0b2b9fe80bbcd81a6647ff13108738cfb482d481d826cc0e02f5b35e5c88d2c
numpy==2.4.6 --hash=sha256:1e254a00cdf42b1e4d5b3d68d33af63268d41340d8885df2ab6470f2e1500147
omegaconf==2.0.0 --hash=sha256:80e4f4aa932b10699baf94d43b0e9e44e504d140b6c94b92ab4ed12ce6b77ec6
onnxruntime==1.20.1 --hash=sha256:8508887eb1c5f9537a4071768723ec7c30c28eb2518a00d0adcd32c89dea3221
opencv-python==5.0.0.93 --hash=sha256:f90ba04b8f73bc5c3814037699739f0156f597338a98f05956c684e7c3ca10d2
packaging==26.2 --hash=sha256:5fc45236b9446107ff2415ce77c807cee2862cb6fac22b8a73826d0693b0980e
pefile==2024.8.26 --hash=sha256:76f8b485dcd3b1bb8166f1128d395fa3d87af26360c2358fb75b80019b957c6f
pillow==12.3.0 --hash=sha256:8e95e1385e4998ae9694eeaa4730ba5457ff61185b3a55e2e7bea0880aef452a
protobuf==7.35.1 --hash=sha256:230a75ddfc2de4806e56696ce9640c1cdfdb6543b7cfce98d42a4c0a0e7bdb87
pyclipper==1.4.0 --hash=sha256:e9b973467d9c5fa9bc30bb6ac95f9f4d7c3d9fc25f6cf2d1cc972088e5955c01
pyinstaller==6.17.0 --hash=sha256:b019940dbf7a01489d6b26f9fb97db74b504e0a757010f7ad078675befc85a82
pyinstaller-hooks-contrib==2026.6 --hash=sha256:fd13b8ac126b35361175edacd41a0d97080b75dd5f4b594ecefefff969509dd3
pyreadline3==3.5.6 --hash=sha256:8449b734232e42a5dcd74048e39b60db2839a4c38cf3ae2bf7707d58b5389c0d
pywin32-ctypes==0.2.3 --hash=sha256:8a1513379d709975552d202d942d9837758905c8d01eb82b8bcc30918929e7b8
PyYAML==6.0.3 --hash=sha256:9f3bfb4965eb874431221a3ff3fdcddc7e74e3b07799e0e84ca4a0f867d449bf
rapidocr==3.8.1 --hash=sha256:650044b1fbce9e6bae5cae462dcf8be754cde11e2f23fc51f65dcc08deae2c46
requests==2.34.2 --hash=sha256:2a0d60c172f83ac6ab31e4554906c0f3b3588d37b5cb939b1c061f4907e278e0
setuptools==83.0.0 --hash=sha256:29b23c360f22f414dc7336bb39178cc7bcbf6021ed2733cde173f09dba19abb3
shapely==2.1.2 --hash=sha256:c64d5c97b2f47e3cd9b712eaced3b061f2b71234b3fc263e0fcf7d889c6559dc
six==1.17.0 --hash=sha256:4721f391ed90541fddacab5acf947aa0d3dc7d27b2e1e8eda2be8970586c3274
sympy==1.14.0 --hash=sha256:e091cc3e99d2141a0ba2847328f5479b05d94a6635cb96148ccb3f34671bd8f5
tqdm==4.69.0 --hash=sha256:9979978912be667a6ef21fd5d8abf54e324e63d82f7f43c360792ebc2bc4e622
typing-extensions==4.16.0 --hash=sha256:481caa481374e813c1b176ada14e97f1f67a4539ce9cfeb3f350d78d6370c2e8
urllib3==2.7.0 --hash=sha256:9fb4c81ebbb1ce9531cce37674bbc6f1360472bc18ca9a553ede278ef7276897
'@

if ([string]::IsNullOrWhiteSpace($RequirementsLockPath)) {
    $RequirementsLockPath = Join-Path $cache "rapidocr-runner-requirements.lock"
    [IO.File]::WriteAllText(
        $RequirementsLockPath,
        $inlineRequirementsLock + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false)
    )
}
elseif (-not (Test-Path -LiteralPath $RequirementsLockPath -PathType Leaf)) {
    throw "Requirements lock file does not exist: $RequirementsLockPath"
}
$requirementsHash = (Get-FileHash -LiteralPath $RequirementsLockPath -Algorithm SHA256).Hash
$runnerScript = Join-Path $PSScriptRoot "rapidocr_runner.py"
$runnerScriptHash = (Get-FileHash -LiteralPath $runnerScript -Algorithm SHA256).Hash
$runnerSpecPath = Join-Path $PSScriptRoot "rapidocr_runner.spec"
$runnerSpecHash = (Get-FileHash -LiteralPath $runnerSpecPath -Algorithm SHA256).Hash
$pythonCommand = Resolve-Python311
$pythonVersion = & $pythonCommand.Executable @($pythonCommand.Prefix) -c (
    "import platform,sys; print(f'{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}|{platform.machine()}')"
)
if ($LASTEXITCODE -ne 0 -or $pythonVersion.Trim() -ne "3.11.9|AMD64") {
    throw "OCR dependency build requires CPython 3.11.9 x64; found '$($pythonVersion.Trim())'."
}
$runnerBuildFingerprint = [Convert]::ToHexString(
    [Security.Cryptography.SHA256]::HashData(
        [Text.Encoding]::UTF8.GetBytes(
            "$requirementsHash|$runnerScriptHash|$runnerSpecHash|$($pythonVersion.Trim())"
        )
    )
)

# Remove artifacts from the retired native RapidOcrOnnx fallback.
foreach ($retiredFile in @("rapidocr_api.dll", "onnxruntime.dll")) {
    Remove-Item -LiteralPath (Join-Path $OcrRoot $retiredFile) -Force -ErrorAction SilentlyContinue
}

# Remove only reviewed non-runtime PyInstaller artifacts. Unknown empty files
# remain during this repair pass so a damaged cached runner/model can be rebuilt
# or downloaded; fresh output and the final prepared tree are fail-closed.
Remove-NonRuntimeRunnerFiles -Root $OcrRoot -AllowUnexpectedEmpty

$runnerExe = Join-Path $OcrRoot "rapidocr_runner.exe"
$runnerInternal = Join-Path $OcrRoot "_internal"
$runnerStamp = Join-Path $cache "rapidocr-runner-output.sha256"
$installedStamp = if (Test-Path -LiteralPath $runnerStamp) {
    (Get-Content -LiteralPath $runnerStamp -Raw).Trim()
} else {
    ""
}
$stampParts = @($installedStamp -split "\|", 2)
$installedOutputFingerprint = Get-RunnerOutputFingerprint -Root $OcrRoot
$needsRunnerBuild = (
    $Force -or
    -not (Test-Path -LiteralPath $runnerExe -PathType Leaf) -or
    -not (Test-Path -LiteralPath $runnerInternal -PathType Container) -or
    $stampParts.Count -ne 2 -or
    $stampParts[0] -ne $runnerBuildFingerprint -or
    $stampParts[1] -ne $installedOutputFingerprint
)

if ($needsRunnerBuild) {
    $runnerVenv = Join-Path $cache "runner-venv"
    $runnerPython = Join-Path $runnerVenv "Scripts\python.exe"
    if (Test-Path -LiteralPath $runnerVenv) {
        Remove-Item -LiteralPath $runnerVenv -Recurse -Force
    }
    Invoke-CheckedNative `
        -FilePath $pythonCommand.Executable `
        -ArgumentList (@($pythonCommand.Prefix) + @("-m", "venv", $runnerVenv))
    Invoke-CheckedNative `
        -FilePath $runnerPython `
        -ArgumentList @(
            "-m", "pip", "install",
            "--disable-pip-version-check",
            "--require-hashes",
            "--only-binary=:all:",
            "-r", $RequirementsLockPath
        )

    $runnerDist = Join-Path $staging "rapidocr-runner-dist"
    $runnerBuild = Join-Path $staging "rapidocr-runner-build"
    foreach ($directory in @($runnerDist, $runnerBuild)) {
        if (Test-Path -LiteralPath $directory) {
            Remove-Item -LiteralPath $directory -Recurse -Force
        }
    }

    $env:AIRSHOT_RAPIDOCR_RUNNER_SCRIPT = $runnerScript
    try {
        Invoke-CheckedNative `
            -FilePath $runnerPython `
            -ArgumentList @(
                "-m", "PyInstaller",
                "--clean",
                "--noconfirm",
                "--distpath", $runnerDist,
                "--workpath", $runnerBuild,
                $runnerSpecPath
            )
    }
    finally {
        Remove-Item Env:\AIRSHOT_RAPIDOCR_RUNNER_SCRIPT -ErrorAction SilentlyContinue
    }

    $runnerOutput = Join-Path $runnerDist "rapidocr_runner"
    $embeddedModels = Get-ChildItem -LiteralPath $runnerOutput -Filter "*.onnx" -File -Recurse
    if ($embeddedModels) {
        throw "PyInstaller output unexpectedly contains bundled OCR models: $($embeddedModels.FullName -join ', ')"
    }

    Remove-NonRuntimeRunnerFiles -Root $runnerOutput

    Remove-Item -LiteralPath $runnerExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $runnerInternal -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -Path (Join-Path $runnerOutput "*") -Destination $OcrRoot -Recurse -Force
    $outputFingerprint = Get-RunnerOutputFingerprint -Root $OcrRoot
    if ([string]::IsNullOrWhiteSpace($outputFingerprint)) {
        throw "Unable to fingerprint the prepared OCR runner output."
    }
    Set-Content `
        -LiteralPath $runnerStamp `
        -Value "$runnerBuildFingerprint|$outputFingerprint" `
        -Encoding ascii `
        -NoNewline
}

$models = @(
    @{
        Profile = "rapidocr-v5-fast"
        Files = @(
            @{ Name = "det.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_mobile.onnx"; Sha256 = "4d97c44a20d30a81aad087d6a396b08f786c4635742afc391f6621f5c6ae78ae" },
            @{ Name = "rec.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile.onnx"; Sha256 = "5825fc7ebf84ae7a412be049820b4d86d77620f204a041697b0494669b1742c5" },
            @{ Name = "cls.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/cls/ch_PP-LCNet_x0_25_textline_ori_cls_mobile.onnx"; Sha256 = "54379ae5174d026780215fc748a7f31910dee36818e63d49e17dc598ecc82df7" },
            @{ Name = "dict.txt"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/paddle/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile/ppocrv5_dict.txt"; Sha256 = "d1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b" }
        )
    },
    @{
        Profile = "rapidocr-v5-accurate"
        Files = @(
            @{ Name = "det.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_server.onnx"; Sha256 = "0f8846b1d4bba223a2a2f9d9b44022fbc22cc019051a602b41a7fda9667e4cad" },
            @{ Name = "rec.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/rec/ch_PP-OCRv5_rec_server.onnx"; Sha256 = "e09385400eaaaef34ceff54aeb7c4f0f1fe014c27fa8b9905d4709b65746562a" },
            @{ Name = "cls.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/cls/ch_PP-LCNet_x1_0_textline_ori_cls_server.onnx"; Sha256 = "7d3c02ef6c7da8ae08b4347cc7695b2081aae68c325d64375724ecf39c99e743" },
            @{ Name = "dict.txt"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/paddle/PP-OCRv5/rec/ch_PP-OCRv5_rec_server/ppocrv5_dict.txt"; Sha256 = "d1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b" }
        )
    },
    @{
        Profile = "rapidocr-v4-compat"
        Files = @(
            @{ Name = "det.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv4/det/ch_PP-OCRv4_det_mobile.onnx"; Sha256 = "d2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9" },
            @{ Name = "rec.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv4/rec/ch_PP-OCRv4_rec_mobile.onnx"; Sha256 = "48fc40f24f6d2a207a2b1091d3437eb3cc3eb6b676dc3ef9c37384005483683b" },
            @{ Name = "cls.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx"; Sha256 = "e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c" },
            @{ Name = "dict.txt"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/paddle/PP-OCRv4/rec/ch_PP-OCRv4_rec_mobile/ppocr_keys_v1.txt"; Sha256 = "28b2362ad4ab2dc38769aa72feb535e3a9ddb3fd2a7585a05920e6393b1dc7f7" }
        )
    }
)

foreach ($profile in $models) {
    foreach ($file in $profile.Files) {
        $destination = Join-Path $OcrRoot "models\$($profile.Profile)\$($file.Name)"
        Get-DependencyFile `
            -Url $file.Url `
            -Destination $destination `
            -Sha256 $file.Sha256
    }
}

$required = @("rapidocr_runner.exe")
foreach ($profile in $models.Profile) {
    foreach ($file in @("det.onnx", "rec.onnx", "cls.onnx", "dict.txt")) {
        $required += "models\$profile\$file"
    }
}
foreach ($relative in $required) {
    $path = Join-Path $OcrRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-Item -LiteralPath $path).Length -le 0) {
        throw "Prepared OCR dependency is missing or empty: $relative"
    }
}

$emptyPreparedFiles = @(
    Get-ChildItem -LiteralPath $OcrRoot -File -Recurse |
        Where-Object Length -EQ 0
)
if ($emptyPreparedFiles.Count -gt 0) {
    $relative = @(
        $emptyPreparedFiles | ForEach-Object {
            [IO.Path]::GetRelativePath($OcrRoot, $_.FullName).Replace("\", "/")
        }
    )
    throw "Prepared OCR dependencies contain empty files: $($relative -join ', ')"
}

$invalidPreparedPaths = @(
    Get-ChildItem -LiteralPath $OcrRoot -File -Recurse |
        ForEach-Object {
            [IO.Path]::GetRelativePath($OcrRoot, $_.FullName).Replace("\", "/")
        } |
        Where-Object { -not (Test-OcrManifestRelativePath -Path $_) }
)
if ($invalidPreparedPaths.Count -gt 0) {
    throw "Prepared OCR dependencies contain paths outside the signed manifest policy: $($invalidPreparedPaths -join ', ')"
}

Write-Host "OCR dependencies ready: $OcrRoot"
