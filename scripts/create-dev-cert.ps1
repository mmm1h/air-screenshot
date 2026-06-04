[CmdletBinding()]
param(
    [string]$Password = "air-screenshot-dev"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$certDir = Join-Path $root "dist\cert"
New-Item -ItemType Directory -Path $certDir -Force | Out-Null

$cert = Get-ChildItem "Cert:\CurrentUser\My" |
    Where-Object {
        $_.Subject -eq "CN=AirScreenshot Dev" -and
        $_.HasPrivateKey -and
        $_.NotAfter -gt (Get-Date).AddDays(30)
    } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if (-not $cert) {
    $cert = New-SelfSignedCertificate `
        -Type Custom `
        -Subject "CN=AirScreenshot Dev" `
        -FriendlyName "Air Screenshot Development" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -HashAlgorithm SHA256 `
        -KeyUsage DigitalSignature `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
}

$secure = ConvertTo-SecureString -String $Password -Force -AsPlainText
$pfx = Join-Path $certDir "AirScreenshot.Dev.pfx"
$cer = Join-Path $certDir "AirScreenshot.Dev.cer"
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $secure | Out-Null
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
Write-Host "开发证书已生成：$pfx"
Write-Host "安装 MSIX 前，请在管理员 PowerShell 中运行 scripts\trust-dev-cert.ps1。"
