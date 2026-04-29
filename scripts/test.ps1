param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$cleanupScript = Join-Path $PSScriptRoot "cleanup-test-data.ps1"

if (Test-Path $cleanupScript) {
    & $cleanupScript
}

Get-Content -Raw (Join-Path $repoRoot "task.json") | ConvertFrom-Json | Out-Null

$requiredPaths = @(
    "PathOverlay.sln",
    "driver\PathOverlayFlt",
    "service\PathOverlaySvc",
    "cli\pathoverlay",
    "common",
    "tests",
    "scripts\build.ps1",
    "scripts\test.ps1"
)

foreach ($relativePath in $requiredPaths) {
    $fullPath = Join-Path $repoRoot $relativePath
    if (-not (Test-Path $fullPath)) {
        throw "Required path is missing: $relativePath"
    }
}

function Find-SqliteRuntime {
    $candidates = @(
        (Join-Path $repoRoot "x64\Debug\sqlite3.dll"),
        (Join-Path $repoRoot "tests\x64\Debug\sqlite3.dll"),
        (Join-Path $repoRoot "service\PathOverlaySvc\x64\Debug\sqlite3.dll")
    )

    return $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

$candidateTestExes = @(
    (Join-Path $repoRoot "tests\x64\$Configuration\PathOverlayTests.exe"),
    (Join-Path $repoRoot "x64\$Configuration\PathOverlayTests.exe")
)

$candidateCliExes = @(
    (Join-Path $repoRoot "x64\$Configuration\pathoverlay.exe"),
    (Join-Path $repoRoot "cli\pathoverlay\x64\$Configuration\pathoverlay.exe")
)

$testExe = $candidateTestExes | Where-Object { Test-Path $_ } | Select-Object -First 1
$cliExe = $candidateCliExes | Where-Object { Test-Path $_ } | Select-Object -First 1
$diagnosticsTestDir = Join-Path $env:TEMP ("PathOverlay-Diagnostics-Test-" + [guid]::NewGuid().ToString("N"))
try {
    if ($testExe) {
        $testExeDirectory = Split-Path -Parent $testExe
        $sqliteTarget = Join-Path $testExeDirectory "sqlite3.dll"
        if (-not (Test-Path $sqliteTarget)) {
            $sqliteSource = Find-SqliteRuntime
            if ($sqliteSource) {
                Copy-Item -LiteralPath $sqliteSource -Destination $sqliteTarget -Force
            }
        }

        & $testExe
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    } else {
        Write-Host "PathOverlayTests.exe not found; layout and task metadata checks passed."
    }

    if ($cliExe) {
        & $cliExe diagnostics collect --output $diagnosticsTestDir
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }

        foreach ($requiredFile in @("rule-show.txt", "changes.txt", "status.txt", "doctor.txt", "driver-status.txt", "scm-service.txt", "scm-driver.txt", "manifest.txt")) {
            $fullPath = Join-Path $diagnosticsTestDir $requiredFile
            if (-not (Test-Path -LiteralPath $fullPath)) {
                throw "Diagnostics collect did not create required file: $requiredFile"
            }
        }
    } else {
        Write-Host "pathoverlay.exe not found; diagnostics collect smoke test skipped."
    }
} finally {
    if (Test-Path -LiteralPath $diagnosticsTestDir) {
        Remove-Item -LiteralPath $diagnosticsTestDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $cleanupScript) {
        & $cleanupScript
    }
}

Write-Host "User-mode test script completed without requiring driver installation."
