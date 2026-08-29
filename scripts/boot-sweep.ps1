param(
    [Parameter(Mandatory=$true)][string]$root,          # extracted package root
    [string]$rpcs3 = "C:\Users\FirebirdTA01\Desktop\Emulators\RPCS3\rpcs3.exe",
    [int]$seconds  = 30,
    [string]$label = "baseline"
)

# Boot a prioritised subset of already-built samples on the DESKTOP RELEASE build
# of RPCS3 and record, per sample and separately:
#   - the emulator's own exit status  (did the process survive?)
#   - the guest's TTY output          (did the program get anywhere?)
#   - a WIDE fatal grep               (not a hand-picked pattern list)
# These are three different questions.  Conflating them is how a hang at frame
# 117 and ENOSYS sockets both read as "fine" for months.
#
# House rules this encodes: desktop release build only, ONE instance, existing
# logs are COPIED to a timestamped folder before clearing (never deleted).

$log  = Join-Path (Split-Path $rpcs3) "log"
$out  = "C:\ps3boot\$label"
$csv  = "$out\boot-results.csv"
New-Item -ItemType Directory -Force $out | Out-Null
"sample,emulator,tty_lines,fatal_hits,first_fatal" | Out-File -Encoding ascii $csv

# Preserve whatever is in log/ right now, once, before the run touches anything.
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$keep  = Join-Path $log "preserved-$stamp"
New-Item -ItemType Directory -Force $keep | Out-Null
Get-ChildItem $log -Filter *.log -EA SilentlyContinue | Copy-Item -Destination $keep -Force

$targets = Get-Content "$out\targets.txt" -EA SilentlyContinue
if (-not $targets) { Write-Host "No $out\targets.txt ??? write one sample name per line."; exit 1 }

foreach ($name in $targets) {
    $name = $name.Trim(); if (-not $name) { continue }
    $self = Get-ChildItem "$root\samples" -Recurse -Filter "$name.fake.self" -EA SilentlyContinue | Select-Object -First 1
    if (-not $self) { "$name,NOT_BUILT,0,0," | Out-File -Encoding ascii -Append $csv; continue }

    Get-ChildItem $log -Filter *.log -EA SilentlyContinue | Remove-Item -Force -EA SilentlyContinue
    $p = Start-Process -FilePath $rpcs3 -ArgumentList "--no-gui","`"$($self.FullName)`"" -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds $seconds
    $survived = -not $p.HasExited
    if ($survived) { Stop-Process -Id $p.Id -Force -EA SilentlyContinue }
    Start-Sleep -Seconds 2

    $sdir = "$out\$name"; New-Item -ItemType Directory -Force $sdir | Out-Null
    Get-ChildItem $log -Filter *.log -EA SilentlyContinue | Copy-Item -Destination $sdir -Force

    $tty = @(Get-Content "$sdir\TTY.log" -EA SilentlyContinue)
    # Wide, not a curated list: the severity marker plus the usual suspects.
    $pat = 'Fatal|Access violation|frozen|Dead FIFO|recover_fifo|runtime_error|ENOSYS'
    $hits = @(Select-String -Path "$sdir\RPCS3.log" -Pattern $pat -EA SilentlyContinue)
    $fatal = @($hits | Where-Object { $_.Line -notmatch 'Show fatal error hints' })
    $first = if ($fatal.Count) { ($fatal[0].Line -replace ',',';').Trim() } else { "" }
    $emu = if ($survived) { "ran_${seconds}s" } else { "exited_early" }
    "$name,$emu,$($tty.Count),$($fatal.Count),$first" | Out-File -Encoding ascii -Append $csv
    Write-Host ("  {0,-28} {1,-13} tty={2,-3} fatal={3}" -f $name,$emu,$tty.Count,$fatal.Count)
}
Write-Host "DONE -> $csv"
