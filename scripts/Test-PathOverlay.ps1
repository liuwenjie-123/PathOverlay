param(
    [switch]$SkipCleanup
)

$ErrorActionPreference = "Stop"
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$logDir = Join-Path $root "logs"
$driverName = "PathOverlayFlt"
$svcName = "PathOverlaySvc"
$driverPath = Join-Path $root "PathOverlayFlt.sys"
$servicePath = Join-Path $root "PathOverlaySvc.exe"
$cliPath = Join-Path $root "pathoverlay.exe"
$certPath = Join-Path $root "PathOverlayTest.cer"

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "== $Message" -ForegroundColor Cyan
}

function Write-Check([string]$Name, [bool]$Passed, [string]$Detail = "") {
    $status = if ($Passed) { "PASS" } else { "FAIL" }
    $color = if ($Passed) { "Green" } else { "Red" }
    $line = "[$status] $Name"
    if ($Detail) {
        $line += " - $Detail"
    }
    Write-Host $line -ForegroundColor $color
    if (-not $Passed) {
        throw $line
    }
}

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host "Requesting administrator elevation..."
        $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
        if ($SkipCleanup) {
            $argList += "-SkipCleanup"
        }
        Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs
        exit 0
    }
}

function Invoke-Checked([string]$FilePath, [string[]]$Arguments, [string]$FailureMessage) {
    $display = "$FilePath $($Arguments -join ' ')"
    Write-Host $display
    $output = & $FilePath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $output | Out-Host
    if ($exitCode -ne 0) {
        throw "$FailureMessage ExitCode=$exitCode Output=$($output -join ' ')"
    }
    return ($output -join "`n")
}

function Invoke-Cli([string[]]$Arguments, [string]$FailureMessage) {
    return Invoke-Checked $cliPath $Arguments $FailureMessage
}

function Invoke-CliExpectFailure([string[]]$Arguments, [string]$FailureMessage) {
    $display = "$cliPath $($Arguments -join ' ')"
    Write-Host $display
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $cliPath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    $output | Out-Host
    if ($exitCode -eq 0) {
        throw "$FailureMessage Command unexpectedly succeeded. Output=$($output -join ' ')"
    }
    return ($output -join "`n")
}

