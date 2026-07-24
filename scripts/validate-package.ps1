#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Version,
    [Parameter(Mandatory)]
    [string]$Publisher,
    [string]$DownloadUrl = "https://mmm1h.github.io/air-screenshot/AirScreenshot.exe",
    [string]$SignerSha256 = "CB864DBFC63D94625D2173DD9B5C0426A88A415A29F054D2936165FA793D7E1E",
    [Parameter(Mandatory)]
    [string]$OcrManifestKeyId,
    [Parameter(Mandatory)]
    [string]$OcrManifestPublicKeyHex,
    [switch]$RequireTrustedSignature,
    [switch]$RequireSbom
)

$PSNativeCommandUseErrorActionPreference = $true
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "version.ps1")
$parsedVersion = ConvertTo-AirshotVersion -Version $Version -Subject "发布版本"
$site = Join-Path $root "dist\site"
$releaseAssets = Join-Path $root "dist\release-assets"
$executable = Join-Path $site "AirScreenshot.exe"
$pdb = Join-Path $releaseAssets "AirScreenshot.pdb"
$latestPath = Join-Path $site "latest.json"
$indexPath = Join-Path $site "index.html"
$ocrManifestPath = Join-Path $site "ocr-dependencies.json"
$ocrSignaturePath = Join-Path $site "ocr-dependencies.json.sig"
$checksumsPath = Join-Path $releaseAssets "SHA256SUMS.txt"
$licensePath = Join-Path $releaseAssets "LICENSE"
$noticesPath = Join-Path $releaseAssets "THIRD_PARTY_NOTICES.md"
$sbomPath = Join-Path $releaseAssets "AirScreenshot.spdx.json"

$requiredPaths = @(
    $executable,
    $pdb,
    $latestPath,
    $indexPath,
    $ocrManifestPath,
    $ocrSignaturePath,
    $checksumsPath,
    $licensePath,
    $noticesPath
)
if ($RequireSbom) {
    $requiredPaths += $sbomPath
}
foreach ($path in $requiredPaths) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "缺少发布文件：$path"
    }
}
$licenseText = Get-Content -LiteralPath $licensePath -Raw
if ($licenseText -notmatch "GNU GENERAL PUBLIC LICENSE" -or
    $licenseText -notmatch "GNU LESSER GENERAL PUBLIC LICENSE") {
    throw "LICENSE 必须同时包含 GNU GPLv3 与 LGPLv3 的完整许可文本。"
}

$unexpectedFiles = Get-ChildItem -LiteralPath $site -File |
    Where-Object Name -NotIn @(
        "AirScreenshot.exe",
        "latest.json",
        "index.html",
        ".nojekyll",
        "ocr-dependencies.json",
        "ocr-dependencies.json.sig"
    )
if ($unexpectedFiles) {
    throw "发布目录包含多余文件：$($unexpectedFiles.Name -join ', ')"
}
$unexpectedDirectories = Get-ChildItem -LiteralPath $site -Directory |
    Where-Object Name -NE "ocr"
if ($unexpectedDirectories) {
    throw "发布目录包含多余目录：$($unexpectedDirectories.Name -join ', ')"
}

