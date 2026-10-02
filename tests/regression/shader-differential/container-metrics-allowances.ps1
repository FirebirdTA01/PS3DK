# Opt-in, exact historical metrics exceptions. No default allowance is loaded.
function Assert-ContainerMetricsAllowancePin($Pin) {
    if ($null -eq $Pin -or $Pin.Path -isnot [string] -or
        [string]::IsNullOrWhiteSpace($Pin.Path) -or
        $Pin.Sha256 -isnot [string] -or $Pin.Sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw 'container metrics allowance: invalid provenance pin'
    }
    if (-not (Test-Path -LiteralPath $Pin.Path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $Pin.Path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Pin.Sha256) {
        throw "container metrics allowance: pin mismatch: $($Pin.Path)"
    }
}

function Get-ContainerMetricsAllowanceMatch([object[]]$Rows, [object[]]$BaselineRows, [string]$Path) {
    function Bad([string]$why) { throw "container metrics allowance: $why" }
    function Properties($obj, [string[]]$names, [string]$where) {
        if ($null -eq $obj -or $obj -isnot [pscustomobject]) { Bad "$where must be an object" }
        $actual = @($obj.PSObject.Properties.Name)
        if ($actual.Count -ne $names.Count) { Bad "$where has missing or unknown fields" }
        foreach ($name in $names) { if ($actual -cnotcontains $name) { Bad "$where lacks '$name'" } }
    }
    function Integer($value, [string]$where) {
        if (($value -isnot [int] -and $value -isnot [long]) -or $value -lt [int]::MinValue -or $value -gt [int]::MaxValue) {
            Bad "$where must be a 32-bit JSON integer"
        }
    }
    $identity = @('role', 'name', 'profile', 'source', 'uniform_set')
    $metrics = @('ours_instruction_count', 'reference_instruction_count', 'instruction_delta',
                 'ours_register_count', 'reference_register_count', 'register_delta')
    function Key($row) {
        foreach ($field in $identity) {
            if ($row.$field -isnot [string] -or [string]::IsNullOrWhiteSpace($row.$field) -or $row.$field.Contains('|')) {
                Bad "invalid identity field '$field'"
            }
        }
        return (($identity | ForEach-Object { $row.$_ }) -join '|')
    }
    function Counts($counts, [string]$where) {
        Properties $counts $metrics $where
        foreach ($field in $metrics) {
            Integer $counts.$field "$where.$field"
            if ($field -like '*_count' -and $counts.$field -lt 0) { Bad "$where.$field is negative" }
        }
        if ($counts.instruction_delta -ne ([long]$counts.ours_instruction_count - $counts.reference_instruction_count) -or
            $counts.register_delta -ne ([long]$counts.ours_register_count - $counts.reference_register_count)) {
            Bad "$where has inconsistent deltas"
        }
    }
    function MatchCounts($expected, $actual, [string]$where) {
        foreach ($field in $metrics) {
            # Imported CSV numbers are strings; reject empty/null/nonintegral fields.
            $value = $actual.$field
            $number = 0
            if ($null -eq $value -or "$value" -cnotmatch '^-?[0-9]+$' -or
                -not [int]::TryParse("$value", [ref]$number) -or $number -ne $expected.$field) {
                Bad "stale or changed $where.$field (expected $($expected.$field), got '$value')"
            }
        }
    }
    function UniqueJsonMembers([string]$json) {
        # Windows PowerShell accepts JavaScript syntax and silently replaces duplicate
        # members. Validate strict JSON structure and unique decoded names first.
        $tokens = [regex]::Matches($json, '"(?:\\.|[^"\\])*"|[{}\[\]:,]|[^\s{}\[\]:,"]+')
        $end = 0
        foreach ($t in $tokens) {
            if ($json.Substring($end, $t.Index - $end) -cnotmatch '^[ \t\r\n]*$') { Bad 'invalid JSON token' }
            $end = $t.Index + $t.Length
            if ($t.Value.StartsWith('"') -and $t.Value -cnotmatch '^"(?:\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4})|[^"\\\x00-\x1f])*"$') {
                Bad 'invalid JSON string'
            }
        }
        if ($json.Substring($end) -cnotmatch '^[ \t\r\n]*$') { Bad 'invalid JSON token' }
        $cursor = @{ Index=0 }
        function Peek {
            if ($cursor.Index -ge $tokens.Count) { Bad 'unexpected end of JSON' }
            return $tokens[$cursor.Index].Value
        }
        function Take([string]$expected) {
            if ((Peek) -cne $expected) { Bad "expected JSON token '$expected'" }
            $cursor.Index++
        }
        function JsonValue([int]$depth) {
            if ($depth -gt 64) { Bad 'JSON nesting exceeds 64 levels' }
            $token = Peek
            if ($token -ceq '{') {
                $cursor.Index++
                $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
                if ((Peek) -ceq '}') { $cursor.Index++; return }
                while ($true) {
                    $member = Peek
                    if (-not $member.StartsWith('"')) { Bad 'JSON member must be double-quoted' }
                    $name = ('{"member":' + $member + '}' | ConvertFrom-Json -ErrorAction Stop).member
                    if (-not $names.Add($name)) { Bad "duplicate JSON member: $name" }
                    $cursor.Index++
                    Take ':'
                    JsonValue ($depth + 1)
                    if ((Peek) -ceq '}') { $cursor.Index++; return }
                    Take ','
                }
            } elseif ($token -ceq '[') {
                $cursor.Index++
                if ((Peek) -ceq ']') { $cursor.Index++; return }
                while ($true) {
                    JsonValue ($depth + 1)
                    if ((Peek) -ceq ']') { $cursor.Index++; return }
                    Take ','
                }
            } elseif ($token.StartsWith('"') -or $token -cmatch '^(true|false|null|-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)$') {
                $cursor.Index++
            } else { Bad "invalid JSON value: $token" }
        }
        JsonValue 0
        if ($cursor.Index -ne $tokens.Count) { Bad 'extra JSON tokens' }
    }
    try {
        $resolved = (Get-Item -LiteralPath $Path -ErrorAction Stop).FullName
        $bytes = [IO.File]::ReadAllBytes($resolved)
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
        # Hash precisely the bytes parsed, including any UTF-8 BOM.
        $json = [Text.UTF8Encoding]::new($false, $true).GetString($bytes).TrimStart([char]0xfeff)
        UniqueJsonMembers $json
        $doc = $json | ConvertFrom-Json -ErrorAction Stop
    } catch { Bad "cannot read JSON '$Path': $($_.Exception.Message)" }
    Properties $doc @('schema_version', 'entries') 'document'
    Integer $doc.schema_version 'schema_version'
    if ($doc.schema_version -ne 1) { Bad 'unsupported schema_version' }
    if ($doc.entries -isnot [array]) { Bad 'entries must be an array' }

    $current = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    $historical = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    $baselineIndex = Get-ContainerMetricBaselineIndex $BaselineRows
    foreach ($row in $Rows) {
        $key = Key $row
        $null = Get-ContainerMetricIdentity $row
        if ($baselineIndex.Versions.ContainsKey($key)) { continue }
        if ($current.ContainsKey($key)) { Bad "duplicate current row: $key" }
        $current.Add($key, $row)
    }
    foreach ($row in $BaselineRows) {
        $key = Key $row
        if ($baselineIndex.Versions.ContainsKey($key)) { continue }
        if ($historical.ContainsKey($key)) { Bad "duplicate historical row: $key" }
        $historical.Add($key, $row)
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $matched = @()
    foreach ($entry in $doc.entries) {
        Properties $entry ($identity + @('card', 'reason', 'count', 'historical', 'current')) 'entry'
        $key = Key $entry
        if ($baselineIndex.Versions.ContainsKey($key)) { Bad "schema-v1 allowance cannot cover a versioned identity: $key" }
        if (-not $seen.Add($key)) { Bad "duplicate entry: $key" }
        foreach ($field in @('card', 'reason')) {
            if ($entry.$field -isnot [string] -or [string]::IsNullOrWhiteSpace($entry.$field)) { Bad "entry requires $field" }
        }
        Integer $entry.count 'count'
        if ($entry.count -ne 1) { Bad 'count must be 1 for a unique metrics row' }
        Counts $entry.historical 'historical'
        Counts $entry.current 'current'
        if (-not $current.ContainsKey($key) -or -not $historical.ContainsKey($key)) { Bad "stale entry (row missing): $key" }
        MatchCounts $entry.historical $historical[$key] "historical[$key]"
        MatchCounts $entry.current $current[$key] "current[$key]"
        if ($entry.current.instruction_delta -le $entry.historical.instruction_delta -and
            $entry.current.register_delta -le $entry.historical.register_delta) { Bad "stale entry (no regression): $key" }
        $matched += $key
    }
    return [pscustomobject]@{
        Pin = [pscustomobject]@{ Path=$resolved; Sha256=$hash }
        MatchedKeys = @($matched)
    }
}
