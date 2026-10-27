<#
Builds every PSGL sample with the ps3dk-run hook linked in (run-hook-inject.cmake),
for a hardware boot gate.  The sample sources are not changed.

  build-samples.ps1 -Out <new dir> -HookDir <spike/ps3dk-run checkout> [-Sdk <PS3DK>]
                    [-Samples a,b] [-LibDir <dir with libPSGL.a / libgcm_cmd.a>]
                    [-RsxCg <rsx-cg-compiler.exe>]
                    [-Cmake <cmake.exe>] [-Ninja <ninja.exe>]

-LibDir puts private ILP32 archives first on the link path (for testing a
tree before it is installed); without it the samples link the SDK's own.
-RsxCg selects the rsx-cg-compiler for the samples' shaders and shader
libraries; without it the SDK's own is used.
Writes <Out>/selfs.tsv (sample, SELF, fake SELF, sha256) and exits nonzero
on the first configure or build failure.  It takes no build slot itself.
#>
param(
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$HookDir,
    [string]$Sdk = $env:PS3DK,
    [string[]]$Samples = @(),
    [string]$LibDir = '',
    [string]$RsxCg = '',
    [string]$Cmake = 'cmake',
    [string]$Ninja = 'ninja'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$inject = "$PSScriptRoot/run-hook-inject.cmake"
if (-not $Sdk) { throw 'set -Sdk or PS3DK' }
if (Test-Path $Out) { throw "output exists: $Out" }
if (-not (Test-Path "$HookDir/hook/ps3dk_run_hook.c")) { throw "no run hook in $HookDir" }
New-Item -ItemType Directory $Out | Out-Null
if ($Samples.Count -eq 0) {
    $Samples = Get-ChildItem "$repo/samples/toolchain" -Directory |
        Where-Object { $_.Name -like 'hello-psgl*' } | ForEach-Object { $_.Name } | Sort-Object
}
$env:PS3DK = $Sdk; $env:PS3DEV = $Sdk; $env:PSL1GHT = $Sdk
$extra = @()
if ($LibDir) {
    $extra += "-DCMAKE_EXE_LINKER_FLAGS=-mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections -L$LibDir"
}
if ($RsxCg) {
    if (-not (Test-Path $RsxCg)) { throw "no compiler $RsxCg" }
    $extra += "-DPSGL_RSX_CG_COMPILER=$RsxCg"
    $extra += "-DPS3_TOOL_rsxcgc=$RsxCg"
}
$rows = @("sample`tself`tfake_self`tsha256")
# Native tools report through exit codes; stderr lines are not errors.
$ErrorActionPreference = 'Continue'
foreach ($s in $Samples) {
    $dir = "$repo/samples/toolchain/$s"
    if (-not (Test-Path "$dir/CMakeLists.txt")) { throw "no sample $s" }
    $b = "$Out/$s"
    & $Cmake -S $dir -B $b -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DCMAKE_TOOLCHAIN_FILE=$Sdk/cmake/ps3-ppu-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PROJECT_INCLUDE=$inject" "-DPS3DK_RUN_HOOK_DIR=$HookDir" "-DPS3_SELF_OUTPUT_DIRECTORY=$b" @extra *> "$Out/$s.configure.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$Out/$s.configure.log" -Tail 15; throw "configure $s" }
    if (-not (Select-String -Quiet -Path "$Out/$s.configure.log" -Pattern 'run-hook-inject: hook linked into [1-9]')) { throw "hook not injected into $s" }
    & $Cmake --build $b --parallel 2 *> "$Out/$s.build.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$Out/$s.build.log" -Tail 25; throw "build $s" }
    $self = Get-ChildItem $b -Filter "*.self" | Where-Object { $_.Name -notlike '*.fake.self' } | Select-Object -First 1
    $fake = Get-ChildItem $b -Filter "*.fake.self" | Select-Object -First 1
    if (-not $self -or -not $fake) { throw "no SELF for $s" }
    $h = (Get-FileHash -Algorithm SHA256 $self.FullName).Hash.ToLower()
    $rows += "$s`t$($self.FullName)`t$($fake.FullName)`t$h"
}
[System.IO.File]::WriteAllLines("$Out/selfs.tsv", $rows)
"built $($Samples.Count) sample(s): $Out/selfs.tsv"
exit 0
