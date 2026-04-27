param(
    [switch]$IncludeServiceData,
    [switch]$IncludeDriverAndService
)

$ErrorActionPreference = "Stop"

$driverName = "PathOverlayFlt"
$svcName = "PathOverlaySvc"
$repoRoot = Split-Path -Parent $PSScriptRoot
$serviceCandidates = @(
    (Join-Path $PSScriptRoot "PathOverlaySvc.exe"),
    (Join-Path $repoRoot "x64\Debug\PathOverlaySvc.exe"),
    (Join-Path $repoRoot "x64\Release\PathOverlaySvc.exe"),
    (Join-Path $PSScriptRoot "..\test-machine-package\PathOverlaySvc.exe")
)

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "IncludeDriverAndService and IncludeServiceData cleanup require an elevated PowerShell session."
    }
}

function Get-ServiceBinary {
    return $serviceCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

function Remove-TempPathOverlayData {
    $targets = @(
        (Join-Path $env:TEMP "PathOverlayTests")
    )

    Get-ChildItem -LiteralPath $env:TEMP -Directory -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -like "PathOverlayNoRule-*" -or
            $_.Name -like "PathOverlayRule-*" -or
            $_.Name -like "PathOverlayCommit-*"
        } |
        ForEach-Object { $targets += $_.FullName }

    foreach ($target in $targets | Select-Object -Unique) {
        if (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host "Removed $target"
        }
    }
}

function Remove-ServiceData {
    $programDataPath = Join-Path $env:ProgramData "PathOverlay"
    if (Test-Path -LiteralPath $programDataPath) {
        Remove-Item -LiteralPath $programDataPath -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Removed $programDataPath"
    }
}

function Remove-InstalledComponents {
    $serviceBinary = Get-ServiceBinary
    $service = Get-Service -Name $svcName -ErrorAction SilentlyContinue
    if ($null -ne $service) {
        if ($service.Status -ne "Stopped" -and $serviceBinary) {
            & $serviceBinary stop | Out-Host
        }
        if ($serviceBinary) {
            & $serviceBinary uninstall | Out-Host
        } else {
            sc.exe delete $svcName | Out-Host
        }
    }

    $filter = & fltmc.exe filters 2>$null | Select-String -SimpleMatch $driverName
    if ($filter) {
        fltmc.exe unload $driverName | Out-Host
    }

    $driver = & sc.exe query $driverName 2>$null
    if ($LASTEXITCODE -eq 0) {
        sc.exe stop $driverName | Out-Null
        sc.exe delete $driverName | Out-Host
    }
}

Remove-TempPathOverlayData

if ($IncludeServiceData -or $IncludeDriverAndService) {
    Assert-Admin
    Remove-ServiceData
}

if ($IncludeDriverAndService) {
    Remove-InstalledComponents
}

Write-Host "PathOverlay test cleanup completed."
