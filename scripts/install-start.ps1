param(
    [switch]$ResetData,
    [switch]$SkipDriver,
    [switch]$SkipTestSigningCheck,
    [switch]$ValidateOnly,
    [string]$CertificateSubject = "CN=PathOverlay Test Certificate"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$driverName = "PathOverlayFlt"
$serviceName = "PathOverlaySvc"
$driverPath = Join-Path $root "PathOverlayFlt.sys"
$servicePath = Join-Path $root "PathOverlaySvc.exe"
$cliPath = Join-Path $root "pathoverlay.exe"
$sqlitePath = Join-Path $root "sqlite3.dll"
$certPath = Join-Path $root "PathOverlayTest.cer"

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "== $Message" -ForegroundColor Cyan
}

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        return
    }

    Write-Host "Requesting administrator elevation..."
    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$PSCommandPath`""
    )
    if ($ResetData) {
        $arguments += "-ResetData"
    }
    if ($SkipDriver) {
        $arguments += "-SkipDriver"
    }
    if ($SkipTestSigningCheck) {
        $arguments += "-SkipTestSigningCheck"
    }
    if ($ValidateOnly) {
        $arguments += "-ValidateOnly"
    }
    if ($CertificateSubject) {
        $arguments += @("-CertificateSubject", "`"$CertificateSubject`"")
    }

    Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -Verb RunAs
    exit 0
}

function Invoke-Checked([string]$FilePath, [string[]]$Arguments, [string]$FailureMessage) {
    Write-Host "$FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage ExitCode=$LASTEXITCODE"
    }
}

function Assert-RequiredFiles {
    $required = @($servicePath, $cliPath, $sqlitePath)
    if (-not $SkipDriver) {
        $required += $driverPath
    }

    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Missing required file: $path. Run the script from x64\Debug or x64\Release after building the project."
        }
        Write-Host "Found $path"
    }
}

function Assert-TestSigning {
    if ($SkipDriver -or $SkipTestSigningCheck) {
        return
    }

    $bcd = & bcdedit.exe /enum "{current}" | Out-String
    if ($bcd -notmatch "(?im)^\s*testsigning\s+Yes\s*$") {
        throw "Windows test-signing is not enabled. Run 'bcdedit /set testsigning on' as administrator, reboot, then run this script again."
    }
}

function Import-TestCertificate {
    if ($SkipDriver -or -not (Test-Path -LiteralPath $certPath)) {
        return
    }

    Write-Step "Importing test certificate"
    Import-Certificate -FilePath $certPath -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
    Import-Certificate -FilePath $certPath -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null
}

function Find-SignTool {
    $kitsRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    $tool = Get-ChildItem -Path $kitsRoot -Recurse -Filter "signtool.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\signtool.exe$" } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $tool) {
        throw "signtool.exe was not found under $kitsRoot. Install Windows SDK/WDK or use scripts\package-test-machine.ps1 to create a signed package."
    }
    return $tool.FullName
}

function Get-OrCreateCertificate {
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

    Export-Certificate -Cert $cert -FilePath $certPath | Out-Null
    return ($CertificateSubject -replace '^CN=', '')
}

function Ensure-DriverSignature {
    if ($SkipDriver) {
        return
    }

    $signature = Get-AuthenticodeSignature -FilePath $driverPath
    if ($signature.Status -eq "Valid") {
        Write-Host "Driver signature is valid."
        return
    }

    Write-Step "Signing driver for test mode"
    $subjectName = Get-OrCreateCertificate
    $signTool = Find-SignTool
    & $signTool sign /v /fd SHA256 /s My /n $subjectName $driverPath
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed to sign $driverPath."
    }
}

function Stop-PathOverlayService {
    $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
    if ($null -ne $service -and $service.Status -ne "Stopped") {
        & $servicePath stop | Out-Host
        Start-Sleep -Seconds 1
    }
}

function Remove-PathOverlayService {
    $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
    if ($null -ne $service) {
        Stop-PathOverlayService
        & $servicePath uninstall | Out-Host
        Start-Sleep -Seconds 1
    }
}

function Unload-PathOverlayDriver {
    if ($SkipDriver) {
        return
    }

    $filter = & fltmc.exe filters | Select-String -SimpleMatch $driverName
    if ($filter) {
        & fltmc.exe unload $driverName | Out-Host
        Start-Sleep -Seconds 1
    }
}

function Remove-PathOverlayDriverService {
    if ($SkipDriver) {
        return
    }

    $existing = & sc.exe query $driverName 2>$null
    if ($LASTEXITCODE -eq 0) {
        Unload-PathOverlayDriver
        & sc.exe stop $driverName | Out-Null
        & sc.exe delete $driverName | Out-Host
        Start-Sleep -Seconds 1
    }
}

function Reset-PathOverlayData {
    if (-not $ResetData) {
        return
    }

    Write-Step "Resetting service data"
    $programDataPath = Join-Path $env:ProgramData "PathOverlay"
    if (Test-Path -LiteralPath $programDataPath) {
        Remove-Item -LiteralPath $programDataPath -Recurse -Force
    }
}

function Install-PathOverlayDriver {
    if ($SkipDriver) {
        Write-Host "Skipping driver installation by request."
        return
    }

    Write-Step "Installing and loading minifilter driver"
    Remove-PathOverlayDriverService

    Invoke-Checked "sc.exe" @(
        "create", $driverName,
        "type=", "filesys",
        "binPath=", $driverPath,
        "start=", "demand",
        "error=", "normal",
        "depend=", "FltMgr",
        "DisplayName=", "PathOverlay minifilter driver"
    ) "Failed to create driver service."

    & reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\$driverName\Instances" /v DefaultInstance /t REG_SZ /d "PathOverlayFlt Instance" /f | Out-Host
    & reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\$driverName\Instances\PathOverlayFlt Instance" /v Altitude /t REG_SZ /d "370030" /f | Out-Host
    & reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\$driverName\Instances\PathOverlayFlt Instance" /v Flags /t REG_DWORD /d 0 /f | Out-Host

    Invoke-Checked "fltmc.exe" @("load", $driverName) "Failed to load PathOverlayFlt."
}

function Install-PathOverlayService {
    Write-Step "Installing and starting user-mode service"
    Remove-PathOverlayService
    Reset-PathOverlayData

    Invoke-Checked $servicePath @("install") "Failed to install PathOverlaySvc."
    Invoke-Checked $servicePath @("start") "Failed to start PathOverlaySvc."
}

function Test-PathOverlayStartup {
    Write-Step "Verifying service and driver"
    Invoke-Checked $cliPath @("rule", "show") "CLI IPC verification failed."
    if (-not $SkipDriver) {
        Invoke-Checked $cliPath @("driver", "status") "Driver connection verification failed."
    }
}

Write-Step "Checking build output"
Assert-RequiredFiles

if ($ValidateOnly) {
    Write-Host "Validation completed. No driver or service changes were made."
    exit 0
}

Assert-Admin
Assert-TestSigning
Ensure-DriverSignature
Import-TestCertificate
Install-PathOverlayDriver
Install-PathOverlayService
Test-PathOverlayStartup

Write-Step "Ready"
Write-Host "PathOverlay is installed and running."
Write-Host ""
Write-Host "Example commands:"
Write-Host "  .\pathoverlay.exe rule add C:\Temp\PathOverlaySource"
Write-Host "  .\pathoverlay.exe rule show"
Write-Host "  .\pathoverlay.exe changes"
Write-Host "  .\pathoverlay.exe commit"
Write-Host "  .\pathoverlay.exe discard"
