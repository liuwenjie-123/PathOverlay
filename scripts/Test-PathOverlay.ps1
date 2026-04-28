param(
    [switch]$SkipCleanup
)

$ErrorActionPreference = "Stop"
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
    $output = & $cliPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
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
        Write-Check "CLI to service IPC" ($ruleShow -match "^OK") $ruleShow
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
    Restart-ServiceWithCleanData "T024 file rename test"
    Test-FileRenameDriverBehavior
    Restart-ServiceWithCleanData "T025 directory rename test"
    Test-DirectoryRenameDriverBehavior

    Write-Step "Result"
    Write-Host "PathOverlay test package passed all automated checks." -ForegroundColor Green
} catch {
    Write-Step "Result"
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-ServiceDiagnostics
    throw
} finally {
    Cleanup
    Stop-Transcript | Out-Null
    Write-Host ""
    Write-Host "Log: $transcript"
}
