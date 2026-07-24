#Requires -Version 7.0

[CmdletBinding()]
param(
    [string]$Version,
    [switch]$SkipBuild,
    [string]$InputExecutable,
    [string]$InputPdb,
    [string]$OcrSourceRoot,
    [string]$OcrManifestKeyId,
    [string]$OcrManifestPublicKeyHex,
    [string]$OcrManifestPrivateKeyPem,
    [string]$OcrMinSequence,
    [switch]$Sign,
    [string]$CertPath,
    [string]$CertPassword,
    [string]$TimestampUrl = "https://timestamp.digicert.com",
    [string]$DownloadUrl = "https://mmm1h.github.io/air-screenshot/AirScreenshot.exe"
)

$PSNativeCommandUseErrorActionPreference = $true
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "version.ps1")

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content -LiteralPath (Join-Path $root "VERSION") -Raw).Trim()
}
$parsedVersion = ConvertTo-AirshotVersion -Version $Version -Subject "发布版本"
$ocrManifestSequence = $parsedVersion.OcrSequence
if ([string]::IsNullOrWhiteSpace($OcrMinSequence)) {
    $OcrMinSequence = [string]$ocrManifestSequence
}
$parsedOcrMinSequence = [uint64]0
if (-not [uint64]::TryParse(
        $OcrMinSequence,
        [Globalization.NumberStyles]::None,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$parsedOcrMinSequence
    ) -or
    $parsedOcrMinSequence -lt 1 -or
    $parsedOcrMinSequence -gt $ocrManifestSequence) {
    throw "OcrMinSequence 必须位于 1..当前 OCR 清单序列号。"
}
if ($DownloadUrl -notmatch "^https://") {
    throw "下载地址必须使用 HTTPS。"
}
if ($TimestampUrl -notmatch "^https://") {
    throw "RFC 3161 时间戳地址必须使用 HTTPS。"
}
if (($OcrManifestKeyId -or $OcrManifestPublicKeyHex -or $OcrManifestPrivateKeyPem) -and
    ($OcrManifestKeyId -notmatch "^[A-Za-z0-9._-]{1,64}$" -or
     $OcrManifestPublicKeyHex -notmatch "^[A-Fa-f0-9]{128}$" -or
     [string]::IsNullOrWhiteSpace($OcrManifestPrivateKeyPem) -or
     $OcrManifestPrivateKeyPem -notmatch "-----BEGIN PRIVATE KEY-----")) {
    throw "OCR 清单签名必须同时提供 key id、128 位十六进制公钥和 PKCS#8 PEM 私钥。"
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") `
        -Configuration Release `
        -Version $Version `
        -OcrManifestKeyId $OcrManifestKeyId `
        -OcrManifestPublicKeyHex $OcrManifestPublicKeyHex `
        -OcrMinSequence $parsedOcrMinSequence
}

if ([string]::IsNullOrWhiteSpace($InputExecutable)) {
    $InputExecutable = Join-Path $root "build\release\bin\AirScreenshot.exe"
}
if ([string]::IsNullOrWhiteSpace($InputPdb)) {
    $InputPdb = Join-Path $root "build\release\symbols\AirScreenshot.pdb"
}
if ([string]::IsNullOrWhiteSpace($OcrSourceRoot)) {
    $OcrSourceRoot = Join-Path $root "dist\ocr-dependencies"
}
foreach ($requiredInput in @($InputExecutable, $InputPdb)) {
    if (-not (Test-Path -LiteralPath $requiredInput -PathType Leaf)) {
        throw "缺少发布输入：$requiredInput"
    }
}

