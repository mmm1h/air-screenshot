#Requires -Version 7.0

[CmdletBinding()]
param(
    [string]$Executable = (
        Join-Path (Split-Path -Parent $PSScriptRoot) "build\release\bin\AirScreenshot.exe"
    )
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathFullyQualified($Executable)) {
    $Executable = Join-Path $root $Executable
}
$Executable = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "AirScreenshot executable was not found: $Executable"
}

$temporary = Join-Path (
    [IO.Path]::GetTempPath()
) ("AirScreenshot-Lifecycle-" + [Guid]::NewGuid().ToString("N"))
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$originalDataDirectory = $env:AIRSHOT_DATA_DIR
$originalRunValue = $null
$hadRunValue = $false
try {
    $originalRunValue = (
        Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot -ErrorAction Stop
    ).AirScreenshot
    $hadRunValue = $true
}
catch {
}

Add-Type -TypeDefinition @"
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

public static class AirScreenshotSmokeNative {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateMutex(IntPtr attributes, bool initialOwner, string name);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr handle);

    private static string PipeName()
    {
        byte[] sid = new byte[WindowsIdentity.GetCurrent().User.BinaryLength];
        WindowsIdentity.GetCurrent().User.GetBinaryForm(sid, 0);
        ulong digest = 14695981039346656037UL;
        unchecked
        {
            foreach (byte value in sid)
            {
                digest ^= value;
                digest *= 1099511628211UL;
            }
        }
        return string.Format(
            "LOCAL\\AirScreenshot.v2.s{0}.u{1:X16}",
            Process.GetCurrentProcess().SessionId,
            digest);
    }

    private static void ReadExact(Stream stream, byte[] buffer)
    {
        int offset = 0;
        while (offset < buffer.Length)
        {
            int count = stream.Read(buffer, offset, buffer.Length - offset);
            if (count <= 0)
            {
                throw new EndOfStreamException();
            }
            offset += count;
        }
    }

    public static string SendWithoutAcknowledgement(string request)
    {
        using (var pipe = new NamedPipeClientStream(
            ".",
            PipeName(),
            PipeDirection.InOut,
            PipeOptions.None))
        {
            pipe.Connect(5000);
            byte[] requestBytes = Encoding.UTF8.GetBytes(request);
            byte[] requestLength = BitConverter.GetBytes((uint)requestBytes.Length);
            pipe.Write(requestLength, 0, requestLength.Length);
            pipe.Write(requestBytes, 0, requestBytes.Length);
            pipe.Flush();

            byte[] responseLengthBytes = new byte[4];
            ReadExact(pipe, responseLengthBytes);
            uint responseLength = BitConverter.ToUInt32(responseLengthBytes, 0);
            if (responseLength > 1024 * 1024)
            {
                throw new InvalidDataException("Response frame is too large.");
            }
            byte[] responseBytes = new byte[responseLength];
            ReadExact(pipe, responseBytes);
            return Encoding.UTF8.GetString(responseBytes);
        }
    }
}
"@

function Start-AirScreenshot {
    param([Parameter(Mandatory)][string[]]$Arguments)

    $id = [Guid]::NewGuid().ToString("N")
    $stdout = Join-Path $temporary "stdout-$id.txt"
    $stderr = Join-Path $temporary "stderr-$id.txt"
    $process = Start-Process `
        -FilePath $Executable `
        -ArgumentList $Arguments `
        -PassThru `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr
    [pscustomobject]@{
        Process = $process
        Stdout = $stdout
        Stderr = $stderr
    }
}

function Complete-AirScreenshot {
    param(
        [Parameter(Mandatory)]$Run,
        [int]$TimeoutMilliseconds = 15000
    )

    if (-not $Run.Process.WaitForExit($TimeoutMilliseconds)) {
        Stop-Process -Id $Run.Process.Id -Force -ErrorAction SilentlyContinue
        throw "AirScreenshot command timed out (PID $($Run.Process.Id))."
    }
    $Run.Process.WaitForExit()
    $result = [pscustomobject]@{
        ExitCode = $Run.Process.ExitCode
        Output = Read-RedirectedText -Path $Run.Stdout
        Error = Read-RedirectedText -Path $Run.Stderr
    }
    Remove-Item -LiteralPath $Run.Stdout, $Run.Stderr -Force -ErrorAction SilentlyContinue
    return $result
}

function Read-RedirectedText {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ""
    }
    $value = Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue
    if ($null -eq $value) {
        return ""
    }
    return $value.Trim()
}

