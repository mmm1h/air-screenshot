[CmdletBinding()]
param(
    [string]$Executable = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\release\bin\AirScreenshot.exe")
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
        if (-not $process.WaitForExit(30000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw "Air Screenshot command timed out after 30 seconds: $Path $($Arguments -join ' ')"
        }
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

function Wait-AirScreenshotProcessesExited {
    param([int]$TimeoutMilliseconds = 5000)

    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $remaining = @(
            Get-Process AirScreenshot -ErrorAction SilentlyContinue |
                Where-Object {
                    try {
                        $_.Path -and $_.Path.StartsWith(
                            $temporary,
                            [StringComparison]::OrdinalIgnoreCase
                        )
                    }
                    catch {
                        $false
                    }
                }
        )
        if ($remaining.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMilliseconds)

    $details = ($remaining | Select-Object Id, Path | Out-String).Trim()
    throw "Air Screenshot processes did not exit within $TimeoutMilliseconds ms: $details"
}

function Stop-AirScreenshot {
    param([Parameter(Mandatory)][string]$Path)

    $stop = Invoke-AirScreenshot $Path @("app", "stop")
    if ($stop.ExitCode -ne 0) {
        throw "宿主停止失败：$($stop.Output) $($stop.Error)"
    }
    Wait-AirScreenshotProcessesExited
}

function Test-PngSignature {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }

    $stream = [IO.File]::OpenRead($Path)
    try {
        $signature = [byte[]]::new(8)
        if ($stream.Read($signature, 0, $signature.Length) -ne $signature.Length) {
            return $false
        }
        return [Convert]::ToHexString($signature) -eq "89504E470D0A1A0A"
    }
    finally {
        $stream.Dispose()
    }
}

function Invoke-DirectoryCapture {
    param(
        [Parameter(Mandatory)][string]$ExecutablePath,
        [Parameter(Mandatory)][string]$RequestedPath,
        [Parameter(Mandatory)][string]$ExpectedDirectory
    )

    $capture = Invoke-AirScreenshot $ExecutablePath @(
        "capture", "screen", "--monitor", "primary", "--output", "file",
        "--path", "`"$RequestedPath`"", "--json"
    )
    if ($capture.ExitCode -ne 0) {
        throw "目录截图失败：$($capture.Output) $($capture.Error)"
    }
    try {
        $response = $capture.Output | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "目录截图未返回有效 JSON：$($capture.Output)"
    }
    if (-not $response.ok -or $response.code -ne 0 -or [string]::IsNullOrWhiteSpace($response.path)) {
        throw "目录截图响应不完整：$($capture.Output)"
    }

    $actualPath = [IO.Path]::GetFullPath([string]$response.path)
    $actualDirectory = [IO.Path]::GetDirectoryName($actualPath)
    $expectedFullDirectory = [IO.Path]::GetFullPath($ExpectedDirectory).TrimEnd('\', '/')
    if (-not $actualDirectory.Equals($expectedFullDirectory, [StringComparison]::OrdinalIgnoreCase)) {
        throw "截图未保存到请求目录。请求：$expectedFullDirectory；实际：$actualPath"
    }
    if ([IO.Path]::GetExtension($actualPath) -ine ".png" -or -not (Test-PngSignature $actualPath)) {
        throw "目录截图不是有效 PNG：$actualPath"
    }
    return $actualPath
}

try {
    $existing = Get-Process AirScreenshot -ErrorAction SilentlyContinue
    if ($existing) {
        throw "运行便携烟测前请先退出现有 Air Screenshot 进程。"
    }
    New-Item -ItemType Directory -Path $firstDirectory, $movedDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $env:AIRSHOT_DATA_DIR -Force | Out-Null
    $firstExecutable = Join-Path $firstDirectory "AirScreenshot.exe"
    $movedExecutable = Join-Path $movedDirectory "AirScreenshot.exe"
    Copy-Item -LiteralPath $Executable -Destination $firstExecutable

    $version = Invoke-AirScreenshot $firstExecutable @("--version")
    if ($version.ExitCode -ne 0 -or $version.Output -notmatch "^AirScreenshot \d+\.\d+\.\d+$") {
        throw "便携版版本命令失败：$($version.Output) $($version.Error)"
    }

    # Exercise the updater rejection path before this fixture deliberately
    # writes and repairs the user's Run entry. Combining rapid persistence,
    # relocation, and updater-like launches on one unsigned test copy can
    # trigger behavior-based antivirus heuristics even though no replacement
    # is attempted in this negative test.
    $replacementTarget = Join-Path $temporary "replacement.exe"
    Set-Content -LiteralPath $replacementTarget -Value "old"
    $replacementHash = (Get-FileHash $replacementTarget -Algorithm SHA256).Hash
    $unusedProcessId = [int]::MaxValue
    while (Get-Process -Id $unusedProcessId -ErrorAction SilentlyContinue) {
        $unusedProcessId--
    }
    $updater = Start-Process -FilePath $firstExecutable `
        -ArgumentList @("--apply-update", "`"$replacementTarget`"", "$unusedProcessId", "no-restart") `
        -Wait -PassThru -WindowStyle Hidden
    if ($updater.ExitCode -eq 0 -or
        (Get-FileHash $replacementTarget -Algorithm SHA256).Hash -ne $replacementHash) {
        throw "便携更新替换器未拒绝不存在的非零父进程。"
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
    if (-not (Test-PngSignature $capturePath)) {
        throw "全屏截图文件没有有效 PNG 签名：$capturePath"
    }

    $existingOutputDirectory = Join-Path $temporary "existing-output"
    New-Item -ItemType Directory -Path $existingOutputDirectory | Out-Null
    $existingDirectoryCapture = Invoke-DirectoryCapture `
        -ExecutablePath $firstExecutable `
        -RequestedPath $existingOutputDirectory `
        -ExpectedDirectory $existingOutputDirectory

    $trailingOutputDirectory = Join-Path $temporary "trailing-output"
    $trailingDirectoryArgument = ($trailingOutputDirectory -replace '\\', '/') + "/"
    $trailingDirectoryCapture = Invoke-DirectoryCapture `
        -ExecutablePath $firstExecutable `
        -RequestedPath $trailingDirectoryArgument `
        -ExpectedDirectory $trailingOutputDirectory
    if (-not (Test-Path -LiteralPath $trailingOutputDirectory -PathType Container)) {
        throw "尾部分隔符指定的不存在目录未被创建。"
    }
    if ($existingDirectoryCapture -eq $trailingDirectoryCapture) {
        throw "两个目录截图意外返回同一路径。"
    }

    if ((Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot -ErrorAction SilentlyContinue).AirScreenshot) {
        throw "全新配置不应默认创建开机启动项。"
    }
    Stop-AirScreenshot -Path $firstExecutable

    Set-Content -LiteralPath (Join-Path $env:AIRSHOT_DATA_DIR "config.v2.json") `
        -Value '{"schemaVersion":2,"shell":{"enabled":true,"startAtLogin":true}}' -Encoding utf8NoBOM
    Invoke-AirScreenshot $firstExecutable @("app", "start") | Out-Null
    Start-Sleep -Milliseconds 500
    $runValue = (Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot).AirScreenshot
    if ($runValue -ne "`"$firstExecutable`"") { throw "显式启用开机启动后路径不正确：$runValue" }
    Stop-AirScreenshot -Path $firstExecutable

    Move-Item -LiteralPath $firstExecutable -Destination $movedExecutable
    Invoke-AirScreenshot $movedExecutable @("app", "start") | Out-Null
    Start-Sleep -Milliseconds 500
    $runValue = (Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot).AirScreenshot
    if ($runValue -ne "`"$movedExecutable`"") { throw "移动后未修复启动项：$runValue" }
    Stop-AirScreenshot -Path $movedExecutable

    Set-Content -LiteralPath (Join-Path $env:AIRSHOT_DATA_DIR "config.v2.json") `
        -Value '{"schemaVersion":2,"shell":{"enabled":true,"startAtLogin":false}}' -Encoding utf8NoBOM
    Invoke-AirScreenshot $movedExecutable @("app", "start") | Out-Null
    Start-Sleep -Milliseconds 500
    if ((Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot -ErrorAction SilentlyContinue).AirScreenshot) {
        throw "关闭开机启动后未删除启动项。"
    }
    Stop-AirScreenshot -Path $movedExecutable

    Set-ItemProperty -LiteralPath $movedExecutable -Name IsReadOnly -Value $true
    $readOnlyCheck = Invoke-AirScreenshot $movedExecutable @("--check-update-target")
    Set-ItemProperty -LiteralPath $movedExecutable -Name IsReadOnly -Value $false
    if ($readOnlyCheck.ExitCode -eq 0) {
        throw "便携更新未拒绝只读目标。"
    }

    Write-Host "便携版烟测通过。"
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
