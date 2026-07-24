function ConvertTo-AirshotVersion {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$Version,
        [string]$Subject = "Version"
    )

    if ($Version -notmatch "^(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$") {
        throw "$Subject must use canonical X.Y.Z format without leading zeros."
    }

    [uint64]$major = [uint64]::Parse(
        $Matches[1],
        [Globalization.CultureInfo]::InvariantCulture
    )
    [uint64]$minor = [uint64]::Parse(
        $Matches[2],
        [Globalization.CultureInfo]::InvariantCulture
    )
    [uint64]$patch = [uint64]::Parse(
        $Matches[3],
        [Globalization.CultureInfo]::InvariantCulture
    )
    if ($major -gt 9000 -or $minor -gt 65535 -or $patch -gt 65535) {
        throw "$Subject components must satisfy major <= 9000 and minor/patch <= 65535."
    }
    if ($major -eq 0 -and $minor -eq 0 -and $patch -eq 0) {
        throw "$Subject must be greater than 0.0.0."
    }

    return [pscustomobject]@{
        Major = $major
        Minor = $minor
        Patch = $patch
        OcrSequence = [uint64](
            $major * 1000000000000 +
            $minor * 1000000 +
            $patch
        )
    }
}
