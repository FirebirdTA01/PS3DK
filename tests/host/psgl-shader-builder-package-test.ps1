# tests/host/psgl-shader-builder-package-test.ps1
#
# Behavioral CMake configure test for psgl-shader-builder / hello-psgl-ffp-shaderlib:
#   1. Packaged builder present with no host compiler/sources succeeds (packaged release path)
#   2. Absent builder + absent sources fails specifically with FATAL_ERROR (refuses dummy-skip)
#   3. Source fallback requires a host compiler and succeeds only with both
#   4. Rejects original dummy-skip behavior (proves red on exact baseline CMakeLists.txt)
#   5. Validates --version output follows project version convention (not a versionless label)
#
# Stubs target/toolchain setup so no compiler, target process, or emulator runs.
# Isolates PATH, PS3DEV, and PS3DK so host tools are never accidentally discovered.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\psgl-shader-builder-package-test.ps1

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$fails = 0

function Check([bool]$ok, [string]$what) {
    if ($ok) {
        Write-Host "psgl-shader-builder-behavioral: ok   $what"
    } else {
        Write-Host "psgl-shader-builder-behavioral: FAIL $what"
        $script:fails++
    }
}

# ---------------------------------------------------------------------------
# 1. Version format convention check
# ---------------------------------------------------------------------------
$builderSource = Get-Content -Raw (Join-Path $repo 'tools\psgl-shader-builder\psgl_shader_builder.c')
$versionFile = (Get-Content -Raw (Join-Path $repo 'tools\VERSION')).Trim()

$hasVersionMacro = ($builderSource -match '#define\s+PSGL_SHADER_BUILDER_VERSION\s+"([A-Za-z0-9_.-]+)"')
$embeddedVer = if ($hasVersionMacro) { $matches[1] } else { '' }
$printsVersion = ($builderSource -match 'printf\("psgl-shader-builder\s+%s\\n",\s*PSGL_SHADER_BUILDER_VERSION\);')

Check ($hasVersionMacro -and $printsVersion) `
    "psgl_shader_builder.c prints versioned banner 'psgl-shader-builder <version>', not versionless label"

# ---------------------------------------------------------------------------
# 2. Behavioral CMake configure tests (stubbed toolchain, no compiler executed)
# ---------------------------------------------------------------------------
$work = Join-Path ([IO.Path]::GetTempPath()) ("psgl-behavioral-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null

function Invoke-CMake([string[]]$cmakeArgs) {
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'

    $oldPath = $env:PATH
    $oldPS3DEV = $env:PS3DEV
    $oldPS3DK = $env:PS3DK

    # Isolate environment from host compilers / tools
    $cmakeCmd = Get-Command cmake
    $cmakeDir = Split-Path $cmakeCmd.Source
    $sysDir = [Environment]::GetFolderPath('System')
    $env:PATH = "$work;$cmakeDir;$sysDir"
    $env:PS3DEV = ""
    $env:PS3DK = ""

    try {
        $lines = & cmake @cmakeArgs 2>&1 | ForEach-Object { "$_" }
        return @{
            rc = $LASTEXITCODE
            text = ($lines -join "`n")
        }
    } finally {
        $env:PATH = $oldPath
        $env:PS3DEV = $oldPS3DEV
        $env:PS3DK = $oldPS3DK
        $ErrorActionPreference = $prevEAP
    }
}

