#Requires -Version 7.0

$ErrorActionPreference = "Stop"
$scriptsRoot = $PSScriptRoot
. (Join-Path $scriptsRoot "ocr-runner-payload.ps1")

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

$fixture = Join-Path ([IO.Path]::GetTempPath()) (
    "airshot-ocr-payload-policy-" + [guid]::NewGuid().ToString("N")
)
$outside = "$fixture-outside.txt"
$resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd(
    [IO.Path]::DirectorySeparatorChar
)
$resolvedFixture = [IO.Path]::GetFullPath($fixture)
if (-not $resolvedFixture.StartsWith(
        $resolvedTemp + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    ) -or
    -not [IO.Path]::GetFileName($resolvedFixture).StartsWith(
        "airshot-ocr-payload-policy-",
        [StringComparison]::Ordinal
    )) {
    throw "Unsafe OCR payload policy fixture path."
}

function Add-FixtureFile {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [byte[]]$Bytes = [byte[]](1, 2, 3, 4)
    )
    $path = Join-Path $fixture $RelativePath
    [IO.Directory]::CreateDirectory((Split-Path -Parent $path)) | Out-Null
    [IO.File]::WriteAllBytes($path, $Bytes)
    return $path
}

try {
    $legacy = Add-FixtureFile -RelativePath "_internal/python311.dll"
    $retired = Add-FixtureFile -RelativePath "DirectML.dll"
    [IO.File]::WriteAllBytes($outside, [byte[]](9))
    Remove-LegacyOcrRunnerFiles -Root $fixture
    Assert-True -Condition (-not (Test-Path -LiteralPath $legacy)) `
        -Message "Legacy Python runtime was not removed."
    Assert-True -Condition (-not (Test-Path -LiteralPath $retired)) `
        -Message "Retired DirectML runtime was not removed."
    Assert-True -Condition (Test-Path -LiteralPath $outside -PathType Leaf) `
        -Message "Legacy cleanup escaped the OCR root."
    Remove-LegacyOcrRunnerFiles -Root $fixture

    $fingerprintRoot = Join-Path $fixture "fingerprint"
    foreach ($relative in Get-NativeOcrRuntimeFiles) {
        Add-FixtureFile -RelativePath "fingerprint/$relative" | Out-Null
    }
    $firstFingerprint = Get-RunnerOutputFingerprint -Root $fingerprintRoot
    Assert-True `
        -Condition ($firstFingerprint -cmatch "^[A-F0-9]{64}$") `
        -Message "A complete native OCR runtime did not produce a fingerprint."
    $runner = Join-Path $fingerprintRoot "rapidocr_runner.exe"
    [IO.File]::WriteAllBytes($runner, [byte[]](4, 3, 2, 1))
    $secondFingerprint = Get-RunnerOutputFingerprint -Root $fingerprintRoot
    Assert-True -Condition ($secondFingerprint -cne $firstFingerprint) `
        -Message "Native runner fingerprint did not detect a content change."
    [IO.File]::WriteAllBytes((Join-Path $fingerprintRoot "onnxruntime.dll"), [byte[]]::new(0))
    Assert-True `
        -Condition ((Get-RunnerOutputFingerprint -Root $fingerprintRoot) -ceq "") `
        -Message "A truncated native OCR runtime did not request a rebuild."

    foreach ($valid in @(
            "rapidocr_runner.exe",
            "onnxruntime.dll",
            "licenses/paddle-ocr-rs-LICENSE.txt",
            "models/rapidocr-v5-fast/det.onnx"
        )) {
        Assert-True -Condition (Test-OcrManifestRelativePath -Path $valid) `
            -Message "Valid signed dependency path was rejected: $valid"
    }
    foreach ($invalid in @(
            "licenses/Third Party.txt",
            "../escape.dll",
            "/rooted.dll",
            "models//det.onnx",
            "models/CON/file.onnx",
            "models/trailing./file.onnx"
        )) {
        Assert-True -Condition (-not (Test-OcrManifestRelativePath -Path $invalid)) `
            -Message "Invalid signed dependency path was accepted: $invalid"
    }

    Write-Host "Native OCR payload policy tests passed."
}
finally {
    if (Test-Path -LiteralPath $outside) {
        Remove-Item -LiteralPath $outside -Force
    }
    if (Test-Path -LiteralPath $fixture) {
        [IO.Directory]::Delete($fixture, $true)
    }
}
