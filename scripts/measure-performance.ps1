[CmdletBinding()]
param(
    [int]$WarmCaptures = 10
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "build.ps1") -Configuration Release
$hostPath = Join-Path $root "build\AirScreenshot.exe"
$cliPath = $hostPath
if (Get-Process AirScreenshot -ErrorAction SilentlyContinue) {
    throw "性能测量前请先退出现有 Air Screenshot 进程。"
}
$temporary = Join-Path ([IO.Path]::GetTempPath()) "AirScreenshot-Performance-$PID"
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$originalDataDirectory = $env:AIRSHOT_DATA_DIR
$originalRunValue = $null
$hadRunValue = $false
try {
    $originalRunValue = (Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot -ErrorAction Stop).AirScreenshot
    $hadRunValue = $true
}
catch {
}
$env:AIRSHOT_DATA_DIR = Join-Path $temporary "data"

$startedHost = $null
$outputPath = $null
try {
    New-Item -ItemType Directory -Path $temporary -Force | Out-Null
    $startedHost = Start-Process -FilePath $hostPath -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 500
    if ($startedHost.HasExited) {
        throw "Air Screenshot 宿主启动失败，退出码：$($startedHost.ExitCode)"
    }
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
    if ($startedHost -and -not $startedHost.HasExited) {
        & $cliPath app stop 2>$null | Out-Null
        $startedHost.WaitForExit(5000) | Out-Null
    }
    if ($outputPath -and (Test-Path -LiteralPath $outputPath)) {
        Remove-Item -LiteralPath $outputPath -Force
    }
    if ($hadRunValue) {
        Set-ItemProperty -LiteralPath $runKey -Name AirScreenshot -Value $originalRunValue
    } else {
        Remove-ItemProperty -LiteralPath $runKey -Name AirScreenshot -ErrorAction SilentlyContinue
    }
    if ($null -eq $originalDataDirectory) {
        Remove-Item Env:\AIRSHOT_DATA_DIR -ErrorAction SilentlyContinue
    } else {
        $env:AIRSHOT_DATA_DIR = $originalDataDirectory
    }
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
