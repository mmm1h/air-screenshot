[CmdletBinding()]
param(
    [string]$CertPath
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $CertPath) {
    $CertPath = Join-Path $root "dist\cert\AirScreenshot.Dev.cer"
}

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "信任开发证书需要管理员权限。请在管理员 PowerShell 中重新运行此脚本。"
}
if (-not (Test-Path -LiteralPath $CertPath)) {
    throw "未找到开发证书：$CertPath。请先运行 scripts\create-dev-cert.ps1。"
}

$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($CertPath)
$storePath = "Cert:\LocalMachine\TrustedPeople"
if (-not (Test-Path (Join-Path $storePath $certificate.Thumbprint))) {
    Import-Certificate -FilePath $CertPath -CertStoreLocation $storePath | Out-Null
}

Write-Host "开发证书已加入本地计算机受信任的人：$($certificate.Thumbprint)"
