# ps3dk-debug-rpcs3.ps1 -- boot a .fake.self in RPCS3 ready for GDB and attach.
#
#   ps3dk-debug-rpcs3.ps1 -Self app.fake.self -GdbInit app.gdbinit
#                         [-GdbServer 127.0.0.1:2345] [-Rpcs3 path\rpcs3.exe]
#                         [-Gdb path\powerpc64-ps3-elf-gdb.exe] [-NoGdb]
#                         [-TimeoutSeconds 60]
#
# Normally run through the `debug-<target>` target that ps3_add_debug_rpcs3()
# adds. RPCS3 is started with a per-run copy of its configuration in which
# only two settings differ: the PPU decoder is the interpreter (RPCS3 ignores
# breakpoints under the LLVM recompiler), and the GDB server listens on
# -GdbServer. A marker beside the SELF enables the CMake helper's pre-main
# wait until GDB attaches. The shared RPCS3 configuration is never written.
#
# -Rpcs3 defaults to $env:PS3DK_RPCS3. -Gdb defaults to the SDK's
# powerpc64-ps3-elf-gdb.exe; -NoGdb only boots RPCS3 and prints the GDB
# command to run, for a GDB on another host (e.g. inside WSL).
# RPCS3 has no detach. The launcher closes its RPCS3 process when local GDB exits.
param(
    [Parameter(Mandatory=$true)][string]$Self,
    [Parameter(Mandatory=$true)][string]$GdbInit,
    [string]$GdbServer = '127.0.0.1:2345',
    [string]$Rpcs3 = $env:PS3DK_RPCS3,
    [string]$Gdb = '',
    [switch]$NoGdb,
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'

function Fail([string]$msg) { Write-Host "ps3dk-debug-rpcs3: $msg"; exit 2 }

if (-not (Test-Path -LiteralPath $Self -PathType Leaf)) { Fail "no SELF at '$Self' (build the target first)" }
if (-not (Test-Path -LiteralPath $GdbInit -PathType Leaf)) { Fail "no GDB command file at '$GdbInit'" }
if ($GdbServer -notmatch '^\d+\.\d+\.\d+\.\d+:\d+$') { Fail "-GdbServer must be IPv4 address:port, got '$GdbServer'" }
if (-not $Rpcs3) { Fail "set -Rpcs3 or PS3DK_RPCS3 to rpcs3.exe" }
if (-not (Test-Path -LiteralPath $Rpcs3 -PathType Leaf)) { Fail "no rpcs3.exe at '$Rpcs3'" }

if (-not $NoGdb) {
    if (-not $Gdb) {
        $root = if ($env:PS3DK) { $env:PS3DK } else { $env:PS3DEV }
        if ($root) { $Gdb = Join-Path $root 'ppu\bin\powerpc64-ps3-elf-gdb.exe' }
    }
    if (-not $Gdb -or -not (Test-Path -LiteralPath $Gdb -PathType Leaf)) {
        Fail "no powerpc64-ps3-elf-gdb.exe found (set -Gdb, or use -NoGdb and attach from another GDB)"
    }
}

$rpcs3Dir = Split-Path -Parent (Resolve-Path -LiteralPath $Rpcs3)
$sharedCfg = Join-Path $rpcs3Dir 'config\config.yml'
if (-not (Test-Path -LiteralPath $sharedCfg -PathType Leaf)) { Fail "no RPCS3 configuration at '$sharedCfg' (start RPCS3 once first)" }
$lines = Get-Content -LiteralPath $sharedCfg
$decoder = 0; $server = 0
$out = foreach ($l in $lines) {
    if ($l -match '^(\s+)PPU Decoder:') { $decoder++; "$($Matches[1])PPU Decoder: Interpreter (static)" }
    elseif ($l -match '^(\s+)GDB Server:') { $server++; "$($Matches[1])GDB Server: $GdbServer" }
    else { $l }
}
if ($decoder -ne 1 -or $server -ne 1) {
    Fail "unexpected RPCS3 configuration layout ($decoder 'PPU Decoder', $server 'GDB Server' lines; expected one each)"
}
$runCfg = Join-Path ([System.IO.Path]::GetTempPath()) ("ps3dk-debug-rpcs3-{0}.yml" -f [guid]::NewGuid())
$utf8 = New-Object System.Text.UTF8Encoding($false)
$selfPath = (Resolve-Path -LiteralPath $Self).Path
$waitMarker = Join-Path (Split-Path -Parent $selfPath) 'ps3dk-debug-wait'
$ownsMarker = $false
$proc = $null
$log = Join-Path $rpcs3Dir 'log\RPCS3.log'
try {
    # CreateNew preserves an existing session's marker, including on failures.
    try {
        $markerFile = [System.IO.File]::Open($waitMarker, [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    } catch [System.IO.IOException] {
        Fail "cannot create wait marker '$waitMarker'. Remove an existing marker only after confirming no debug session is running. $($_.Exception.Message)"
    }
    $ownsMarker = $true
    $markerFile.Dispose()
    [System.IO.File]::WriteAllLines($runCfg, [string[]]$out, $utf8)
    $started = Get-Date
    try {
        $proc = Start-Process -FilePath $Rpcs3 -ArgumentList @('--no-gui', '--config', "`"$runCfg`"", "`"$selfPath`"") -WindowStyle Hidden -PassThru
    } catch {
        Fail "cannot start RPCS3 '$Rpcs3': $($_.Exception.Message)"
    }
    $ready = $false
    $deadline = $started.AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
        # Windows can defer LastWriteTime while RPCS3 holds its log open.
        # The unique configuration path in argv binds this log to our launch.
        if (Test-Path -LiteralPath $log) {
            # RPCS3 opens the log with delete access, so readers must share it.
            $text = $null
            $reader = $null
            try {
                $stream = [IO.File]::Open($log, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                    ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                $reader = New-Object IO.StreamReader($stream)
                $text = $reader.ReadToEnd()
            } catch [IO.IOException] {
                # The log may be replaced during startup; retry until the deadline.
            } finally {
                if ($reader) { $reader.Dispose() }
            }
            if ($text -and $text.Contains($runCfg) -and $text.Contains("GDB: Started listening on '$GdbServer'")) { $ready = $true; break }
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $ready) {
        if ($proc.HasExited) { Fail "RPCS3 exited (code $($proc.ExitCode)) before its GDB server started; see $log" }
        Fail "RPCS3's GDB server did not report listening on $GdbServer within $TimeoutSeconds s; see $log"
    }
    if ($NoGdb) {
        Write-Host "ps3dk-debug-rpcs3: RPCS3 is waiting for GDB on $GdbServer."
        Write-Host "  attach with: powerpc64-ps3-elf-gdb -x `"$GdbInit`""
        Write-Host "  Press Ctrl-C in this launcher terminal to end the session and close RPCS3."
        # Yield to PowerShell so Ctrl-C reaches finally even while RPCS3 is idle.
        while (-not $proc.HasExited) {
            Start-Sleep -Milliseconds 250
            $proc.Refresh()
        }
        exit $proc.ExitCode
    }
    & $Gdb -x $GdbInit
    exit $LASTEXITCODE
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue }
    if ($ownsMarker) { Remove-Item -LiteralPath $waitMarker -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $runCfg -ErrorAction SilentlyContinue
}
