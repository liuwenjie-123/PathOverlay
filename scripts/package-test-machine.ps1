param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64")]
    [string]$Platform = "x64",

    [string]$CertificateSubject = "CN=PathOverlay Test Certificate"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$packageRoot = Join-Path $repoRoot "test-machine-package"
$outputRoot = Join-Path $repoRoot "$Platform\$Configuration"
$driverProjectOutput = Join-Path $repoRoot "driver\PathOverlayFlt\$Platform\$Configuration"
$certOutput = Join-Path $packageRoot "PathOverlayTest.cer"

function Find-SignTool {
    $kitsRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    $tool = Get-ChildItem -Path $kitsRoot -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\signtool.exe$" } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $tool) {
        throw "signtool.exe was not found under $kitsRoot."
    }
    return $tool.FullName
}

function Get-OrCreateCertificate {
    $subjectName = $CertificateSubject -replace '^CN=', ''
    $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.Subject -eq $CertificateSubject } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

    if (-not $cert) {
        $cert = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject $CertificateSubject `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -KeyExportPolicy Exportable `
            -HashAlgorithm SHA256 `
            -NotAfter (Get-Date).AddYears(5)
    }

    Export-Certificate -Cert $cert -FilePath $certOutput | Out-Null
    return $subjectName
}

function Copy-VCRuntime {
    $redistRoot = Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\Community\VC\Redist\MSVC"
    $crtDir = Get-ChildItem -Path $redistRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC143.CRT" } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1

    if (-not $crtDir) {
        Write-Warning "VC++ x64 redist DLL directory was not found. The test VM may need the Visual C++ Redistributable installed."
        return
    }

    Get-ChildItem -Path $crtDir -Filter *.dll | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $packageRoot $_.Name) -Force
    }
}

function Find-SqliteRuntime {
    $candidates = @(
        (Join-Path $outputRoot "sqlite3.dll"),
        (Join-Path $repoRoot "x64\Debug\sqlite3.dll"),
        (Join-Path $repoRoot "tests\x64\Debug\sqlite3.dll"),
        (Join-Path $repoRoot "service\PathOverlaySvc\x64\Debug\sqlite3.dll")
    )

    return $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

& (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -Platform $Platform

New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

$requiredFiles = @(
    @{ Source = Join-Path $outputRoot "PathOverlayFlt.sys"; Target = "PathOverlayFlt.sys" },
    @{ Source = Join-Path $repoRoot "driver\PathOverlayFlt\PathOverlayFlt.inf"; Target = "PathOverlayFlt.inf" },
    @{ Source = Join-Path $outputRoot "PathOverlaySvc.exe"; Target = "PathOverlaySvc.exe" },
    @{ Source = Join-Path $outputRoot "pathoverlay.exe"; Target = "pathoverlay.exe" },
    @{ Source = Join-Path $repoRoot "scripts\cleanup-test-data.ps1"; Target = "cleanup-test-data.ps1" },
    @{ Source = Join-Path $repoRoot "scripts\install-start.ps1"; Target = "install-start.ps1" },
    @{ Source = Join-Path $repoRoot "scripts\Install-Start-PathOverlay.cmd"; Target = "Install-Start-PathOverlay.cmd" },
    @{ Source = Join-Path $repoRoot "scripts\uninstall.ps1"; Target = "uninstall.ps1" },
    @{ Source = Join-Path $repoRoot "scripts\Uninstall-PathOverlay.cmd"; Target = "Uninstall-PathOverlay.cmd" }
)

foreach ($file in $requiredFiles) {
    if (-not (Test-Path $file.Source)) {
        throw "Required build artifact was not found: $($file.Source)"
    }
    Copy-Item -LiteralPath $file.Source -Destination (Join-Path $packageRoot $file.Target) -Force
}

$sqlite = Find-SqliteRuntime
if ($sqlite) {
    Copy-Item -LiteralPath $sqlite -Destination (Join-Path $packageRoot "sqlite3.dll") -Force
} else {
    Write-Warning "sqlite3.dll was not found. Service metadata operations will fail on a clean test VM."
}

Copy-VCRuntime

$subjectName = Get-OrCreateCertificate
$signTool = Find-SignTool
$driverToSign = Join-Path $packageRoot "PathOverlayFlt.sys"
& $signTool sign /v /fd SHA256 /s My /n $subjectName $driverToSign
if ($LASTEXITCODE -ne 0) {
    throw "signtool failed to sign $driverToSign."
}

Write-Host "Test machine package is ready:"
Write-Host "  $packageRoot"
Write-Host ""
Write-Host "Copy the whole directory to the test VM and double-click Run-PathOverlay-Test.cmd."
