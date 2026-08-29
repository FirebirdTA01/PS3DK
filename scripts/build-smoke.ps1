param(
    [Parameter(Mandatory = $true)]
    [string]$SdkRoot,
    [string]$RepoRoot = "",
    [string]$Manifest = "",
    [string]$BuildRoot = "",
    [string]$Generator = "Ninja"
)

$ErrorActionPreference = "Stop"

function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Find-Ninja {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
        "C:\Program Files\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    )
    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path) {
            return $path
        }
    }

    return $null
}

$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
if (-not $Manifest) {
    $Manifest = Join-Path $RepoRoot "tests\smoke\manifest.txt"
}
if (-not $BuildRoot) {
    $BuildRoot = Join-Path $env:TEMP ("ps3-smoke-build-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}

$toolchain = Join-Path $SdkRoot "cmake\ps3-ppu-toolchain.cmake"
if (-not (Test-Path -LiteralPath $toolchain)) {
    throw "missing package toolchain file: $toolchain"
}

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

$env:PS3DK = $SdkRoot
$env:PS3DEV = $SdkRoot
$env:PSL1GHT = $SdkRoot
$env:PATH = (Join-Path $SdkRoot "bin") + [System.IO.Path]::PathSeparator + $env:PATH

$cmakeMakeProgram = ""
if ($Generator -eq "Ninja") {
    $ninja = Find-Ninja
    if (-not $ninja) {
        throw "Ninja generator requested but ninja.exe is not on PATH and was not found in known Visual Studio locations. Install Ninja or pass -Generator for another CMake backend."
    }
    $cmakeMakeProgram = $ninja
}

$smokeRoot = Join-Path $RepoRoot "tests\smoke"
$rows = Import-Csv -LiteralPath $Manifest
$manifestByName = @{}
foreach ($row in $rows) {
    $manifestByName[$row.name] = $row.relative_self
}

$probeDirs = Get-ChildItem -LiteralPath $smokeRoot -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "CMakeLists.txt") } |
    Sort-Object Name

if ($probeDirs.Count -eq 0) {
    throw "no smoke probes found under $smokeRoot"
}

$results = @()
foreach ($probe in $probeDirs) {
    $buildDir = Join-Path $BuildRoot $probe.Name
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    $configure = "PASS"
    $build = "PASS"
    try {
        $configureArgs = @(
            "-S", $probe.FullName,
            "-B", $buildDir,
            "-G", $Generator,
            "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
        )
        if ($cmakeMakeProgram) {
            $configureArgs += "-DCMAKE_MAKE_PROGRAM=$cmakeMakeProgram"
        }
        Invoke-Checked -FilePath "cmake" -Arguments $configureArgs
        Invoke-Checked -FilePath "cmake" -Arguments @("--build", $buildDir)
    } catch {
        if ($configure -eq "PASS" -and -not (Test-Path -LiteralPath (Join-Path $buildDir "CMakeCache.txt"))) {
            $configure = "FAIL"
        } else {
            $build = "FAIL"
        }
        $results += [pscustomobject]@{
            name = $probe.Name
            configure = $configure
            build = $build
            self = $manifestByName[$probe.Name]
            message = $_.Exception.Message
        }
        continue
    }

    $results += [pscustomobject]@{
        name = $probe.Name
        configure = $configure
        build = $build
        self = $manifestByName[$probe.Name]
        message = ""
    }
}

foreach ($row in $rows) {
    $selfPath = Join-Path $RepoRoot $row.relative_self
    if (-not (Test-Path -LiteralPath $selfPath)) {
        throw "manifest self was not built: $($row.relative_self)"
    }
}

$csv = Join-Path $BuildRoot "smoke-build.csv"
$results | Export-Csv -NoTypeInformation -Path $csv
Write-Host "smoke build results: $csv"
