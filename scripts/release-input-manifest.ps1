#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Root,
    [switch]$Verify
)

$ErrorActionPreference = "Stop"

$rootPath = if ([IO.Path]::IsPathFullyQualified($Root)) {
    [IO.Path]::GetFullPath($Root)
}
else {
    [IO.Path]::GetFullPath((Join-Path (Get-Location) $Root))
}
if (-not (Test-Path -LiteralPath $rootPath -PathType Container)) {
    throw "Release input root does not exist: $rootPath"
}

$manifestName = "release-input.sha256.json"
$manifestPath = Join-Path $rootPath $manifestName
$rootPrefix = $rootPath.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar

function Get-RelativeInputPath {
    param([Parameter(Mandatory)][string]$Path)

    return [IO.Path]::GetRelativePath($rootPath, $Path).Replace('\', '/')
}

function Get-InputFiles {
    return @(
        Get-ChildItem -LiteralPath $rootPath -Recurse -File |
            Where-Object {
                -not $_.FullName.Equals(
                    $manifestPath,
                    [StringComparison]::OrdinalIgnoreCase
                )
            } |
            Sort-Object {
                Get-RelativeInputPath -Path $_.FullName
            }
    )
}

if (-not $Verify) {
    $entries = @(
        foreach ($file in Get-InputFiles) {
            [ordered]@{
                path = Get-RelativeInputPath -Path $file.FullName
                size = [Int64]$file.Length
                sha256 = (
                    Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256
                ).Hash.ToLowerInvariant()
            }
        }
    )
    if ($entries.Count -eq 0) {
        throw "Release input root contains no files."
    }
    [ordered]@{
        schemaVersion = 1
        files = $entries
    } |
        ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
    Write-Host "Release input manifest created: $manifestPath"
    return
}

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Release input manifest is missing: $manifestPath"
}
$document = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$documentProperties = @($document.PSObject.Properties.Name)
if ($documentProperties.Count -ne 2 -or
    $documentProperties -notcontains "schemaVersion" -or
    $documentProperties -notcontains "files" -or
    [Int64]$document.schemaVersion -ne 1) {
    throw "Release input manifest schema is invalid."
}

$expected = @{}
foreach ($entry in @($document.files)) {
    $properties = @($entry.PSObject.Properties.Name)
    $relative = [string]$entry.path
    if ($properties.Count -ne 3 -or
        $properties -notcontains "path" -or
        $properties -notcontains "size" -or
        $properties -notcontains "sha256" -or
        [string]::IsNullOrWhiteSpace($relative) -or
        $relative.Contains('\') -or
        [IO.Path]::IsPathFullyQualified($relative) -or
        @($relative.Split('/')) -contains ".." -or
        $relative -eq $manifestName -or
        [string]$entry.sha256 -notmatch "^[A-Fa-f0-9]{64}$" -or
        [Int64]$entry.size -lt 0 -or
        $expected.ContainsKey($relative)) {
        throw "Release input manifest entry is invalid: $relative"
    }

    $fullPath = [IO.Path]::GetFullPath(
        (Join-Path $rootPath $relative.Replace('/', [IO.Path]::DirectorySeparatorChar))
    )
    if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Release input manifest path escapes its root: $relative"
    }
    $expected[$relative] = $entry
}
if ($expected.Count -eq 0) {
    throw "Release input manifest contains no files."
}

$actualFiles = @(Get-InputFiles)
if ($actualFiles.Count -ne $expected.Count) {
    throw "Release input file count does not match its manifest."
}
foreach ($file in $actualFiles) {
    $relative = Get-RelativeInputPath -Path $file.FullName
    if (-not $expected.ContainsKey($relative)) {
        throw "Unexpected release input file: $relative"
    }
    $entry = $expected[$relative]
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    if ([Int64]$file.Length -ne [Int64]$entry.size -or
        $hash -ne ([string]$entry.sha256).ToUpperInvariant()) {
        throw "Release input file does not match its manifest: $relative"
    }
}

Write-Host "Release input manifest verified: $manifestPath"
