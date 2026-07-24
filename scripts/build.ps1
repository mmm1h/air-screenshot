#Requires -Version 7.0

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string[]]$Configuration = @("Release"),
    [string]$Version,
    [string]$OcrManifestKeyId,
    [string]$OcrManifestPublicKeyHex,
    [string]$OcrMinSequence,
    [switch]$Clean
)

$PSNativeCommandUseErrorActionPreference = $true
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "version.ps1")
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content -LiteralPath (Join-Path $root "VERSION") -Raw).Trim()
}
$parsedVersion = ConvertTo-AirshotVersion -Version $Version
if (($OcrManifestKeyId -or $OcrManifestPublicKeyHex) -and
    ($OcrManifestKeyId -notmatch "^[A-Za-z0-9._-]{1,64}$" -or
     $OcrManifestPublicKeyHex -notmatch "^[A-Fa-f0-9]{128}$")) {
    throw "OCR manifest key id and 128-character P-256 public key must be provided together."
}
if ([string]::IsNullOrWhiteSpace($OcrMinSequence)) {
    $OcrMinSequence = [string]$parsedVersion.OcrSequence
}
$parsedOcrMinSequence = [uint64]0
if (-not [uint64]::TryParse(
        $OcrMinSequence,
        [Globalization.NumberStyles]::None,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$parsedOcrMinSequence
    ) -or
    $parsedOcrMinSequence -lt 1 -or
    $parsedOcrMinSequence -gt 9007199254740991) {
    throw "OcrMinSequence must be an integer in 1..2^53-1."
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer (vswhere.exe) was not found."
}

$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    throw "Visual Studio 2022 with the Desktop development with C++ workload was not found."
}

$devShell = Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

$sdkVersionText = "$env:WindowsSDKVersion".TrimEnd("\")
$sdkVersion = $null
if (-not [Version]::TryParse($sdkVersionText, [ref]$sdkVersion) -or $sdkVersion -lt [Version]"10.0.19041.0") {
    throw "Windows SDK 10.0.19041 or newer is required; selected SDK is '$sdkVersionText'."
}
if ($env:VSCMD_ARG_TGT_ARCH -ne "x64") {
    throw "The Visual Studio developer environment did not select the x64 target."
}

$cmakeDirectory = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaDirectory = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$env:PATH = "$cmakeDirectory;$ninjaDirectory;$env:PATH"
$cmake = Join-Path $cmakeDirectory "cmake.exe"
if (-not (Test-Path -LiteralPath $cmake) -or -not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    throw "The Visual Studio CMake and Ninja components are required."
}

Push-Location $root
try {
    foreach ($item in $Configuration) {
        $preset = $item.ToLowerInvariant()
        $configureArguments = @("--preset", $preset)
        if ($Clean) {
            $configureArguments += "--fresh"
        }
        $configureArguments += "-DAIRSHOT_BUILD_VERSION=$Version"
        $configureArguments += "-DAIRSHOT_OCR_MIN_SEQUENCE=$parsedOcrMinSequence"
        $configureArguments += "-DAIRSHOT_OCR_MANIFEST_KEY_ID=$OcrManifestKeyId"
        $configureArguments += "-DAIRSHOT_OCR_MANIFEST_PUBLIC_KEY_HEX=$OcrManifestPublicKeyHex"

        & $cmake @configureArguments
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed with exit code $LASTEXITCODE."
        }
        & $cmake --build --preset "build-$preset"
        if ($LASTEXITCODE -ne 0) {
            throw "CMake build failed with exit code $LASTEXITCODE."
        }
        Write-Host "Build complete: $(Join-Path $root "build\$preset")"
    }
}
finally {
    Pop-Location
}
