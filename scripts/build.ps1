[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "未找到包含 C++ 工具链的 Visual Studio。" }

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if ($Clean -and (Test-Path $build)) {
    Remove-Item -LiteralPath $build -Recurse -Force
}

$command = "`"$vcvars`" && `"$cmake`" -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Configuration -DBUILD_TESTING=ON && `"$cmake`" --build `"$build`" --config $Configuration"
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "构建失败，退出码 $LASTEXITCODE。" }

Write-Host "构建完成：$build"
