#Requires -Version 7.0

[CmdletBinding()]
param(
    [string]$OcrRoot,
    [string]$CargoExecutable,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "ocr-runner-payload.ps1")
if ([string]::IsNullOrWhiteSpace($OcrRoot)) {
    $OcrRoot = Join-Path $root "dist\ocr-dependencies\rapidocr-onnx"
}
$OcrRoot = [IO.Path]::GetFullPath($OcrRoot)
$cache = Join-Path $root "dist\ocr-cache"
$downloadCache = Join-Path $cache "downloads"
$cargoTarget = Join-Path $cache "rust-target"
New-Item -ItemType Directory -Force -Path $cache, $downloadCache, $cargoTarget, $OcrRoot | Out-Null

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
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        $actualHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
        if (-not $Force -and $actualHash -eq $expectedHash) {
            return
        }
        Remove-Item -LiteralPath $Destination -Force
    }
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $temporary = Join-Path $parent (
        ".$([IO.Path]::GetFileName($Destination)).$([guid]::NewGuid().ToString('N')).download"
    )
    try {
        Write-Host "Downloading $Url"
        Invoke-WebRequest `
            -Uri $Url `
            -OutFile $temporary `
            -MaximumRedirection 5 `
            -Headers @{ "User-Agent" = "AirScreenshot-OCR-Build/2" }
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

function Copy-PreparedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf) -or
        (Get-Item -LiteralPath $Source).Length -le 0) {
        throw "Missing prepared dependency: $Source"
    }
    $destination = Join-Path $OcrRoot $RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination -Force
}

