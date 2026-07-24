#Requires -Version 7.0

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "version.ps1")

$validVersions = @(
    @{
        Version = "0.0.1"
        Sequence = [uint64]1
    },
    @{
        Version = "0.2.3"
        Sequence = [uint64]2000003
    },
    @{
        Version = "9000.65535.65535"
        Sequence = [uint64]9000065535065535
    }
)
foreach ($test in $validVersions) {
    $parsed = ConvertTo-AirshotVersion -Version $test.Version
    if ($parsed.OcrSequence -ne $test.Sequence) {
        throw "Version contract produced the wrong OCR sequence for $($test.Version)."
    }
}

$invalidVersions = @(
    "",
    "0.0.0",
    "00.0.1",
    "0.00.1",
    "0.0.01",
    "1.2",
    "1.2.3.4",
    "1.2.3-preview",
    "12345678901234567890.0.0",
    "$([char]0xFF11).2.3",
    "9001.0.0",
    "1.65536.0",
    "1.0.65536"
)
foreach ($version in $invalidVersions) {
    $accepted = $true
    try {
        $null = ConvertTo-AirshotVersion -Version $version
    }
    catch {
        $accepted = $false
    }
    if ($accepted) {
        throw "Version contract accepted invalid version '$version'."
    }
}

Write-Host "Version contract tests passed."
