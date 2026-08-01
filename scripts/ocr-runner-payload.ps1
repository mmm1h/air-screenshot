#Requires -Version 7.0

$script:NativeOcrRuntimeFiles = @(
    "rapidocr_runner.exe",
    "onnxruntime.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)

function Get-NativeOcrRuntimeFiles {
    [CmdletBinding()]
    param()
    return @($script:NativeOcrRuntimeFiles)
}

function Remove-LegacyOcrRunnerFiles {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Root)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return
    }
    $resolvedRoot = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar
    )
    $legacyInternal = [IO.Path]::GetFullPath((Join-Path $resolvedRoot "_internal"))
    if (-not $legacyInternal.StartsWith(
            $resolvedRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase
        ) -or
        [IO.Path]::GetFileName($legacyInternal) -cne "_internal") {
        throw "Unsafe legacy OCR runtime cleanup path: $legacyInternal"
    }
    if (Test-Path -LiteralPath $legacyInternal -PathType Container) {
        Remove-Item -LiteralPath $legacyInternal -Recurse -Force
    }
    foreach ($retiredFile in @("rapidocr_api.dll", "DirectML.dll")) {
        $path = Join-Path $resolvedRoot $retiredFile
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
        }
    }
}

function Get-RunnerOutputFingerprint {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Root)

    $files = [Collections.Generic.List[IO.FileInfo]]::new()
    foreach ($relative in Get-NativeOcrRuntimeFiles) {
        $path = Join-Path $Root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            return ""
        }
        $file = Get-Item -LiteralPath $path
        if ($file.Length -le 0 -or
            ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            return ""
        }
        $files.Add($file)
    }

    $inventory = foreach ($file in $files | Sort-Object FullName) {
        $relative = [IO.Path]::GetRelativePath($Root, $file.FullName).Replace("\", "/")
        "$relative|$($file.Length)|$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
    }
    return [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData(
            [Text.Encoding]::UTF8.GetBytes($inventory -join "`n")
        )
    )
}

function Test-OcrManifestRelativePath {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrEmpty($Path) -or $Path.Length -gt 240 -or
        $Path.StartsWith("\", [StringComparison]::Ordinal) -or
        $Path.StartsWith("/", [StringComparison]::Ordinal) -or
        $Path.EndsWith("\", [StringComparison]::Ordinal) -or
        $Path.EndsWith("/", [StringComparison]::Ordinal)) {
        return $false
    }

    $normalized = $Path.Replace("\", "/")
    foreach ($component in $normalized.Split("/")) {
        if ([string]::IsNullOrEmpty($component) -or
            $component -in @(".", "..") -or
            $component.EndsWith(".", [StringComparison]::Ordinal) -or
            $component.EndsWith(" ", [StringComparison]::Ordinal) -or
            $component -cnotmatch "^[A-Za-z0-9._-]+$") {
            return $false
        }

        $base = $component.Split(".", 2)[0].ToUpperInvariant()
        if ($base -in @("CON", "PRN", "AUX", "NUL") -or
            $base -match "^(COM|LPT)[1-9]$") {
            return $false
        }
    }
    return $true
}