function Expand-PinnedZipEntry {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$EntryPath,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][ValidatePattern("^[A-Fa-f0-9]{64}$")][string]$Sha256
    )
    $expectedHash = $Sha256.ToUpperInvariant()
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        $actualHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
        if (-not $Force -and $actualHash -eq $expectedHash) {
            return
        }
        Remove-Item -LiteralPath $Destination -Force
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $entries = @($archive.Entries | Where-Object FullName -CEQ $EntryPath)
        if ($entries.Count -ne 1 -or $entries[0].Length -le 0) {
            throw "Pinned archive entry is missing or ambiguous: $EntryPath"
        }
        $parent = Split-Path -Parent $Destination
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        $temporary = Join-Path $parent (
            ".$([IO.Path]::GetFileName($Destination)).$([guid]::NewGuid().ToString('N')).extract"
        )
        try {
            $sourceStream = $entries[0].Open()
            $destinationStream = [IO.File]::Create($temporary)
            try {
                $sourceStream.CopyTo($destinationStream)
            }
            finally {
                $destinationStream.Dispose()
                $sourceStream.Dispose()
            }
            $actualHash = (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash
            if ($actualHash -ne $expectedHash) {
                throw "SHA256 mismatch for archive entry $EntryPath`nExpected: $expectedHash`nActual:   $actualHash"
            }
            Move-Item -LiteralPath $temporary -Destination $Destination -Force
        }
        finally {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Enter-AirshotVisualStudioEnvironment {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "Visual Studio Installer (vswhere.exe) was not found."
    }
    $vs = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vs) {
        throw "Visual Studio 2022 with the Desktop development with C++ workload was not found."
    }
    Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell `
        -VsInstallPath $vs `
        -SkipAutomaticLocation `
        -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
    if ($env:VSCMD_ARG_TGT_ARCH -ne "x64") {
        throw "The Visual Studio developer environment did not select x64."
    }
    return $vs
}

function Resolve-CargoExecutable {
    if (-not [string]::IsNullOrWhiteSpace($CargoExecutable)) {
        return (Get-Command $CargoExecutable -ErrorAction Stop).Source
    }
    $command = Get-Command cargo.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    throw "Rust/Cargo is required to build the native OCR worker. Install the pinned rust-toolchain.toml toolchain or pass -CargoExecutable."
}

function Find-VcRuntimeDirectory {
    param([Parameter(Mandatory = $true)][string]$VisualStudioRoot)
    $redistRoot = Join-Path $VisualStudioRoot "VC\Redist\MSVC"
    $required = @("msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll")
    foreach ($version in @(Get-ChildItem -LiteralPath $redistRoot -Directory | Sort-Object Name -Descending)) {
        $candidate = Join-Path $version.FullName "x64\Microsoft.VC143.CRT"
        if (@($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $candidate $_) -PathType Leaf) }).Count -eq 0) {
            return $candidate
        }
    }
    throw "The Visual C++ x64 redistributable runtime was not found."
}

function Write-RustCrateNotices {
    param([Parameter(Mandatory = $true)][string]$Cargo)

    $manifest = Join-Path $root "ocr-worker\Cargo.toml"
    $metadataJson = & $Cargo metadata `
        --manifest-path $manifest `
        --locked `
        --format-version 1 `
        --filter-platform x86_64-pc-windows-msvc
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($metadataJson)) {
        throw "Unable to resolve native OCR worker license metadata."
    }
    $metadata = $metadataJson | ConvertFrom-Json -Depth 100
    if (-not $metadata.resolve.root) {
        throw "Cargo metadata did not identify the native OCR worker root package."
    }

    $nodes = @{}
    foreach ($node in @($metadata.resolve.nodes)) {
        $nodes[[string]$node.id] = $node
    }
    $reachable = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal
    )
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue([string]$metadata.resolve.root)
    while ($pending.Count -gt 0) {
        $id = $pending.Dequeue()
        if (-not $reachable.Add($id)) {
            continue
        }
        if (-not $nodes.ContainsKey($id)) {
            throw "Cargo metadata dependency graph is incomplete: $id"
        }
        foreach ($dependency in @($nodes[$id].deps)) {
            $pending.Enqueue([string]$dependency.pkg)
        }
    }

    $packages = @(
        $metadata.packages |
            Where-Object { $reachable.Contains([string]$_.id) } |
            Sort-Object name, version
    )
    if ($packages.Count -lt 2) {
        throw "Cargo metadata did not resolve the native OCR dependency graph."
    }

    $notice = [Text.StringBuilder]::new()
    [void]$notice.AppendLine("Air Screenshot native OCR worker - Rust crate notices")
    [void]$notice.AppendLine("Generated from the locked x86_64-pc-windows-msvc dependency graph.")
    [void]$notice.AppendLine()
    $inventoryPackages = [Collections.Generic.List[object]]::new()
    foreach ($package in $packages) {
        $packageRoot = Split-Path -Parent ([string]$package.manifest_path)
        $licenseFiles = @()
        if (-not [string]::IsNullOrWhiteSpace([string]$package.license_file)) {
            $licenseFiles = @(
                Join-Path $packageRoot ([string]$package.license_file)
            )
        } else {
            $licenseFiles = @(
                Get-ChildItem -LiteralPath $packageRoot -File |
                    Where-Object Name -Match "^(LICENSE|COPYING|NOTICE)([-_.].*)?$" |
                    Sort-Object Name |
                    ForEach-Object FullName
            )
        }
        $validLicenseFiles = @(
            $licenseFiles |
                Where-Object {
                    (Test-Path -LiteralPath $_ -PathType Leaf) -and
                    (Get-Item -LiteralPath $_).Length -gt 0
                }
        )
        if ([string]::IsNullOrWhiteSpace([string]$package.license) -and
            $validLicenseFiles.Count -eq 0) {
            throw "Rust crate has no license metadata: $($package.name) $($package.version)"
        }

        $source = if (-not [string]::IsNullOrWhiteSpace([string]$package.repository)) {
            [string]$package.repository
        } elseif (-not [string]::IsNullOrWhiteSpace([string]$package.source)) {
            [string]$package.source
        } else {
            "Air Screenshot source tree"
        }
        [void]$notice.AppendLine("================================================================================")
        [void]$notice.AppendLine("$($package.name) $($package.version)")
        [void]$notice.AppendLine("License: $($package.license)")
        [void]$notice.AppendLine("Source: $source")
        $fileInventory = [Collections.Generic.List[object]]::new()
        foreach ($licenseFile in $validLicenseFiles) {
            $licenseName = [IO.Path]::GetFileName($licenseFile)
            $licenseHash = (Get-FileHash -LiteralPath $licenseFile -Algorithm SHA256).Hash
            $fileInventory.Add([ordered]@{
                name = $licenseName
                sha256 = $licenseHash
            })
            [void]$notice.AppendLine("--- $licenseName ($licenseHash) ---")
            [void]$notice.AppendLine((Get-Content -LiteralPath $licenseFile -Raw))
            [void]$notice.AppendLine()
        }
        if ($validLicenseFiles.Count -eq 0) {
            [void]$notice.AppendLine("No separate license file was included in this crate archive; the SPDX license expression above applies.")
            [void]$notice.AppendLine()
        }
        $inventoryPackages.Add([ordered]@{
            name = [string]$package.name
            version = [string]$package.version
            license = [string]$package.license
            source = $source
            licenseFiles = @($fileInventory)
        })
    }

    $noticePath = Join-Path $OcrRoot "licenses\rust-crates-NOTICES.txt"
    $inventoryPath = Join-Path $OcrRoot "licenses\rust-crates.json"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $noticePath) | Out-Null
    [IO.File]::WriteAllText(
        $noticePath,
        $notice.ToString(),
        [Text.UTF8Encoding]::new($false)
    )
    $inventory = [ordered]@{
        schemaVersion = 1
        target = "x86_64-pc-windows-msvc"
        cargoLockSha256 = (Get-FileHash `
            -LiteralPath (Join-Path $root "ocr-worker\Cargo.lock") `
            -Algorithm SHA256).Hash
        packages = @($inventoryPackages)
    } | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText(
        $inventoryPath,
        $inventory + "`n",
        [Text.UTF8Encoding]::new($false)
    )
}

