[CmdletBinding()]
param(
    [string]$Executable = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\AirScreenshot.exe")
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$temporary = Join-Path ([IO.Path]::GetTempPath()) "AirScreenshot-Smoke-$PID"
$firstDirectory = Join-Path $temporary "first"
$movedDirectory = Join-Path $temporary "moved"
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

function Invoke-AirScreenshot {
    param([string]$Path, [string[]]$Arguments)
    $stdout = Join-Path $temporary "stdout-$([Guid]::NewGuid()).txt"
    $stderr = Join-Path $temporary "stderr-$([Guid]::NewGuid()).txt"
    try {
        $process = Start-Process -FilePath $Path -ArgumentList $Arguments -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $process.WaitForExit()
        [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = if (Test-Path $stdout) { ((Get-Content $stdout -ErrorAction SilentlyContinue) -join "`n").Trim() } else { "" }
            Error = if (Test-Path $stderr) { ((Get-Content $stderr -ErrorAction SilentlyContinue) -join "`n").Trim() } else { "" }
        }
    }
    finally {
        Remove-Item $stdout, $stderr -Force -ErrorAction SilentlyContinue
    }
}

try {
    $existing = Get-Process AirScreenshot -ErrorAction SilentlyContinue
    if ($existing) {
        throw "运行便携烟测前请先退出现有 Air Screenshot 进程。"
    }
    New-Item -ItemType Directory -Path $firstDirectory, $movedDirectory -Force | Out-Null
    $firstExecutable = Join-Path $firstDirectory "AirScreenshot.exe"
    $movedExecutable = Join-Path $movedDirectory "AirScreenshot.exe"
    Copy-Item -LiteralPath $Executable -Destination $firstExecutable

    $version = Invoke-AirScreenshot $firstExecutable @("--version")
    if ($version.ExitCode -ne 0 -or $version.Output -notmatch "^AirScreenshot \d+\.\d+\.\d+$") {
        throw "单 EXE 版本命令失败：$($version.Output) $($version.Error)"
    }

    $start = Invoke-AirScreenshot $firstExecutable @("app", "start")
    if ($start.ExitCode -ne 0) { throw "宿主启动失败：$($start.Error)" }
    Start-Sleep -Milliseconds 500
    $status = Invoke-AirScreenshot $firstExecutable @("app", "status")
    if ($status.ExitCode -ne 0 -or $status.Output -notmatch "正在运行") { throw "宿主状态检查失败。" }
    $capturePath = Join-Path $temporary "capture.png"
    $capture = Invoke-AirScreenshot $firstExecutable @("capture", "screen", "--monitor", "primary", "--output", "file", "--path", "`"$capturePath`"")
    if ($capture.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $capturePath) -or (Get-Item $capturePath).Length -eq 0) {
        throw "全屏截图与文件保存失败：$($capture.Output) $($capture.Error)"
    }
    $runValue = (Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot).AirScreenshot
    if ($runValue -ne "`"$firstExecutable`"") { throw "首次启动未写入正确启动项：$runValue" }
    Invoke-AirScreenshot $firstExecutable @("app", "stop") | Out-Null
    Get-Process AirScreenshot -ErrorAction SilentlyContinue | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue

    Move-Item -LiteralPath $firstExecutable -Destination $movedExecutable
    Invoke-AirScreenshot $movedExecutable @("app", "start") | Out-Null
    Start-Sleep -Milliseconds 500
    $runValue = (Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot).AirScreenshot
    if ($runValue -ne "`"$movedExecutable`"") { throw "移动后未修复启动项：$runValue" }
    Invoke-AirScreenshot $movedExecutable @("app", "stop") | Out-Null
    Get-Process AirScreenshot -ErrorAction SilentlyContinue | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue

    Set-Content -LiteralPath (Join-Path $env:AIRSHOT_DATA_DIR "config.json") `
        -Value '{"shell":{"enabled":true,"startAtLogin":false}}' -Encoding utf8NoBOM
    Invoke-AirScreenshot $movedExecutable @("app", "start") | Out-Null
    Start-Sleep -Milliseconds 500
    if ((Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot -ErrorAction SilentlyContinue).AirScreenshot) {
        throw "关闭开机启动后未删除启动项。"
    }
    Invoke-AirScreenshot $movedExecutable @("app", "stop") | Out-Null
    Get-Process AirScreenshot -ErrorAction SilentlyContinue | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue

    Set-ItemProperty -LiteralPath $movedExecutable -Name IsReadOnly -Value $true
    $readOnlyCheck = Invoke-AirScreenshot $movedExecutable @("--check-update-target")
    Set-ItemProperty -LiteralPath $movedExecutable -Name IsReadOnly -Value $false
    if ($readOnlyCheck.ExitCode -eq 0) {
        throw "便携更新未拒绝只读目标。"
    }

    $replacementTarget = Join-Path $temporary "replacement.exe"
    Set-Content -LiteralPath $replacementTarget -Value "old"
    $updater = Start-Process -FilePath $movedExecutable `
        -ArgumentList @("--apply-update", "`"$replacementTarget`"", "0", "no-restart") `
        -Wait -PassThru -WindowStyle Hidden
    if ($updater.ExitCode -ne 0 -or
        (Get-FileHash $replacementTarget -Algorithm SHA256).Hash -ne (Get-FileHash $movedExecutable -Algorithm SHA256).Hash) {
        throw "便携更新替换器未正确覆盖目标文件。"
    }

    Write-Host "便携单 EXE 烟测通过。"
}
finally {
    Get-Process AirScreenshot -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($temporary, [StringComparison]::OrdinalIgnoreCase) } |
        Stop-Process -Force -ErrorAction SilentlyContinue
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
