#Requires -Version 7.0

$ErrorActionPreference = "Stop"
$scriptsRoot = $PSScriptRoot
. (Join-Path $scriptsRoot "ocr-runner-payload.ps1")

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

$fixture = Join-Path ([IO.Path]::GetTempPath()) (
    "airshot-ocr-payload-policy-" + [guid]::NewGuid().ToString("N")
)
$outside = "$fixture-outside.txt"
$resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd(
    [IO.Path]::DirectorySeparatorChar
)
$resolvedFixture = [IO.Path]::GetFullPath($fixture)
if (-not $resolvedFixture.StartsWith(
        $resolvedTemp + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    ) -or
    -not [IO.Path]::GetFileName($resolvedFixture).StartsWith(
        "airshot-ocr-payload-policy-",
        [StringComparison]::Ordinal
    )) {
    throw "Unsafe OCR payload policy fixture path."
}

function Add-FixtureFile {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [byte[]]$Bytes = [byte[]]::new(0)
    )
    $path = Join-Path $fixture $RelativePath
    [IO.Directory]::CreateDirectory((Split-Path -Parent $path)) | Out-Null
    [IO.File]::WriteAllBytes($path, $Bytes)
    return $path
}

try {
    $allowedEmpty = @(
        "_internal/certifi/py.typed",
        "_internal/numpy-2.4.6.dist-info/REQUESTED",
        "_internal/pyreadline3-3.5.6.dist-info/REQUESTED",
        "_internal/tqdm-4.69.0.dist-info/REQUESTED"
    )
    foreach ($relative in $allowedEmpty) {
        Add-FixtureFile -RelativePath $relative | Out-Null
    }
    $sample = Add-FixtureFile `
        -RelativePath "_internal/setuptools/_vendor/jaraco/text/Lorem ipsum.txt" `
        -Bytes ([Text.Encoding]::UTF8.GetBytes("sample"))
    $unknownTyping = Add-FixtureFile -RelativePath "_internal/other/py.typed"
    $packageInitializer = Add-FixtureFile -RelativePath "_internal/other/__init__.py"
    $runtimeDll = Add-FixtureFile -RelativePath "_internal/runtime.dll"
    [IO.File]::WriteAllBytes($outside, [byte[]](9))

    Remove-NonRuntimeRunnerFiles -Root $fixture -AllowUnexpectedEmpty
    foreach ($relative in $allowedEmpty) {
        Assert-True `
            -Condition (-not (Test-Path -LiteralPath (Join-Path $fixture $relative))) `
            -Message "Approved empty metadata was not removed: $relative"
    }
    Assert-True -Condition (-not (Test-Path -LiteralPath $sample)) `
        -Message "Non-runtime sample data was not removed."
    $nonEmptyMarker = Add-FixtureFile `
        -RelativePath "_internal/certifi/py.typed" `
        -Bytes ([byte[]](1, 2, 3))
    foreach ($path in @($unknownTyping, $packageInitializer, $runtimeDll, $nonEmptyMarker, $outside)) {
        Assert-True -Condition (Test-Path -LiteralPath $path -PathType Leaf) `
            -Message "Payload policy removed an unapproved file: $path"
    }

    $rejected = $false
    try {
        Remove-NonRuntimeRunnerFiles -Root $fixture
    }
    catch {
        $rejected = $_.Exception.Message -like "*unexpected empty OCR runner files*"
    }
    Assert-True -Condition $rejected `
        -Message "Unexpected empty runtime files were not rejected."

    foreach ($path in @($unknownTyping, $packageInitializer, $runtimeDll)) {
        Remove-Item -LiteralPath $path -Force
    }
    Remove-NonRuntimeRunnerFiles -Root $fixture
    Remove-NonRuntimeRunnerFiles -Root $fixture
    Assert-True -Condition ((Get-Item -LiteralPath $nonEmptyMarker).Length -eq 3) `
        -Message "Sanitization is not idempotent for retained files."

    $fingerprintRoot = Join-Path $fixture "fingerprint"
    Add-FixtureFile `
        -RelativePath "fingerprint/rapidocr_runner.exe" `
        -Bytes ([byte[]](1, 2, 3, 4)) | Out-Null
    $truncatedRuntime = Add-FixtureFile `
        -RelativePath "fingerprint/_internal/runtime.dll"
    Assert-True `
        -Condition ((Get-RunnerOutputFingerprint -Root $fingerprintRoot) -ceq "") `
        -Message "A truncated cached runner did not request a rebuild."
    [IO.File]::WriteAllBytes($truncatedRuntime, [byte[]](5, 6, 7, 8))
    $firstFingerprint = Get-RunnerOutputFingerprint -Root $fingerprintRoot
    Assert-True `
        -Condition ($firstFingerprint -cmatch "^[A-F0-9]{64}$") `
        -Message "A complete cached runner did not produce a fingerprint."
    [IO.File]::WriteAllBytes($truncatedRuntime, [byte[]](5, 6, 7, 9))
    $secondFingerprint = Get-RunnerOutputFingerprint -Root $fingerprintRoot
    Assert-True `
        -Condition ($secondFingerprint -cne $firstFingerprint) `
        -Message "Runner fingerprint did not detect a file content change."

    foreach ($valid in @(
            "rapidocr_runner.exe",
            "_internal/numpy.libs/library.dll",
            "models/rapidocr-v5-fast/det.onnx"
        )) {
        Assert-True -Condition (Test-OcrManifestRelativePath -Path $valid) `
            -Message "Valid signed dependency path was rejected: $valid"
    }
    foreach ($invalid in @(
            "_internal/setuptools/Lorem ipsum.txt",
            "../escape.dll",
            "/rooted.dll",
            "models//det.onnx",
            "models/CON/file.onnx",
            "models/trailing./file.onnx"
        )) {
        Assert-True -Condition (-not (Test-OcrManifestRelativePath -Path $invalid)) `
            -Message "Invalid signed dependency path was accepted: $invalid"
    }

    Write-Host "OCR payload policy tests passed."
}
finally {
    if (Test-Path -LiteralPath $outside) {
        Remove-Item -LiteralPath $outside -Force
    }
    if (Test-Path -LiteralPath $fixture) {
        [IO.Directory]::Delete($fixture, $true)
    }
}