$output = Join-Path ([IO.Path]::GetTempPath()) "airshot-version-$PID.txt"
try {
    $process = Start-Process `
        -FilePath $executable `
        -ArgumentList "--version" `
        -Wait `
        -PassThru `
        -WindowStyle Hidden `
        -RedirectStandardOutput $output
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
if ((Get-Item -LiteralPath $pdb).Length -lt 1KB) {
    throw "AirScreenshot.pdb 无效。"
}

$versionInfo = $file.VersionInfo
if ($versionInfo.FileVersion -ne $Version -or $versionInfo.ProductVersion -ne $Version) {
    throw "Windows 版本资源与发布版本不匹配：$($versionInfo.FileVersion) / $($versionInfo.ProductVersion)"
}
if ($versionInfo.FileMajorPart -ne $parsedVersion.Major -or
    $versionInfo.FileMinorPart -ne $parsedVersion.Minor -or
    $versionInfo.FileBuildPart -ne $parsedVersion.Patch) {
    throw "Windows 数字版本资源与发布版本不匹配。"
}

function Find-VisualStudioTool {
    param([Parameter(Mandatory)][string]$Name)

    $available = Get-Command $Name -ErrorAction SilentlyContinue
    if ($available) {
        return $available.Source
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $tool = Get-ChildItem (Join-Path $vs "VC\Tools\MSVC") -Directory |
        Sort-Object { [Version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\$Name" } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (-not $tool) {
        throw "Visual Studio tool not found: $Name"
    }
    return $tool
}

function Find-WindowsSdkTool {
    param([Parameter(Mandatory)][string]$Name)

    $available = Get-Command $Name -ErrorAction SilentlyContinue
    if ($available) {
        return $available.Source
    }

    $kitsRoot = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots").KitsRoot10
    $tool = Get-ChildItem (Join-Path $kitsRoot "bin") -Directory |
        Where-Object Name -Match "^\d+\.\d+\.\d+\.\d+$" |
        Sort-Object { [Version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\$Name" } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if (-not $tool) {
        throw "Windows SDK tool not found: $Name"
    }
    return $tool
}

function Get-PdbCodeViewIdentity {
    param([Parameter(Mandatory)][string]$PdbPath)

    [byte[]]$data = [IO.File]::ReadAllBytes($PdbPath)
    $magic = "Microsoft C/C++ MSF 7.00`r`n$([char]0x1a)DS$([char]0)$([char]0)$([char]0)"
    if ($data.Length -lt 56 -or
        [Text.Encoding]::ASCII.GetString($data, 0, 32) -cne $magic) {
        throw "PDB 不是受支持的 MSF 7.0 文件：$PdbPath"
    }

    function Read-UInt32 {
        param(
            [Parameter(Mandatory)][byte[]]$Source,
            [Parameter(Mandatory)][Int64]$Offset
        )

        if ($Offset -lt 0 -or $Offset + 4 -gt $Source.LongLength) {
            throw "PDB 结构越界：$PdbPath"
        }
        return [BitConverter]::ToUInt32($Source, [int]$Offset)
    }

    [uint32]$blockSize = Read-UInt32 -Source $data -Offset 32
    [uint32]$blockCount = Read-UInt32 -Source $data -Offset 40
    [uint32]$directorySize = Read-UInt32 -Source $data -Offset 44
    [uint32]$blockMapAddress = Read-UInt32 -Source $data -Offset 52
    $declaredLength = [Int64]$blockSize * $blockCount
    if ($blockSize -lt 512 -or
        $blockSize -gt 65536 -or
        ($blockSize -band ($blockSize - 1)) -ne 0 -or
        $blockCount -eq 0 -or
        $declaredLength -gt $data.LongLength -or
        $directorySize -lt 12 -or
        $directorySize -gt [int]::MaxValue -or
        $directorySize -gt $data.LongLength -or
        $blockMapAddress -ge $blockCount) {
        throw "PDB MSF 超级块无效：$PdbPath"
    }

    $directoryBlockCount = [int][Math]::Ceiling($directorySize / [double]$blockSize)
    $blockMapOffset = [Int64]$blockMapAddress * $blockSize
    if ($directoryBlockCount -gt ($blockSize / 4) -or
        $blockMapOffset + [Int64]$directoryBlockCount * 4 -gt $data.LongLength) {
        throw "PDB 流目录块映射无效：$PdbPath"
    }

    [byte[]]$directory = [byte[]]::new([int]$directorySize)
    $copied = 0
    for ($index = 0; $index -lt $directoryBlockCount; $index++) {
        [uint32]$block = Read-UInt32 `
            -Source $data `
            -Offset ($blockMapOffset + [Int64]$index * 4)
        if ($block -ge $blockCount) {
            throw "PDB 流目录引用了无效块：$PdbPath"
        }
        $length = [Math]::Min([int]$blockSize, [int]$directorySize - $copied)
        [Array]::Copy(
            $data,
            [Int64]$block * $blockSize,
            $directory,
            $copied,
            $length
        )
        $copied += $length
    }

    [uint32]$streamCount = Read-UInt32 -Source $directory -Offset 0
    $streamSizesEnd = 4 + [Int64]$streamCount * 4
    if ($streamCount -lt 2 -or $streamSizesEnd -gt $directory.LongLength) {
        throw "PDB 流目录无效：$PdbPath"
    }
    [uint32]$streamZeroSize = Read-UInt32 -Source $directory -Offset 4
    [uint32]$infoStreamSize = Read-UInt32 -Source $directory -Offset 8
    if ($infoStreamSize -eq [uint32]::MaxValue -or
        $infoStreamSize -lt 28 -or
        $infoStreamSize -gt [int]::MaxValue -or
        $infoStreamSize -gt $data.LongLength) {
        throw "PDB 信息流无效：$PdbPath"
    }

    $streamZeroBlockCount = if ($streamZeroSize -eq [uint32]::MaxValue) {
        0
    }
    else {
        [int][Math]::Ceiling($streamZeroSize / [double]$blockSize)
    }
    $infoBlockCount = [int][Math]::Ceiling($infoStreamSize / [double]$blockSize)
    $infoBlockListOffset = $streamSizesEnd + [Int64]$streamZeroBlockCount * 4
    if ($infoBlockListOffset + [Int64]$infoBlockCount * 4 -gt $directory.LongLength) {
        throw "PDB 信息流块列表无效：$PdbPath"
    }

    [byte[]]$infoStream = [byte[]]::new([int]$infoStreamSize)
    $copied = 0
    for ($index = 0; $index -lt $infoBlockCount; $index++) {
        [uint32]$block = Read-UInt32 `
            -Source $directory `
            -Offset ($infoBlockListOffset + [Int64]$index * 4)
        if ($block -ge $blockCount) {
            throw "PDB 信息流引用了无效块：$PdbPath"
        }
        $length = [Math]::Min([int]$blockSize, [int]$infoStreamSize - $copied)
        [Array]::Copy(
            $data,
            [Int64]$block * $blockSize,
            $infoStream,
            $copied,
            $length
        )
        $copied += $length
    }

    return [pscustomobject]@{
        Guid = [guid]::new([byte[]]$infoStream[12..27])
        Age = [BitConverter]::ToUInt32($infoStream, 8)
    }
}

$dumpbin = Find-VisualStudioTool "dumpbin.exe"
$headers = (& $dumpbin /nologo /headers $executable | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "DumpBin 检查失败，退出码：$LASTEXITCODE"
}
if ($headers -notmatch "(?im)8664 machine \(x64\)") {
    throw "AirScreenshot.exe 不是 x64 二进制。"
}
if ($headers -notmatch "(?im)Guard CF") {
    throw "AirScreenshot.exe 未启用 Control Flow Guard。"
}
$codeViewPattern = "Format:\s*RSDS,\s*\{(?<guid>[A-Fa-f0-9-]{36})\},\s*(?<age>\d+)"
$codeViewMatches = [regex]::Matches($headers, $codeViewPattern)
if ($codeViewMatches.Count -ne 1) {
    throw "AirScreenshot.exe 必须包含唯一的 RSDS CodeView 标识。"
}
$executableCodeView = $codeViewMatches[0]
$pdbCodeView = Get-PdbCodeViewIdentity -PdbPath $pdb
if ([guid]$executableCodeView.Groups["guid"].Value -ne $pdbCodeView.Guid -or
    [uint32]$executableCodeView.Groups["age"].Value -ne $pdbCodeView.Age) {
    throw "AirScreenshot.exe 与 AirScreenshot.pdb 的 CodeView GUID/age 不匹配。"
}

$manifestOutput = Join-Path ([IO.Path]::GetTempPath()) "airshot-manifest-$PID.xml"
try {
    $mt = Find-WindowsSdkTool "mt.exe"
    & $mt -nologo "-inputresource:$executable;#1" "-out:$manifestOutput"
    if ($LASTEXITCODE -ne 0) {
        throw "manifest 提取失败，退出码：$LASTEXITCODE"
    }
    [xml]$manifest = Get-Content -LiteralPath $manifestOutput -Raw
    $identity = $manifest.DocumentElement.SelectSingleNode("*[local-name()='assemblyIdentity']")
    $supportedWindows = $manifest.DocumentElement.SelectSingleNode(
        "//*[local-name()='supportedOS' and @Id='{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}']"
    )
    if ($identity.version -ne "$Version.0" -or $identity.processorArchitecture -ne "amd64") {
        throw "嵌入 manifest 的版本或架构不匹配。"
    }
    if (-not $supportedWindows) {
        throw "嵌入 manifest 未声明 Windows 10 支持。"
    }
}
finally {
    Remove-Item -LiteralPath $manifestOutput -Force -ErrorAction SilentlyContinue
}

$signature = Get-AuthenticodeSignature -FilePath $executable
if (-not $signature.SignerCertificate -or $signature.Status -in @("NotSigned", "HashMismatch")) {
    throw "AirScreenshot.exe 签名无效：$($signature.StatusMessage)"
}
if ($signature.SignerCertificate.Subject -ne $Publisher) {
    throw "签名证书主题与 Publisher 不匹配：$($signature.SignerCertificate.Subject)"
}
$actualSignerSha256 = $signature.SignerCertificate.GetCertHashString(
    [Security.Cryptography.HashAlgorithmName]::SHA256
)
if ($SignerSha256 -notmatch "^[A-Fa-f0-9]{64}$" -or
    $actualSignerSha256 -ne $SignerSha256.ToUpperInvariant()) {
    throw "签名证书指纹与程序内置发布证书不匹配：$actualSignerSha256"
}
if (-not $signature.TimeStamperCertificate) {
    throw "AirScreenshot.exe 缺少 RFC 3161 时间戳签名。"
}
if ($RequireTrustedSignature -and $signature.Status -ne [Management.Automation.SignatureStatus]::Valid) {
    throw "EXE 签名未通过信任验证：$($signature.StatusMessage)"
}

$latest = Get-Content -LiteralPath $latestPath -Raw | ConvertFrom-Json
$expectedHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
if ($latest.version -ne $Version -or
    $latest.url -ne $DownloadUrl -or
    $latest.sha256 -ne $expectedHash -or
    $latest.size -ne $file.Length) {
    throw "latest.json 与发布 EXE 不匹配。"
}

$runtimeVerification = Start-Process `
    -FilePath $executable `
    -ArgumentList @("--verify-update", "`"$executable`"", "`"$latestPath`"") `
    -Wait `
    -PassThru `
    -WindowStyle Hidden
if ($runtimeVerification.ExitCode -ne 0) {
    throw "AirScreenshot.exe 自身未通过更新哈希与签名校验。"
}

$ocrPackageId = "rapidocr-onnx"
$requiredOcrFiles = @(
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
$manifestProperties = @($ocrManifest.PSObject.Properties.Name)
if ($manifestProperties.Count -ne 6 -or
    $manifestProperties -notcontains "schemaVersion" -or
    $manifestProperties -notcontains "packageId" -or
    $manifestProperties -notcontains "sequence" -or
    $manifestProperties -notcontains "issuedAt" -or
    $manifestProperties -notcontains "expiresAt" -or
    $manifestProperties -notcontains "files") {
    throw "OCR 依赖清单顶层 schema 无效。"
}
if ($ocrManifest.packageId -ne $ocrPackageId) {
    throw "OCR 依赖清单 packageId 错误：$($ocrManifest.packageId)"
}
$expectedSequence = $parsedVersion.OcrSequence
$now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
if ($ocrManifest.schemaVersion -ne 1 -or
    $ocrManifest.sequence -ne $expectedSequence -or
    $ocrManifest.issuedAt -gt ($now + 300) -or
    $ocrManifest.issuedAt -lt ($now - (24 * 60 * 60)) -or
    $ocrManifest.expiresAt -ne ($ocrManifest.issuedAt + (180 * 24 * 60 * 60))) {
    throw "OCR 依赖清单版本、序列号或有效期无效。"
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
    if (-not (Test-Path -LiteralPath $localPath -PathType Leaf)) {
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

$signatureDocument = Get-Content -LiteralPath $ocrSignaturePath -Raw | ConvertFrom-Json
$signatureProperties = @($signatureDocument.PSObject.Properties.Name)
if ($signatureProperties.Count -ne 2 -or
    $signatureProperties -notcontains "keyId" -or
    $signatureProperties -notcontains "signature" -or
    $signatureDocument.keyId -ne $OcrManifestKeyId -or
    $signatureDocument.signature -notmatch "^[A-Fa-f0-9]{128}$" -or
    $OcrManifestPublicKeyHex -notmatch "^[A-Fa-f0-9]{128}$") {
    throw "OCR 清单签名 sidecar 格式或 key id 无效。"
}
$publicKeyBytes = [Convert]::FromHexString($OcrManifestPublicKeyHex)
$publicParameters = [Security.Cryptography.ECParameters]@{
    Curve = [Security.Cryptography.ECCurve+NamedCurves]::nistP256
    Q = [Security.Cryptography.ECPoint]@{
        X = [byte[]]$publicKeyBytes[0..31]
        Y = [byte[]]$publicKeyBytes[32..63]
    }
}
$ocrVerifier = [Security.Cryptography.ECDsa]::Create()
try {
    $ocrVerifier.ImportParameters($publicParameters)
    $validOcrSignature = $ocrVerifier.VerifyData(
        [IO.File]::ReadAllBytes($ocrManifestPath),
        [Convert]::FromHexString($signatureDocument.signature),
        [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation
    )
}
finally {
    $ocrVerifier.Dispose()
}
if (-not $validOcrSignature) {
    throw "OCR 清单 ECDSA 签名验证失败。"
}

$runtimeOcrVerification = Start-Process `
    -FilePath $executable `
    -ArgumentList @(
        "--verify-ocr-manifest",
        "`"$ocrManifestPath`"",
        "`"$ocrSignaturePath`""
    ) `
    -Wait `
    -PassThru `
    -WindowStyle Hidden
if ($runtimeOcrVerification.ExitCode -ne 0) {
    throw "最终 AirScreenshot.exe 的内置 OCR 公钥未通过发布清单验证。"
}

$tamperedOcrSignaturePath = Join-Path (
    [IO.Path]::GetTempPath()
) "airshot-ocr-signature-tampered-$PID.json"
try {
    $firstSignatureCharacter = $signatureDocument.signature.Substring(0, 1)
    $replacementCharacter = if ($firstSignatureCharacter -ceq "0") { "1" } else { "0" }
    [ordered]@{
        keyId = $signatureDocument.keyId
        signature = $replacementCharacter + $signatureDocument.signature.Substring(1)
    } | ConvertTo-Json -Compress |
        Set-Content -LiteralPath $tamperedOcrSignaturePath -Encoding utf8NoBOM

    $tamperedOcrVerification = Start-Process `
        -FilePath $executable `
        -ArgumentList @(
            "--verify-ocr-manifest",
            "`"$ocrManifestPath`"",
            "`"$tamperedOcrSignaturePath`""
        ) `
        -Wait `
        -PassThru `
        -WindowStyle Hidden
    if ($tamperedOcrVerification.ExitCode -ne 5) {
        throw "AirScreenshot.exe 未按预期拒绝篡改的 OCR 清单签名：$($tamperedOcrVerification.ExitCode)"
    }
}
finally {
    Remove-Item -LiteralPath $tamperedOcrSignaturePath -Force -ErrorAction SilentlyContinue
}

$checksumTargets = @{
    "AirScreenshot.exe" = $executable
    "AirScreenshot.pdb" = $pdb
    "LICENSE" = $licensePath
    "THIRD_PARTY_NOTICES.md" = $noticesPath
    "ocr-dependencies.json" = $ocrManifestPath
    "ocr-dependencies.json.sig" = $ocrSignaturePath
}
if ($RequireSbom) {
    $checksumTargets["AirScreenshot.spdx.json"] = $sbomPath
}
$checksumEntries = @{}
foreach ($line in Get-Content -LiteralPath $checksumsPath) {
    if ($line -notmatch "^([A-Fa-f0-9]{64}) \*(.+)$") {
        throw "SHA256SUMS.txt 格式无效：$line"
    }
    $checksumEntries[$Matches[2]] = $Matches[1].ToUpperInvariant()
}
foreach ($entry in $checksumTargets.GetEnumerator()) {
    $actual = (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash
    if ($checksumEntries[$entry.Key] -ne $actual) {
        throw "SHA256SUMS.txt 校验失败：$($entry.Key)"
    }
}

$mismatchedLatestPath = Join-Path ([IO.Path]::GetTempPath()) "airshot-version-mismatch-$PID.json"
try {
    $mismatchedVersion = if ($Version -ceq "0.0.1") { "0.0.2" } else { "0.0.1" }
    [ordered]@{
        version = $mismatchedVersion
        url = $latest.url
        sha256 = $latest.sha256
        size = $latest.size
    } | ConvertTo-Json | Set-Content -LiteralPath $mismatchedLatestPath -Encoding utf8NoBOM
    $mismatchedVerification = Start-Process `
        -FilePath $executable `
        -ArgumentList @("--verify-update", "`"$executable`"", "`"$mismatchedLatestPath`"") `
        -Wait `
        -PassThru `
        -WindowStyle Hidden
    if ($mismatchedVerification.ExitCode -eq 0) {
        throw "AirScreenshot.exe 接受了与自身版本不匹配的更新清单。"
    }
}
finally {
    Remove-Item -LiteralPath $mismatchedLatestPath -Force -ErrorAction SilentlyContinue
}

$index = Get-Content -LiteralPath $indexPath -Raw
if ($index -notmatch [Regex]::Escape($Version) -or
    $index -notmatch "AirScreenshot.exe" -or
    $index -match "airshot_ocr\.exe|MSIX|AppInstaller|Install-AirScreenshot") {
    throw "下载页未指向当前便携版。"
}

Write-Host "便携发布包验证通过：$executable"
