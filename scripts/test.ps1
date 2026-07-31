#Requires -Version 7.0

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string[]]$Configuration = @("Debug", "Release"),
    [string]$Version,
    [string]$OcrManifestKeyId,
    [string]$OcrManifestPublicKeyHex,
    [switch]$Clean
)

$PSNativeCommandUseErrorActionPreference = $true
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot "test-version-contract.ps1")
& (Join-Path $PSScriptRoot "test-ocr-payload-policy.ps1")

$python = (Get-Command python -ErrorAction Stop).Source
$pythonVersion = (& $python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')").Trim()
$pythonVersionParts = $pythonVersion.Split(".")
$pythonMajor = 0
$pythonMinor = 0
if ($LASTEXITCODE -ne 0 -or
    $pythonVersionParts.Count -ne 2 -or
    -not [int]::TryParse($pythonVersionParts[0], [ref]$pythonMajor) -or
    -not [int]::TryParse($pythonVersionParts[1], [ref]$pythonMinor) -or
    $pythonMajor -ne 3 -or
    $pythonMinor -lt 11) {
    throw "RapidOCR runner tests require Python 3.11 or newer; found '$pythonVersion'."
}
& $python -B (Join-Path $root "tests\test_rapidocr_runner.py")
if ($LASTEXITCODE -ne 0) {
    throw "RapidOCR runner tests failed with exit code $LASTEXITCODE."
}

& (Join-Path $PSScriptRoot "build.ps1") `
    -Configuration $Configuration `
    -Version $Version `
    -OcrManifestKeyId $OcrManifestKeyId `
    -OcrManifestPublicKeyHex $OcrManifestPublicKeyHex `
    -Clean:$Clean

$ctest = Get-Command ctest.exe -ErrorAction Stop
Push-Location $root
try {
    foreach ($item in $Configuration) {
        $preset = $item.ToLowerInvariant()
        & $ctest.Source --preset "test-$preset" --no-tests=error
        if ($LASTEXITCODE -ne 0) {
            throw "CTest failed for $item with exit code $LASTEXITCODE."
        }
    }
}
finally {
    Pop-Location
}
