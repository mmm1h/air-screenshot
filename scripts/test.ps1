#Requires -Version 7.0

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string[]]$Configuration = @("Debug", "Release"),
    [string]$Version,
    [string]$OcrManifestKeyId,
    [string]$OcrManifestPublicKeyHex,
    [string]$CargoExecutable,
    [switch]$Clean
)

$PSNativeCommandUseErrorActionPreference = $true
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot "test-version-contract.ps1")
& (Join-Path $PSScriptRoot "test-ocr-payload-policy.ps1")

& (Join-Path $PSScriptRoot "build.ps1") `
    -Configuration $Configuration `
    -Version $Version `
    -OcrManifestKeyId $OcrManifestKeyId `
    -OcrManifestPublicKeyHex $OcrManifestPublicKeyHex `
    -Clean:$Clean

$cargo = if (-not [string]::IsNullOrWhiteSpace($CargoExecutable)) {
    (Get-Command $CargoExecutable -ErrorAction Stop).Source
} else {
    (Get-Command cargo.exe -ErrorAction Stop).Source
}
$env:CARGO_TARGET_DIR = Join-Path $root "build\ocr-worker-tests"
try {
    & $cargo fmt `
        --manifest-path (Join-Path $root "ocr-worker\Cargo.toml") `
        -- `
        --check
    if ($LASTEXITCODE -ne 0) {
        throw "Native OCR worker formatting check failed with exit code $LASTEXITCODE."
    }
    & $cargo clippy `
        --manifest-path (Join-Path $root "ocr-worker\Cargo.toml") `
        --all-targets `
        --locked `
        --target x86_64-pc-windows-msvc `
        -- `
        -D warnings
    if ($LASTEXITCODE -ne 0) {
        throw "Native OCR worker lint failed with exit code $LASTEXITCODE."
    }
    & $cargo test `
        --manifest-path (Join-Path $root "ocr-worker\Cargo.toml") `
        --locked `
        --target x86_64-pc-windows-msvc
    if ($LASTEXITCODE -ne 0) {
        throw "Native OCR worker tests failed with exit code $LASTEXITCODE."
    }
}
finally {
    Remove-Item Env:\CARGO_TARGET_DIR -ErrorAction SilentlyContinue
}

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