Remove-LegacyOcrRunnerFiles -Root $OcrRoot
$visualStudioRoot = Enter-AirshotVisualStudioEnvironment
$cargo = Resolve-CargoExecutable
$rustc = Join-Path (Split-Path -Parent $cargo) "rustc.exe"
if (-not (Test-Path -LiteralPath $rustc -PathType Leaf) -or
    (& $rustc --version).Trim() -notmatch "^rustc 1\.88\.0 ") {
    throw "The native OCR worker must be built with the pinned Rust 1.88.0 toolchain."
}
$env:CARGO_TARGET_DIR = $cargoTarget
try {
    Invoke-CheckedNative `
        -FilePath $cargo `
        -ArgumentList @(
            "build",
            "--manifest-path", (Join-Path $root "ocr-worker\Cargo.toml"),
            "--release",
            "--locked",
            "--target", "x86_64-pc-windows-msvc"
        )
}
finally {
    Remove-Item Env:\CARGO_TARGET_DIR -ErrorAction SilentlyContinue
}
$runner = Join-Path $cargoTarget "x86_64-pc-windows-msvc\release\rapidocr_runner.exe"
Copy-PreparedFile -Source $runner -RelativePath "rapidocr_runner.exe"

$ortArchive = Join-Path $downloadCache "onnxruntime-win-x64-1.22.0.zip"
Get-DependencyFile `
    -Url "https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-win-x64-1.22.0.zip" `
    -Destination $ortArchive `
    -Sha256 "174c616efc0271194488642a72f1a514e01487da4dfe84c49296d66e40ebe0da"
Expand-PinnedZipEntry `
    -ArchivePath $ortArchive `
    -EntryPath "onnxruntime-win-x64-1.22.0/lib/onnxruntime.dll" `
    -Destination (Join-Path $OcrRoot "onnxruntime.dll") `
    -Sha256 "579b636403983254346a5c1d80bd28f1519cd1e284cd204f8d4ff41f8d711559"
Expand-PinnedZipEntry `
    -ArchivePath $ortArchive `
    -EntryPath "onnxruntime-win-x64-1.22.0/LICENSE" `
    -Destination (Join-Path $OcrRoot "licenses\onnxruntime-LICENSE.txt") `
    -Sha256 "c250d6278f0b47a6439fb7592b08b58a55eb9f535aa49a1db63211c3f982b674"
Expand-PinnedZipEntry `
    -ArchivePath $ortArchive `
    -EntryPath "onnxruntime-win-x64-1.22.0/ThirdPartyNotices.txt" `
    -Destination (Join-Path $OcrRoot "licenses\onnxruntime-ThirdPartyNotices.txt") `
    -Sha256 "e00f828e0a33de591a355ae6606d2625f5758da7d2c844db7821c9dd3e3647b6"

$vcRuntime = Find-VcRuntimeDirectory -VisualStudioRoot $visualStudioRoot
foreach ($runtimeFile in @("msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
    Copy-PreparedFile `
        -Source (Join-Path $vcRuntime $runtimeFile) `
        -RelativePath $runtimeFile
}

$paddleLicense = Join-Path $downloadCache "paddle-ocr-rs-0.6.1-LICENSE.txt"
Get-DependencyFile `
    -Url "https://raw.githubusercontent.com/mg-chao/paddle-ocr-rs/0667be342ea45816c993421e78d2cbb428f15097/LICENSE" `
    -Destination $paddleLicense `
    -Sha256 "bcb782df7a575486f85a9b7b181902910c664dea4d537ada33e46dd96974cc1e"
