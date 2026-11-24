# sample-sprx-vsh-plugin

A module that works as a VSH plugin, loaded into VSH by a plugin loader
(webMAN MOD's `/loadprx.ps3` and `/unloadprx.ps3`, through Cobra) on a CFW or
HEN console, and also through `sys_prx_start_module` / `sys_prx_stop_module`
in a game process. A loader for that second path is included, so the same
plugin can be checked in RPCS3.

## What the plugin does

`module_start` starts one worker thread with `sys_ppu_thread_create` and
ends. `module_stop` tells the worker to stop, joins it and ends. The worker
does the plugin's job:

- it appends a line to `/dev_hdd0/tmp/ps3dk-vsh-plugin.log` and flushes it
  with `fsync`: `start`, then one `tick` line every 250 ms for the first eight
  ticks and every 2 s after that, then `stop`;
- each tick adds one to a counter in `.data` (starting at `0x0da70000`) and
  to one in `.bss` (starting at 0), and the line shows both. A module that
  can write its own data is what the code/data segment layout is for.

A log from a VSH load and unload looks like:

```
ps3dk-vsh-plugin: start caller=bare data=0x0da70000 bss=0
ps3dk-vsh-plugin: tick 1 data=0x0da70001 bss=1
...
ps3dk-vsh-plugin: tick 12 data=0x0da7000c bss=12
ps3dk-vsh-plugin: stop caller=bare ticks=12
```

`caller=` says which path the entry point was reached by (below).

## The two paths

| | Loader path (`sys_prx_start_module` in a game) | Plugin loader path (Cobra in VSH) |
|---|---|---|
| `module_start` / `module_stop` run on | a normal thread with a TLS area and a caller | a thread the kernel makes: no TLS area, no caller, 4 KiB of stack |
| They end by | returning | system call 41, the bare thread exit |
| Final stop report | sent by liblv2 after `module_stop` returns | sent by `module_stop` itself |
| `caller=` in the log | `loader` | `bare` |

Both entry points check which path they are on before doing anything: a
usable caller has a return address that is not 0, is word-aligned and is
not the plugin loader's poison value `0xBADADD00`, and a TLS area (r13 at
least `0x7000`). Either missing is the plugin loader path. The check reads
two registers and no memory, so it is safe on a thread with no TLS.

**Why the bare exit.** On the plugin loader's thread, returning from
`module_start` jumps to the poison return address and faults. liblv2's
`sys_ppu_thread_exit`, which PSL1GHT's `sysThreadExit` calls, reads TLS, and
the thread has none. System call 41 ends the thread and nothing else; its
exit code is the entry point's result (`SYS_PRX_RESIDENT`, `SYS_PRX_STOP_OK`).
The SDK's `sys_ppu_thread_exit` takes the same route on a thread with no TLS
area, but the sample makes the system call itself so that the pattern is
explicit.

**What the stop report does.** A module only unloads once Lv-2 has it in the
stopped state. `sys_prx_stop_module` gets it there in two steps: command 1
(stopping) before `module_stop` runs and command 2 with the result after.
The plugin loader sends only the first, so on that path `module_stop` sends
the second itself: system call 482 on its own module id (from system call
461 with an address inside the module), flags 0, and the option block
`{ 0x28, 2, 0, 0, 0 }` (size, command 2, entry, result 0 = stopped, entry).
Without it the unload waits for ever. On the loader path liblv2 sends that
report itself after `module_stop` returns, so the plugin must not; a second
report would find the module already stopped. (Semantics as RPCS3 models
them in `rpcs3/Emu/Cell/lv2/sys_prx.cpp`, `_sys_prx_stop_module`.)

**Why a worker thread.** The plugin loader's thread has no TLS area and only
4 KiB of stack, so the entry points only start and stop the worker. The
worker is a normal thread with its own 16 KiB stack. Even so, the plugin
writes its log with Lv-2 file system calls and formats its own numbers, so
nothing in it depends on newlib.

## The loader

`vsh-plugin-loader` runs the loader path in a game process, twice: load,
start, wait 1.5 s, stop, unload. It passes report blocks to `module_start`
and `module_stop` through `argp` and checks them, then reads the log back:

