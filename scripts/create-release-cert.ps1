[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Password,
    [string]$Subject = "CN=Air Screenshot",
    [int]$ValidYears = 5
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$certDir = Join-Path $root "dist\cert"
New-Item -ItemType Directory -Path $certDir -Force | Out-Null

$existing = Get-ChildItem "Cert:\CurrentUser\My" |
    Where-Object {
        $_.Subject -eq $Subject -and
        $_.HasPrivateKey -and
        $_.NotAfter -gt (Get-Date).AddDays(30)
    } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

$certificate = if ($existing) {
    $existing
} else {
    New-SelfSignedCertificate `
        -Type Custom `
        -Subject $Subject `
        -FriendlyName "Air Screenshot Release Signing" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -HashAlgorithm SHA256 `
        -KeyExportPolicy Exportable `
        -KeyUsage DigitalSignature `
        -NotAfter (Get-Date).AddYears($ValidYears) `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
}

$secure = ConvertTo-SecureString -String $Password -Force -AsPlainText
$pfx = Join-Path $certDir "AirScreenshot.Release.pfx"
$cer = Join-Path $certDir "AirScreenshot.Release.cer"
Export-PfxCertificate -Cert $certificate -FilePath $pfx -Password $secure | Out-Null
Export-Certificate -Cert $certificate -FilePath $cer | Out-Null

Write-Host "发布证书已生成：$pfx"
Write-Host "请将 PFX 与密码保存到 GitHub Actions Secrets，不要提交到仓库。"