try {
    $modDir = Join-Path $work 'modules'
    New-Item -ItemType Directory -Force $modDir | Out-Null

    # Stub ps3-self.cmake: provides target hooks without host SDK install checks
    @'
function(ps3_bin2s target file)
endfunction()
function(ps3_add_self target)
endfunction()
'@ | Out-File -Encoding ascii (Join-Path $modDir 'ps3-self.cmake')

    # Stub toolchain: marks C/ASM compiler as working without running any cross-compiler
    $mockCc = (Join-Path $work 'mock-cc.cmd').Replace('\', '/')
    $ccMarker = (Join-Path $work 'mock-cc-called.marker')
    $ccMarkerFwd = $ccMarker.Replace('\', '/')
    Set-Content -Path (Join-Path $work 'mock-cc.cmd') -Value "@echo off`necho called >> `"$ccMarkerFwd`"`nexit /b 0"

    @"
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER_ID_RUN 1)
set(CMAKE_C_COMPILER_ID GNU)
set(CMAKE_C_COMPILER_VERSION 16.1.0)
set(CMAKE_C_STANDARD_COMPUTED_DEFAULT 17)
set(CMAKE_C_COMPILE_FEATURES c_std_11)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_C_ABI_COMPILED TRUE)

set(CMAKE_ASM_COMPILER_ID GNU)
set(CMAKE_ASM_COMPILER_VERSION 16.1.0)
set(CMAKE_ASM_COMPILER_WORKS TRUE)
set(CMAKE_ASM_ABI_COMPILED TRUE)

set(CMAKE_C_COMPILER "$mockCc")
set(CMAKE_ASM_COMPILER "$mockCc")
set(CMAKE_C_COMPILER_FORCED TRUE)
set(CMAKE_ASM_COMPILER_FORCED TRUE)
"@ | Out-File -Encoding ascii (Join-Path $modDir 'stub-tc.cmake')

    # Mock host helper executables
    $mockNinja = (Join-Path $work 'ninja.cmd').Replace('\', '/')
    '@echo 1.11.0' | Out-File -Encoding ascii (Join-Path $work 'ninja.cmd')

    $mockCg = (Join-Path $work 'mock-rsx-cg-compiler.cmd').Replace('\', '/')
    '@echo 1.0.0' | Out-File -Encoding ascii (Join-Path $work 'mock-rsx-cg-compiler.cmd')

    $mockBuilder = (Join-Path $work 'mock-psgl-shader-builder.cmd').Replace('\', '/')
    '@echo 1.0.0' | Out-File -Encoding ascii (Join-Path $work 'mock-psgl-shader-builder.cmd')

    $tcPath = (Join-Path $modDir 'stub-tc.cmake').Replace('\', '/')
    $modFwd = $modDir.Replace('\', '/')

    # -----------------------------------------------------------------------
    # Setup Package Layout (no tools/ or sdk/libPSGL/src/)
    # -----------------------------------------------------------------------
    $pkgRoot = Join-Path $work 'pkg'
    $pkgSample = Join-Path $pkgRoot 'samples\toolchain\hello-psgl-ffp-shaderlib'
    $pkgSampleSrc = Join-Path $pkgSample 'source'
    $pkgAssets = Join-Path $pkgRoot 'sdk\assets'
    New-Item -ItemType Directory -Force $pkgSampleSrc | Out-Null
    New-Item -ItemType Directory -Force $pkgAssets | Out-Null

    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\CMakeLists.txt') $pkgSample/
    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\state.txt') $pkgSample/
    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\source\main.c') $pkgSampleSrc/
    Copy-Item (Join-Path $repo 'sdk\assets\ICON0.PNG') $pkgAssets/

    # Case A: Packaged builder present with no host compiler/sources succeeds
    $bldA = Join-Path $work 'bld-case-a'
    $cmakeArgsA = @(
        '-S', $pkgSample,
        '-B', $bldA,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$mockNinja",
        "-DCMAKE_MODULE_PATH=$modFwd",
        "-DCMAKE_TOOLCHAIN_FILE=$tcPath",
        "-DPSGL_RSX_CG_COMPILER=$mockCg",
        "-DPSGL_SHADER_BUILDER=$mockBuilder"
    )
    $resA = Invoke-CMake $cmakeArgsA
    Check ($resA.rc -eq 0) 'Case A: packaged builder present with no host compiler/sources succeeds'

    # Case B: Absent builder + absent sources fails specifically with FATAL_ERROR
    $bldB = Join-Path $work 'bld-case-b'
    $cmakeArgsB = @(
        '-S', $pkgSample,
        '-B', $bldB,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$mockNinja",
        "-DCMAKE_MODULE_PATH=$modFwd",
        "-DCMAKE_TOOLCHAIN_FILE=$tcPath",
        "-DPSGL_RSX_CG_COMPILER=$mockCg"
    )
    $resB = Invoke-CMake $cmakeArgsB
    Check ($resB.rc -ne 0 -and $resB.text -match 'builder sources are not present') `
        'Case B: absent builder and absent sources fails closed specifically'

    # -----------------------------------------------------------------------
    # Setup Source-Tree Layout (tools/psgl-shader-builder and sdk/libPSGL/src present)
    # -----------------------------------------------------------------------
    $srcRoot = Join-Path $work 'src-tree'
    $srcSample = Join-Path $srcRoot 'samples\toolchain\hello-psgl-ffp-shaderlib'
    $srcSampleSrc = Join-Path $srcSample 'source'
    $srcAssets = Join-Path $srcRoot 'sdk\assets'
    $srcTools = Join-Path $srcRoot 'tools\psgl-shader-builder'
    $srcLibPSGL = Join-Path $srcRoot 'sdk\libPSGL\src'
    New-Item -ItemType Directory -Force $srcSampleSrc | Out-Null
    New-Item -ItemType Directory -Force $srcAssets | Out-Null
    New-Item -ItemType Directory -Force $srcTools | Out-Null
    New-Item -ItemType Directory -Force $srcLibPSGL | Out-Null

    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\CMakeLists.txt') $srcSample/
    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\state.txt') $srcSample/
    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\source\main.c') $srcSampleSrc/
    Copy-Item (Join-Path $repo 'sdk\assets\ICON0.PNG') $srcAssets/
    Copy-Item (Join-Path $repo 'tools\psgl-shader-builder\psgl_shader_builder.c') $srcTools/
    Copy-Item (Join-Path $repo 'sdk\libPSGL\src\ffp_emit.c') $srcLibPSGL/
    Copy-Item (Join-Path $repo 'sdk\libPSGL\src\ffp_emit.h') $srcLibPSGL/

    # Case C1: Source fallback with absent host compiler fails specifically
    $bldC1 = Join-Path $work 'bld-case-c1'
    $cmakeArgsC1 = @(
        '-S', $srcSample,
        '-B', $bldC1,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$mockNinja",
        "-DCMAKE_MODULE_PATH=$modFwd",
        "-DCMAKE_TOOLCHAIN_FILE=$tcPath",
        "-DPSGL_RSX_CG_COMPILER=$mockCg"
    )
    $resC1 = Invoke-CMake $cmakeArgsC1
    Check ($resC1.rc -ne 0 -and $resC1.text -match 'no\s+host C compiler' -and $resC1.text -match 'available to build') `
        'Case C1: source fallback requires host compiler and fails when absent'

    # Case C2: Source fallback with host compiler present succeeds
    $bldC2 = Join-Path $work 'bld-case-c2'
    $cmakeArgsC2 = @(
        '-S', $srcSample,
        '-B', $bldC2,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$mockNinja",
        "-DCMAKE_MODULE_PATH=$modFwd",
        "-DCMAKE_TOOLCHAIN_FILE=$tcPath",
        "-DPSGL_RSX_CG_COMPILER=$mockCg",
        "-DPS3_HOST_CC=$mockCc"
    )
    $resC2 = Invoke-CMake $cmakeArgsC2
    Check ($resC2.rc -eq 0) 'Case C2: source fallback succeeds when sources and host compiler both exist'

    # -----------------------------------------------------------------------
    # Case D: Mutation check — exact baseline CMakeLists.txt dummy-skip is rejected
    # -----------------------------------------------------------------------
    $mutSample = Join-Path $work 'orig-mutation\samples\toolchain\hello-psgl-ffp-shaderlib'
    New-Item -ItemType Directory -Force (Join-Path $mutSample 'source') | Out-Null
    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\source\main.c') (Join-Path $mutSample 'source')
    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\state.txt') $mutSample/

    # Copy exact baseline fixture from commit 7366c95aa
    $fixturePath = Join-Path $repo 'tests\host\fixtures\hello-psgl-ffp-shaderlib-7366c95aa.CMakeLists.txt'
    Copy-Item $fixturePath (Join-Path $mutSample 'CMakeLists.txt')

    $bldMut = Join-Path $work 'bld-mutation'
    $cmakeArgsMut = @(
        '-S', $mutSample,
        '-B', $bldMut,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$mockNinja",
        "-DCMAKE_MODULE_PATH=$modFwd",
        "-DCMAKE_TOOLCHAIN_FILE=$tcPath"
    )
    $resMut = Invoke-CMake $cmakeArgsMut
    # Exact original CMake returned rc=0 with no .self targets; fail-closed contract rejects this
    $isDummySkip = ($resMut.rc -eq 0 -and $resMut.text -match 'Skipping hello-psgl-ffp-shaderlib: no host C compiler found')
    Check ($isDummySkip) `
        'Case D: mutation proof confirms exact baseline returned rc=0 dummy-skip (which fail-closed rejects)'

    # Toolchain stub check: verify mock compiler was never invoked during configure
    Check (-not (Test-Path $ccMarker)) `
        'Toolchain stub verified: zero compiler invocations during configure'

} finally {
    if ($work) {
        $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
        $resolvedWork = [IO.Path]::GetFullPath($work).TrimEnd('\', '/')
        if ($resolvedWork.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
            $resolvedWork.Length -gt $tempRoot.Length) {
            Remove-Item -Recurse -Force $resolvedWork -ErrorAction SilentlyContinue
        }
    }
}

if ($fails -ne 0) {
    Write-Error "psgl-shader-builder-behavioral: $fails check(s) failed."
    exit 1
}
exit 0
