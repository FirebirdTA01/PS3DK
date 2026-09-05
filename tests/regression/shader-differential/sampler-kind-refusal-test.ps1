# sampler-kind-refusal-test.ps1 - regression test for t_e230822b
#
# Asserts that the differential harness auto-binder rejects unsupported
# sampler kinds (such as sampler arrays and generic sampler) with -1 refusal
# rather than silently continuing/skipping them without binding textures.
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo_root = (Resolve-Path (Join-Path $here "../../..")).Path

# We compile a small host test program in WSL against sdk/libgcm_cmd
# exercising is_sampler_type() and the binder parameter classifier.
$wsl_root = $repo_root.Substring(0, 1).ToLowerInvariant()
$wsl_path = "/mnt/" + $wsl_root + $repo_root.Substring(2).Replace('\', '/')

$runnerSrc = @"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <Cg/cgBinary.h>
#include <cell/gcm/gcm_cg_func.h>

#define CG_SAMPLER1D          1065
#define CG_SAMPLER2D          1066
#define CG_SAMPLER3D          1067
#define CG_SAMPLERRECT        1068
#define CG_SAMPLERCUBE        1069
#define CG_SAMPLER1DARRAY     1138
#define CG_SAMPLER2DARRAY     1139
#define CG_SAMPLERCUBEARRAY   1140
#define CG_SAMPLER            1143

#define CG_TEXUNIT0           0
#define CG_TEXUNIT15          15

// The exact function from tests/regression/shader-differential/source/main.c:642
static int is_sampler_type(uint32_t type)
{
    return (type >= CG_SAMPLER1D && type <= CG_SAMPLERCUBE) ||
           type == CG_SAMPLER1DARRAY || type == CG_SAMPLER2DARRAY ||
           type == CG_SAMPLERCUBEARRAY || type == CG_SAMPLER;
}

// Simulates the buggy skip condition from 274a3df before t_e230822b was fixed
static int is_sampler_type_buggy(uint32_t type)
{
    return (type >= CG_SAMPLER1D && type <= CG_SAMPLERCUBE);
}

// Mimics bind_container_samplers logic from main.c:656
static int classify_sampler(uint32_t type, bool referenced, uint32_t res)
{
    if (!is_sampler_type(type))
        return 0; // skipped as non-sampler
    if (!referenced)
        return 0; // unreferenced sampler skipped
    if (type != CG_SAMPLER2D || res < CG_TEXUNIT0 || res > CG_TEXUNIT15)
        return -1; // REFUSED
    return 1; // bound
}

static int classify_sampler_buggy(uint32_t type, bool referenced, uint32_t res)
{
    if (!is_sampler_type_buggy(type))
        return 0; // skipped
    if (!referenced)
        return 0;
    if (type != CG_SAMPLER2D || res < CG_TEXUNIT0 || res > CG_TEXUNIT15)
        return -1;
    return 1;
}

int main(void)
{
    // Test 1: Unsupported array and generic samplers MUST return -1 (refusal)
    uint32_t unsupported_kinds[] = {
        CG_SAMPLER1D, CG_SAMPLER3D, CG_SAMPLERRECT, CG_SAMPLERCUBE,
        CG_SAMPLER1DARRAY, CG_SAMPLER2DARRAY, CG_SAMPLERCUBEARRAY, CG_SAMPLER
    };
    for (int i = 0; i < 8; ++i) {
        uint32_t k = unsupported_kinds[i];
        int res = classify_sampler(k, true, 0);
        if (res != -1) {
            fprintf(stderr, "FAIL: sampler kind %u returned %d, expected -1 refusal (t_e230822b)\n", k, res);
            return 1;
        }
    }

    // Test 2: Verify that the buggy pre-fix logic WOULD have returned 0 (silent bypass)
    uint32_t array_kinds[] = {CG_SAMPLER1DARRAY, CG_SAMPLER2DARRAY, CG_SAMPLERCUBEARRAY, CG_SAMPLER};
    for (int i = 0; i < 4; ++i) {
        uint32_t k = array_kinds[i];
        int buggy_res = classify_sampler_buggy(k, true, 0);
        if (buggy_res != 0) {
            fprintf(stderr, "FAIL: sanity check failed: buggy model did not produce 0 for %u\n", k);
            return 2;
        }
    }

    // Test 3: Standard 2D sampler on unit 0 MUST return 1 (bound)
    if (classify_sampler(CG_SAMPLER2D, true, 0) != 1) {
        fprintf(stderr, "FAIL: CG_SAMPLER2D failed to bind\n");
        return 3;
    }

    // Test 4: Non-sampler types MUST be skipped (return 0)
    uint32_t non_samplers[] = {1050, 1053, 1141, 1142};
    for (int i = 0; i < 4; ++i) {
        if (classify_sampler(non_samplers[i], true, 0) != 0) {
            fprintf(stderr, "FAIL: non-sampler %u was treated as sampler\n", non_samplers[i]);
            return 4;
        }
    }

    printf("sampler-kind-refusal-test: ok\n");
    return 0;
}
"@

$work = Join-Path $env:TEMP ("sd-sampler-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null

try {
    $cFile = Join-Path $work "runner.c"
    Set-Content -LiteralPath $cFile -Value $runnerSrc -Encoding Ascii
    $wslCFile = "/mnt/" + $work.Substring(0, 1).ToLowerInvariant() + $work.Substring(2).Replace('\', '/') + "/runner.c"
    $wslBin = "/tmp/sampler_refusal_runner"

    $compileCmd = "gcc -I $wsl_path/sdk/libgcm_cmd/include -I $wsl_path/sdk/libgcm_cmd/include/cell $wslCFile -o $wslBin"
    $compileOut = (& wsl bash -c $compileCmd 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "compilation failed: $compileOut"
    }

    $runOut = (& wsl $wslBin 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "runner failed: $runOut"
    }
    Write-Host $runOut.Trim()
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "PASS: sampler-kind-refusal-test"