function ConvertTo-CanonicalPath([string]$Path) {
    $fullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd("\")
    if (Test-Path -LiteralPath $fullPath) {
        return (Get-Item -LiteralPath $fullPath).FullName.TrimEnd("\")
    }

    $parent = Split-Path -Parent $fullPath
    $leaf = Split-Path -Leaf $fullPath
    if ($parent -and (Test-Path -LiteralPath $parent)) {
        return (Join-Path (Get-Item -LiteralPath $parent).FullName $leaf).TrimEnd("\")
    }

    return $fullPath
}

function ConvertTo-ShadowPath([string]$Store, [string]$RealPath) {
    $fullPath = ConvertTo-CanonicalPath $RealPath
    $pathWithoutDriveColon = $fullPath -replace "^([A-Za-z]):", '$1'
    return Join-Path (Join-Path $Store "drive") $pathWithoutDriveColon
}

function ConvertTo-ShortPath([string]$Path) {
    $fullPath = ConvertTo-CanonicalPath $Path
    $shortPath = cmd.exe /d /c "for %I in (""$fullPath"") do @echo %~sI" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $shortPath) {
        return ""
    }
    return ($shortPath | Select-Object -Last 1).Trim()
}

function Parse-Rules([string]$RuleShowOutput) {
    $rules = @()
    foreach ($line in ($RuleShowOutput -split "`r?`n")) {
        if ($line -match "^(?<id>\S+)\s+enabled=(?<enabled>true|false)\s+source=(?<source>.+?)\s+store=(?<store>.+)$") {
            $rules += [pscustomobject]@{
                Id = $Matches.id
                Enabled = ($Matches.enabled -eq "true")
                Source = $Matches.source.Trim()
                Store = $Matches.store.Trim()
            }
        }
    }
    return $rules
}

function Get-RuleBySource([string]$RuleShowOutput, [string]$Source) {
    $canonicalSource = ConvertTo-CanonicalPath $Source
    $rules = Parse-Rules $RuleShowOutput
    foreach ($rule in $rules) {
        if ((ConvertTo-CanonicalPath $rule.Source) -ieq $canonicalSource) {
            return $rule
        }
    }
    throw "rule show output did not include source '$canonicalSource'. Output=$RuleShowOutput"
}

function Test-ChangesContainsRule([string]$ChangesOutput, [string]$RuleId, [string]$Source, [string]$Store, [string]$State, [string]$Path) {
    $canonicalSource = [regex]::Escape((ConvertTo-CanonicalPath $Source))
    $canonicalStore = [regex]::Escape((ConvertTo-CanonicalPath $Store))
    $canonicalPath = [regex]::Escape((ConvertTo-CanonicalPath $Path))
    $rulePattern = "rule=$([regex]::Escape($RuleId))\s+enabled=(true|false)\s+source=$canonicalSource\s+store=$canonicalStore"
    $changePattern = "\s+$([regex]::Escape($State))\s+$canonicalPath"
    return ($ChangesOutput -match $rulePattern) -and ($ChangesOutput -match $changePattern)
}

function Write-ServiceDiagnostics {
    Write-Step "PathOverlaySvc diagnostics"

    & sc.exe queryex $svcName | Out-Host

    $serviceLog = Join-Path $env:ProgramData "PathOverlay\logs\PathOverlaySvc.log"
    if (Test-Path $serviceLog) {
        Write-Host ""
        Write-Host "PathOverlaySvc.log tail:"
        Get-Content -LiteralPath $serviceLog -Tail 160 | Out-Host
    } else {
        Write-Host "PathOverlaySvc.log was not found at $serviceLog"
    }

    Write-Host ""
    Write-Host "Recent Service Control Manager events:"
    Get-WinEvent -FilterHashtable @{LogName="System"; ProviderName="Service Control Manager"; StartTime=(Get-Date).AddMinutes(-15)} -MaxEvents 12 -ErrorAction SilentlyContinue |
        Select-Object TimeCreated, Id, ProviderName, Message |
        Format-List |
        Out-Host
}

function Invoke-DiagnosticsCollect {
    Write-Step "Collecting PathOverlay diagnostics package"

    if (-not (Test-Path -LiteralPath $cliPath)) {
        Write-Host "pathoverlay.exe was not found; diagnostics collect skipped: $cliPath"
        return
    }

    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    $diagnosticsPath = Join-Path $logDir ("PathOverlay-Diagnostics-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    try {
        $output = & $cliPath diagnostics collect --output $diagnosticsPath 2>&1
        $exitCode = $LASTEXITCODE
        $output | Out-Host
        if ($exitCode -eq 0) {
            Write-Host "Diagnostics package: $diagnosticsPath" -ForegroundColor Yellow
        } else {
            Write-Host "Diagnostics collect failed with ExitCode=$exitCode. Partial path: $diagnosticsPath" -ForegroundColor Yellow
        }
    } catch {
        Write-Host "Diagnostics collect failed: $($_.Exception.Message). Partial path: $diagnosticsPath" -ForegroundColor Yellow
    }
}

function Stop-PathOverlayService {
    $service = Get-Service -Name $svcName -ErrorAction SilentlyContinue
    if ($null -ne $service -and $service.Status -ne "Stopped") {
        & $servicePath stop | Out-Host
        Start-Sleep -Seconds 1
    }
}

function Remove-PathOverlayService {
    $service = Get-Service -Name $svcName -ErrorAction SilentlyContinue
    if ($null -ne $service) {
        Stop-PathOverlayService
        & $servicePath uninstall | Out-Host
        Start-Sleep -Seconds 1
    }
}

function Reset-PathOverlayData {
    $programDataPath = Join-Path $env:ProgramData "PathOverlay"
    if (Test-Path $programDataPath) {
        Remove-Item -LiteralPath $programDataPath -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Unload-PathOverlayDriver {
    $filter = & fltmc.exe filters | Select-String -SimpleMatch $driverName
    if ($filter) {
        & fltmc.exe unload $driverName | Out-Host
        Start-Sleep -Seconds 1
    }
}

function Remove-PathOverlayDriverService {
    $existing = & sc.exe query $driverName 2>$null
    if ($LASTEXITCODE -eq 0) {
        Unload-PathOverlayDriver
        & sc.exe stop $driverName | Out-Null
        & sc.exe delete $driverName | Out-Host
        Start-Sleep -Seconds 1
    }
}

function Install-TestCertificate {
    if (-not (Test-Path $certPath)) {
        Write-Host "No PathOverlayTest.cer found. Assuming the driver is already trusted."
        return
    }

    Write-Step "Importing test certificate"
    Import-Certificate -FilePath $certPath -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
    Import-Certificate -FilePath $certPath -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null
    Write-Check "test certificate imported" $true $certPath
}

function Assert-TestSigning {
    $bcd = & bcdedit.exe /enum "{current}" | Out-String
    Write-Host $bcd
    Write-Check "Windows test-signing enabled" ($bcd -match "(?im)^\s*testsigning\s+Yes\s*$") "required for test-signed PathOverlayFlt.sys"
}

function Install-PathOverlayDriver {
    Write-Step "Installing minifilter service"
    Remove-PathOverlayDriverService

    Invoke-Checked "sc.exe" @(
        "create", $driverName,
        "type=", "filesys",
        "binPath=", $driverPath,
        "start=", "demand",
        "error=", "normal",
        "depend=", "FltMgr",
        "DisplayName=", "PathOverlay minifilter driver"
    ) "Failed to create driver service." | Out-Null

    & reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\$driverName\Instances" /v DefaultInstance /t REG_SZ /d "PathOverlayFlt Instance" /f | Out-Host
    & reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\$driverName\Instances\PathOverlayFlt Instance" /v Altitude /t REG_SZ /d "370030" /f | Out-Host
    & reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\$driverName\Instances\PathOverlayFlt Instance" /v Flags /t REG_DWORD /d 0 /f | Out-Host

    Invoke-Checked "fltmc.exe" @("load", $driverName) "Failed to load PathOverlayFlt." | Out-Null
    $filters = & fltmc.exe filters | Out-String
    Write-Host $filters
    Write-Check "PathOverlayFlt loaded" ($filters -match $driverName)
}

function Test-DriverCommunicationPort {
    Write-Step "Testing driver communication port"
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class PathOverlayFltLib {
    [DllImport("fltlib.dll", CharSet = CharSet.Unicode)]
    public static extern int FilterConnectCommunicationPort(
        string lpPortName,
        uint dwOptions,
        IntPtr lpContext,
        ushort wSizeOfContext,
        IntPtr lpSecurityAttributes,
        out IntPtr hPort);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr hObject);
}
"@
    $handle = [IntPtr]::Zero
    $hr = [PathOverlayFltLib]::FilterConnectCommunicationPort("\PathOverlayPort", 0, [IntPtr]::Zero, 0, [IntPtr]::Zero, [ref]$handle)
    Write-Check "FilterConnectCommunicationPort" ($hr -eq 0) ("HRESULT=0x{0:X8}" -f ($hr -band 0xffffffff))
    if ($handle -ne [IntPtr]::Zero) {
        [PathOverlayFltLib]::CloseHandle($handle) | Out-Null
    }
}

function Install-And-TestService {
    Write-Step "Installing and starting user-mode service"
    Remove-PathOverlayService
    Reset-PathOverlayData

    Invoke-Checked $servicePath @("install") "Failed to install PathOverlaySvc." | Out-Null
    Invoke-Checked $servicePath @("start") "Failed to start PathOverlaySvc." | Out-Null
    Start-Sleep -Seconds 1

    try {
        $ruleShow = Invoke-Cli @("rule", "show") "CLI IPC test failed."
        Write-Check "fresh service install leaves no rules" ($ruleShow -match "OK no rules") $ruleShow
        $driverStatus = Invoke-Cli @("driver", "status") "Service did not connect to the driver communication port."
        Write-Check "service to driver port" ($driverStatus -match "OK driver connected") $driverStatus
    } catch {
        Write-ServiceDiagnostics
        throw
    }
}

function Restart-ServiceWithCleanData([string]$Reason) {
    Write-Step "Resetting service metadata for $Reason"
    Stop-PathOverlayService
    Reset-PathOverlayData
    Invoke-Checked $servicePath @("start") "Failed to restart PathOverlaySvc after metadata reset." | Out-Null
    Start-Sleep -Seconds 1

    $ruleShow = Invoke-Cli @("rule", "show") "CLI IPC test failed after metadata reset."
    Write-Check "metadata reset leaves no rules" ($ruleShow -match "OK no rules") $ruleShow
    $driverStatus = Invoke-Cli @("driver", "status") "Service did not reconnect to the driver communication port after metadata reset."
    Write-Check "service to driver port after metadata reset" ($driverStatus -match "OK driver connected") $driverStatus
}

function Test-NoRulePassthrough {
    Write-Step "Testing no-rule filesystem passthrough"
    $testRoot = Join-Path $env:TEMP ("PathOverlayNoRule-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    try {
        $file = Join-Path $testRoot "sample.txt"
        Set-Content -Path $file -Value "before" -Encoding ASCII
        Add-Content -Path $file -Value "after" -Encoding ASCII
        $content = Get-Content -Path $file -Raw
        Write-Check "source outside any rule is writable" ($content -match "before" -and $content -match "after") "path=$file content=$($content.Trim())"
        Remove-Item -LiteralPath $file -Force
        Write-Check "source outside any rule delete works" (-not (Test-Path $file)) "path=$file"
    } finally {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Test-MultiRuleDriverBehavior {
    Write-Step "Testing T022 multi-rule driver cache and matching"
    $testRoot = Join-Path $env:TEMP ("PathOverlayMultiRule-" + [guid]::NewGuid().ToString("N"))
    $source1 = Join-Path $testRoot "source-one"
    $source2 = Join-Path $testRoot "source-two"
    $outside = Join-Path $testRoot "outside"
    New-Item -ItemType Directory -Path $source1, $source2, $outside -Force | Out-Null

    try {
        $real1 = Join-Path $source1 "alpha.txt"
        $real2 = Join-Path $source2 "beta.txt"
        $outsideFile = Join-Path $outside "outside.txt"
        $disabledFile = Join-Path $source2 "disabled.txt"
        $serviceFile = Join-Path $source1 "service-process.txt"

        Set-Content -Path $real1 -Value "real-one-before" -Encoding ASCII
        Set-Content -Path $real2 -Value "real-two-before" -Encoding ASCII
        Set-Content -Path $outsideFile -Value "outside-before" -Encoding ASCII
        Set-Content -Path $serviceFile -Value "service-before" -Encoding ASCII

        Invoke-Cli @("rule", "add", $source1) "Failed to add first overlay rule." | Out-Null
        Invoke-Cli @("rule", "add", $source2) "Failed to add second overlay rule." | Out-Null
        $ruleShow = Invoke-Cli @("rule", "show") "Failed to show multi-rule state."
        $rule1 = Get-RuleBySource $ruleShow $source1
        $rule2 = Get-RuleBySource $ruleShow $source2
        Write-Check "two enabled rules are visible" ($rule1.Enabled -and $rule2.Enabled -and $rule1.Id -ne $rule2.Id) "rule1=$($rule1.Id) store1=$($rule1.Store); rule2=$($rule2.Id) store2=$($rule2.Store)"

        Set-Content -Path $real1 -Value "overlay-one-after" -Encoding ASCII
        Set-Content -Path $real2 -Value "overlay-two-after" -Encoding ASCII
        $shadow1 = ConvertTo-ShadowPath $rule1.Store $real1
        $shadow2 = ConvertTo-ShadowPath $rule2.Store $real2
        $shadow1Content = Get-Content -Path $shadow1 -Raw
        $shadow2Content = Get-Content -Path $shadow2 -Raw
        Write-Check "rule1 write redirected to rule1 store" ($shadow1Content -match "overlay-one-after") "shadow=$shadow1 content=$($shadow1Content.Trim())"
        Write-Check "rule2 write redirected to rule2 store" ($shadow2Content -match "overlay-two-after") "shadow=$shadow2 content=$($shadow2Content.Trim())"

        Invoke-Cli @("rule", "disable", "--rule", $rule1.Id) "Failed to disable first rule." | Out-Null
        $real1Content = Get-Content -Path $real1 -Raw
        Write-Check "disabled rule1 exposes unchanged real file" ($real1Content -match "real-one-before") "path=$real1 content=$($real1Content.Trim())"
        Invoke-Cli @("rule", "enable", "--rule", $rule1.Id) "Failed to re-enable first rule." | Out-Null

        Invoke-Cli @("rule", "disable", "--rule", $rule2.Id) "Failed to disable second rule." | Out-Null
        Set-Content -Path $disabledFile -Value "disabled-real-two" -Encoding ASCII
        $disabledContent = Get-Content -Path $disabledFile -Raw
        $disabledShadow = ConvertTo-ShadowPath $rule2.Store $disabledFile
        Write-Check "disabled rule2 writes real path" ($disabledContent -match "disabled-real-two" -and -not (Test-Path $disabledShadow)) "real=$disabledFile shadow=$disabledShadow"
        Invoke-Cli @("rule", "enable", "--rule", $rule2.Id) "Failed to re-enable second rule." | Out-Null

        Set-Content -Path $outsideFile -Value "outside-after" -Encoding ASCII
        $outsideContent = Get-Content -Path $outsideFile -Raw
        Write-Check "outside path remains unaffected" ($outsideContent -match "outside-after") "path=$outsideFile content=$($outsideContent.Trim())"

        $storeProbe = Join-Path (ConvertTo-ShadowPath $rule1.Store $source1) "store-probe.txt"
        New-Item -ItemType Directory -Path (Split-Path -Parent $storeProbe) -Force | Out-Null
        Set-Content -Path $storeProbe -Value "store-direct" -Encoding ASCII
        $storeProbeContent = Get-Content -Path $storeProbe -Raw
        Write-Check "store path is not recursively redirected" ($storeProbeContent -match "store-direct") "path=$storeProbe content=$($storeProbeContent.Trim())"

        Invoke-Cli @("debug", "service-write", $serviceFile, "service-after") "Service process debug write failed." | Out-Null
        Invoke-Cli @("rule", "disable", "--rule", $rule1.Id) "Failed to disable first rule for service exclusion verification." | Out-Null
        $serviceRealContent = Get-Content -Path $serviceFile -Raw
        Write-Check "service process writes real path" ($serviceRealContent -match "service-after") "path=$serviceFile content=$($serviceRealContent.Trim())"
        Invoke-Cli @("rule", "enable", "--rule", $rule1.Id) "Failed to re-enable first rule after service exclusion verification." | Out-Null

        Invoke-Cli @("rule", "disable", "--rule", $rule1.Id) "Failed to disable first rule before creating real nested overlap source." | Out-Null
        $nestedSource = Join-Path $source1 "nested-overlap"
        New-Item -ItemType Directory -Path $nestedSource -Force | Out-Null
        Invoke-Cli @("rule", "enable", "--rule", $rule1.Id) "Failed to re-enable first rule before overlap verification." | Out-Null
        $overlapFailure = Invoke-CliExpectFailure @("rule", "add", $nestedSource) "Overlapping rule should be rejected."
        Write-Check "overlapping rule add is rejected" ($overlapFailure -match "overlap|contain|contain each other") $overlapFailure

        Set-Content -Path $real2 -Value "overlay-two-after-overlap-reject" -Encoding ASCII
        $shadow2AfterReject = Get-Content -Path $shadow2 -Raw
        Write-Check "driver sync still works after rejected overlap" ($shadow2AfterReject -match "overlay-two-after-overlap-reject") "shadow=$shadow2 content=$($shadow2AfterReject.Trim())"

        $pendingDeleteFailure = Invoke-CliExpectFailure @("rule", "delete", "--rule", $rule1.Id) "Rule delete with pending changes should be rejected."
        Write-Check "rule delete rejects pending changes" ($pendingDeleteFailure -match "pending changes") $pendingDeleteFailure
        $missingDeleteFailure = Invoke-CliExpectFailure @("rule", "delete", "--rule", "missing-rule-id") "Deleting a missing rule should fail."
        Write-Check "rule delete rejects missing id" ($missingDeleteFailure -match "rule not found") $missingDeleteFailure

        $source3 = Join-Path $testRoot "source-three"
        $source4 = Join-Path $testRoot "source-four"
        New-Item -ItemType Directory -Path $source3, $source4 -Force | Out-Null
        Invoke-Cli @("rule", "add", $source3) "Failed to add third rule for delete verification." | Out-Null
        Invoke-Cli @("rule", "add", $source4) "Failed to add fourth rule for del alias verification." | Out-Null
        $deleteRuleShow = Invoke-Cli @("rule", "show") "Failed to show rules before delete verification."
        $rule3 = Get-RuleBySource $deleteRuleShow $source3
        $rule4 = Get-RuleBySource $deleteRuleShow $source4
        Invoke-Cli @("rule", "delete", "--rule", $rule3.Id) "Failed to delete third rule." | Out-Null
        Invoke-Cli @("rule", "del", "--rule", $rule4.Id) "Failed to delete fourth rule with del alias." | Out-Null
        $afterDeleteRuleShow = Invoke-Cli @("rule", "show") "Failed to show rules after delete verification."
        Write-Check "rule delete removes rule from show" ($afterDeleteRuleShow -notmatch [regex]::Escape($rule3.Id)) $afterDeleteRuleShow
        Write-Check "rule del alias removes rule from show" ($afterDeleteRuleShow -notmatch [regex]::Escape($rule4.Id)) $afterDeleteRuleShow

        Set-Content -Path $real2 -Value "overlay-two-after-delete" -Encoding ASCII
        $shadow2AfterDelete = Get-Content -Path $shadow2 -Raw
        Write-Check "driver sync still works after rule delete" ($shadow2AfterDelete -match "overlay-two-after-delete") "shadow=$shadow2 content=$($shadow2AfterDelete.Trim())"

        $serviceLog = Join-Path $env:ProgramData "PathOverlay\logs\PathOverlaySvc.log"
        if (Test-Path $serviceLog) {
            Write-Host ""
            Write-Host "PathOverlaySvc.log tail after multi-rule test:"
            Get-Content -LiteralPath $serviceLog -Tail 120 | Out-Host
        }
    } finally {
        try {
            $ruleShow = Invoke-Cli @("rule", "show") "Failed to show rules during cleanup."
            foreach ($rule in (Parse-Rules $ruleShow)) {
                if ($rule.Enabled) {
                    Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable rule during cleanup." | Out-Null
                }
            }
        } catch {
            Write-Host "Ignoring cleanup rule disable failure: $($_.Exception.Message)"
        }
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Test-FileRenameDriverBehavior {
    Write-Step "Testing T024 file rename overlay behavior"
    $testRoot = Join-Path $env:TEMP ("PathOverlayRename-" + [guid]::NewGuid().ToString("N"))
    $source = Join-Path $testRoot "source"
    New-Item -ItemType Directory -Path $source -Force | Out-Null

    try {
        $sourceFile = Join-Path $source "before.txt"
        $targetFile = Join-Path $source "after.txt"
        $existingTarget = Join-Path $source "exists.txt"
        Set-Content -Path $sourceFile -Value "rename-before" -Encoding ASCII
        Set-Content -Path $existingTarget -Value "target-exists" -Encoding ASCII

        Invoke-Cli @("rule", "add", $source) "Failed to add rename overlay rule." | Out-Null
        $ruleShow = Invoke-Cli @("rule", "show") "Failed to show rename rule state."
        $rule = Get-RuleBySource $ruleShow $source

        Rename-Item -LiteralPath $sourceFile -NewName "after.txt"
        Write-Check "renamed source path is hidden" (-not (Test-Path -LiteralPath $sourceFile)) "source=$sourceFile"
        Write-Check "renamed target path is visible" (Test-Path -LiteralPath $targetFile) "target=$targetFile"
        $targetContent = Get-Content -LiteralPath $targetFile -Raw
        Write-Check "renamed target content is readable" ($targetContent -match "rename-before") "content=$($targetContent.Trim())"

        $names = Get-ChildItem -LiteralPath $source | Select-Object -ExpandProperty Name
        Write-Check "directory enumeration hides rename source" ($names -notcontains "before.txt") ($names -join ",")
        Write-Check "directory enumeration shows rename target" ($names -contains "after.txt") ($names -join ",")

        $targetShadow = ConvertTo-ShadowPath $rule.Store $targetFile
        Write-Check "rename target is materialized in shadow" (Test-Path -LiteralPath $targetShadow) "shadow=$targetShadow"

        $conflictSource = Join-Path $source "conflict-source.txt"
        Set-Content -Path $conflictSource -Value "conflict-source" -Encoding ASCII
        $conflictFailed = $false
        try {
            Rename-Item -LiteralPath $conflictSource -NewName "exists.txt" -ErrorAction Stop
        } catch {
            $conflictFailed = $true
            Write-Host "Expected rename conflict failure: $($_.Exception.Message)"
        }
        Write-Check "rename to existing target is rejected" $conflictFailed "source=$conflictSource target=$existingTarget"

        Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable rename rule." | Out-Null
        Write-Check "rename did not modify real source before commit" (Test-Path -LiteralPath $sourceFile) "source=$sourceFile"
        Write-Check "rename did not create real target before commit" (-not (Test-Path -LiteralPath $targetFile)) "target=$targetFile"
    } finally {
        try {
            $ruleShow = Invoke-Cli @("rule", "show") "Failed to show rules during rename cleanup."
            foreach ($rule in (Parse-Rules $ruleShow)) {
                if ($rule.Enabled) {
                    Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable rule during rename cleanup." | Out-Null
                }
            }
        } catch {
            Write-Host "Ignoring rename cleanup rule disable failure: $($_.Exception.Message)"
        }
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Test-DirectoryRenameDriverBehavior {
    Write-Step "Testing T025 directory rename overlay behavior"
    $testRoot = Join-Path $env:TEMP ("PathOverlayDirRename-" + [guid]::NewGuid().ToString("N"))
    $source = Join-Path $testRoot "source"
    New-Item -ItemType Directory -Path $source -Force | Out-Null

    try {
        $sourceDir = Join-Path $source "before-dir"
        $targetDir = Join-Path $source "after-dir"
        $existingTarget = Join-Path $source "exists-dir"
        New-Item -ItemType Directory -Path $sourceDir, $existingTarget -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $sourceDir "nested") -Force | Out-Null
        Set-Content -Path (Join-Path $sourceDir "child.txt") -Value "directory-rename-before" -Encoding ASCII
        Set-Content -Path (Join-Path $sourceDir "nested\grandchild.txt") -Value "directory-rename-nested-before" -Encoding ASCII
        Set-Content -Path (Join-Path $existingTarget "existing.txt") -Value "target-exists" -Encoding ASCII

        Invoke-Cli @("rule", "add", $source) "Failed to add directory rename overlay rule." | Out-Null
        $ruleShow = Invoke-Cli @("rule", "show") "Failed to show directory rename rule state."
        $rule = Get-RuleBySource $ruleShow $source

        Rename-Item -LiteralPath $sourceDir -NewName "after-dir"
        Write-Check "renamed source directory is hidden" (-not (Test-Path -LiteralPath $sourceDir)) "source=$sourceDir"
        Write-Check "renamed target directory is visible" (Test-Path -LiteralPath $targetDir) "target=$targetDir"
        $targetShadow = ConvertTo-ShadowPath $rule.Store $targetDir
        $targetShadowChild = ConvertTo-ShadowPath $rule.Store (Join-Path $targetDir "child.txt")
        if (-not (Test-Path -LiteralPath $targetShadowChild)) {
            Write-Host "Directory rename diagnostics:"
            Invoke-Cli @("changes") "Failed to query changes for directory rename diagnostics." | Out-Host
            if (Test-Path -LiteralPath $targetShadow) {
                Get-ChildItem -LiteralPath $targetShadow -Recurse -Force | Select-Object FullName, Length, Mode | Format-Table -AutoSize | Out-Host
            } else {
                Write-Host "Target shadow directory was not found: $targetShadow"
            }
            $serviceLog = Join-Path $env:ProgramData "PathOverlay\logs\PathOverlaySvc.log"
            if (Test-Path -LiteralPath $serviceLog) {
                Get-Content -LiteralPath $serviceLog -Tail 120 | Out-Host
            }
        }
        Write-Check "rename target child is materialized in shadow before read" (Test-Path -LiteralPath $targetShadowChild) "shadow=$targetShadowChild"
        $targetChild = Join-Path $targetDir "child.txt"
        $targetChildLong = ConvertTo-CanonicalPath $targetChild
        $targetChildContent = ""
        $targetChildReadError = ""
        try {
            $targetChildContent = Get-Content -LiteralPath $targetChild -Raw
        } catch {
            $targetChildReadError = $_.Exception.Message
            Write-Host "Target child read via raw path failed: path=$targetChild error=$targetChildReadError"
        }
        $targetChildReadOk = $targetChildContent -match "directory-rename-before"
        if ($targetChildReadOk) {
            $targetChildRawDetail = "path=$targetChild content=$($targetChildContent.Trim())"
        } else {
            $targetChildRawDetail = "path=$targetChild error=$targetChildReadError"
        }
        Write-Host "Raw target child read result: ok=$targetChildReadOk $targetChildRawDetail"

        $targetChildLongReadOk = $false
        $targetChildLongDetail = "canonical long path matched raw path or was unavailable: path=$targetChildLong"
        if ($targetChildLong -and ($targetChildLong -ine $targetChild)) {
            $targetChildLongContent = ""
            $targetChildLongReadError = ""
            try {
                $targetChildLongContent = Get-Content -LiteralPath $targetChildLong -Raw
            } catch {
                $targetChildLongReadError = $_.Exception.Message
                Write-Host "Target child read via canonical long path failed: path=$targetChildLong error=$targetChildLongReadError"
            }
            $targetChildLongReadOk = $targetChildLongContent -match "directory-rename-before"
            if ($targetChildLongReadOk) {
                $targetChildLongDetail = "path=$targetChildLong content=$($targetChildLongContent.Trim())"
            } else {
                $targetChildLongDetail = "path=$targetChildLong error=$targetChildLongReadError"
            }
            Write-Host "Canonical long target child read result: ok=$targetChildLongReadOk $targetChildLongDetail"
        }

        $targetChildShortReadOk = $false
        $targetChildShortDetail = "short path matched raw path or was unavailable"
        $targetChildShort = ConvertTo-ShortPath $targetChild
        if ($targetChildShort -and ($targetChildShort -ine $targetChild)) {
            $targetChildShortContent = ""
            $targetChildShortReadError = ""
            try {
                $targetChildShortContent = Get-Content -LiteralPath $targetChildShort -Raw
            } catch {
                $targetChildShortReadError = $_.Exception.Message
                Write-Host "Target child read via short path failed: path=$targetChildShort error=$targetChildShortReadError"
            }
            $targetChildShortReadOk = $targetChildShortContent -match "directory-rename-before"
            if ($targetChildShortReadOk) {
                $targetChildShortDetail = "path=$targetChildShort content=$($targetChildShortContent.Trim())"
            } else {
                $targetChildShortDetail = "path=$targetChildShort error=$targetChildShortReadError"
            }
            Write-Host "Short target child read result: ok=$targetChildShortReadOk $targetChildShortDetail"
        }
        Write-Check "renamed target child content is readable via raw path" $targetChildReadOk $targetChildRawDetail
        if ($targetChildLong -and ($targetChildLong -ine $targetChild)) {
            Write-Check "renamed target child content is readable via canonical long path" $targetChildLongReadOk $targetChildLongDetail
        }
        if ($targetChildShort -and ($targetChildShort -ine $targetChild)) {
            Write-Check "renamed target child content is readable via short path" $targetChildShortReadOk $targetChildShortDetail
        }
        $targetNestedChild = Join-Path $targetDir "nested\grandchild.txt"
        $targetNestedChildContent = Get-Content -LiteralPath $targetNestedChild -Raw
        Write-Check "renamed target nested child content is readable" ($targetNestedChildContent -match "directory-rename-nested-before") "content=$($targetNestedChildContent.Trim())"

        $names = Get-ChildItem -LiteralPath $source | Select-Object -ExpandProperty Name
        Write-Check "directory enumeration hides renamed source directory" ($names -notcontains "before-dir") ($names -join ",")
        Write-Check "directory enumeration shows renamed target directory" ($names -contains "after-dir") ($names -join ",")

        Write-Check "directory rename target is materialized in shadow" (Test-Path -LiteralPath $targetShadow) "shadow=$targetShadow"

        $conflictSource = Join-Path $source "conflict-dir"
        New-Item -ItemType Directory -Path $conflictSource -Force | Out-Null
        $conflictFailed = $false
        try {
            Rename-Item -LiteralPath $conflictSource -NewName "exists-dir" -ErrorAction Stop
        } catch {
            $conflictFailed = $true
            Write-Host "Expected directory rename conflict failure: $($_.Exception.Message)"
        }
        Write-Check "directory rename to existing target is rejected" $conflictFailed "source=$conflictSource target=$existingTarget"

        Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable directory rename rule." | Out-Null
        Write-Check "directory rename did not modify real source before commit" (Test-Path -LiteralPath $sourceDir) "source=$sourceDir"
        Write-Check "directory rename did not create real target before commit" (-not (Test-Path -LiteralPath $targetDir)) "target=$targetDir"
    } finally {
        try {
            $ruleShow = Invoke-Cli @("rule", "show") "Failed to show rules during directory rename cleanup."
            foreach ($rule in (Parse-Rules $ruleShow)) {
                if ($rule.Enabled) {
                    Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable rule during directory rename cleanup." | Out-Null
                }
            }
        } catch {
            Write-Host "Ignoring directory rename cleanup rule disable failure: $($_.Exception.Message)"
        }
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Test-DirectoryDeleteAndEnumerationDriverBehavior {
    Write-Step "Testing T023/T028 directory tombstone and enumeration behavior"
    $testRoot = Join-Path $env:TEMP ("PathOverlayDirDelete-" + [guid]::NewGuid().ToString("N"))
    $source = Join-Path $testRoot "source"
    New-Item -ItemType Directory -Path $source -Force | Out-Null

    try {
        $deleteDir = Join-Path $source "delete-dir"
        $keepDir = Join-Path $source "keep-dir"
        New-Item -ItemType Directory -Path $deleteDir, $keepDir -Force | Out-Null
        Set-Content -Path (Join-Path $deleteDir "child.txt") -Value "delete-dir-child" -Encoding ASCII
        Set-Content -Path (Join-Path $keepDir "keep.txt") -Value "keep-dir-child" -Encoding ASCII

        Invoke-Cli @("rule", "add", $source) "Failed to add directory tombstone rule." | Out-Null
        $ruleShow = Invoke-Cli @("rule", "show") "Failed to show directory tombstone rule state."
        $rule = Get-RuleBySource $ruleShow $source

        Remove-Item -LiteralPath $deleteDir -Recurse -Force
        Write-Check "tombstoned directory is hidden" (-not (Test-Path -LiteralPath $deleteDir)) "rule=$($rule.Id) source=$source store=$($rule.Store) path=$deleteDir"
        Write-Check "tombstoned directory child is hidden" (-not (Test-Path -LiteralPath (Join-Path $deleteDir "child.txt"))) "rule=$($rule.Id) path=$(Join-Path $deleteDir "child.txt")"
        $names = Get-ChildItem -LiteralPath $source | Select-Object -ExpandProperty Name
        Write-Check "directory enumeration hides tombstoned directory" ($names -notcontains "delete-dir") "rule=$($rule.Id) names=$($names -join ',')"
        Write-Check "directory enumeration keeps unaffected directory" ($names -contains "keep-dir") "rule=$($rule.Id) names=$($names -join ',')"

        $changes = Invoke-Cli @("changes") "Failed to query directory tombstone changes."
        Write-Check "changes include directory tombstone rule context" (Test-ChangesContainsRule $changes $rule.Id $source $rule.Store "tombstone" $deleteDir) $changes

        Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable directory tombstone rule." | Out-Null
        Write-Check "directory tombstone did not delete real directory before commit" (Test-Path -LiteralPath $deleteDir) "path=$deleteDir"
    } finally {
        try {
            $ruleShow = Invoke-Cli @("rule", "show") "Failed to show rules during directory tombstone cleanup."
            foreach ($rule in (Parse-Rules $ruleShow)) {
                if ($rule.Enabled) {
                    Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable rule during directory tombstone cleanup." | Out-Null
                }
            }
        } catch {
            Write-Host "Ignoring directory tombstone cleanup rule disable failure: $($_.Exception.Message)"
        }
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Test-CommitDiscardByRuleDriverBehavior {
    Write-Step "Testing T026/T028 commit and discard by rule id"
    $testRoot = Join-Path $env:TEMP ("PathOverlayRuleOps-" + [guid]::NewGuid().ToString("N"))
    $source1 = Join-Path $testRoot "source-commit"
    $source2 = Join-Path $testRoot "source-discard"
    New-Item -ItemType Directory -Path $source1, $source2 -Force | Out-Null

    try {
        $file1 = Join-Path $source1 "commit.txt"
        $file2 = Join-Path $source2 "discard.txt"
        Set-Content -Path $file1 -Value "commit-base" -Encoding ASCII
        Set-Content -Path $file2 -Value "discard-base" -Encoding ASCII

        Invoke-Cli @("rule", "add", $source1) "Failed to add commit rule." | Out-Null
        Invoke-Cli @("rule", "add", $source2) "Failed to add discard rule." | Out-Null
        $ruleShow = Invoke-Cli @("rule", "show") "Failed to show rule operation state."
        $commitRule = Get-RuleBySource $ruleShow $source1
        $discardRule = Get-RuleBySource $ruleShow $source2

        Set-Content -Path $file1 -Value "commit-overlay" -Encoding ASCII
        Set-Content -Path $file2 -Value "discard-overlay" -Encoding ASCII
        $changesBefore = Invoke-Cli @("changes") "Failed to query changes before rule operations."
        Write-Check "changes include commit rule before commit" (Test-ChangesContainsRule $changesBefore $commitRule.Id $source1 $commitRule.Store "modified" $file1) $changesBefore
        Write-Check "changes include discard rule before discard" (Test-ChangesContainsRule $changesBefore $discardRule.Id $source2 $discardRule.Store "modified" $file2) $changesBefore
        $commitRuleChanges = Invoke-Cli @("changes", "--rule", $commitRule.Id) "Failed to query changes for commit rule."
        Write-Check "changes --rule includes selected rule only" ((Test-ChangesContainsRule $commitRuleChanges $commitRule.Id $source1 $commitRule.Store "modified" $file1) -and ($commitRuleChanges -notmatch [regex]::Escape($discardRule.Id))) $commitRuleChanges
        $commitDryRun = Invoke-Cli @("commit", "--dry-run", "--rule", $commitRule.Id) "Commit dry-run failed."
        $canonicalFile1 = ConvertTo-CanonicalPath $file1
        Write-Check "commit dry-run reports write and backup paths" ($commitDryRun -match "commit dry-run" -and $commitDryRun -match "WRITE real=$([regex]::Escape($canonicalFile1))" -and $commitDryRun -match "BACKUP real=$([regex]::Escape($canonicalFile1))" -and $commitDryRun -match "backup=") $commitDryRun
        $changesAfterCommitDryRun = Invoke-Cli @("changes") "Failed to query changes after commit dry-run."
        Write-Check "commit dry-run preserves metadata" ((Test-ChangesContainsRule $changesAfterCommitDryRun $commitRule.Id $source1 $commitRule.Store "modified" $file1) -and (Test-ChangesContainsRule $changesAfterCommitDryRun $discardRule.Id $source2 $discardRule.Store "modified" $file2)) $changesAfterCommitDryRun
        Invoke-Cli @("rule", "disable", "--rule", $commitRule.Id) "Failed to disable commit rule after dry-run." | Out-Null
        Write-Check "commit dry-run does not write real file" ((Get-Content -LiteralPath $file1 -Raw) -match "commit-base") "rule=$($commitRule.Id) path=$file1"
        Invoke-Cli @("rule", "enable", "--rule", $commitRule.Id) "Failed to re-enable commit rule after dry-run." | Out-Null
        Write-Check "commit dry-run keeps shadow file" ((Get-Content -LiteralPath (ConvertTo-ShadowPath $commitRule.Store $file1) -Raw) -match "commit-overlay") "rule=$($commitRule.Id)"

        $discardDryRun = Invoke-Cli @("discard", "--dry-run", "--rule", $discardRule.Id) "Discard dry-run failed."
        Write-Check "discard dry-run reports cleanup scope" ($discardDryRun -match "discard dry-run" -and $discardDryRun -match "clear_changes=1" -and $discardDryRun -match "cleanup_shadow_root=" -and $discardDryRun -match "metadata_scope=rule:$([regex]::Escape($discardRule.Id))") $discardDryRun
        $changesAfterDiscardDryRun = Invoke-Cli @("changes", "--rule", $discardRule.Id) "Failed to query changes after discard dry-run."
        Write-Check "discard dry-run preserves metadata" (Test-ChangesContainsRule $changesAfterDiscardDryRun $discardRule.Id $source2 $discardRule.Store "modified" $file2) $changesAfterDiscardDryRun
        Write-Check "discard dry-run keeps shadow file" ((Get-Content -LiteralPath (ConvertTo-ShadowPath $discardRule.Store $file2) -Raw) -match "discard-overlay") "rule=$($discardRule.Id)"

        $commitOutput = Invoke-Cli @("commit", "--rule", $commitRule.Id) "Commit by rule id failed."
        Write-Check "commit output includes rule context" ($commitOutput -match "rule=$([regex]::Escape($commitRule.Id))" -and $commitOutput -match "source=" -and $commitOutput -match "store=") $commitOutput
        Write-Check "selected rule commit writes real file" ((Get-Content -LiteralPath $file1 -Raw) -match "commit-overlay") "rule=$($commitRule.Id) path=$file1"
        Invoke-Cli @("rule", "disable", "--rule", $discardRule.Id) "Failed to disable discard rule for real-file verification." | Out-Null
        Write-Check "commit does not modify other rule real file" ((Get-Content -LiteralPath $file2 -Raw) -match "discard-base") "rule=$($discardRule.Id) path=$file2"
        Invoke-Cli @("rule", "enable", "--rule", $discardRule.Id) "Failed to re-enable discard rule." | Out-Null

        $changesAfterCommit = Invoke-Cli @("changes") "Failed to query changes after commit."
        Write-Check "commit clears selected rule changes only" (-not (Test-ChangesContainsRule $changesAfterCommit $commitRule.Id $source1 $commitRule.Store "modified" $file1)) $changesAfterCommit
        Write-Check "commit preserves other rule changes" (Test-ChangesContainsRule $changesAfterCommit $discardRule.Id $source2 $discardRule.Store "modified" $file2) $changesAfterCommit

        $discardOutput = Invoke-Cli @("discard", "--rule", $discardRule.Id) "Discard by rule id failed."
        Write-Check "discard output includes rule context" ($discardOutput -match "rule=$([regex]::Escape($discardRule.Id))" -and $discardOutput -match "source=" -and $discardOutput -match "store=") $discardOutput
        Write-Check "selected rule discard keeps real file" ((Get-Content -LiteralPath $file2 -Raw) -match "discard-base") "rule=$($discardRule.Id) path=$file2"
        $changesAfterDiscard = Invoke-Cli @("changes") "Failed to query changes after discard."
        Write-Check "discard clears selected rule changes" (-not (Test-ChangesContainsRule $changesAfterDiscard $discardRule.Id $source2 $discardRule.Store "modified" $file2)) $changesAfterDiscard
    } finally {
        try {
            $ruleShow = Invoke-Cli @("rule", "show") "Failed to show rules during rule operation cleanup."
            foreach ($rule in (Parse-Rules $ruleShow)) {
                if ($rule.Enabled) {
                    Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable rule during rule operation cleanup." | Out-Null
                }
            }
        } catch {
            Write-Host "Ignoring rule operation cleanup rule disable failure: $($_.Exception.Message)"
        }
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Test-PathAndAttributeCompatibilityDriverBehavior {
    Write-Step "Testing T043 path and attribute compatibility"
    $testRoot = Join-Path $env:TEMP ("PathOverlayCompat-" + [guid]::NewGuid().ToString("N"))
    $source = Join-Path $testRoot "source"
    New-Item -ItemType Directory -Path $source -Force | Out-Null

    try {
        $unicodeSegment = "unicode-$([char]0x8DEF)$([char]0x5F84)"
        $unicodeFileName = "$([char]0x6570)$([char]0x636E)-file.txt"
        $unicodeDir = Join-Path $source $unicodeSegment
        $longDir = Join-Path $unicodeDir "deep-segment-001\deep-segment-002\deep-segment-003\deep-segment-004\deep-segment-005"
        New-Item -ItemType Directory -Path $longDir -Force | Out-Null
        $longUnicodeFile = Join-Path $longDir $unicodeFileName
        Set-Content -LiteralPath $longUnicodeFile -Value "long-unicode-base" -Encoding ASCII

        $caseFile = Join-Path $source "CaseName.txt"
        Set-Content -LiteralPath $caseFile -Value "case-base" -Encoding ASCII
        $shortProbe = Join-Path $source "ShortNameProbe.txt"
        Set-Content -LiteralPath $shortProbe -Value "short-base" -Encoding ASCII

        $readonlyFile = Join-Path $source "readonly-time.txt"
        Set-Content -LiteralPath $readonlyFile -Value "readonly-base" -Encoding ASCII
        $expectedTime = [datetime]::SpecifyKind([datetime]"2024-01-02T03:04:05", [System.DateTimeKind]::Utc)
        (Get-Item -LiteralPath $readonlyFile).LastWriteTimeUtc = $expectedTime
        Set-ItemProperty -LiteralPath $readonlyFile -Name IsReadOnly -Value $true

        Invoke-Cli @("rule", "add", $source) "Failed to add compatibility rule." | Out-Null
        $ruleShow = Invoke-Cli @("rule", "show") "Failed to show compatibility rule."
        $rule = Get-RuleBySource $ruleShow $source

        Set-Content -LiteralPath $longUnicodeFile -Value "long-unicode-overlay" -Encoding ASCII
        $longUnicodeShadow = ConvertTo-ShadowPath $rule.Store $longUnicodeFile
        Write-Check "long unicode path writes to shadow" ((Get-Content -LiteralPath $longUnicodeShadow -Raw) -match "long-unicode-overlay") "shadow=$longUnicodeShadow"

        $caseVariant = Join-Path $source "casename.txt"
        Set-Content -LiteralPath $caseVariant -Value "case-overlay" -Encoding ASCII
        $caseShadow = ConvertTo-ShadowPath $rule.Store $caseFile
        Write-Check "case-variant path writes to canonical shadow" ((Get-Content -LiteralPath $caseShadow -Raw) -match "case-overlay") "shadow=$caseShadow"

        $shortPath = ConvertTo-ShortPath $shortProbe
        if ($shortPath -and ($shortPath -ine $shortProbe)) {
            Set-Content -LiteralPath $shortPath -Value "short-overlay" -Encoding ASCII
            $shortShadow = ConvertTo-ShadowPath $rule.Store $shortProbe
            Write-Check "8.3 short path writes to shadow" ((Get-Content -LiteralPath $shortShadow -Raw) -match "short-overlay") "short=$shortPath shadow=$shortShadow"
        } else {
            Write-Check "8.3 short path unavailable" $true "volume did not return a distinct short path"
        }

        $prepareOutput = Invoke-Cli @("debug", "prepare-cow", "--rule", $rule.Id, $readonlyFile) "Failed to prepare readonly compatibility shadow."
        Write-Check "readonly prepare-cow returns shadow path" ($prepareOutput -match "^OK shadow=") $prepareOutput
        $readonlyShadow = ($prepareOutput -replace "^OK shadow=", "").Trim()
        $readonlyShadowItem = Get-Item -LiteralPath $readonlyShadow
        $shadowTimeDelta = [math]::Abs(($readonlyShadowItem.LastWriteTimeUtc - $expectedTime).TotalSeconds)
        Write-Check "readonly attribute is preserved in shadow" $readonlyShadowItem.IsReadOnly "shadow=$readonlyShadow"
        Write-Check "last-write timestamp is preserved in shadow" ($shadowTimeDelta -le 2) "shadow=$readonlyShadow expected=$expectedTime actual=$($readonlyShadowItem.LastWriteTimeUtc)"
        Set-ItemProperty -LiteralPath $readonlyShadow -Name IsReadOnly -Value $false
        Set-ItemProperty -LiteralPath $readonlyFile -Name IsReadOnly -Value $false

        $emptyNested = Join-Path $source "empty-root\nested\leaf"
        New-Item -ItemType Directory -Path $emptyNested -Force | Out-Null
        $emptyShadow = ConvertTo-ShadowPath $rule.Store (Join-Path $source "empty-root")
        Write-Check "empty nested directory is visible through overlay" (Test-Path -LiteralPath $emptyNested) "path=$emptyNested"
        Write-Check "empty nested directory is created in shadow" (Test-Path -LiteralPath (Join-Path $emptyShadow "nested\leaf")) "shadow=$emptyShadow"
        Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable compatibility rule for real path verification." | Out-Null
        Write-Check "empty nested directory did not pre-create real source" (-not (Test-Path -LiteralPath (Join-Path $source "empty-root"))) "source=$source"
        Invoke-Cli @("rule", "enable", "--rule", $rule.Id) "Failed to re-enable compatibility rule." | Out-Null

        $changes = Invoke-Cli @("changes", "--rule", $rule.Id) "Failed to query compatibility changes."
        Write-Check "compatibility changes include long unicode path" (Test-ChangesContainsRule $changes $rule.Id $source $rule.Store "modified" $longUnicodeFile) $changes
    } finally {
        try {
            $ruleShow = Invoke-Cli @("rule", "show") "Failed to show rules during compatibility cleanup."
            foreach ($rule in (Parse-Rules $ruleShow)) {
                if ($rule.Enabled) {
                    Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable rule during compatibility cleanup." | Out-Null
                }
            }
        } catch {
            Write-Host "Ignoring compatibility cleanup rule disable failure: $($_.Exception.Message)"
        }
        Get-ChildItem -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
            if (-not $_.PSIsContainer) {
                try { Set-ItemProperty -LiteralPath $_.FullName -Name IsReadOnly -Value $false -ErrorAction SilentlyContinue } catch {}
            }
        }
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Test-OccupiedCommitDriverBehavior {
    Write-Step "Testing T027/T028 occupied commit detection"
    $testRoot = Join-Path $env:TEMP ("PathOverlayOccupied-" + [guid]::NewGuid().ToString("N"))
    $source = Join-Path $testRoot "source"
    New-Item -ItemType Directory -Path $source -Force | Out-Null
    $handle = $null

    try {
        $file = Join-Path $source "occupied.txt"
        Set-Content -Path $file -Value "occupied-base" -Encoding ASCII
        Invoke-Cli @("rule", "add", $source) "Failed to add occupied commit rule." | Out-Null
        $ruleShow = Invoke-Cli @("rule", "show") "Failed to show occupied rule state."
        $rule = Get-RuleBySource $ruleShow $source

        Set-Content -Path $file -Value "occupied-overlay" -Encoding ASCII
        Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable occupied rule before opening real file." | Out-Null
        $handle = [System.IO.File]::Open($file, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::None)
        Invoke-Cli @("rule", "enable", "--rule", $rule.Id) "Failed to re-enable occupied rule before commit." | Out-Null

        $commitFailure = Invoke-CliExpectFailure @("commit", "--rule", $rule.Id) "Occupied commit should fail without --confirm-close."
        Write-Check "occupied commit failure includes rule context" ($commitFailure -match "occupied files detected" -and $commitFailure -match "rule=$([regex]::Escape($rule.Id))" -and $commitFailure -match "pid=") $commitFailure
        $changes = Invoke-Cli @("changes") "Failed to query changes after occupied commit failure."
        Write-Check "occupied commit failure preserves metadata" (Test-ChangesContainsRule $changes $rule.Id $source $rule.Store "modified" $file) $changes
    } finally {
        if ($null -ne $handle) {
            $handle.Dispose()
        }
        try {
            $ruleShow = Invoke-Cli @("rule", "show") "Failed to show rules during occupied cleanup."
            foreach ($rule in (Parse-Rules $ruleShow)) {
                if ($rule.Enabled) {
                    Invoke-Cli @("rule", "disable", "--rule", $rule.Id) "Failed to disable rule during occupied cleanup." | Out-Null
                }
            }
        } catch {
            Write-Host "Ignoring occupied cleanup rule disable failure: $($_.Exception.Message)"
        }
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Cleanup {
    if ($SkipCleanup) {
        Write-Host "Skipping cleanup by request."
        return
    }

    Write-Step "Cleaning up"
    Remove-PathOverlayService
    Remove-PathOverlayDriverService
}

Assert-Admin
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$transcript = Join-Path $logDir ("PathOverlay-Test-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
Start-Transcript -Path $transcript | Out-Null

try {
    Write-Step "Checking package files"
    foreach ($path in @($driverPath, $servicePath, $cliPath)) {
        Write-Check "required file exists" (Test-Path $path) $path
    }

    Assert-TestSigning
    Install-TestCertificate
    Install-PathOverlayDriver
    Test-DriverCommunicationPort
    Install-And-TestService
    Test-NoRulePassthrough
    Test-MultiRuleDriverBehavior
    Restart-ServiceWithCleanData "T023 directory tombstone test"
    Test-DirectoryDeleteAndEnumerationDriverBehavior
    Restart-ServiceWithCleanData "T024 file rename test"
    Test-FileRenameDriverBehavior
    Restart-ServiceWithCleanData "T025 directory rename test"
    Test-DirectoryRenameDriverBehavior
    Restart-ServiceWithCleanData "T026 commit/discard by rule test"
    Test-CommitDiscardByRuleDriverBehavior
    Restart-ServiceWithCleanData "T043 path and attribute compatibility test"
    Test-PathAndAttributeCompatibilityDriverBehavior
    Restart-ServiceWithCleanData "T027 occupied commit test"
    Test-OccupiedCommitDriverBehavior

    Write-Step "Result"
    Write-Host "PathOverlay test package passed all automated checks." -ForegroundColor Green
} catch {
    Write-Step "Result"
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-ServiceDiagnostics
    Invoke-DiagnosticsCollect
    throw
} finally {
    Cleanup
    Stop-Transcript | Out-Null
    Write-Host ""
    Write-Host "Log: $transcript"
}
