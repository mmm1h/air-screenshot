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
