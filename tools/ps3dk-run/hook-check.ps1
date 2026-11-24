# hook-check.ps1 <build dir> <SELF target> <main object, relative to CMakeFiles/<target>.dir> [<module target>]
# Does a built SELF carry the ps3dk-run hook?  For a Ninja build on Windows; hook-check.py is the same check for any
# generator and host (and the copy CI exercises).  A SELF is compressed and its ELF usually stripped, so the build is
# checked: (1) the target's link line holds ps3dk_run_hook.c.obj and its hook_main wrapper; (2) nm: ps3dk_run_hook_start
# defined, the wrapper's main calls ps3dk_sample_main, the target's main object defines ps3dk_sample_main (PPC64
# function symbols are .opd descriptors, nm type D or T; C++ names are mangled); (3) the ELF holds "ps3dk-run-watchdog"
# and "ps3dk-run="; (4) a named module has neither string and no hook object on its link line; (5) the build was
# configured with run-hook-inject.cmake and the SELF is not older than its ELF.  Prints the SELF sha256; exit 0 only
# when every check holds.  nm: $env:PS3DEV/ppu/bin/powerpc64-ps3-elf-nm.exe, or $env:PS3DK_NM.
$ErrorActionPreference = 'Stop'
if ($args.Count -lt 3) { 'usage: hook-check.ps1 <build dir> <SELF target> <main object rel path> [<module target>]'; exit 2 }
$b, $t, $mainRel = $args[0], $args[1], $args[2]; $mod = if ($args.Count -ge 4) { $args[3] } else { $null }
$nm = if ($env:PS3DK_NM) { $env:PS3DK_NM } elseif ($env:PS3DEV) { "$env:PS3DEV/ppu/bin/powerpc64-ps3-elf-nm.exe" } else { 'powerpc64-ps3-elf-nm' }
$ninja = Get-Content -Raw "$b/build.ninja"
function LinkLine([string]$x) { ([regex]::Match($ninja, "(?m)^build $([regex]::Escape($x))(?: |:)[^\r\n]*")).Value }
$o = "$b/CMakeFiles/$t.dir"
$wrapName = Get-ChildItem "$o/run-hook" -Filter "hook_main_$t.*.obj" -ErrorAction SilentlyContinue | Select-Object -First 1
function Syms([string]$f) { if ($f -and (Test-Path $f)) { & $nm $f | ForEach-Object { $_.Trim() } } else { @() } }
$ll = LinkLine $t
$c1 = $ll.Contains('ps3dk_run_hook.c.obj') -and $wrapName -and $ll.Contains($wrapName.Name)
$hs = Syms "$o/run-hook/hook/ps3dk_run_hook.c.obj"; $ws = Syms $(if ($wrapName) { $wrapName.FullName }); $ms = Syms "$o/$mainRel"
$c2 = ($hs -match ' [TD] ps3dk_run_hook_start$').Count -gt 0 -and ($ws -match ' [TD] main$').Count -gt 0 -and
      ($ws -match '(^| )U (ps3dk_sample_main|_Z17ps3dk_sample_main[A-Za-z0-9_]*)$').Count -gt 0 -and ($ms -match ' [TD] (ps3dk_sample_main|_Z17ps3dk_sample_main[A-Za-z0-9_]*)$').Count -gt 0
$elf = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$b/$t.elf"))
$c3 = $elf.Contains('ps3dk-run-watchdog') -and $elf.Contains('ps3dk-run=')
$c4 = $true
if ($mod) {
    $m = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$b/$mod")); $ml = LinkLine $mod
    $c4 = -not $m.Contains('ps3dk-run-watchdog') -and -not $m.Contains('ps3dk-run=') -and $ml -and -not $ml.Contains('ps3dk_run_hook')
}
$cfg = @(Select-String -Path "$b/CMakeCache.txt" -Pattern '^CMAKE_PROJECT_INCLUDE:[^=]*=.*run-hook-inject\.cmake' | ForEach-Object Line)
$self = Get-Item "$b/$t.self"
$c5 = $cfg.Count -eq 1 -and $self.LastWriteTime -ge (Get-Item "$b/$t.elf").LastWriteTime
"$t link line has the hook and wrapper objects: $c1"
"objects: ps3dk_run_hook_start defined, wrapper main calls ps3dk_sample_main, target main renamed: $c2"
"ELF strings ps3dk-run-watchdog and ps3dk-run=: $c3"
if ($mod) { "module $mod has no hook: $c4" }
"configured with run-hook-inject.cmake, SELF not older than its ELF: $c5"
"self $($self.Name) sha256 $((Get-FileHash -Algorithm SHA256 $self.FullName).Hash.ToLower())"
if ($c1 -and $c2 -and $c3 -and $c4 -and $c5) { 'HOOK PRESENT' + $(if ($mod) { ' (module: ABSENT)' } else { '' }); exit 0 }
'HOOK CHECK FAILED'; exit 1
