param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64")]
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $repoRoot "PathOverlay.sln"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$windowsKits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"

if (-not (Test-Path $solution)) {
    throw "PathOverlay.sln was not found at $solution."
}

if (-not (Test-Path $vswhere)) {
    throw "Visual Studio Installer vswhere.exe was not found. Install Visual Studio 2022 with C++ workload."
}

$msbuild = & $vswhere -latest -version "[17.0,18.0)" -requires Microsoft.Component.MSBuild -find "MSBuild\Current\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) {
    throw "MSBuild for Visual Studio 2022 was not found. Install Visual Studio 2022 with the Desktop development with C++ workload."
}

if (-not (Test-Path $windowsKits)) {
    throw "Windows Kits 10 directory was not found. Install the Windows SDK and WDK before building the driver."
}

$wdkTargets = Get-ChildItem -Path $windowsKits -Recurse -Filter "WindowsDriver*.targets" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $wdkTargets) {
    throw "WDK MSBuild targets were not found under $windowsKits. Install the Windows Driver Kit for Visual Studio 2022."
}

& $msbuild $solution /m /restore /p:Configuration=$Configuration /p:Platform=$Platform
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$outputRoot = Join-Path $repoRoot "$Platform\$Configuration"
$installScripts = @(
    "install-start.ps1",
    "Install-Start-PathOverlay.cmd",
    "uninstall.ps1",
    "Uninstall-PathOverlay.cmd"
)

foreach ($scriptName in $installScripts) {
    $source = Join-Path $PSScriptRoot $scriptName
    if (Test-Path $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $outputRoot $scriptName) -Force
    }
}
