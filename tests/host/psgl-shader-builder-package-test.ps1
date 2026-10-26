# tests/host/psgl-shader-builder-package-test.ps1
#
# Behavioral CMake configure test for psgl-shader-builder / hello-psgl-ffp-shaderlib:
#   1. A compiler without --psgl-ffp-library is refused at configure with an
#      actionable FATAL_ERROR, whether or not a packaged builder, the builder
#      sources or a host C compiler are present (no psgl-shader-builder fallback)
#   2. A compiler whose --help lists the mode but exits nonzero is refused too
#   3. Rejects original dummy-skip behavior (proves red on exact baseline CMakeLists.txt)
#   4. Validates --version output follows project version convention (not a versionless label)
#   5. A compiler that lists --psgl-ffp-library builds the library itself: no builder,
#      no sources, no host compiler, and the generated rule runs that mode
#   6. The shaders.bin rule depends on the compiler binary, so replacing it rebuilds
#
# Stubs target/toolchain setup so no compiler, target process, or emulator runs.
# Isolates PATH, PS3DEV, and PS3DK so host tools are never accidentally discovered.
#
# -SampleCMakeLists runs the cases against another copy of the sample's
# CMakeLists.txt (a red check against an older one).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\psgl-shader-builder-package-test.ps1

param([string]$SampleCMakeLists = '')

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$fails = 0
if (-not $SampleCMakeLists) {
    $SampleCMakeLists = Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\CMakeLists.txt'
}
if (-not (Test-Path -LiteralPath $SampleCMakeLists -PathType Leaf)) {
    Write-Error "psgl-shader-builder-behavioral: no such CMakeLists.txt: $SampleCMakeLists"
    exit 2
}
Write-Host "psgl-shader-builder-behavioral: sample CMakeLists.txt $SampleCMakeLists sha256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $SampleCMakeLists).Hash.ToLower())"

function Check([bool]$ok, [string]$what) {
    if ($ok) {
        Write-Host "psgl-shader-builder-behavioral: ok   $what"
    } else {
        Write-Host "psgl-shader-builder-behavioral: FAIL $what"
        $script:fails++
    }
}

