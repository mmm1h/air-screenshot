#Requires -Version 7.0

function Remove-NonRuntimeRunnerFiles {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [switch]$AllowUnexpectedEmpty
    )

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return
    }

    # Dependencies and PyInstaller are hash-pinned. Keep this list exact so a
    # changed dependency layout cannot silently broaden what the release drops.
    $emptyMetadata = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal
    )
    foreach ($relative in @(
            "_internal/certifi/py.typed",
            "_internal/numpy-2.4.6.dist-info/REQUESTED",
            "_internal/pyreadline3-3.5.6.dist-info/REQUESTED",
            "_internal/tqdm-4.69.0.dist-info/REQUESTED"
        )) {
        $null = $emptyMetadata.Add($relative)
    }

    # jaraco.text's bundled sample is not used by the OCR runner and its space
    # is intentionally outside the signed dependency path grammar.
    $nonRuntimeData = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal
    )
    $null = $nonRuntimeData.Add(
        "_internal/setuptools/_vendor/jaraco/text/Lorem ipsum.txt"
    )

    $unexpected = [Collections.Generic.List[string]]::new()
    foreach ($file in @(Get-ChildItem -LiteralPath $Root -File -Recurse)) {
        $relative = [IO.Path]::GetRelativePath($Root, $file.FullName).Replace("\", "/")
        $remove = $nonRuntimeData.Contains($relative) -or
            ($file.Length -eq 0 -and $emptyMetadata.Contains($relative))
        if ($remove) {
            Write-Host "Removing non-runtime OCR runner file: $relative"
            Remove-Item -LiteralPath $file.FullName -Force
            continue
        }
        if ($file.Length -eq 0) {
            $unexpected.Add($relative)
        }
    }

    if (-not $AllowUnexpectedEmpty -and $unexpected.Count -gt 0) {
        $listed = ($unexpected | Sort-Object) -join ", "
        throw "PyInstaller output contains unexpected empty OCR runner files: $listed"
    }
}

function Get-RunnerOutputFingerprint {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Root)

    $runner = Join-Path $Root "rapidocr_runner.exe"
    $internal = Join-Path $Root "_internal"
    if (-not (Test-Path -LiteralPath $runner -PathType Leaf) -or
        -not (Test-Path -LiteralPath $internal -PathType Container)) {
        return ""
    }

    $files = @(
        Get-Item -LiteralPath $runner
        Get-ChildItem -LiteralPath $internal -File -Recurse
    ) | Sort-Object FullName
    if (@($files | Where-Object Length -EQ 0).Count -gt 0) {
        return ""
    }

    $inventory = foreach ($file in $files) {
        $relative = [IO.Path]::GetRelativePath($Root, $file.FullName).Replace("\", "/")
        "$relative|$($file.Length)|$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
    }
    $inventoryText = $inventory -join "`n"
    return [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData(
            [Text.Encoding]::UTF8.GetBytes($inventoryText)
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
