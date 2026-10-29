param([string]$Output='')
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot)
$work=if($Output){[IO.Path]::GetFullPath($Output)}else{Join-Path ([IO.Path]::GetTempPath()) ('sweep-artifacts-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path -LiteralPath $work){throw 'Fresh output required'}
New-Item -ItemType Directory -Path $work | Out-Null
$pass=0;$fail=0
function Check([bool]$ok,[string]$name){if($ok){$script:pass++;Write-Host "ok $name"}else{$script:fail++;Write-Host "FAIL $name"}}
function Refuses([scriptblock]$action){try{& $action | Out-Null;return $false}catch{return $true}}
try {
    . (Join-Path $repo 'scripts/sample-sweep-artifacts.ps1')
    $run=Join-Path $work 'run-a'
    New-Item -ItemType Directory -Path "$run/logs","$run/b1","$work/pkg/samples" | Out-Null
    'family,sample,configure,build,self,log' | Set-Content "$run/results.csv"
    'artifact' | Set-Content "$run/b1/one.fake.self"
    'stale' | Set-Content "$work/pkg/samples/two.fake.self"
    Check ((Resolve-SampleSweepArtifact $run 'one').FullName -eq [IO.Path]::GetFullPath("$run/b1/one.fake.self")) 'exact artifact in named run'
    Check ($null -eq (Resolve-SampleSweepArtifact $run 'two')) 'source-only artifact not found'
    Check (Refuses {Resolve-SampleSweepArtifact $work 'one'}) 'parent output directory refused'
    Check (Refuses {Resolve-SampleSweepArtifact '' 'one'}) 'empty run refused'
    Check (Refuses {Resolve-SampleSweepArtifact $run '../one'}) 'path traversal name refused'
    Check (Refuses {Resolve-SampleSweepArtifact $run '*'}) 'wildcard name refused'
    New-Item -ItemType Directory -Path "$run/b2" | Out-Null
    'duplicate' | Set-Content "$run/b2/one.fake.self"
    Check (Refuses {Resolve-SampleSweepArtifact $run 'one'}) 'ambiguous artifacts refused'
} catch { $fail++;Write-Host "FAIL harness: $_" }
finally {
    if(-not $Output){
        $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if(-not $work.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe cleanup path'}
        Remove-Item -LiteralPath $work -Recurse -Force
    }
}
Write-Host "sample-sweep-artifacts: $pass pass, $fail fail"
if($fail){exit 1}