```
VSHP begin step=load-1
VSHP try path=/app_home/vsh_plugin.fake.sprx rc=0x...
VSHP PASS step=load-1 id=0x... path=/app_home/vsh_plugin.fake.sprx
VSHP PASS step=start-1 rc=0x00000000 modres=0
VSHP PASS step=start-caller caller=1 want=1
VSHP PASS step=worker-ticks ticks=6 want>=2
VSHP PASS step=data-counter got=0x0da70006 want=0x0da70006
VSHP PASS step=log-stop want="ps3dk-vsh-plugin: stop caller=loader ticks=6"
...
VSHP_ALL_PASS steps=36 abi=ilp32
```

A failed check prints `VSHP FAIL step=<name> ...` and the run ends with
`VSHP_RESULT FAIL failed=<n>`. The second cycle shows that a reload starts
from a fresh image: its `.data` counter is back at the initial value.

The loader looks for the plugin, as `.sprx`, then `.fake.sprx`, then the
unsigned `.prx`, in:

1. the directory given as `argv[1]`, when it is an absolute path;
2. its own directory (the directory of `argv[0]`, when that is absolute);
3. `/dev_hdd0/tmp/ps3dk-vsh-plugin`, a fixed staging directory (copy the
   plugin there over FTP when the launcher's arguments name no directory);
4. `/app_home`.

Each directory is tried once. Before searching, the loader prints the arguments
it was started with: `VSHP argv argc=<n> [0]=... [1]=...`. A child started with
`exitspawn` (the ps3dk-run loader, for example) gets no usable argv, so stage the
plugin in the fixed directory for that launcher.

A `.self` loads the real-signed `.sprx`, a `.fake.self` the `.fake.sprx`,
and on RPCS3 a raw `.elf` boot the `.prx`.

## Build outputs

Both ABIs are built. Everything lands in the build directory:

| ABI | Plugin | Loader |
|---|---|---|
| ILP32 | `vsh_plugin.{prx,sprx,fake.sprx}` | `vsh-plugin-loader.{elf,self,fake.self}` |
| LP64 | `vsh_plugin_lp64.{prx,sprx,fake.sprx}` | `vsh-plugin-loader-lp64.{elf,self,fake.self}` |

The regression row `tests/regression/sprx-vsh-plugin` stages these for
RPCS3; its rows run the ILP32 build (see Known limits).

## Running it

**In VSH.** Copy `vsh_plugin.sprx` to the console (for example to
`/dev_hdd0/tmp/vsh_plugin.sprx`) and load it with webMAN MOD's
`/loadprx.ps3` page, naming that path; unload it with `/unloadprx.ps3`.
After the unload, `/dev_hdd0/tmp/ps3dk-vsh-plugin.log` should end with a
`stop caller=bare` line. Use the real-signed `.sprx`; whether a plugin
loader accepts the `.fake.sprx` depends on the firmware.

**In a game process.** Copy `vsh-plugin-loader.self` and `vsh_plugin.sprx`
(or the `.fake.self` and `.fake.sprx`) into one directory and boot the
loader.

## Known limits

- RPCS3 has no VSH plugin loader, so the plugin loader path runs only on a
  console. The loader rows cover the loader path and the plugin's worker.
- The path check expects the plugin loader's thread to have neither a TLS
  area nor a normal return address. A loader that gave its thread both, and
  still did not finish the stop itself, would not be detected.
- A module has no thread_local or pthread API (it has no TLS of its own).
  `sys_ppu_thread_create` in a module has 16 start records, held only until
  each new thread starts; past that it returns `EAGAIN`.
- VSH plugins are normally ILP32, and the regression rows run the ILP32
  build only. The LP64 build (with `-fno-jump-tables`, see
  `docs/tools/prx-gen.md`) links and passes `prx-gen check`, but on RPCS3 the
  PPU recompiler fails on the LP64 plugin ("Unsupported relocation(1)") and
  the loader hangs after the load; that is not yet understood.

See [docs/tools/prx-gen.md](../../../docs/tools/prx-gen.md#6-vsh-plugins)
for the module side of the pattern.
