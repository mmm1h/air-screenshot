[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$PngPath,
    [string]$IcoPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "src\host\app.ico")
)

if (-not (Test-Path -LiteralPath $PngPath)) {
    throw "Source PNG not found at $PngPath"
}

Add-Type -AssemblyName System.Drawing

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$source = [System.Drawing.Image]::FromFile($PngPath)
$entries = [System.Collections.Generic.List[object]]::new()

try {
    foreach ($size in $sizes) {
        $bitmap = [System.Drawing.Bitmap]::new(
            $size,
            $size,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $memory = [System.IO.MemoryStream]::new()
        try {
            $graphics.Clear([System.Drawing.Color]::Transparent)
            $graphics.CompositingMode =
                [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.CompositingQuality =
                [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
            $graphics.InterpolationMode =
                [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.SmoothingMode =
                [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $graphics.PixelOffsetMode =
                [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.DrawImage(
                $source,
                [System.Drawing.Rectangle]::new(0, 0, $size, $size))
            $bitmap.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
            $entries.Add([pscustomobject]@{
                Size = $size
                Bytes = $memory.ToArray()
            })
        }
        finally {
            $memory.Dispose()
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
}
finally {
    $source.Dispose()
}

$outputDirectory = Split-Path -Parent $IcoPath
if ($outputDirectory) {
    [System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
}

$stream = [System.IO.FileStream]::new(
    $IcoPath,
    [System.IO.FileMode]::Create,
    [System.IO.FileAccess]::Write,
    [System.IO.FileShare]::None)
$writer = [System.IO.BinaryWriter]::new($stream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$entries.Count)

    [uint32]$offset = 6 + (16 * $entries.Count)
    foreach ($entry in $entries) {
        $writer.Write([byte]$(if ($entry.Size -eq 256) { 0 } else { $entry.Size }))
        $writer.Write([byte]$(if ($entry.Size -eq 256) { 0 } else { $entry.Size }))
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$entry.Bytes.Length)
        $writer.Write($offset)
        $offset += [uint32]$entry.Bytes.Length
    }

    foreach ($entry in $entries) {
        $writer.Write([byte[]]$entry.Bytes)
    }
}
finally {
    $writer.Dispose()
    $stream.Dispose()
}

Write-Host "Created $($entries.Count)-size icon at $IcoPath"
