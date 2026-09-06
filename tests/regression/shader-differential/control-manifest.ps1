# control-manifest.ps1 -- the proving-control invariant of a shader-differential
# manifest, as data and as two functions (t_678a4dab).
#
# GATE EXISTENCE IS NOT GATE SUCCESS.  The guest opens its MRT and depth
# instruments only after every member of the proving set has RUN AND PASSED
# ahead of the row that needs it, so a manifest is only useful when the set is
# complete and precedes the first corpus row.  The stager builds that order by
# construction (Add-ProvingControls) and refuses to write anything else
# (Get-ControlManifestProblems); the guest is the second line.
#
# Dot-sourced by stage-differential.ps1 and by manifest-controls-test.ps1.

# Every role the guest requires before it opens the gate that role belongs to.
$script:ProvingControlRoles = @(
    "control-mrt-identical",
    "control-mrt-mismatch",
    "control-sparse-identical",
    "control-sparse-mismatch",
    "control-sparse-skip",
    "control-depth-identical",
    "control-depth-mismatch",
    "control-depth-blind",
    "control-depthonly-identical",
    "control-depthonly-mismatch"
)

function Get-ProvingControlRoles { return @($script:ProvingControlRoles) }

function Get-ManifestRole([string]$line) {
    if ($line -notmatch '^B\|') { return $null }
    $f = $line.Split('|')
    if ($f.Count -lt 3) { return $null }
    return $f[1]
}

# Insert the proving rows immediately before the first B row whose role is
# not a control, so they precede every corpus row whatever else the stager
# appended and in whatever order.  Rows are placed at the end when the
# manifest has no corpus row at all.
function Add-ProvingControls([string[]]$Manifest, [string[]]$Rows) {
    $out = New-Object System.Collections.Generic.List[string]
    $inserted = $false
    foreach ($line in $Manifest) {
        $role = Get-ManifestRole $line
        if (-not $inserted -and $role -and -not $role.StartsWith("control-")) {
            foreach ($r in $Rows) { $out.Add($r) }
            $inserted = $true
        }
        $out.Add($line)
    }
    if (-not $inserted) { foreach ($r in $Rows) { $out.Add($r) } }
    # The comma keeps a one-element result an array: PowerShell unrolls a
    # single-element array to its element on return, and a caller's $p[0]
    # would then index a character rather than a problem.
    return ,$out.ToArray()
}

# Problems with a manifest's proving set: a required role absent, present more
# than once, or placed after the first non-control B row.  An empty result is
# the only acceptable one; the stager throws on anything else.
function Get-ControlManifestProblems([string[]]$Manifest) {
    $problems = New-Object System.Collections.Generic.List[string]
    $firstCorpus = -1
    $seen = @{}
    for ($i = 0; $i -lt $Manifest.Count; $i++) {
        $role = Get-ManifestRole $Manifest[$i]
        if (-not $role) { continue }
        if (-not $role.StartsWith("control-")) {
            if ($firstCorpus -lt 0) { $firstCorpus = $i }
            continue
        }
        if ($script:ProvingControlRoles -contains $role) {
            if ($seen.ContainsKey($role)) {
                $problems.Add("proving control '$role' appears more than once (lines $($seen[$role] + 1) and $($i + 1))")
            } else {
                $seen[$role] = $i
            }
        }
    }
    foreach ($role in $script:ProvingControlRoles) {
        if (-not $seen.ContainsKey($role)) {
            $problems.Add("proving control '$role' is missing: the guest's gate cannot open")
        } elseif ($firstCorpus -ge 0 -and $seen[$role] -gt $firstCorpus) {
            $problems.Add("proving control '$role' (line $($seen[$role] + 1)) comes after the first corpus row (line $($firstCorpus + 1)): rows before it would be judged blind")
        }
    }
    return ,$problems.ToArray()
}
