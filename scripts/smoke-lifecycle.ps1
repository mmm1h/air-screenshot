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
$smokeHotkey = $null
$heldSmokeHotkey = $false
$smokeHotkeyFixtureId = 0x4157
try {
    $originalRunValue = (
        Get-ItemProperty -LiteralPath $runKey -Name AirScreenshot -ErrorAction Stop
    ).AirScreenshot
    $hadRunValue = $true
}
catch {
}

if (-not ("AirScreenshotSmokeNative" -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

public static class AirScreenshotSmokeNative {
    [DllImport(
        "user32.dll",
        EntryPoint = "FindWindowW",
        CharSet = CharSet.Unicode,
        ExactSpelling = true)]
    private static extern IntPtr FindWindowNative(
        string className,
        string windowName);

    public static IntPtr FindWindowByClass(string className)
    {
        return FindWindowNative(className, null);
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCursorPos(out NativePoint point);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(IntPtr window, out NativeRect rect);

    public static bool DragRegionSelection(IntPtr window)
    {
        const uint WM_MOUSEMOVE = 0x0200;
        const uint WM_LBUTTONDOWN = 0x0201;
        const uint WM_LBUTTONUP = 0x0202;
        const int MK_LBUTTON = 0x0001;

        NativeRect rect;
        NativePoint original;
        if (!GetWindowRect(window, out rect) || !GetCursorPos(out original))
        {
            return false;
        }
        int availableWidth = rect.Right - rect.Left;
        int availableHeight = rect.Bottom - rect.Top;
        if (availableWidth < 16 || availableHeight < 16)
        {
            return false;
        }

        int startX = rect.Left + Math.Min(32, Math.Max(2, availableWidth / 4));
        int startY = rect.Top + Math.Min(32, Math.Max(2, availableHeight / 4));
        int endX = Math.Min(rect.Right - 2, startX + Math.Min(240, Math.Max(8, availableWidth / 3)));
        int endY = Math.Min(rect.Bottom - 2, startY + Math.Min(160, Math.Max(8, availableHeight / 3)));
        try
        {
            if (!SetCursorPos(startX, startY))
            {
                return false;
            }
            SendMessage(window, WM_LBUTTONDOWN, new IntPtr(MK_LBUTTON), IntPtr.Zero);
            if (!SetCursorPos(endX, endY))
            {
                return false;
            }
            SendMessage(window, WM_MOUSEMOVE, new IntPtr(MK_LBUTTON), IntPtr.Zero);
            SendMessage(window, WM_LBUTTONUP, IntPtr.Zero, IntPtr.Zero);
            return true;
        }
        finally
        {
            SetCursorPos(original.X, original.Y);
        }
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool RegisterHotKey(IntPtr window, int id, uint modifiers, uint virtualKey);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool UnregisterHotKey(IntPtr window, int id);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint desiredAccess);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseDesktop(IntPtr desktop);

    [DllImport("user32.dll")]
    private static extern IntPtr GetProcessWindowStation();

    [DllImport("user32.dll")]
    private static extern IntPtr GetThreadDesktop(uint threadId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetUserObjectInformation(
        IntPtr handle,
        int index,
        StringBuilder value,
        int length,
        out int required);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("user32.dll")]
    public static extern IntPtr GetShellWindow();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateMutex(IntPtr attributes, bool initialOwner, string name);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr handle);

    public static bool HasInteractiveDesktop()
    {
        const uint DESKTOP_READOBJECTS = 0x0001;
        const uint DESKTOP_SWITCHDESKTOP = 0x0100;
        const int UOI_NAME = 2;
        IntPtr desktop = OpenInputDesktop(
            0,
            false,
            DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
        if (desktop == IntPtr.Zero)
        {
            return false;
        }
        try
        {
            StringBuilder stationName = new StringBuilder(256);
            StringBuilder currentDesktopName = new StringBuilder(256);
            StringBuilder inputDesktopName = new StringBuilder(256);
            int required;
            bool namesAvailable =
                GetUserObjectInformation(
                    GetProcessWindowStation(), UOI_NAME, stationName,
                    stationName.Capacity * sizeof(char), out required) &&
                GetUserObjectInformation(
                    GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                    currentDesktopName,
                    currentDesktopName.Capacity * sizeof(char), out required) &&
                GetUserObjectInformation(
                    desktop, UOI_NAME, inputDesktopName,
                    inputDesktopName.Capacity * sizeof(char), out required);
            return namesAvailable &&
                   Process.GetCurrentProcess().SessionId != 0 &&
                   stationName.ToString().Equals(
                       "WinSta0", StringComparison.OrdinalIgnoreCase) &&
                   currentDesktopName.ToString().Equals(
                       inputDesktopName.ToString(),
                       StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            CloseDesktop(desktop);
        }
    }

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
}

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

function Convert-CompatibleAppStatus {
    param([Parameter(Mandatory)]$Data)

    $hotkeysProperty = $Data.PSObject.Properties["hotkeysAvailable"]
    $runtimeErrorProperty = $Data.PSObject.Properties["runtimeError"]
    $trayProperty = $Data.PSObject.Properties["trayIconVisible"]
    return [pscustomobject]@{
        Running = [bool]$Data.running
        Transient = [bool]$Data.transient
        Shell = [bool]$Data.shell
        TrayIconVisible = if ($trayProperty) {
            [Nullable[bool]]([bool]$trayProperty.Value)
        } else {
            $null
        }
        HotkeysAvailable = if ($hotkeysProperty) {
            [Nullable[bool]]([bool]$hotkeysProperty.Value)
        } else {
            $null
        }
        RuntimeError = if ($runtimeErrorProperty) {
            [string]$runtimeErrorProperty.Value
        } else {
            $null
        }
    }
}

function Find-FreeSmokeHotkey {
    $modifiers = [uint32](0x0001 -bor 0x0002 -bor 0x0004 -bor 0x4000)
    $candidates = @(
        [pscustomobject]@{ Text = "Ctrl+Alt+Shift+F12"; VirtualKey = [uint32]0x7B },
        [pscustomobject]@{ Text = "Ctrl+Alt+Shift+F11"; VirtualKey = [uint32]0x7A },
        [pscustomobject]@{ Text = "Ctrl+Alt+Shift+F10"; VirtualKey = [uint32]0x79 },
        [pscustomobject]@{ Text = "Ctrl+Alt+Shift+F9"; VirtualKey = [uint32]0x78 }
    )
    foreach ($candidate in $candidates) {
        if ([AirScreenshotSmokeNative]::RegisterHotKey(
                [IntPtr]::Zero,
                $smokeHotkeyFixtureId,
                $modifiers,
                $candidate.VirtualKey)) {
            [AirScreenshotSmokeNative]::UnregisterHotKey(
                [IntPtr]::Zero,
                $smokeHotkeyFixtureId
            ) | Out-Null
            $candidate | Add-Member -NotePropertyName Modifiers -NotePropertyValue $modifiers
            return $candidate
        }
    }
    return $null
}

function Assert-SmokeHotkeyOccupied {
    param([Parameter(Mandatory)]$Hotkey)

    if ([AirScreenshotSmokeNative]::RegisterHotKey(
            [IntPtr]::Zero,
            $smokeHotkeyFixtureId,
            $Hotkey.Modifiers,
            $Hotkey.VirtualKey)) {
        [AirScreenshotSmokeNative]::UnregisterHotKey(
            [IntPtr]::Zero,
            $smokeHotkeyFixtureId
        ) | Out-Null
        throw "The configured capture hotkey became unreserved while settings were open."
    }
    $registrationError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    if ($registrationError -ne 1409) {
        throw "Unable to verify hotkey ownership. RegisterHotKey returned Win32 $registrationError."
    }
}

function Wait-SettingsWindow {
    param([int]$TimeoutMilliseconds = 5000)

    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
        $window = [AirScreenshotSmokeNative]::FindWindowByClass(
            "AirScreenshot.Settings"
        )
        if ($window -ne [IntPtr]::Zero) {
            return $window
        }
        Start-Sleep -Milliseconds 50
    }
    $processState = Get-TestHostProcessIds
    $statusText = "unavailable"
    try {
        $statusText = (Invoke-AirScreenshot -Arguments @(
            "app", "status", "--json"
        )).Output
    }
    catch {
        $statusText = $_.Exception.Message
    }
    throw "The settings window did not appear within $TimeoutMilliseconds ms. hostPids=$([string]::Join(',', $processState)) status=$statusText"
}

function Wait-SettingsWindowClosed {
    param([int]$TimeoutMilliseconds = 5000)

    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
        if ([AirScreenshotSmokeNative]::FindWindowByClass(
                "AirScreenshot.Settings"
            ) -eq [IntPtr]::Zero) {
            return
        }
        Start-Sleep -Milliseconds 50
    }
    throw "The settings window did not close within $TimeoutMilliseconds ms."
}

function Send-SettingsKey {
    param(
        [Parameter(Mandatory)][IntPtr]$Window,
        [Parameter(Mandatory)][int]$VirtualKey
    )

    if (-not [AirScreenshotSmokeNative]::PostMessage(
            $Window,
            0x0100,
            [IntPtr]$VirtualKey,
            [IntPtr]::Zero
        )) {
        throw "Unable to post virtual key 0x$($VirtualKey.ToString('X')) to settings."
    }
}

function Get-TestHostProcessIds {
    return @(
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
            Select-Object -ExpandProperty Id
    )
}

function Write-IsolatedSmokeConfig {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][bool]$ShellEnabled,
        [Parameter(Mandatory)][bool]$TrayIconVisible,
        [Parameter(Mandatory)][string]$CaptureHotkey
    )

    $config = [ordered]@{
        schemaVersion = 2
        shell = [ordered]@{
            enabled = $ShellEnabled
            trayIconVisible = $TrayIconVisible
            startAtLogin = $false
            notificationsEnabled = $false
        }
        update = [ordered]@{
            automatic = $false
            lastCheckUnix = 0
            warnedTarget = ""
        }
        hotkey = [ordered]@{
            capture = $CaptureHotkey
            pin = ""
            globalOcrEnabled = $false
            globalOcr = "Ctrl+Alt+O"
        }
    }
    $json = $config | ConvertTo-Json -Compress -Depth 5
    Set-Content -LiteralPath $Path -Value $json -Encoding utf8NoBOM
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
        $window = [AirScreenshotSmokeNative]::FindWindowByClass(
            "AirScreenshot.Overlay"
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

    $legacyStatusData =
        '{"running":true,"transient":false,"shell":true}' |
        ConvertFrom-Json -ErrorAction Stop
    $legacyStatus = Convert-CompatibleAppStatus $legacyStatusData
    if (-not $legacyStatus.Running -or
        $legacyStatus.Transient -or
        -not $legacyStatus.Shell -or
        $null -ne $legacyStatus.HotkeysAvailable -or
        $null -ne $legacyStatus.RuntimeError) {
        throw "The app status parser is not backward-compatible with responses that predate runtime health fields."
    }

    $interactiveDesktop =
        [AirScreenshotSmokeNative]::HasInteractiveDesktop()
    $smokeHotkey = Find-FreeSmokeHotkey
    $captureHotkeyText = if ($smokeHotkey) {
        [string]$smokeHotkey.Text
    } else {
        "Ctrl+Alt+Shift+F12"
    }

    if ($interactiveDesktop) {
        Write-IsolatedSmokeConfig `
            -Path $configPath `
            -ShellEnabled $false `
            -TrayIconVisible $false `
            -CaptureHotkey $captureHotkeyText
        $settingsConfigHash =
            (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash

        $transientSettings = Invoke-AirScreenshot -Arguments @(
            "app", "settings", "--json"
        )
        Assert-JsonResult `
            -Result $transientSettings `
            -ExpectedCode 0 | Out-Null
        $transientSettingsWindow = Wait-SettingsWindow

        # Move keyboard focus from the first category to the first editable
        # capture setting, then toggle it. This creates an actual in-memory
        # draft before Escape cancels the dialog.
        for ($tab = 0; $tab -lt 6; $tab++) {
            Send-SettingsKey `
                -Window $transientSettingsWindow `
                -VirtualKey 0x09
        }
        Send-SettingsKey `
            -Window $transientSettingsWindow `
            -VirtualKey 0x20
        Start-Sleep -Milliseconds 100
        if ((Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash -ne
            $settingsConfigHash) {
            throw "Editing a settings draft wrote the isolated configuration before Save."
        }

        $settingsCancelWatch = [Diagnostics.Stopwatch]::StartNew()
        Send-SettingsKey `
            -Window $transientSettingsWindow `
            -VirtualKey 0x1B
        Wait-SettingsWindowClosed -TimeoutMilliseconds 3000
        Wait-AirScreenshotProcessesExited -TimeoutMilliseconds 5000
        $settingsCancelWatch.Stop()
        if ($settingsCancelWatch.ElapsedMilliseconds -ge 5000) {
            throw "A shell-disabled settings-only host did not exit promptly after Escape."
        }
        if ((Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash -ne
            $settingsConfigHash) {
            throw "Cancelling settings persisted the in-memory draft."
        }
        $afterCancelledSettings =
            Convert-CompatibleAppStatus (Get-AppStatus)
        if ($afterCancelledSettings.Running) {
            throw "A shell-disabled settings-only host remained running after cancellation."
        }
        Wait-AirScreenshotProcessesExited
    }
    else {
        Write-Host "SKIP: shell=false settings cancellation requires an unlocked interactive desktop."
    }

    if ($smokeHotkey) {
        Write-IsolatedSmokeConfig `
            -Path $configPath `
            -ShellEnabled $true `
            -TrayIconVisible $false `
            -CaptureHotkey $smokeHotkey.Text

        $showTray = Invoke-AirScreenshot -Arguments @(
            "app", "tray", "show", "--json"
        )
        Assert-JsonResult -Result $showTray -ExpectedCode 0 | Out-Null
        $trayStatusData = Get-AppStatus
        $trayStatus = Convert-CompatibleAppStatus $trayStatusData
        if (-not $trayStatus.Running -or
            $trayStatus.Transient -or
            -not $trayStatus.Shell -or
            $trayStatus.TrayIconVisible -ne $true -or
            $null -eq $trayStatus.HotkeysAvailable -or
            $trayStatus.HotkeysAvailable -ne $true) {
            throw "app tray show did not restore a persistent, healthy shell host."
        }
        $persistedTrayConfig =
            Get-Content -LiteralPath $configPath -Raw |
            ConvertFrom-Json -ErrorAction Stop
        if (-not [bool]$persistedTrayConfig.shell.enabled -or
            -not [bool]$persistedTrayConfig.shell.trayIconVisible) {
            throw "app tray show did not persist trayIconVisible=true in the isolated configuration."
        }

        $firstTrayHash =
            (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
        $firstTrayWrite = (Get-Item -LiteralPath $configPath).LastWriteTimeUtc.Ticks
        $firstTrayHostIds = @(Get-TestHostProcessIds | Sort-Object)
        if ($firstTrayHostIds.Count -ne 1) {
            throw "app tray show did not leave exactly one test host running."
        }
        Start-Sleep -Milliseconds 100
        $showTrayAgain = Invoke-AirScreenshot -Arguments @(
            "app", "tray", "show", "--json"
        )
        Assert-JsonResult -Result $showTrayAgain -ExpectedCode 0 | Out-Null
        $secondTrayHostIds = @(Get-TestHostProcessIds | Sort-Object)
        if ((Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash -ne
                $firstTrayHash -or
            (Get-Item -LiteralPath $configPath).LastWriteTimeUtc.Ticks -ne
                $firstTrayWrite -or
            [string]::Join(",", $secondTrayHostIds) -ne
                [string]::Join(",", $firstTrayHostIds)) {
            throw "Repeated app tray show was not idempotent."
        }

        if ($interactiveDesktop) {
            $persistentSettings = Invoke-AirScreenshot -Arguments @(
                "app", "settings", "--json"
            )
            Assert-JsonResult `
                -Result $persistentSettings `
                -ExpectedCode 0 | Out-Null
            $persistentSettingsWindow = Wait-SettingsWindow
            $duringSettingsStatus =
                Convert-CompatibleAppStatus (Get-AppStatus)
            if ($duringSettingsStatus.HotkeysAvailable -ne $true) {
                throw "app status reported unavailable hotkeys while settings were open."
            }
            $settingsOpenConfigHash =
                (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
            $busyTrayShow = Invoke-AirScreenshot -Arguments @(
                "app", "tray", "show", "--json"
            )
            Assert-JsonResult `
                -Result $busyTrayShow `
                -ExpectedCode 5 `
                -ExpectedErrorType "busy" | Out-Null
            if ((Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash -ne
                    $settingsOpenConfigHash) {
                throw "app tray show changed configuration while settings were open."
            }
            Assert-SmokeHotkeyOccupied -Hotkey $smokeHotkey
            Send-SettingsKey `
                -Window $persistentSettingsWindow `
                -VirtualKey 0x1B
            Wait-SettingsWindowClosed
            Assert-SmokeHotkeyOccupied -Hotkey $smokeHotkey
        }
        else {
            Write-Host "SKIP: settings hotkey reservation requires an unlocked interactive desktop."
        }
        Stop-App

        if ($interactiveDesktop -and
            [AirScreenshotSmokeNative]::GetShellWindow() -ne [IntPtr]::Zero) {
            if (-not [AirScreenshotSmokeNative]::RegisterHotKey(
                    [IntPtr]::Zero,
                    $smokeHotkeyFixtureId,
                    $smokeHotkey.Modifiers,
                    $smokeHotkey.VirtualKey
                )) {
                throw "Unable to acquire the selected hotkey for the runtime-error fixture."
            }
            $heldSmokeHotkey = $true
            try {
                Write-IsolatedSmokeConfig `
                    -Path $configPath `
                    -ShellEnabled $true `
                    -TrayIconVisible $true `
                    -CaptureHotkey $smokeHotkey.Text
                $conflictedStart = Invoke-AirScreenshot -Arguments @(
                    "app", "start", "--json"
                )
                Assert-JsonResult `
                    -Result $conflictedStart `
                    -ExpectedCode 0 | Out-Null
                $conflictedStatusData = Get-AppStatus
                if (-not $conflictedStatusData.PSObject.Properties[
                        "hotkeysAvailable"] -or
                    -not $conflictedStatusData.PSObject.Properties[
                        "runtimeError"]) {
                    throw "app status omitted runtime hotkey health fields."
                }
                $conflictedStatus =
                    Convert-CompatibleAppStatus $conflictedStatusData
                if ($conflictedStatus.HotkeysAvailable -ne $false -or
                    [string]::IsNullOrWhiteSpace(
                        $conflictedStatus.RuntimeError)) {
                    throw "app status did not describe the injected hotkey registration failure."
                }
                Stop-App
            }
            finally {
                [AirScreenshotSmokeNative]::UnregisterHotKey(
                    [IntPtr]::Zero,
                    $smokeHotkeyFixtureId
                ) | Out-Null
                $heldSmokeHotkey = $false
            }
        }
        else {
            Write-Host "SKIP: runtimeError status fixture requires an interactive Explorer shell and a free hotkey."
        }
    }
    else {
        Write-Host "SKIP: no candidate smoke-test hotkey is currently available."
    }

    Remove-Item -LiteralPath $configPath -Force -ErrorAction SilentlyContinue

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

    if ($interactiveDesktop) {
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

        $missingRepeat = Invoke-AirScreenshot -Arguments @(
            "capture", "repeat", "--output", "file",
            "--path", "`"$(Join-Path $temporary 'missing-repeat.png')`"",
            "--json"
        )
        Assert-JsonResult `
            -Result $missingRepeat `
            -ExpectedCode 5 `
            -ExpectedErrorType "operation_failed" | Out-Null
        Wait-AirScreenshotProcessesExited

        $selectedRegionPath = Join-Path $temporary "selected-region.png"
        $selectedRegion = Start-AirScreenshot -Arguments @(
            "capture", "region", "--output", "file",
            "--path", "`"$selectedRegionPath`"", "--json"
        )
        $selectionOverlay = Wait-OverlayWindow -Run $selectedRegion
        if (-not [AirScreenshotSmokeNative]::DragRegionSelection(
                $selectionOverlay)) {
            throw "Unable to create a region selection for repeat capture."
        }
        # An explicit --output file request commits as soon as the initial
        # region drag ends. Do not post Enter to the old overlay HWND: on a
        # fast machine the session has already completed and destroyed it.
        $selectedRegionResult = Complete-AirScreenshot -Run $selectedRegion
        Assert-JsonResult `
            -Result $selectedRegionResult `
            -ExpectedCode 0 | Out-Null
        if (-not (Test-Png $selectedRegionPath)) {
            throw "The completed region fixture did not produce a PNG."
        }
        Wait-AppState -Running $false | Out-Null
        Wait-AirScreenshotProcessesExited

        $repeatPath = Join-Path $temporary "repeat-region.png"
        $repeatRegion = Invoke-AirScreenshot -Arguments @(
            "capture", "repeat", "--output", "file",
            "--path", "`"$repeatPath`"", "--json"
        )
        $repeatResponse = Assert-JsonResult `
            -Result $repeatRegion `
            -ExpectedCode 0
        if (-not [IO.Path]::GetFullPath([string]$repeatResponse.path).Equals(
                [IO.Path]::GetFullPath($repeatPath),
                [StringComparison]::OrdinalIgnoreCase
            ) -or -not (Test-Png $repeatPath)) {
            throw "Repeat capture did not reproduce the last completed region."
        }
        $repeatConfig = Get-Content -LiteralPath $configPath -Raw |
            ConvertFrom-Json -ErrorAction Stop
        if (-not $repeatConfig.capture.lastRegion -or
            [int]$repeatConfig.capture.lastRegion.width -lt 2 -or
            [int]$repeatConfig.capture.lastRegion.height -lt 2) {
            throw "Repeat capture did not retain a valid physical region history."
        }
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
    }
    else {
        Write-Host "SKIP: region overlay lifecycle requires an unlocked interactive desktop."
    }

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
    if ($heldSmokeHotkey) {
        [AirScreenshotSmokeNative]::UnregisterHotKey(
            [IntPtr]::Zero,
            $smokeHotkeyFixtureId
        ) | Out-Null
        $heldSmokeHotkey = $false
    }
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