$site = Join-Path $root "dist\site"
$releaseAssets = Join-Path $root "dist\release-assets"
foreach ($directory in @($site, $releaseAssets)) {
    if (Test-Path -LiteralPath $directory) {
        Remove-Item -LiteralPath $directory -Recurse -Force
    }
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

$executable = Join-Path $site "AirScreenshot.exe"
$pdb = Join-Path $releaseAssets "AirScreenshot.pdb"
Copy-Item -LiteralPath $InputExecutable -Destination $executable
Copy-Item -LiteralPath $InputPdb -Destination $pdb

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

if ($Sign) {
    if ([string]::IsNullOrWhiteSpace($CertPassword)) {
        throw "签名时必须显式提供 CertPassword。"
    }
    if ([string]::IsNullOrWhiteSpace($CertPath) -or -not (Test-Path -LiteralPath $CertPath -PathType Leaf)) {
        throw "未找到签名证书：$CertPath"
    }

    $signTool = Find-WindowsSdkTool "signtool.exe"
    & $signTool sign `
        /fd SHA256 `
        /td SHA256 `
        /tr $TimestampUrl `
        /f $CertPath `
        /p $CertPassword `
        $executable
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool 签名失败，退出码：$LASTEXITCODE"
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
$ocrSource = Join-Path $OcrSourceRoot $ocrPackageId
if (Test-Path -LiteralPath $ocrSource -PathType Container) {
    $ocrTarget = Join-Path $site "ocr\$ocrPackageId"
    New-Item -ItemType Directory -Path $ocrTarget -Force | Out-Null
    Copy-Item -Path (Join-Path $ocrSource "*") -Destination $ocrTarget -Recurse -Force

    $files = Get-ChildItem -LiteralPath $ocrTarget -File -Recurse |
        Sort-Object FullName |
        ForEach-Object {
            $relative = [IO.Path]::GetRelativePath($ocrTarget, $_.FullName).Replace("\", "/")
            [ordered]@{
                path = $relative
                url = "https://mmm1h.github.io/air-screenshot/ocr/$ocrPackageId/$relative"
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
                size = $_.Length
            }
        }

    $issuedAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $ocrManifest = [ordered]@{
        schemaVersion = 1
        packageId = $ocrPackageId
        sequence = $ocrManifestSequence
        issuedAt = $issuedAt
        expiresAt = $issuedAt + (180 * 24 * 60 * 60)
        files = @($files)
    } | ConvertTo-Json -Depth 4
    $ocrManifestPath = Join-Path $site "ocr-dependencies.json"
    Set-Content -LiteralPath $ocrManifestPath -Value $ocrManifest -Encoding utf8NoBOM

    if (-not [string]::IsNullOrWhiteSpace($OcrManifestPrivateKeyPem)) {
        $ecdsa = [Security.Cryptography.ECDsa]::Create()
        $privateKeyBytes = $null
        try {
            $pemMatch = [regex]::Match(
                $OcrManifestPrivateKeyPem,
                "\A\s*-----BEGIN PRIVATE KEY-----\s*(?<body>[A-Za-z0-9+/=\s]+?)\s*-----END PRIVATE KEY-----\s*\z",
                [Text.RegularExpressions.RegexOptions]::CultureInvariant
            )
            if (-not $pemMatch.Success) {
                throw "OCR 清单签名私钥必须是单个 PKCS#8 PRIVATE KEY PEM 块。"
            }
            $privateKeyBase64 = [regex]::Replace(
                $pemMatch.Groups["body"].Value,
                "\s",
                ""
            )
            try {
                $privateKeyBytes = [Convert]::FromBase64String($privateKeyBase64)
            }
            catch {
                throw "OCR 清单签名私钥包含无效的 PKCS#8 base64 数据。"
            }
            if ($privateKeyBytes.Length -eq 0) {
                throw "OCR 清单签名私钥 PKCS#8 数据为空。"
            }

            $bytesRead = 0
            try {
                $ecdsa.ImportPkcs8PrivateKey($privateKeyBytes, [ref]$bytesRead)
            }
            catch {
                throw "无法导入 OCR 清单签名 PKCS#8 私钥：$($_.Exception.Message)"
            }
            if ($bytesRead -ne $privateKeyBytes.Length) {
                throw "OCR 清单签名 PKCS#8 私钥包含尾随数据。"
            }
            if ($ecdsa.KeySize -ne 256) {
                throw "OCR 清单签名私钥必须使用 ECDSA P-256。"
            }
            $parameters = $ecdsa.ExportParameters($false)
            $derivedPublicKey = [Convert]::ToHexString(
                [byte[]]($parameters.Q.X + $parameters.Q.Y)
            )
            if ($derivedPublicKey -ne $OcrManifestPublicKeyHex.ToUpperInvariant()) {
                throw "OCR 清单签名私钥与构建时公钥不匹配。"
            }
            $manifestBytes = [IO.File]::ReadAllBytes($ocrManifestPath)
            $signatureBytes = $ecdsa.SignData(
                $manifestBytes,
                [Security.Cryptography.HashAlgorithmName]::SHA256,
                [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation
            )
            if (-not $ecdsa.VerifyData(
                    $manifestBytes,
                    $signatureBytes,
                    [Security.Cryptography.HashAlgorithmName]::SHA256,
                    [Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation
                )) {
                throw "OCR 清单签名自验证失败。"
            }
        }
        finally {
            if ($privateKeyBytes) {
                [Array]::Clear($privateKeyBytes, 0, $privateKeyBytes.Length)
            }
            $ecdsa.Dispose()
        }
        if ($signatureBytes.Length -ne 64) {
            throw "OCR 清单签名长度无效。"
        }
        $ocrSignature = [ordered]@{
            keyId = $OcrManifestKeyId
            signature = [Convert]::ToHexString($signatureBytes).ToLowerInvariant()
        } | ConvertTo-Json -Compress
        Set-Content `
            -LiteralPath (Join-Path $site "ocr-dependencies.json.sig") `
            -Value $ocrSignature `
            -Encoding utf8NoBOM
    }
}

$license = Join-Path $releaseAssets "LICENSE"
$notices = Join-Path $releaseAssets "THIRD_PARTY_NOTICES.md"
Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination $license
Copy-Item -LiteralPath (Join-Path $root "THIRD_PARTY_NOTICES.md") -Destination $notices

$checksumInputs = [ordered]@{
    "AirScreenshot.exe" = $executable
    "AirScreenshot.pdb" = $pdb
    "LICENSE" = $license
    "THIRD_PARTY_NOTICES.md" = $notices
}
$ocrManifestPath = Join-Path $site "ocr-dependencies.json"
if (Test-Path -LiteralPath $ocrManifestPath) {
    $checksumInputs["ocr-dependencies.json"] = $ocrManifestPath
}
$ocrSignaturePath = Join-Path $site "ocr-dependencies.json.sig"
if (Test-Path -LiteralPath $ocrSignaturePath) {
    $checksumInputs["ocr-dependencies.json.sig"] = $ocrSignaturePath
}
$checksumLines = foreach ($entry in $checksumInputs.GetEnumerator()) {
    "$((Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash.ToLowerInvariant()) *$($entry.Key)"
}
Set-Content `
    -LiteralPath (Join-Path $releaseAssets "SHA256SUMS.txt") `
    -Value $checksumLines `
    -Encoding ascii

Write-Host "便携版已生成：$executable"
Write-Host "调试符号与校验和：$releaseAssets"