# The configure-time refusal for a compiler without --psgl-ffp-library.
# CMake wraps FATAL_ERROR text, so whitespace is collapsed before matching.
function Test-Refused($res) {
    $flat = ($res.text -replace '\s+', ' ')
    return ($res.rc -ne 0 -and
            $flat -match 'CMake Error' -and
            $flat -match 'does not support --psgl-ffp-library' -and
            $flat -match 'An rsx-cg-compiler with --psgl-ffp-library is required')
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

    Copy-Item -LiteralPath $SampleCMakeLists (Join-Path $pkgSample 'CMakeLists.txt')
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
    Check (Test-Refused $resA) `
        'Case A: compiler without --psgl-ffp-library is refused even with a packaged builder'

    # Case B: compiler without the mode, no builder and no sources: same refusal
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
    Check (Test-Refused $resB) `
        'Case B: compiler without --psgl-ffp-library, no builder or sources, is refused with the actionable message'
    Write-Host "psgl-shader-builder-behavioral: Case B rc=$($resB.rc) configure output: $($resB.text -replace '\s+', ' ')"

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

    Copy-Item -LiteralPath $SampleCMakeLists (Join-Path $srcSample 'CMakeLists.txt')
    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\state.txt') $srcSample/
    Copy-Item (Join-Path $repo 'samples\toolchain\hello-psgl-ffp-shaderlib\source\main.c') $srcSampleSrc/
    Copy-Item (Join-Path $repo 'sdk\assets\ICON0.PNG') $srcAssets/
    Copy-Item (Join-Path $repo 'tools\psgl-shader-builder\psgl_shader_builder.c') $srcTools/
    Copy-Item (Join-Path $repo 'sdk\libPSGL\src\ffp_emit.c') $srcLibPSGL/
    Copy-Item (Join-Path $repo 'sdk\libPSGL\src\ffp_emit.h') $srcLibPSGL/

    # Case C1: builder sources present, no host compiler: refused, not a source fallback
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
    Check (Test-Refused $resC1) `
        'Case C1: compiler without --psgl-ffp-library is refused in the source tree with no host compiler'

    # Case C2: builder sources and a host compiler present: still refused, the
    # wrapper built from those sources would call the missing mode
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
    Check (Test-Refused $resC2) `
        'Case C2: compiler without --psgl-ffp-library is refused even with builder sources and a host compiler'
    $ninjaC2 = Join-Path $bldC2 'build.ninja'
    Check (-not (Test-Path $ninjaC2)) 'Case C2: no build.ninja is generated for the refused combination'

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

    # -----------------------------------------------------------------------
    # Case E: the compiler lists --psgl-ffp-library in --help, so the sample
    # uses it directly, even in the package layout with no builder or sources.
    # -----------------------------------------------------------------------
    $mockCgFfp = (Join-Path $work 'mock-rsx-cg-ffp.cmd').Replace('\', '/')
    @'
@echo off
echo usage: rsx-cg-compiler [options] ^<shader.cg^> 1>&2
echo   --psgl-ffp-library ^<state.txt^> -o ^<shaders.bin^> 1>&2
exit /b 0
'@ | Out-File -Encoding ascii (Join-Path $work 'mock-rsx-cg-ffp.cmd')

    $bldE = Join-Path $work 'bld-case-e'
    $cmakeArgsE = @(
        '-S', $pkgSample,
        '-B', $bldE,
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$mockNinja",
        "-DCMAKE_MODULE_PATH=$modFwd",
        "-DCMAKE_TOOLCHAIN_FILE=$tcPath",
        "-DPSGL_RSX_CG_COMPILER=$mockCgFfp"
    )
    $resE = Invoke-CMake $cmakeArgsE
    $ninjaE = Join-Path $bldE 'build.ninja'
    $ruleE = if (Test-Path $ninjaE) { Get-Content -Raw $ninjaE } else { '' }
    Check ($resE.rc -eq 0) 'Case E: --psgl-ffp-library compiler configures with no builder, sources or host compiler'
    Check ($ruleE -match '--psgl-ffp-library' -and $ruleE -notmatch 'shaderlib-work') `
        'Case E: generated rule runs rsx-cg-compiler --psgl-ffp-library, not the builder'

    # Case E2: the mode wins over a packaged builder that is also present
    $bldE2 = Join-Path $work 'bld-case-e2'
    $cmakeArgsE2 = $cmakeArgsE.Clone()
    $cmakeArgsE2[3] = $bldE2
    $cmakeArgsE2 += "-DPSGL_SHADER_BUILDER=$mockBuilder"
    $resE2 = Invoke-CMake $cmakeArgsE2
    $ninjaE2 = Join-Path $bldE2 'build.ninja'
    $ruleE2 = if (Test-Path $ninjaE2) { Get-Content -Raw $ninjaE2 } else { '' }
    Check ($resE2.rc -eq 0 -and $ruleE2 -match '--psgl-ffp-library' -and $ruleE2 -notmatch 'mock-psgl-shader-builder') `
        'Case E2: --psgl-ffp-library is used even when psgl-shader-builder is also present'

    # Case E3: the shaders.bin build edge lists the compiler as an input
    $edgeE = @($ruleE -split "`r?`n" | Where-Object {
        $_ -match '^build .*shaders\.bin.*: CUSTOM_COMMAND ' })
    Check ($edgeE.Count -eq 1 -and $edgeE[0] -match 'mock-rsx-cg-ffp\.cmd') `
        'Case E3: the shaders.bin rule depends on the rsx-cg-compiler binary'

    # Case F: --help lists the mode but exits nonzero: not a capability signal
    $mockCgBad = (Join-Path $work 'mock-rsx-cg-ffp-rc1.cmd').Replace('\', '/')
    @'
@echo off
echo   --psgl-ffp-library ^<state.txt^> -o ^<shaders.bin^> 1>&2
exit /b 1
'@ | Out-File -Encoding ascii (Join-Path $work 'mock-rsx-cg-ffp-rc1.cmd')
    $bldF = Join-Path $work 'bld-case-f'
    $cmakeArgsF = $cmakeArgsE.Clone()
    $cmakeArgsF[3] = $bldF
    $cmakeArgsF[-1] = "-DPSGL_RSX_CG_COMPILER=$mockCgBad"
    $resF = Invoke-CMake $cmakeArgsF
    Check ((Test-Refused $resF) -and ($resF.text -replace '\s+', ' ') -match 'exited 1') `
        'Case F: a compiler whose --help exits 1 is refused even though it prints the mode'

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
