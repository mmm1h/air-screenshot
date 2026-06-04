[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$PngPath,
    [string]$IcoPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "src\host\app.ico")
)

if (-not (Test-Path -LiteralPath $PngPath)) {
    throw "Source PNG not found at $PngPath"
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# Resize to standard icon size (e.g. 256x256) using System.Drawing to ensure compatibility
$srcImage = [System.Drawing.Image]::FromFile($PngPath)
$bitmap = New-Object System.Drawing.Bitmap(256, 256)
$g = [System.Drawing.Graphics]::FromImage($bitmap)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.DrawImage($srcImage, 0, 0, 256, 256)

$icon = [System.Drawing.Icon]::FromHandle($bitmap.GetHicon())
$fileStream = New-Object System.IO.FileStream($IcoPath, [System.IO.FileMode]::Create)
$icon.Save($fileStream)

$fileStream.Close()
$fileStream.Dispose()
$icon.Dispose()
$g.Dispose()
$bitmap.Dispose()
$srcImage.Dispose()

Write-Host "Icon created successfully via System.Drawing at $IcoPath"
