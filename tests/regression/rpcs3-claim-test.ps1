param([string]$ClaimScript = (Join-Path $PSScriptRoot '../../scripts/rpcs3-claim.ps1'))

$ErrorActionPreference = 'Stop'
$ClaimScript = (Resolve-Path -LiteralPath $ClaimScript).Path
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ('rpcs3-claim-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$failures = 0

function Assert-Case([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        Write-Host "FAIL: $Message"
        $script:failures++
    }
}

try {
    # Inject faults only at the writer boundary. The unmodified claim script
    # still opens the real file atomically, disposes it and performs cleanup.
    $runner = Join-Path $scratch 'fault-runner.ps1'
    @'
param([string]$ClaimScript, [string]$LockPath, [string]$Mode)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.IO;
public sealed class FaultingLockWriter : StreamWriter {
    readonly string mode;
    public FaultingLockWriter(Stream stream, string mode) : base(stream) { this.mode = mode; }
    public override void WriteLine(string value) {
        base.WriteLine(value);
        if (mode == "write") throw new IOException("injected claim write failure");
    }
    public override void Flush() {
        if (mode == "flush") throw new IOException("injected claim flush failure");
        base.Flush();
    }
    protected override void Dispose(bool disposing) {
        try { base.Dispose(disposing); }
        finally {
            if (disposing && (mode == "flush" || mode == "dispose"))
                throw new IOException("injected claim " + mode + " failure");
        }
    }
}
"@
function New-Object {
    param([string]$TypeName, [object[]]$ArgumentList)
    if ($TypeName -eq 'System.IO.StreamWriter') {
        if ($Mode -eq 'construct') { throw [System.IO.IOException]::new('injected claim construct failure') }
        return [FaultingLockWriter]::new($ArgumentList[0], $Mode)
    }
    Microsoft.PowerShell.Utility\New-Object @PSBoundParameters
}
& $ClaimScript -LockPath $LockPath -Owner 'claim-test-owner'
'@ | Set-Content -LiteralPath $runner -Encoding UTF8

    function Invoke-Claim([string]$Path, [string]$Mode) {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $output = & (Join-Path $PSHOME 'powershell.exe') -NoProfile -ExecutionPolicy Bypass -File $runner -ClaimScript $ClaimScript -LockPath $Path -Mode $Mode 2>&1
            $rc = $LASTEXITCODE
        } finally { $ErrorActionPreference = $previous }
        return [pscustomobject]@{ Code = $rc; Text = ($output | Out-String) }
    }

    $good = Join-Path $scratch 'normal.lock'
    $result = Invoke-Claim $good 'none'
    Assert-Case ($result.Code -eq 0) "normal claim exits 0: $($result.Text)"
    Assert-Case (Test-Path -LiteralPath $good) 'normal claim creates a lock'
    if (Test-Path -LiteralPath $good) {
        $payload = Get-Content -Raw -LiteralPath $good | ConvertFrom-Json
        Assert-Case ($payload.owner -eq 'claim-test-owner' -and $payload.pid -gt 0) 'normal claim writes owner and PID'
    }

    $foreign = Join-Path $scratch 'foreign[1].lock'
    $bytes = [System.Text.Encoding]::UTF8.GetBytes('{"owner":"another-team","pid":12345}')
    [System.IO.File]::WriteAllBytes($foreign, $bytes)
    $result = Invoke-Claim $foreign 'write'
    Assert-Case ($result.Code -eq 1) 'existing lock refuses with exit 1'
    Assert-Case ($result.Text.Contains('lock already')) 'existing lock reports contention'
    Assert-Case ($result.Text.Contains('another-team')) 'existing lock reports current owner text'
    Assert-Case ([Convert]::ToBase64String([System.IO.File]::ReadAllBytes($foreign)) -eq [Convert]::ToBase64String($bytes)) 'existing lock bytes are unchanged'

    foreach ($mode in @('construct', 'write', 'flush', 'dispose')) {
        $path = Join-Path $scratch ("fault-$mode.lock")
        $result = Invoke-Claim $path $mode
        Assert-Case ($result.Code -eq 1) "$mode failure exits 1: $($result.Text)"
        Assert-Case ($result.Text.Contains("injected claim $mode failure")) "$mode failure preserves its diagnostic: $($result.Text)"
        Assert-Case (-not $result.Text.Contains('lock already exists')) "$mode failure is not reported as contention"
        Assert-Case (-not (Test-Path -LiteralPath $path)) "$mode failure removes the newly created lock"
        if (-not (Test-Path -LiteralPath $path)) {
            # A fresh exclusive open must succeed after cleanup: no leaked handle.
            $stream = [System.IO.File]::Open($path, 'CreateNew', 'Write', 'None')
            $stream.Dispose()
            Remove-Item -LiteralPath $path
        }
    }
} finally {
    # This is the unique directory created above, never a shared RPCS3 lock.
    $resolved = [System.IO.Path]::GetFullPath($scratch)
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing cleanup outside test temp directory' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
if ($failures) { throw "rpcs3-claim: $failures failed assertions" }
Write-Host 'rpcs3-claim: PASS (normal claim, existing lock, constructor/write/flush/dispose failures)'
