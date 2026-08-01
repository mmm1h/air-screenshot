[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'src\host'),
    [string]$PreviewPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$script:Ink = [System.Drawing.Color]::FromArgb(255, 23, 25, 30)
$script:Paper = [System.Drawing.Color]::FromArgb(255, 247, 249, 255)
$script:Cobalt = [System.Drawing.Color]::FromArgb(255, 77, 124, 254)
$script:Cyan = [System.Drawing.Color]::FromArgb(255, 26, 175, 181)

function New-RoundedRectanglePath {
    param(
        [float]$X,
        [float]$Y,
        [float]$Width,
        [float]$Height,
        [float]$Radius
    )

    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = [Math]::Max(0.0, $Radius * 2.0)
    if ($diameter -le 0.0) {
        $path.AddRectangle([System.Drawing.RectangleF]::new($X, $Y, $Width, $Height))
        return $path
    }
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-AppIconBitmap {
    param(
        [Parameter(Mandatory)]
        [ValidateSet('focus', 'flow', 'pixel')]
        [string]$Style,
        [Parameter(Mandatory)]
        [ValidateRange(16, 512)]
        [int]$Size
    )

    $bitmap = [System.Drawing.Bitmap]::new(
        $Size,
        $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $graphics.SmoothingMode = if ($Size -le 24) {
        [System.Drawing.Drawing2D.SmoothingMode]::None
    } else {
        [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    }

    $inkBrush = [System.Drawing.SolidBrush]::new($script:Ink)
    $paperBrush = [System.Drawing.SolidBrush]::new($script:Paper)
    $cobaltBrush = [System.Drawing.SolidBrush]::new($script:Cobalt)
    $cyanBrush = [System.Drawing.SolidBrush]::new($script:Cyan)
    try {
        switch ($Style) {
            'focus' {
                $margin = [Math]::Max(1, [Math]::Round($Size * 0.09375))
                $tileSize = $Size - 2 * $margin
                $radius = [Math]::Max(2.0, $Size * 0.1875)
                $tile = New-RoundedRectanglePath $margin $margin $tileSize $tileSize $radius
                try {
                    $graphics.FillPath($inkBrush, $tile)
                } finally {
                    $tile.Dispose()
                }

                $stroke = if ($Size -le 20) {
                    1.0
                } elseif ($Size -le 32) {
                    2.0
                } else {
                    [Math]::Max(2.0, [Math]::Round($Size * 0.046875))
                }
                $pen = [System.Drawing.Pen]::new($script:Paper, $stroke)
                try {
                    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
                    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
                    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
                    $p0 = [Math]::Round($Size * 0.3046875)
                    $p1 = [Math]::Round($Size * 0.4453125)
                    $p2 = [Math]::Round($Size * 0.5546875)
                    $p3 = [Math]::Round($Size * 0.6953125)
                    $graphics.DrawLine($pen, $p0, $p1, $p0, $p0)
                    $graphics.DrawLine($pen, $p0, $p0, $p1, $p0)
                    $graphics.DrawLine($pen, $p2, $p3, $p3, $p3)
                    $graphics.DrawLine($pen, $p3, $p2, $p3, $p3)
                } finally {
                    $pen.Dispose()
                }
                $focusSize = [Math]::Max(2, [Math]::Round($Size * 0.171875))
                $focusOrigin = [Math]::Floor(($Size - $focusSize) / 2.0)
                $focusRadius = [Math]::Max(1.0, $focusSize * 0.22)
                $focus = New-RoundedRectanglePath $focusOrigin $focusOrigin $focusSize $focusSize $focusRadius
                try {
                    $graphics.FillPath($cobaltBrush, $focus)
                } finally {
                    $focus.Dispose()
                }
            }
            'flow' {
                $margin = [Math]::Round($Size * 0.125)
                $diameter = $Size - 2 * $margin
                $graphics.FillEllipse($inkBrush, $margin, $margin, $diameter, $diameter)
                $ringWidth = if ($Size -le 20) {
                    1.0
                } elseif ($Size -le 24) {
                    2.0
                } else {
                    [Math]::Max(2.0, [Math]::Round($Size * 0.125))
                }
                $orbitInset = $margin + $ringWidth / 2.0
                $orbitSize = $Size - 2.0 * $orbitInset
                $orbitSweep = if ($Size -le 24) { 240.0 } else { 270.0 }
                $orbitPen = [System.Drawing.Pen]::new($script:Cobalt, $ringWidth)
                try {
                    $orbitPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Flat
                    $orbitPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Flat
                    $graphics.DrawArc(
                        $orbitPen,
                        $orbitInset,
                        $orbitInset,
                        $orbitSize,
                        $orbitSize,
                        0.0,
                        $orbitSweep)
                } finally {
                    $orbitPen.Dispose()
                }
                $scanHeight = [Math]::Max(1, [Math]::Round($Size * 0.0546875))
                $scanX = [Math]::Round($Size * 0.5)
                $scanY = [Math]::Floor(($Size - $scanHeight) / 2.0)
                $scanWidth = [Math]::Max(2, [Math]::Round($Size * 0.25))
                $graphics.FillRectangle(
                    $cobaltBrush,
                    $scanX,
                    $scanY,
                    $scanWidth,
                    $scanHeight)
                $locator = [Math]::Max(1, [Math]::Round($Size * 0.0625))
                $locatorX = [Math]::Round($Size * 0.71875)
                $locatorY = [Math]::Round($Size * 0.484375)
                $graphics.FillRectangle($cyanBrush, $locatorX, $locatorY, $locator, $locator)
            }
            'pixel' {
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
                $target = [Math]::Round($Size * 0.625)
                $gap = [Math]::Max(1, [Math]::Round($Size / 64.0))
                $cell = [Math]::Max(2, [Math]::Round(($target - 2 * $gap) / 3.0))
                $grid = 3 * $cell + 2 * $gap
                $origin = [Math]::Floor(($Size - $grid) / 2.0)
                for ($row = 0; $row -lt 3; $row++) {
                    for ($column = 0; $column -lt 3; $column++) {
                        $brush = if ($row -eq 1 -and $column -eq 1) {
                            $cyanBrush
                        } elseif (($row + $column) % 2 -eq 1) {
                            $cobaltBrush
                        } else {
                            $inkBrush
                        }
                        $x = $origin + $column * ($cell + $gap)
                        $y = $origin + $row * ($cell + $gap)
                        $graphics.FillRectangle($brush, $x, $y, $cell, $cell)
                    }
                }
            }
        }
    } finally {
        $inkBrush.Dispose()
        $paperBrush.Dispose()
        $cobaltBrush.Dispose()
        $cyanBrush.Dispose()
        $graphics.Dispose()
    }
    return $bitmap
}

function Get-PngBytes {
    param([System.Drawing.Bitmap]$Bitmap)
    $stream = [System.IO.MemoryStream]::new()
    try {
        $Bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        return $stream.ToArray()
    } finally {
        $stream.Dispose()
    }
}

function Write-MultiImageIcon {
    param(
        [Parameter(Mandatory)]
        [ValidateSet('focus', 'flow', 'pixel')]
        [string]$Style,
        [Parameter(Mandatory)]
        [string]$Path
    )

    $sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
    $images = [System.Collections.Generic.List[byte[]]]::new()
    foreach ($size in $sizes) {
        $bitmap = New-AppIconBitmap -Style $Style -Size $size
        try {
            $images.Add((Get-PngBytes -Bitmap $bitmap))
        } finally {
            $bitmap.Dispose()
        }
    }

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create)
    $writer = [System.IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$sizes.Count)
        $offset = 6 + 16 * $sizes.Count
        for ($index = 0; $index -lt $sizes.Count; $index++) {
            $size = $sizes[$index]
            $bytes = $images[$index]
            $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
            $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]32)
            $writer.Write([uint32]$bytes.Length)
            $writer.Write([uint32]$offset)
            $offset += $bytes.Length
        }
        foreach ($bytes in $images) {
            $writer.Write($bytes)
        }
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
Write-MultiImageIcon -Style focus -Path (Join-Path $OutputDirectory 'app.ico')
Write-MultiImageIcon -Style flow -Path (Join-Path $OutputDirectory 'app-flow.ico')
Write-MultiImageIcon -Style pixel -Path (Join-Path $OutputDirectory 'app-pixel.ico')
$logo = New-AppIconBitmap -Style focus -Size 512
try {
    $logo.Save(
        (Join-Path $OutputDirectory 'logo.png'),
        [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $logo.Dispose()
}

if (-not [string]::IsNullOrWhiteSpace($PreviewPath)) {
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $PreviewPath)) | Out-Null
    $preview = [System.Drawing.Bitmap]::new(1200, 620)
    $graphics = [System.Drawing.Graphics]::FromImage($preview)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([System.Drawing.Color]::FromArgb(255, 246, 248, 252))
    $titleFont = [System.Drawing.Font]::new('Segoe UI', 24, [System.Drawing.FontStyle]::Bold)
    $nameFont = [System.Drawing.Font]::new('Microsoft YaHei UI', 15, [System.Drawing.FontStyle]::Bold)
    $captionFont = [System.Drawing.Font]::new('Microsoft YaHei UI', 10, [System.Drawing.FontStyle]::Regular)
    $textBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 21, 32, 51))
    $mutedBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 99, 112, 138))
    $darkBrush = [System.Drawing.SolidBrush]::new($script:Ink)
    try {
        $graphics.DrawString('Air Screenshot · 应用图标系统', $titleFont, $textBrush, 48, 34)
        $items = @(
            @{ Style = 'focus'; Name = '精准取景'; Caption = '角框定位 · 默认款' },
            @{ Style = 'flow'; Name = '流光镜'; Caption = '连续捕捉 · 动态款' },
            @{ Style = 'pixel'; Name = '像素舱'; Caption = '模块输出 · 技术款' }
        )
        for ($index = 0; $index -lt $items.Count; $index++) {
            $item = $items[$index]
            $left = 48 + $index * 384
            $master = New-AppIconBitmap -Style $item.Style -Size 256
            try {
                $graphics.DrawImage($master, $left + 72, 108, 192, 192)
            } finally {
                $master.Dispose()
            }
            $graphics.DrawString($item.Name, $nameFont, $textBrush, $left + 72, 320)
            $graphics.DrawString($item.Caption, $captionFont, $mutedBrush, $left + 72, 354)
            $graphics.FillRectangle($darkBrush, $left + 36, 404, 312, 116)
            $sampleSizes = @(64, 32, 20, 16)
            $sampleX = $left + 58
            foreach ($sampleSize in $sampleSizes) {
                $sample = New-AppIconBitmap -Style $item.Style -Size $sampleSize
                try {
                    $y = 430 + [Math]::Floor((64 - $sampleSize) / 2.0)
                    $graphics.DrawImageUnscaled($sample, $sampleX, $y)
                    $graphics.DrawString(
                        "$sampleSize px",
                        $captionFont,
                        [System.Drawing.Brushes]::White,
                        $sampleX - 2,
                        496)
                } finally {
                    $sample.Dispose()
                }
                $sampleX += 76
            }
        }
        $preview.Save($PreviewPath, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $darkBrush.Dispose()
        $mutedBrush.Dispose()
        $textBrush.Dispose()
        $captionFont.Dispose()
        $nameFont.Dispose()
        $titleFont.Dispose()
        $graphics.Dispose()
        $preview.Dispose()
    }
}

Write-Host "Generated Focus Frame, Flow Lens, Pixel Console, and the 512 px logo in $OutputDirectory"
