param(
    [switch]$RemoveData,
    [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$driverName = "PathOverlayFlt"
$serviceName = "PathOverlaySvc"
$servicePath = Join-Path $root "PathOverlaySvc.exe"

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
    if ($RemoveData) {
        $arguments += "-RemoveData"
    }
    if ($ValidateOnly) {
        $arguments += "-ValidateOnly"
    }

    Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -Verb RunAs
    exit 0
}

function Assert-RequiredFiles {
    if (-not (Test-Path -LiteralPath $servicePath)) {
        throw "Missing required file: $servicePath. Run the script from x64\Debug or x64\Release after building the project."
    }
    Write-Host "Found $servicePath"
}

function Stop-PathOverlayService {
    $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
    if ($null -eq $service) {
        Write-Host "$serviceName service is not installed."
        return
    }

    if ($service.Status -ne "Stopped") {
        Write-Step "Stopping user-mode service"
        & $servicePath stop | Out-Host
        Start-Sleep -Seconds 1
    }
}

function Remove-PathOverlayService {
    $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
    if ($null -eq $service) {
        return
    }

    Write-Step "Uninstalling user-mode service"
    & $servicePath uninstall | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to uninstall $serviceName. ExitCode=$LASTEXITCODE"
    }
    Start-Sleep -Seconds 1
}

function Unload-PathOverlayDriver {
    $filter = & fltmc.exe filters | Select-String -SimpleMatch $driverName
    if (-not $filter) {
        Write-Host "$driverName filter is not loaded."
        return
    }

    Write-Step "Unloading minifilter driver"
    & fltmc.exe unload $driverName | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to unload $driverName. ExitCode=$LASTEXITCODE"
    }
    Start-Sleep -Seconds 1
}

function Remove-PathOverlayDriverService {
    $existing = & sc.exe query $driverName 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "$driverName driver service is not installed."
        return
    }

    Unload-PathOverlayDriver

    Write-Step "Deleting minifilter driver service"
    & sc.exe stop $driverName | Out-Null
    & sc.exe delete $driverName | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to delete $driverName driver service. ExitCode=$LASTEXITCODE"
    }
    Start-Sleep -Seconds 1
}

function Remove-PathOverlayData {
    if (-not $RemoveData) {
        return
    }

    Write-Step "Removing service data"
    $programDataPath = Join-Path $env:ProgramData "PathOverlay"
    if (Test-Path -LiteralPath $programDataPath) {
        Remove-Item -LiteralPath $programDataPath -Recurse -Force
        Write-Host "Removed $programDataPath"
    } else {
        Write-Host "$programDataPath was not found."
    }
}

Write-Step "Checking build output"
Assert-RequiredFiles

if ($ValidateOnly) {
    Write-Host "Validation completed. No service, driver, or data changes were made."
    exit 0
}

Assert-Admin
Stop-PathOverlayService
Remove-PathOverlayService
Remove-PathOverlayDriverService
Remove-PathOverlayData

Write-Step "Done"
Write-Host "PathOverlay service and driver have been uninstalled."
if (-not $RemoveData) {
    Write-Host "ProgramData was preserved. Run .\uninstall.ps1 -RemoveData to remove it."
}
