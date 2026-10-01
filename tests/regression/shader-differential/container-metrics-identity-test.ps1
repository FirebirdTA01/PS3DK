$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'container-metrics.ps1')
$script:passed=0; $script:failed=0
function Check([string]$name, [scriptblock]$body) {
    try { & $body; $script:passed++; Write-Host "PASS: $name" }
    catch { $script:failed++; Write-Host "FAIL: $name ($($_.Exception.Message))" }
}
function Assert($ok) { if (-not $ok) { throw 'assertion failed' } }
function Reject([scriptblock]$body) { $threw=$false; try { & $body | Out-Null } catch { $threw=$true }; Assert $threw }
function Row([string]$hash=('a'*64)) {
    [pscustomobject]@{name='shader';role='reference';profile='sce_fp_rsx';source='test.cg';uniform_set='auto';
        ours_instruction_count=10;reference_instruction_count=6;instruction_delta=4;
        ours_register_count=3;reference_register_count=2;register_delta=1;pixel_status='unknown';
        identity_version='1';source_sha256=$hash;compile_flags='{"ours":[],"reference":["-p","sce_fp_rsx"]}';
        resolved_includes='[]';oracle_sha256=('c'*64)}
}
function Legacy { $r=Row; foreach($p in @('identity_version','source_sha256','compile_flags','resolved_includes','oracle_sha256')) { $r.PSObject.Properties.Remove($p) }; $r }
Check 'changed source is not compared to another program' {
    $s=Get-ContainerMetricsGateSummary @(Row ('b'*64)) @(Row)
    Assert ($s.ComparedBaselineRows -eq 0 -and $s.SourceVersionMismatches -eq 1 -and $s.Status -eq 'identity-mismatch')
}
Check 'two versions coexist and exact version compares' {
    $s=Get-ContainerMetricsGateSummary @(Row ('b'*64)) @((Row),(Row ('b'*64)))
    Assert ($s.ComparedBaselineRows -eq 1 -and $s.Status -eq 'ok')
}
Check 'same identity still protects cost' {
    $r=Row; $r.ours_instruction_count++; $r.instruction_delta++
    Assert ((Get-ContainerMetricsGateSummary @($r) @(Row)).BaselineRegressions -eq 1)
}
Check 'missing identity cannot fall back beside a legacy baseline' {
    $s=Get-ContainerMetricsGateSummary @(Legacy) @((Legacy),(Row))
    Assert ($s.ComparedBaselineRows -eq 0 -and $s.MissingIdentityRows -eq 1 -and $s.Status -eq 'identity-mismatch')
}
Check 'unversioned identities preserve legacy comparison' {
    Assert ((Get-ContainerMetricsGateSummary @(Row) @(Legacy)).ComparedBaselineRows -eq 1)
}
foreach($field in @('source_sha256','oracle_sha256')) {
    Check "malformed $field rejected" { $r=Row; $r.$field='broken'; Reject { Get-ContainerMetricsGateSummary @($r) @(Row) } }
}
Check 'changed compile flags fail coverage' {
    $r=Row; $r.compile_flags='{"ours":["-O0"],"reference":["-p","sce_fp_rsx"]}'
    $s=Get-ContainerMetricsGateSummary @($r) @(Row)
    Assert ($s.CompileFlagsMismatches -eq 1 -and $s.Status -eq 'identity-mismatch')
}
Check 'oracle drift is distinguished from source drift' {
    $r=Row; $r.oracle_sha256='d'*64
    $s=Get-ContainerMetricsGateSummary @($r) @(Row)
    Assert ($s.OracleMismatches -eq 1 -and $s.SourceVersionMismatches -eq 0)
}
Check 'include bytes enter identity' {
    $a=Row; $b=Row
    $a.resolved_includes='[{"side":"ours","path":"common.cg","sha256":"'+('a'*64)+'"}]'
    $b.resolved_includes='[{"side":"ours","path":"common.cg","sha256":"'+('b'*64)+'"}]'
    Assert ((Get-ContainerMetricsGateSummary @($b) @($a)).IncludeMismatches -eq 1)
}
Check 'include resolution path enters identity' {
    $a=Row; $b=Row
    $a.resolved_includes='[{"side":"ours","path":"one/common.cg","sha256":"'+('a'*64)+'"}]'
    $b.resolved_includes='[{"side":"ours","path":"two/common.cg","sha256":"'+('a'*64)+'"}]'
    Assert ((Get-ContainerMetricsGateSummary @($b) @($a)).IncludeMismatches -eq 1)
}
Check 'identity mismatch cannot hide beside a passing legacy row' {
    $legacy=Legacy; $legacy.name='another'
    $s=Get-ContainerMetricsGateSummary @($legacy,(Row ('b'*64))) @($legacy,(Row))
    Assert ($s.ComparedBaselineRows -eq 1 -and $s.Status -eq 'identity-mismatch')
}
Check 'duplicate exact versions rejected' { Reject { Get-ContainerMetricsGateSummary @(Row) @((Row),(Row)) } }
Check 'unknown version rejected' { $r=Row; $r.identity_version='2'; Reject { Get-ContainerMetricsGateSummary @($r) @(Row) } }
Check 'partial identity rejected' { $r=Row; $r.identity_version=''; Reject { Get-ContainerMetricsGateSummary @($r) @(Row) } }
Check 'unresolved provenance cannot claim a versioned identity' {
    $r=Row; $r | Add-Member NoteProperty input_status 'unversioned'
    Reject { Get-ContainerMetricsGateSummary @($r) @(Row) }
}
Check 'malformed include hash rejected' { $r=Row; $r.resolved_includes='[{"side":"ours","path":"common.cg","sha256":"bad"}]'; Reject { Get-ContainerMetricsGateSummary @($r) @(Row) } }
Check 'non-array flags rejected' { $r=Row; $r.compile_flags='{"ours":"-O0","reference":[]}'; Reject { Get-ContainerMetricsGateSummary @($r) @(Row) } }
Check 'source hash folds only CRLF bytes to LF' {
    $lf=[byte[]]@(0x61,0x0a,0xff,0x0d,0x62)
    $crlf=[byte[]]@(0x61,0x0d,0x0a,0xff,0x0d,0x62)
    Assert ((Get-ShaderSourceHash $lf) -ceq (Get-ShaderSourceHash $crlf))
    Assert ((Get-ShaderSourceHash $lf) -cne (Get-ShaderSourceHash ([byte[]]@(0x61,0x0a,0xff,0x62))))
}
$work=Join-Path ([IO.Path]::GetTempPath()) ('sd-identity-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($work) | Out-Null
$utf8=[Text.UTF8Encoding]::new($false)
function Fixture($name,$text) { $p=Join-Path $work $name; [IO.File]::WriteAllText($p,$text,$utf8); return $p }
try {
    $src=Fixture 'shader.cg' "#include `"common.cg`"`nfloat main() {return value;}`n"
    $inc=Fixture 'common.cg' "const float value=1;`n"
    Check 'literal dependency tree is resolved and hashed' {
        $p=Get-ShaderCompileInputs $src @('-p','sce_fp_rsx')
        Assert ($p.Status -eq 'versioned' -and $p.Includes.Count -eq 1 -and $p.Includes[0].path -ceq 'common.cg')
        Assert ($p.Includes[0].sha256 -ceq (Get-ShaderSourceHash ([IO.File]::ReadAllBytes($inc))))
    }
    Check 'CRLF copy of a source fixture retains identity' {
        $copy=Fixture 'crlf.cg' ([IO.File]::ReadAllText($src).Replace("`n","`r`n"))
        Assert ((Get-ShaderCompileInputs $src @()).SourceHash -ceq (Get-ShaderCompileInputs $copy @()).SourceHash)
    }
    Check 'conditional include is explicitly unversioned' {
        $conditional=Fixture 'conditional.cg' "#if 1`n#include `"common.cg`"`n#endif`n"
        $p=Get-ShaderCompileInputs $conditional @()
        Assert ($p.Status -eq 'unversioned' -and $p.Reason -match 'conditional')
    }
    Check 'macro include is explicitly unversioned' {
        $macro=Fixture 'macro.cg' "#define FILE `"common.cg`"`n#include FILE`n"
        Assert ((Get-ShaderCompileInputs $macro @()).Status -eq 'unversioned')
    }
    Check 'missing include is explicitly unversioned' {
        $missing=Fixture 'missing.cg' "#include `"absent.cg`"`n"
        Assert ((Get-ShaderCompileInputs $missing @()).Status -eq 'unversioned')
    }
    Check 'commented include is not a dependency' {
        $comment=Fixture 'comment.cg' "/* #include `"absent.cg`" */`n// #include FILE`nfloat value;`n"
        Assert ((Get-ShaderCompileInputs $comment @()).Status -eq 'versioned')
    }
    Check 'include changes during compile are rejected' {
        $p=Get-ShaderCompileInputs $src @()
        [IO.File]::AppendAllText($inc,"float another;`n",$utf8)
        Reject { Assert-ShaderCompileInputs $p }
    }
    Check 'old pixel status is never inherited by input metadata' {
        $a=Get-ShaderCompileInputs $src @('-O1')
        $b=Get-ShaderCompileInputs $src @('-p','sce_fp_rsx')
        $b | Add-Member NoteProperty OracleHash ('c'*64)
        $p=Join-ShaderCompileInputs $a $b
        Assert ($null -eq $p.pixel_status -and $p.identity_version -eq '1')
    }
    Check 'unversioned rows are named in the gate report' {
        $r=Legacy; $r | Add-Member NoteProperty input_status 'unversioned'; $r | Add-Member NoteProperty input_reason 'conditional include'
        $baseline=Join-Path $work 'legacy.csv'; @(Legacy) | Export-Csv -NoTypeInformation $baseline
        $report=@(Write-ContainerMetricsGateReport @($r) $baseline 6>&1)
        Assert (($report | Out-String) -match 'SDIFF-METRICS-UNVERSIONED\|count=1')
        Assert (($report | Out-String) -match 'reference\|shader\|sce_fp_rsx\|test.cg\|auto.*conditional include')
    }
    Check 'schema-v1 allowance cannot cover a versioned row' {
        $base=Row; $current=Row; $current.ours_instruction_count=11; $current.instruction_delta=5
        $counts=@('ours_instruction_count','reference_instruction_count','instruction_delta','ours_register_count','reference_register_count','register_delta')
        $a=@{};$b=@{};foreach($f in $counts) {$a[$f]=$base.$f;$b[$f]=$current.$f}
        $entry=@{role=$base.role;name=$base.name;profile=$base.profile;source=$base.source;uniform_set=$base.uniform_set;card='test';reason='test';count=1;historical=$a;current=$b}
        $path=Join-Path $work 'allowance.json'
        @{schema_version=1;entries=@($entry)} | ConvertTo-Json -Depth 10 | Set-Content $path
        Reject { Get-ContainerMetricsAllowanceMatch @($current) @($base) $path }
    }
    Check 'removing a migrated entry keeps remaining legacy allowance valid' {
        $base=Legacy; $base.name='remaining'; $current=Legacy; $current.name='remaining'; $current.ours_instruction_count=11;$current.instruction_delta=5
        $counts=@('ours_instruction_count','reference_instruction_count','instruction_delta','ours_register_count','reference_register_count','register_delta')
        $a=@{};$b=@{};foreach($f in $counts) {$a[$f]=$base.$f;$b[$f]=$current.$f}
        $entry=@{role=$base.role;name=$base.name;profile=$base.profile;source=$base.source;uniform_set=$base.uniform_set;card='test';reason='test';count=1;historical=$a;current=$b}
        $path=Join-Path $work 'remaining.json'
        @{schema_version=1;entries=@($entry)} | ConvertTo-Json -Depth 10 | Set-Content $path
        $match=Get-ContainerMetricsAllowanceMatch @($current,(Row)) @($base,(Row),(Row ('b'*64))) $path
        Assert ($match.MatchedKeys.Count -eq 1)
    }
    Check 'identity mismatch fails enforcing boundary even without cost regression' {
        $baseline=Join-Path $work 'versioned.csv'; @(Row) | Export-Csv -NoTypeInformation $baseline
        Reject { Invoke-ContainerMetricsStageGate @(Legacy) $baseline }
    }
    Check 'actual compile boundary records empty flags without null entries' {
        $tokens=$null; $errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'stage-differential.ps1'),[ref]$tokens,[ref]$errors)
        Assert ($errors.Count -eq 0)
        $fn=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Compile-Shader'},$true)
        Invoke-Expression $fn.Extent.Text
        function TestIdentityCompiler {
            $script:seenCompilerArgs=@($args)
            $index=[array]::IndexOf($args,'--emit-container')
            [IO.File]::WriteAllBytes($args[$index+1],[byte[]]@(1,2,3,4))
            $global:LASTEXITCODE=0
        }
        $useWsl=$false; $Rsxcgc='TestIdentityCompiler'; $extraFlags=@('--legacy-lowering')
        $dst=Join-Path $work 'test.fpo'
        $null=Compile-Shader $src $dst @() -Absolute -NoExtraFlags
        $inputs=$script:containerCompileInputs[[IO.Path]::GetFullPath($dst)]
        Assert (($inputs.Flags -join ',') -ceq '-p,sce_fp_rsx')
        Assert (($script:seenCompilerArgs[0..1] -join ',') -ceq '-p,sce_fp_rsx')
        $null=Compile-Shader $src $dst @('--general-lowering','-O0') -Absolute
        $inputs=$script:containerCompileInputs[[IO.Path]::GetFullPath($dst)]
        Assert (($inputs.Flags -join ',') -ceq '--general-lowering,-O0,-p,sce_fp_rsx')
        Assert (($script:seenCompilerArgs[0..3] -join ',') -ceq '--general-lowering,-O0,-p,sce_fp_rsx')
        $null=Compile-Shader $src $dst @() -Absolute
        Assert (($script:containerCompileInputs[[IO.Path]::GetFullPath($dst)].Flags -join ',') -ceq '--legacy-lowering,-p,sce_fp_rsx')
    }
    Check 'refused migrated corpus input cannot disappear from the identity gate' {
        $tokens=$null; $errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'stage-differential.ps1'),[ref]$tokens,[ref]$errors)
        $loop=$ast.Find({param($n) $n -is [Management.Automation.Language.ForEachStatementAst] -and $n.Condition.Extent.Text -eq '$rtFileRows'},$true)
        Assert ($null -ne $loop)
        $failed=Fixture 'failed.cg' "#include `"missing.cg`"`n"
        $rtFileRows=@([pscustomobject]@{File=(Get-Item $failed);RelativePath='failed.cg'})
        $seenNames=@{}; $refScratch=$work; $ReferenceProbeRefused=$false
        $containerMetricRows=@(); $script:containerMetricAttempts=@()
        $rtRefusedRows=@();$rtRefusedOurs=0;$rtRefusedRef=0;$rtBothRefused=0
        function Compile-Shader { $script:lastCompileRc=1; return $false }
        function Compile-Reference { $script:lastCompileRc=1; return $false }
        function Has-FileScopeConst { return $false }
        Invoke-Expression $loop.Extent.Text
        Assert ($containerMetricRows.Count -eq 0 -and $script:containerMetricAttempts.Count -eq 1)
        $baselineRow=Row; $baselineRow.name='failed';$baselineRow.role='reference-tree-corpus';$baselineRow.source='failed.cg'
        $s=Get-ContainerMetricsGateSummary $containerMetricRows @($baselineRow) -AttemptedInputs $script:containerMetricAttempts
        Assert ($s.Status -eq 'identity-mismatch' -and $s.MissingVersionedResults.Count -eq 1 -and $s.UnversionedRows.Count -eq 1)
        $legacy=Legacy; $legacy.name='failed';$legacy.role='reference-tree-corpus';$legacy.source='failed.cg'
        $s=Get-ContainerMetricsGateSummary $containerMetricRows @($legacy) -AttemptedInputs $script:containerMetricAttempts
        Assert ($s.Status -eq 'ok' -and $s.MissingVersionedResults.Count -eq 0)
        # A partial stage never attempted this identity: it is not a missing
        # result and must not be confused with the refused attempt above.
        Assert ((Get-ContainerMetricsGateSummary @() @($baselineRow)).Status -eq 'ok')
    }
} finally {
    if ([IO.Path]::GetFullPath($work).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) -and
        (Split-Path $work -Leaf) -like 'sd-identity-*') { Remove-Item -LiteralPath $work -Recurse -Force }
}
Write-Host "container-metrics-identity-test: tests=$($script:passed+$script:failed) pass=$script:passed fail=$script:failed"
if($script:failed) { exit 1 }