Copy-PreparedFile -Source $paddleLicense -RelativePath "licenses\paddle-ocr-rs-LICENSE.txt"
Copy-PreparedFile `
    -Source (Join-Path $root "THIRD_PARTY_NOTICES.md") `
    -RelativePath "licenses\AirScreenshot-THIRD_PARTY_NOTICES.md"
Copy-PreparedFile `
    -Source (Join-Path $root "LICENSE") `
    -RelativePath "licenses\AirScreenshot-LICENSE.txt"
$rustSysroot = (& $rustc --print sysroot).Trim()
Copy-PreparedFile `
    -Source (Join-Path $rustSysroot "share\doc\rust\COPYRIGHT-library.html") `
    -RelativePath "licenses\rust-standard-library-COPYRIGHT.html"
Write-RustCrateNotices -Cargo $cargo

$models = @(
    @{
        Profile = "rapidocr-v5-fast"
        Files = @(
            @{ Name = "det.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_mobile.onnx"; Sha256 = "4d97c44a20d30a81aad087d6a396b08f786c4635742afc391f6621f5c6ae78ae" },
            @{ Name = "rec.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile.onnx"; Sha256 = "5825fc7ebf84ae7a412be049820b4d86d77620f204a041697b0494669b1742c5" },
            @{ Name = "cls.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx"; Sha256 = "e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c" },
            @{ Name = "dict.txt"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/paddle/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile/ppocrv5_dict.txt"; Sha256 = "d1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b" }
        )
    },
    @{
        Profile = "rapidocr-v5-accurate"
        Files = @(
            @{ Name = "det.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_server.onnx"; Sha256 = "0f8846b1d4bba223a2a2f9d9b44022fbc22cc019051a602b41a7fda9667e4cad" },
            @{ Name = "rec.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile.onnx"; Sha256 = "5825fc7ebf84ae7a412be049820b4d86d77620f204a041697b0494669b1742c5" },
            @{ Name = "cls.onnx"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx"; Sha256 = "e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c" },
            @{ Name = "dict.txt"; Url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.8.0/paddle/PP-OCRv5/rec/ch_PP-OCRv5_rec_mobile/ppocrv5_dict.txt"; Sha256 = "d1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b" }
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
        $cached = Join-Path $downloadCache "$($file.Sha256)-$($file.Name)"
        Get-DependencyFile -Url $file.Url -Destination $cached -Sha256 $file.Sha256
        Copy-PreparedFile `
            -Source $cached `
            -RelativePath "models\$($profile.Profile)\$($file.Name)"
    }
}

$required = @(Get-NativeOcrRuntimeFiles)
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

$invalidPreparedFiles = @(
    Get-ChildItem -LiteralPath $OcrRoot -File -Recurse |
        Where-Object {
            $_.Length -le 0 -or
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        }
)
if ($invalidPreparedFiles.Count -gt 0) {
    throw "Prepared OCR dependencies contain empty or reparse-point files: $($invalidPreparedFiles.FullName -join ', ')"
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
$fingerprint = Get-RunnerOutputFingerprint -Root $OcrRoot
if ($fingerprint -notmatch "^[A-F0-9]{64}$") {
    throw "Unable to fingerprint the native OCR runtime."
}
Write-Host "Native OCR dependencies ready: $OcrRoot"
Write-Host "Native OCR runtime fingerprint: $fingerprint"
