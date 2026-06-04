[CmdletBinding()]
param(
    [int]$WarmCaptures = 10
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "build.ps1") -Configuration Release
$hostPath = Join-Path $root "build\AirScreenshot.exe"
$cliPath = Join-Path $root "build\airshot.exe"

& $cliPath app stop 2>$null | Out-Null
Get-Process AirScreenshot -ErrorAction SilentlyContinue | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
$startedHost = Start-Process -FilePath $hostPath -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 500
if ($startedHost.HasExited) {
    throw "Air Screenshot 宿主启动失败，退出码：$($startedHost.ExitCode)"
}
try {
    $process = $startedHost
    $cpuStart = $process.TotalProcessorTime
    Start-Sleep -Seconds 3
    $process.Refresh()
    $cpuDelta = ($process.TotalProcessorTime - $cpuStart).TotalMilliseconds
    $idlePrivateMb = [Math]::Round($process.PrivateMemorySize64 / 1MB, 2)
    $outputPath = Join-Path ([IO.Path]::GetTempPath()) "airshot-performance.png"

    $times = for ($i = 0; $i -lt $WarmCaptures; $i++) {
        $watch = [Diagnostics.Stopwatch]::StartNew()
        & $cliPath capture screen --monitor primary --output file --path $outputPath | Out-Null
        $watch.Stop()
        if ($LASTEXITCODE -ne 0) {
            throw "第 $($i + 1) 次截图失败，CLI 退出码：$LASTEXITCODE"
        }
        $watch.Elapsed.TotalMilliseconds
    }

    Start-Sleep -Milliseconds 500
    $process.Refresh()
    $afterCapturePrivateMb = [Math]::Round($process.PrivateMemorySize64 / 1MB, 2)
    $p95Index = [Math]::Max(0, [Math]::Ceiling($times.Count * 0.95) - 1)
    $p95 = ($times | Sort-Object)[$p95Index]
    [pscustomobject]@{
        IdlePrivateMemoryMB = $idlePrivateMb
        AfterFileCapturesPrivateMemoryMB = $afterCapturePrivateMb
        IdleCpuMillisecondsOver3Seconds = [Math]::Round($cpuDelta, 2)
        WarmScreenFileCaptureP95Milliseconds = [Math]::Round($p95, 2)
        IdlePrivateMemoryBudgetMB = 15
        WarmOverlayMeasurement = "需要手工矩阵测量"
    } | Format-List
}
finally {
    & $cliPath app stop 2>$null | Out-Null
    if ($outputPath -and (Test-Path -LiteralPath $outputPath)) {
        Remove-Item -LiteralPath $outputPath -Force
    }
}
