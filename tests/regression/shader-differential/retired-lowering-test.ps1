$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$work = Join-Path ([IO.Path]::GetTempPath()) ('sd-retired-lowering-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $work
$sentinel = Join-Path $work 'keep.txt'
[IO.File]::WriteAllText($sentinel, 'untouched')
$pass = 0; $fail = 0
try {
    # These must refuse before root resolution, compiler discovery or mutation.
    # Deliberately invalid compiler/repo paths must never mask the retired-option error.
    foreach ($option in @('LegacyLowering', 'PathPairs', 'PathPairCorpus', 'VpPathPairs', 'PathPairsList', 'PathPairCorpusDir', 'PathPairCorpusManifest')) {
        $argsForStage = @{ Hdd0=$work; RepoRoot=(Join-Path $work 'missing-repo'); Rsxcgc='missing-compiler'; Rpcs3Path='missing-emulator' }
        $argsForStage[$option] = if ($option -in @('PathPairsList','PathPairCorpusDir','PathPairCorpusManifest')) { 'retired-input' } else { $true }
        $message = ''
        try { & (Join-Path $here 'stage-differential.ps1') @argsForStage | Out-Null }
        catch { $message = $_.Exception.Message }
        $files = @(Get-ChildItem -LiteralPath $work -Recurse -Force)
        $family = if ($option -like 'PathPairCorpus*') { 'PathPairCorpus' } elseif ($option -like 'PathPairs*') { 'PathPairs' } else { $option }
        if ($message -like "*-$family*removed*" -and $files.Count -eq 1 -and
            [IO.File]::ReadAllText($sentinel) -ceq 'untouched') {
            $pass++; Write-Host "PASS: $option refuses before mutation"
        } else {
            $fail++; Write-Host "FAIL: $option did not refuse before setup: $message"
        }
    }
    # Exercise the production list-routing function without running the stager.
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $here 'stage-differential.ps1'), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw 'Stager does not parse' }
    $fn = $ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Row-AppliesToPath'}, $true)
    . ([scriptblock]::Create($fn.Extent.Text))
    foreach ($mode in @('', 'general', 'both', 'legacy', 'default', 'typo')) {
        $accepted = $false; $refused = $false
        try { $accepted = Row-AppliesToPath @('shader','0',$mode) 2 } catch { $refused = $true }
        $want = $mode -in @('', 'general', 'both')
        if (($want -and $accepted -and -not $refused) -or (-not $want -and $refused)) {
            $pass++; Write-Host "PASS: list mode '$mode'"
        } else { $fail++; Write-Host "FAIL: list mode '$mode'" }
    }
    $rejectAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $here 'must-reject.ps1'), [ref]$tokens, [ref]$errors)
    $parser = $rejectAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Parse-MustRejectRow'}, $true)
    if (-not $parser) { $fail++; Write-Host 'FAIL: old must-reject schema has no refusal guard' }
    else {
        . ([scriptblock]::Create($parser.Extent.Text))
        $oldRefused = $false
        try { $null = Parse-MustRejectRow 'accept_for_loop|accept|accept/backend-refuse|accept|' }
        catch { $oldRefused = $_.Exception.Message -like '*retired five-column*' }
        if ($oldRefused) { $pass++; Write-Host 'PASS: old must-reject schema refused' }
        else { $fail++; Write-Host 'FAIL: old list can weaken general expectation' }
        $oldRefused = $false
        try { $null = Parse-MustRejectRow 'accept_for_loop|accept|accept/backend-refuse|accept' }
        catch { $oldRefused = $_.Exception.Message -like '*retired*schema*' }
        if ($oldRefused) { $pass++; Write-Host 'PASS: old schema without note refused' }
        else { $fail++; Write-Host 'FAIL: old four-column list can weaken general expectation' }
        $oldRefused = $false
        try { $null = Parse-MustRejectRow 'reject_missing_return|reject|backend-refuse|frontend-reject / accept' }
        catch { $oldRefused = $_.Exception.Message -like '*retired*schema*' }
        if ($oldRefused) { $pass++; Write-Host 'PASS: old schema with spaced alternatives refused' }
        else { $fail++; Write-Host 'FAIL: spaced alternatives can weaken general expectation' }
        $row = Parse-MustRejectRow 'accept_for_loop|accept|accept|note'
        if ($row.WantGeneral -ceq 'accept' -and $row.Note -ceq 'note') { $pass++; Write-Host 'PASS: current must-reject schema' }
        else { $fail++; Write-Host 'FAIL: current must-reject schema' }
    }
    # Preserve every shader/set witness formerly reached by curated path pairs.
    $reference = @(Get-Content (Join-Path $here 'reference-pairs.txt') | Where-Object { $_ -and -not $_.StartsWith('#') })
    foreach ($row in @(Get-Content (Join-Path $here 'path-pairs.txt') | Where-Object { $_ -and -not $_.StartsWith('#') })) {
        $fields = $row.Split('|'); $wanted = $fields[0] + '|' + $fields[1]
        $matches = @($reference | Where-Object { $f=$_.Split('|'); ($f[0]+'|'+$f[1]) -ceq $wanted })
        if ($matches.Count) { $pass++; Write-Host "PASS: retained $wanted" }
        else { $fail++; Write-Host "FAIL: lost reference witness $wanted" }
    }
} finally {
    $resolved = [IO.Path]::GetFullPath($work)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'sd-retired-lowering-*') { throw 'Unexpected cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
Write-Host "retired-lowering: $($pass+$fail) tests, $pass pass, $fail fail"
if ($fail) { exit 1 }
