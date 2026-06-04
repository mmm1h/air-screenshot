[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Version,
    [Parameter(Mandatory)]
    [string]$PackageName,
    [Parameter(Mandatory)]
    [string]$Publisher,
    [switch]$RequireTrustedSignature
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$semver = ($Version -split "\.")[0..2] -join "."
$site = Join-Path $root "dist\site"
$msix = Join-Path $site "AirScreenshot-$semver.msix"
$appInstallerPath = Join-Path $site "AirScreenshot.appinstaller"

foreach ($path in @(
    $msix,
    $appInstallerPath,
    (Join-Path $site "Install-AirScreenshot.ps1"),
    (Join-Path $site "index.html")
)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "缺少发布文件：$path"
    }
}

$package = Get-Item -LiteralPath $msix
if ($package.Length -gt 15MB) {
    throw "MSIX 超过 15MB：$([Math]::Round($package.Length / 1MB, 2))MB"
}

$signature = Get-AuthenticodeSignature -FilePath $msix
if (-not $signature.SignerCertificate) {
    throw "MSIX 没有签名。"
}
if ($signature.SignerCertificate.Subject -ne $Publisher) {
    throw "签名证书主题与 Publisher 不匹配：$($signature.SignerCertificate.Subject)"
}
if ($RequireTrustedSignature -and $signature.Status -ne [Management.Automation.SignatureStatus]::Valid) {
    throw "MSIX 签名未通过信任验证：$($signature.StatusMessage)"
}

[xml]$appInstaller = Get-Content -LiteralPath $appInstallerPath -Raw
$namespace = [Xml.XmlNamespaceManager]::new($appInstaller.NameTable)
$namespace.AddNamespace("a", $appInstaller.DocumentElement.NamespaceURI)
$mainPackage = $appInstaller.SelectSingleNode("/a:AppInstaller/a:MainPackage", $namespace)
$onLaunch = $appInstaller.SelectSingleNode("/a:AppInstaller/a:UpdateSettings/a:OnLaunch", $namespace)
$backgroundTask = $appInstaller.SelectSingleNode("/a:AppInstaller/a:UpdateSettings/a:AutomaticBackgroundTask", $namespace)

if (-not $mainPackage -or $mainPackage.Name -ne $PackageName -or $mainPackage.Publisher -ne $Publisher -or
    $mainPackage.Version -ne $Version) {
    throw "App Installer 的包身份与预期不匹配。"
}
if (-not $onLaunch -or $onLaunch.HoursBetweenUpdateChecks -ne "0" -or $onLaunch.ShowPrompt -ne "false") {
    throw "App Installer 未配置启动时静默更新检查。"
}
if (-not $backgroundTask) {
    throw "App Installer 未配置后台静默更新检查。"
}

Write-Host "发布包验证通过：$msix"