function Invoke-AirScreenshot {
    param(
        [Parameter(Mandatory)][string[]]$Arguments,
        [int]$TimeoutMilliseconds = 15000
    )

    return Complete-AirScreenshot `
        -Run (Start-AirScreenshot -Arguments $Arguments) `
        -TimeoutMilliseconds $TimeoutMilliseconds
}

function Convert-AirScreenshotResponse {
    param([Parameter(Mandatory)]$Result)

    try {
        return $Result.Output | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "Command did not return JSON. exit=$($Result.ExitCode) stdout=$($Result.Output) stderr=$($Result.Error)"
    }
}

function Assert-JsonResult {
    param(
        [Parameter(Mandatory)]$Result,
        [Parameter(Mandatory)][int]$ExpectedCode,
        [string]$ExpectedErrorType
    )

    $response = Convert-AirScreenshotResponse $Result
    if ($Result.ExitCode -ne $ExpectedCode -or [int]$response.code -ne $ExpectedCode) {
        throw "Unexpected command result. expected=$ExpectedCode exit=$($Result.ExitCode) body=$($Result.Output)"
    }
    if ($ExpectedCode -eq 0 -and -not $response.ok) {
        throw "Successful response was marked as failed: $($Result.Output)"
    }
    if ($ExpectedCode -ne 0 -and $response.ok) {
        throw "Failed response was marked as successful: $($Result.Output)"
    }
    if ($ExpectedErrorType -and $response.error.type -ne $ExpectedErrorType) {
        throw "Unexpected error type. expected=$ExpectedErrorType body=$($Result.Output)"
    }
    return $response
}

function Test-Png {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    $stream = [IO.File]::OpenRead($Path)
    try {
        $signature = [byte[]]::new(8)
        return $stream.Read($signature, 0, $signature.Length) -eq $signature.Length -and
            [Convert]::ToHexString($signature) -eq "89504E470D0A1A0A"
    }
    finally {
        $stream.Dispose()
    }
}

function Get-AppStatus {
    $result = Invoke-AirScreenshot -Arguments @("app", "status", "--json")
    $response = Assert-JsonResult -Result $result -ExpectedCode 0
    return $response.data
}

function Wait-AppState {
    param(
        [Parameter(Mandatory)][bool]$Running,
        [int]$TimeoutMilliseconds = 10000
    )

    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
        $result = Invoke-AirScreenshot -Arguments @("app", "status", "--json")
        if ($result.ExitCode -eq 6) {
            $response = Convert-AirScreenshotResponse $result
            if ($response.error.type -eq "ipc_failed") {
                Start-Sleep -Milliseconds 100
                continue
            }
        }
        $status = (Assert-JsonResult -Result $result -ExpectedCode 0).data
        if ([bool]$status.running -eq $Running) {
            return $status
        }
        Start-Sleep -Milliseconds 100
    }
    throw "AirScreenshot did not reach running=$Running."
}

function Wait-AirScreenshotProcessesExited {
    param([int]$TimeoutMilliseconds = 15000)

    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
        if (-not (Get-Process AirScreenshot -ErrorAction SilentlyContinue)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "AirScreenshot processes did not exit after the pipe closed."
}

function Stop-App {
    $status = Get-AppStatus
    if ($status.running) {
        $stop = Invoke-AirScreenshot -Arguments @("app", "stop", "--json")
        Assert-JsonResult -Result $stop -ExpectedCode 0 | Out-Null
    }
    Wait-AppState -Running $false | Out-Null
    Wait-AirScreenshotProcessesExited
}

function Wait-OverlayWindow {
    param($Run)

    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt 10000) {
        if ($Run -and $Run.Process.HasExited) {
            $early = Complete-AirScreenshot -Run $Run
            throw "The region command exited before showing an overlay. exit=$($early.ExitCode) stdout=$($early.Output) stderr=$($early.Error)"
        }
        $window = [AirScreenshotSmokeNative]::FindWindow(
            "AirScreenshot.Overlay",
            $null
        )
        if ($window -ne [IntPtr]::Zero) {
            return $window
        }
        Start-Sleep -Milliseconds 50
    }
    $processState = Get-Process AirScreenshot -ErrorAction SilentlyContinue |
        Select-Object Id, MainWindowHandle, MainWindowTitle, Path |
        Out-String
    $stdout = if ($Run) { Read-RedirectedText -Path $Run.Stdout } else { "" }
    $stderr = if ($Run) { Read-RedirectedText -Path $Run.Stderr } else { "" }
    throw "The region overlay did not appear. processes=$processState stdout=$stdout stderr=$stderr"
}

