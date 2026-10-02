# Versioned input identities. Compiler-under-test identity deliberately stays
# outside this key: cost changes between compiler revisions are what we gate.
function Get-ShaderSourceHash([byte[]]$Bytes) {
    $normalized=[Collections.Generic.List[byte]]::new($Bytes.Length)
    for($i=0; $i -lt $Bytes.Length; $i++) {
        if($Bytes[$i] -eq 13 -and $i+1 -lt $Bytes.Length -and $Bytes[$i+1] -eq 10) { continue }
        $normalized.Add($Bytes[$i])
    }
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($normalized.ToArray()))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-ContainerMetricIdentity($Row) {
    $fields=@('identity_version','source_sha256','compile_flags','resolved_includes','oracle_sha256')
    if (@($fields | Where-Object { -not [string]::IsNullOrEmpty([string]$Row.$_) }).Count -eq 0) { return '' }
    function Bad($why) { throw "container metrics identity: $why" }
    if ([string]$Row.identity_version -cne '1') { Bad 'unsupported or missing identity_version' }
    if ($Row.input_status -and $Row.input_status -cne 'versioned') { Bad 'unresolved provenance cannot claim a versioned identity' }
    foreach($field in @('source_sha256','oracle_sha256')) {
        if ($Row.$field -isnot [string] -or $Row.$field -cnotmatch '^[0-9a-f]{64}$') { Bad "invalid $field" }
    }
    try { $flags=ConvertFrom-Json -InputObject $Row.compile_flags -ErrorAction Stop }
    catch { Bad 'invalid compile_flags JSON' }
    if ($flags -isnot [pscustomobject] -or @($flags.PSObject.Properties).Count -ne 2 -or
        $flags.ours -isnot [array] -or $flags.reference -isnot [array]) { Bad 'compile_flags requires ours/reference arrays' }
    foreach($arg in @($flags.ours)+@($flags.reference)) { if($arg -isnot [string]) { Bad 'compile flag must be a string' } }
    # Canonical serialization also rejects duplicate JSON members, unknown
    # properties and alternate representations rather than silently collapsing.
    $canonicalFlags=ConvertTo-Json -InputObject ([ordered]@{ours=@($flags.ours);reference=@($flags.reference)}) -Compress -Depth 6
    if ($canonicalFlags -cne $Row.compile_flags) { Bad 'noncanonical compile_flags' }
    try { $doc=ConvertFrom-Json -InputObject ('{"items":'+$Row.resolved_includes+'}') -ErrorAction Stop }
    catch { Bad 'invalid resolved_includes JSON' }
    if($doc.items -isnot [array]) { Bad 'resolved_includes must be an array' }
    $includes=@(); $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($inc in $doc.items) {
        if ($inc -isnot [pscustomobject] -or @($inc.PSObject.Properties).Count -ne 3 -or
            $inc.side -cnotin @('ours','reference') -or $inc.path -isnot [string] -or
            [string]::IsNullOrWhiteSpace($inc.path) -or $inc.path.Contains('|') -or
            $inc.sha256 -isnot [string] -or $inc.sha256 -cnotmatch '^[0-9a-f]{64}$') { Bad 'invalid resolved include' }
        if(-not $seen.Add("$($inc.side)|$($inc.path)")) { Bad 'duplicate resolved include' }
        $includes += [ordered]@{side=$inc.side;path=$inc.path;sha256=$inc.sha256}
    }
    $canonicalIncludes=ConvertTo-Json -InputObject @($includes) -Compress -Depth 6
    if($canonicalIncludes -cne $Row.resolved_includes) { Bad 'noncanonical resolved_includes' }
    $identity=ConvertTo-Json -InputObject @('1',$Row.source_sha256,$canonicalFlags,$canonicalIncludes,$Row.oracle_sha256) -Compress
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity)))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-ContainerMetricBaselineIndex([object[]]$Rows) {
    $index=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    $versions=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    foreach($row in $Rows) {
        $base=Get-ContainerMetricKey $row; $identity=Get-ContainerMetricIdentity $row
        $key=if($identity) { "$base|v1:$identity" } else { $base }
        if($index.ContainsKey($key)) { throw "duplicate container metrics baseline row: $key" }
        $index.Add($key,$row)
        if($identity) {
            if(-not $versions.ContainsKey($base)) { $versions.Add($base,@()) }
            $versions[$base] += $row
        }
    }
    return [pscustomobject]@{Rows=$index;Versions=$versions}
}

