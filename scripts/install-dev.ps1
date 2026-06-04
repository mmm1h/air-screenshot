[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$package = Get-ChildItem (Join-Path $root "dist\site\AirScreenshot-*.msix") | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $package) { throw "未找到 MSIX，请先运行 scripts\package.ps1 -Sign。" }

$signer = (Get-AuthenticodeSignature -FilePath $package.FullName).SignerCertificate
if (-not $signer -or -not (Test-Path "Cert:\LocalMachine\TrustedPeople\$($signer.Thumbprint)")) {
    throw "开发证书尚未加入本地计算机受信任的人。请先在管理员 PowerShell 中运行 scripts\trust-dev-cert.ps1。"
}

Add-AppxPackage -Path $package.FullName -ForceApplicationShutdown
Write-Host "已安装：$($package.FullName)"