try {
    if (Get-Process AirScreenshot -ErrorAction SilentlyContinue) {
        throw "Close existing AirScreenshot processes before running lifecycle smoke."
    }
    New-Item -ItemType Directory -Path $temporary -Force | Out-Null
    $env:AIRSHOT_DATA_DIR = Join-Path $temporary "data"

    $version = Invoke-AirScreenshot -Arguments @("--version", "--json")
    $versionResponse = Assert-JsonResult -Result $version -ExpectedCode 0
    if ($versionResponse.text -notmatch "^AirScreenshot \d+\.\d+\.\d+$") {
        throw "Unexpected version response: $($version.Output)"
    }

    $unknown = Invoke-AirScreenshot -Arguments @("unknown", "--json")
    Assert-JsonResult `
        -Result $unknown `
        -ExpectedCode 2 `
        -ExpectedErrorType "invalid_arguments" | Out-Null

    $bareTransient = Invoke-AirScreenshot -Arguments @("--transient", "--json")
    Assert-JsonResult `
        -Result $bareTransient `
        -ExpectedCode 2 `
        -ExpectedErrorType "invalid_arguments" | Out-Null
    if ($unknown.Error) {
        throw "JSON errors must be written to stdout, not stderr."
    }
    $plainUnknown = Invoke-AirScreenshot -Arguments @("unknown")
    if ($plainUnknown.ExitCode -ne 2 -or $plainUnknown.Output -or -not $plainUnknown.Error) {
        throw "Plain-text CLI errors must be written to stderr."
    }

    $missingVerifierArguments = Invoke-AirScreenshot -Arguments @(
        "--verify-ocr-manifest"
    )
    if ($missingVerifierArguments.ExitCode -ne 2) {
        throw "OCR manifest verifier argument errors must return exit code 2."
    }
    $invalidVerifierInput = Invoke-AirScreenshot -Arguments @(
        "--verify-ocr-manifest",
        "`"$(Join-Path $temporary 'missing-manifest.json')`"",
        "`"$(Join-Path $temporary 'missing-manifest.sig')`""
    )
    if ($invalidVerifierInput.ExitCode -ne 5) {
        throw "OCR manifest verification failures must return exit code 5."
    }

    $nonPng = Join-Path $temporary "not-png.jpg"
    $invalidPath = Invoke-AirScreenshot -Arguments @(
        "capture", "screen", "--output", "file", "--path", "`"$nonPng`"", "--json"
    )
    Assert-JsonResult `
        -Result $invalidPath `
        -ExpectedCode 2 `
        -ExpectedErrorType "invalid_arguments" | Out-Null
    if (Test-Path -LiteralPath $nonPng) {
        throw "A rejected non-PNG path was created."
    }

    $hostMutex = [AirScreenshotSmokeNative]::CreateMutex(
        [IntPtr]::Zero,
        $false,
        "Local\AirScreenshot.Host.v1"
    )
    if ($hostMutex -eq [IntPtr]::Zero) {
        throw "Unable to create the host mutex fixture."
    }
    try {
        $blockedHost = Invoke-AirScreenshot `
            -Arguments @(
                "capture", "screen", "--monitor", "primary",
                "--output", "file",
                "--path", "`"$(Join-Path $temporary 'blocked.png')`"",
                "--json"
            ) `
            -TimeoutMilliseconds 20000
        Assert-JsonResult `
            -Result $blockedHost `
            -ExpectedCode 6 `
            -ExpectedErrorType "ipc_failed" | Out-Null
    }
    finally {
        [AirScreenshotSmokeNative]::CloseHandle($hostMutex) | Out-Null
    }
    Wait-AirScreenshotProcessesExited

    $configPath = Join-Path $env:AIRSHOT_DATA_DIR "config.v2.json"
    New-Item -ItemType Directory -Path $env:AIRSHOT_DATA_DIR -Force | Out-Null
    $invalidStartupConfigs = @(
        '{"schemaVersion":2,"shell":{"enabled":"invalid"}}',
        '{"schemaVersion":3}'
    )
    foreach ($invalidConfig in $invalidStartupConfigs) {
        Set-Content -LiteralPath $configPath -Value $invalidConfig -Encoding utf8NoBOM
        $failedStartupWatch = [Diagnostics.Stopwatch]::StartNew()
        $failedStartup = Invoke-AirScreenshot `
            -Arguments @(
                "capture", "screen", "--monitor", "primary",
                "--output", "file",
                "--path", "`"$(Join-Path $temporary 'invalid-config.png')`"",
                "--json"
            ) `
            -TimeoutMilliseconds 8000
        $failedStartupWatch.Stop()
        Assert-JsonResult `
            -Result $failedStartup `
            -ExpectedCode 6 `
            -ExpectedErrorType "ipc_failed" | Out-Null
        if ($failedStartupWatch.ElapsedMilliseconds -ge 5000) {
            throw "A transient host configuration failure did not return promptly."
        }
        Wait-AirScreenshotProcessesExited
    }
    Remove-Item -LiteralPath $configPath -Force

    $coldStartRuns = @(
        (Start-AirScreenshot -Arguments @("module", "list", "--json")),
        (Start-AirScreenshot -Arguments @("module", "list", "--json"))
    )
    foreach ($run in $coldStartRuns) {
        $coldStartResult = Complete-AirScreenshot -Run $run
        Assert-JsonResult -Result $coldStartResult -ExpectedCode 0 | Out-Null
    }
    Wait-AirScreenshotProcessesExited

    $unicodeDirectory = Join-Path $temporary "unicode 路径"
    New-Item -ItemType Directory -Path $unicodeDirectory -Force | Out-Null
    $extensionless = Join-Path $unicodeDirectory "截图 shot"
    $capture = Invoke-AirScreenshot -Arguments @(
        "capture", "screen", "--monitor", "primary", "--output", "file",
        "--path", "`"$extensionless`"", "--json"
    )
    $captureResponse = Assert-JsonResult -Result $capture -ExpectedCode 0
    $expectedPng = "$extensionless.png"
    if (-not [IO.Path]::GetFullPath([string]$captureResponse.path).Equals(
            [IO.Path]::GetFullPath($expectedPng),
            [StringComparison]::OrdinalIgnoreCase
        ) -or -not (Test-Png $expectedPng)) {
        throw "Extensionless capture did not produce the expected PNG: $($capture.Output)"
    }
    Stop-App

    $allMonitors = Join-Path $temporary "all-monitors.png"
    $defaultMonitorCapture = Invoke-AirScreenshot -Arguments @(
        "capture", "screen", "--output", "file",
        "--path", "`"$allMonitors`"", "--json"
    )
    Assert-JsonResult -Result $defaultMonitorCapture -ExpectedCode 0 | Out-Null
    if (-not (Test-Png $allMonitors)) {
        throw "Default all-monitor capture did not produce a PNG."
    }
    Stop-App

    $start = Invoke-AirScreenshot -Arguments @("app", "start", "--json")
    $startResponse = Assert-JsonResult -Result $start -ExpectedCode 0
    if (-not $startResponse.data.running -or $startResponse.data.transient) {
        throw "Explicit app start did not create a persistent host."
    }

    $moduleListRuns = @(
        (Start-AirScreenshot -Arguments @("module", "list", "--json")),
        (Start-AirScreenshot -Arguments @("module", "list", "--json"))
    )
    foreach ($run in $moduleListRuns) {
        $moduleResult = Complete-AirScreenshot -Run $run
        Assert-JsonResult -Result $moduleResult -ExpectedCode 0 | Out-Null
    }

    $disableOcr = Invoke-AirScreenshot -Arguments @(
        "module", "disable", "ocr", "--json"
    )
    Assert-JsonResult -Result $disableOcr -ExpectedCode 0 | Out-Null
    $disabledOcr = Invoke-AirScreenshot -Arguments @("ocr", "region", "--json")
    Assert-JsonResult `
        -Result $disabledOcr `
        -ExpectedCode 4 `
        -ExpectedErrorType "module_unavailable" | Out-Null
    $enableOcr = Invoke-AirScreenshot -Arguments @(
        "module", "enable", "ocr", "--json"
    )
    Assert-JsonResult -Result $enableOcr -ExpectedCode 0 | Out-Null

    $unacknowledgedStop = [AirScreenshotSmokeNative]::SendWithoutAcknowledgement(
        '{"v":1,"json":true,"command":"app","action":"stop"}'
    )
    try {
        $unacknowledgedResponse =
            $unacknowledgedStop | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "Unacknowledged stop returned invalid JSON: $unacknowledgedStop"
    }
    if (-not $unacknowledgedResponse.ok -or
        [int]$unacknowledgedResponse.code -ne 0) {
        throw "Unacknowledged stop did not produce a successful response."
    }
    Wait-AppState -Running $false | Out-Null
    Wait-AirScreenshotProcessesExited

    $cancelledRegion = Start-AirScreenshot -Arguments @(
        "capture", "region", "--output", "clipboard", "--json"
    )
    $overlay = Wait-OverlayWindow -Run $cancelledRegion
    if (-not [AirScreenshotSmokeNative]::PostMessage(
            $overlay,
            0x0100,
            [IntPtr]0x1B,
            [IntPtr]::Zero
        )) {
        throw "Unable to post Escape to the region overlay."
    }
    $cancelled = Complete-AirScreenshot -Run $cancelledRegion
    Assert-JsonResult `
        -Result $cancelled `
        -ExpectedCode 3 `
        -ExpectedErrorType "user_cancelled" | Out-Null
    Wait-AppState -Running $false | Out-Null
    Wait-AirScreenshotProcessesExited

    $activeRegion = Start-AirScreenshot -Arguments @(
        "capture", "region", "--output", "clipboard", "--json"
    )
    $transientStatus = Wait-AppState -Running $true
    if (-not $transientStatus.transient) {
        throw "A region command did not start a transient host."
    }
    Wait-OverlayWindow -Run $activeRegion | Out-Null

    $busyRegion = Invoke-AirScreenshot -Arguments @(
        "capture", "region", "--output", "clipboard", "--json"
    )
    Assert-JsonResult `
        -Result $busyRegion `
        -ExpectedCode 5 `
        -ExpectedErrorType "busy" | Out-Null

    $promote = Invoke-AirScreenshot -Arguments @("app", "start", "--json")
    $promoteResponse = Assert-JsonResult -Result $promote -ExpectedCode 0
    if ($promoteResponse.data.transient) {
        throw "app start did not promote the transient host."
    }

    $stop = Invoke-AirScreenshot -Arguments @("app", "stop", "--json")
    Assert-JsonResult -Result $stop -ExpectedCode 0 | Out-Null
    $stoppedRegion = Complete-AirScreenshot -Run $activeRegion
    Assert-JsonResult `
        -Result $stoppedRegion `
        -ExpectedCode 5 `
        -ExpectedErrorType "shutting_down" | Out-Null
    Wait-AppState -Running $false | Out-Null
    Wait-AirScreenshotProcessesExited

    $settings = Invoke-AirScreenshot -Arguments @("app", "settings", "--json")
    Assert-JsonResult -Result $settings -ExpectedCode 0 | Out-Null
    $settingsStatus = Wait-AppState -Running $true
    if ($settingsStatus.transient) {
        throw "app settings did not promote its host to persistent mode."
    }

    $stopRuns = @(
        (Start-AirScreenshot -Arguments @("app", "stop", "--json")),
        (Start-AirScreenshot -Arguments @("app", "stop", "--json"))
    )
    $stopResults = @(
        (Complete-AirScreenshot -Run $stopRuns[0]),
        (Complete-AirScreenshot -Run $stopRuns[1])
    )
    if (($stopResults | Where-Object ExitCode -eq 0).Count -lt 1) {
        throw "Concurrent stop commands did not produce a successful acknowledgement."
    }
    foreach ($result in $stopResults) {
        if ($result.ExitCode -notin @(0, 5, 6)) {
            throw "Concurrent stop returned an unexpected code: $($result.ExitCode)"
        }
        Convert-AirScreenshotResponse $result | Out-Null
    }
    Wait-AppState -Running $false | Out-Null
    Wait-AirScreenshotProcessesExited

    Write-Host "Lifecycle smoke passed."
}
finally {
    Get-Process AirScreenshot -ErrorAction SilentlyContinue |
        Where-Object {
            try {
                $_.Path -and $_.Path.Equals(
                    $Executable,
                    [StringComparison]::OrdinalIgnoreCase
                )
            }
            catch {
                $false
            }
        } |
        Stop-Process -Force -ErrorAction SilentlyContinue

    if ($hadRunValue) {
        Set-ItemProperty -LiteralPath $runKey -Name AirScreenshot -Value $originalRunValue
    }
    else {
        Remove-ItemProperty -LiteralPath $runKey -Name AirScreenshot -ErrorAction SilentlyContinue
    }
    if ($null -eq $originalDataDirectory) {
        Remove-Item Env:\AIRSHOT_DATA_DIR -ErrorAction SilentlyContinue
    }
    else {
        $env:AIRSHOT_DATA_DIR = $originalDataDirectory
    }
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