# Bounded resolver, not a second preprocessor. Only unconditional quoted
# includes resolved beside the including file are proven here. Conditional,
# macro, system and search-path graphs remain visibly unversioned.
function Get-ShaderCompileInputs([string]$SourcePath, [string[]]$Flags) {
    $root=(Get-Item -LiteralPath $SourcePath -ErrorAction Stop).FullName
    $rootDir=(Split-Path $root -Parent)+[IO.Path]::DirectorySeparatorChar
    $files=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    $includes=[Collections.Generic.List[object]]::new()
    $state=@{Reason=''}
    function Visit([string]$path, [int]$depth) {
        if($depth -gt 64) { $state.Reason='include depth exceeds resolver limit'; return }
        if($files.ContainsKey($path)) { $state.Reason='cyclic or repeated include graph'; return }
        $bytes=[IO.File]::ReadAllBytes($path)
        $hash=Get-ShaderSourceHash $bytes
        $files.Add($path,$hash)
        if($depth -gt 0) {
            $relative=[Uri]::UnescapeDataString(([Uri]$rootDir).MakeRelativeUri([Uri]$path).ToString())
            $includes.Add([ordered]@{path=$relative;sha256=$hash})
        }
        try { $text=[Text.UTF8Encoding]::new($false,$true).GetString($bytes).TrimStart([char]0xfeff) }
        catch { $state.Reason='non-UTF8 dependency text'; return }
        if($text -match '\\\r?\n') { $state.Reason='line-spliced dependency text'; return }
        # Preserve quoted strings while removing comments, including newlines.
        $text=[regex]::Replace($text,'"(?:\\.|[^"\\])*"|/\*[\s\S]*?\*/|//[^\r\n]*',{
            param($m)
            if($m.Value.StartsWith('"')) { return $m.Value }
            return [regex]::Replace($m.Value,'[^\r\n]',' ')
        })
        $conditional=0
        foreach($line in ($text -split '\r?\n')) {
            if($line -notmatch '^\s*#\s*([A-Za-z_]+)\b(.*)$') { continue }
            $command=$Matches[1]; $operand=$Matches[2].Trim()
            if($command -cin @('if','ifdef','ifndef')) { $conditional++; continue }
            if($command -ceq 'endif') { $conditional--; continue }
            if($command -cin @('elif','else')) { continue }
            if($command -ceq 'line') { $state.Reason='line directive can alter include resolution'; return }
            if($command -like 'include*' -or $command -ceq 'import') {
                if($conditional -ne 0) { $state.Reason='conditional include'; return }
                if($command -cne 'include' -or $operand -cnotmatch '^"([^"\r\n]+)"$') { $state.Reason='macro or system include'; return }
                $literal=$Matches[1]
                if([IO.Path]::IsPathRooted($literal)) { $state.Reason='absolute include path'; return }
                $child=[IO.Path]::GetFullPath((Join-Path (Split-Path $path -Parent) $literal))
                if(-not (Test-Path -LiteralPath $child -PathType Leaf)) { $state.Reason='include requires unresolved search path'; return }
                Visit $child ($depth+1)
                if($state.Reason) { return }
            }
        }
        if($conditional -ne 0) { $state.Reason='conditional state crosses file boundary' }
    }
    Visit $root 0
    return [pscustomobject]@{
        Status=$(if($state.Reason) {'unversioned'} else {'versioned'})
        Reason=$state.Reason; SourcePath=$root; SourceHash=$files[$root]
        Flags=@($Flags); Includes=@($includes.ToArray()); Files=$files
    }
}

function Assert-ShaderCompileInputs($Before) {
    $after=Get-ShaderCompileInputs $Before.SourcePath $Before.Flags
    $old=ConvertTo-Json -InputObject @($Before.Status,$Before.Reason,$Before.SourceHash,$Before.Includes) -Compress -Depth 8
    $new=ConvertTo-Json -InputObject @($after.Status,$after.Reason,$after.SourceHash,$after.Includes) -Compress -Depth 8
    if($old -cne $new) { throw "shader inputs changed during compile: $($Before.SourcePath)" }
}

function Join-ShaderCompileInputs($Ours, $Reference) {
    if($null -eq $Ours -or $null -eq $Reference) { throw 'missing actual compile input record' }
    if($Ours.SourceHash -cne $Reference.SourceHash) { throw 'ours/reference compiled different source bytes' }
    $result=[ordered]@{identity_version='';source_sha256='';compile_flags='';resolved_includes='';oracle_sha256='';input_status='unversioned';input_reason=''}
    if($Ours.Status -ne 'versioned' -or $Reference.Status -ne 'versioned') {
        $result.input_reason="ours=$($Ours.Reason); reference=$($Reference.Reason)"
        return [pscustomobject]$result
    }
    $includes=@()
    foreach($side in @('ours','reference')) {
        $inputRecord=if($side -eq 'ours') {$Ours} else {$Reference}
        foreach($inc in $inputRecord.Includes) { $includes += [ordered]@{side=$side;path=$inc.path;sha256=$inc.sha256} }
    }
    $result.identity_version='1'; $result.source_sha256=$Ours.SourceHash
    $result.compile_flags=ConvertTo-Json -InputObject ([ordered]@{ours=@($Ours.Flags);reference=@($Reference.Flags)}) -Compress -Depth 6
    $result.resolved_includes=ConvertTo-Json -InputObject @($includes) -Compress -Depth 6
    $result.oracle_sha256=$Reference.OracleHash; $result.input_status='versioned'
    $row=[pscustomobject]$result
    $null=Get-ContainerMetricIdentity $row
    return $row
}
