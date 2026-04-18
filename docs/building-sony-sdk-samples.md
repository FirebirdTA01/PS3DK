# Building unmodified Sony SDK samples against our toolchain

Procedure for taking a sample tree out of the reference Sony SDK
(`~/cell-sdk/475.001/cell/samples/...`) and compiling + running it
unmodified against **our** toolchain, SDK headers, and emulator
(RPCS3).  Useful for:

- Validating our SDK surface — if Sony's own teaching samples work
  against it, we have reasonable confidence in the API shape, ABI, and
  ucode paths.
- Shaking out missing cellGcm / cellSysutil / cellSnavi etc. stubs
  that real-world code exercises.
- Reproducing graphics bugs against a known-good vendor reference.

**Never commit Sony's sources.**  Build trees live under `/tmp/` or
anywhere outside the repo (see `reference/REFERENCE_POLICY.md`).

## Prerequisites

- Toolchain built and installed under `stage/ps3dev/` (source
  `scripts/env.sh` to set `$PSL1GHT` / `$PS3DEV` / `$PATH`).
- `wine` + the reference Sony SDK mounted so sce-cgc.exe is runnable.
- RPCS3 configured for development (Vulkan or OpenGL renderer, GDB
  server enabled at 127.0.0.1:2345 if debugging).

## One-shot build setup

For a single sample (example: `samples/sdk/graphics/gcm/basic`):

```bash
# 1. Copy the sample into a scratch dir outside the repo.
mkdir -p /tmp/sony-basic-build
cp -r ~/cell-sdk/475.001/cell/samples/sdk/graphics/gcm/basic/* \
      /tmp/sony-basic-build/

# 2. Write a local Makefile that uses our ppu_rules.  Template in the
#    next section — tailor SOURCES / CFLAGS / VPO-FPO file names for
#    the specific sample.
$EDITOR /tmp/sony-basic-build/Makefile

# 3. Build + produce the .self.
source PS3_Custom_Toolchain/scripts/env.sh
cd /tmp/sony-basic-build
make
```

Output: `/tmp/sony-basic-build/sony-basic.self`.

## Makefile template

```make
# /tmp/<sample>-build/Makefile

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT not set — source PS3_Custom_Toolchain/scripts/env.sh")
endif
include $(PSL1GHT)/ppu_rules

TARGET    := <sample-name>
BUILD     := build
SOURCES   := .
DATA      :=
SHADERS   := .
INCLUDES  := .

TITLE     := <Sony sample name> on PS3TC
APPID     := SAMPLEID
CONTENTID := UP0001-$(APPID)_00-0000000000000000

CFLAGS    = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
# Enable bridge trace to debug Cg → RSX binding issues:
# CFLAGS   += -DPS3TC_GCM_CG_BRIDGE_TRACE=1
CXXFLAGS  = $(CFLAGS) -fpermissive -fno-exceptions -fno-rtti
LDFLAGS   = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS      := -lgcm_cmd -lrsx -lgcm_sys -lio -lsysutil -lsysmodule -lrt -llv2 -lm

# sce-cgc via wine — produces Sony-format .vpo / .fpo blobs.
WINE       ?= wine
SCE_CGC    ?= $(HOME)/cell-sdk/475.001/cell/host-win32/Cg/bin/sce-cgc.exe

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(SHADERS),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
VCGFILES  := vpshader.cg         # adjust to match sample
FCGFILES  := fpshader.cg
VPOFILES  := vpshader.vpo
FPOFILES  := fpshader.fpo

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES := $(addsuffix .o,$(VPOFILES)) \
                 $(addsuffix .o,$(FPOFILES)) \
                 $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(LIBPSL1GHT_INC) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).fake.self \
	        vpshader.vpo fpshader.vpo

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

# `objcopy -I binary` produces _binary_<file>_start / _end / _size
# symbols — Sony's main.cpp references those via `extern … _binary_
# vpshader_vpo_start`.
%.vpo.o: %.vpo
	@echo $(notdir $<)
	@powerpc64-ps3-elf-objcopy \
	    -I binary -O elf64-powerpc -B powerpc \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(notdir $<) $@

%.fpo.o: %.fpo
	@echo $(notdir $<)
	@powerpc64-ps3-elf-objcopy \
	    -I binary -O elf64-powerpc -B powerpc \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(notdir $<) $@

%.vpo: %.cg
	@echo sce-cgc vp: $(notdir $<)
	@WINEDEBUG=-all $(WINE) $(SCE_CGC) --quiet --profile sce_vp_rsx -o $@ $< \
	    2>&1 | grep -vE "libEGL|pci id|^$$" || true

%.fpo: %.cg
	@echo sce-cgc fp: $(notdir $<)
	@WINEDEBUG=-all $(WINE) $(SCE_CGC) --quiet --profile sce_fp_rsx -o $@ $< \
	    2>&1 | grep -vE "libEGL|pci id|^$$" || true

-include $(DEPENDS)

endif
```

## Running in RPCS3

1. `File → Boot SELF/ELF`, pick the built `.self`.
2. Grey screen / immediate exit almost always means a missing stub or
   bad forwarder.  Check `~/.cache/rpcs3/RPCS3.log` — the last
   `sys_tty_write` calls are the app's own `printf`s, and RPCS3's own
   error lines (`·E` / `·F`) flag unimplemented syscalls or unmapped
   memory accesses.
3. If the app visibly runs, RPCS3's `Tools → Debugger` populates the
   PPU debugger once execution starts.  If it freezes, RPCS3 pauses
   emulation and writes a `Segfault ... at 00007f...` line.

## Debugging a crash with gdb

RPCS3's GDB server (default 127.0.0.1:2345) speaks the standard
remote-gdb protocol.  System `gdb` works if you tell it the PPC
architecture:

```bash
cat > /tmp/probe.gdb <<'EOF'
set architecture powerpc:common64
set endian big
set confirm off
file /tmp/<sample>-build/build/<sample>.elf
target remote 127.0.0.1:2345

echo \n=== GP REGISTERS ===\n
info registers r0 r1 r3 r4 r5 r6 r7 r8 r9 r10
info registers r11 r12 r24 r25 r26 r27 r28 r29 r30 r31
info registers pc lr ctr cr

echo \n=== BACKTRACE ===\n
bt 20

echo \n=== DISASM AROUND PC ===\n
disassemble $pc-0x20,$pc+0x10

echo \n=== MEMORY AT KEY REGS ===\n
x/8wx $r11
x/8wx $r28
x/8wx $r3

detach
quit
EOF

# 1. Let the sample crash in RPCS3 (leaves emulation paused).
# 2. Capture:
gdb -x /tmp/probe.gdb 2>&1 | tee /tmp/crash-dump.txt
```

Skip the automatic register dump's vector registers — older RPCS3 GDB
stub replies `E01` for VSCR / FPSCR / VSX, and gdb aborts the script
when that happens.  The script above lists only the GPRs / PC / LR /
CTR / CR which the stub reliably supplies.

## Bridge-trace mode

If graphics commands aren't landing right, enable the Cg→RSX bridge
trace in the sample's Makefile:

```make
CFLAGS += -DPS3TC_GCM_CG_BRIDGE_TRACE=1
```

Rebuild; all `cellGcmSetVertexProgram` / `SetFragmentProgram` /
`SetVertexProgramParameter` calls then print their ucode bytes,
parameter table, masks, and FP_CONTROL value via `printf` (visible as
`[cg_bridge] …` lines in RPCS3's TTY log at `~/.cache/rpcs3/TTY.log`).
Useful for byte-comparing against `wine sce-cgcdisasm.exe foo.vpo`.

Disable it for production — the printf+fflush per call is noisy and
adds real stdio work every frame.

## Known gotchas

- **`_binary_...` symbol names.** `objcopy -I binary` names the symbols
  after the FILE NAME (not the variable), dashes become underscores.
  `vpshader.vpo` → `_binary_vpshader_vpo_start`.  Sony's sample source
  expects that exact name; don't rename the .vpo file.
- **FP_CONTROL output-register flag.** R0 output needs 0x40 in the low
  byte; H0 needs 0x0e.  Our bridge reads this from the CgBinaryFragment
  Program.outputFromH0 flag — if a sample renders solid black, suspect
  this bit got dropped.
- **Sony-SDK shader-name conventions.**  Samples tend to name their
  shaders `vpshader.cg` / `fpshader.cg`; stick with that so the
  Makefile template matches.
- **PRX OPD calls.**  PSL1GHT's `ATTRIBUTE_PRXPTR` fields (including
  `CellGcmContextData::callback`) are 32-bit OPD handles, not full
  function pointers.  Invoke via `lwz 0,0(cb); lwz 2,4(cb); mtctr 0;
  bctrl` or via a `noinline` helper — direct C `cb(args)` on a PRX
  handle lands on garbage.
- **`sce-cgc` shader profiles.**  `sce_vp_rsx` for vertex, `sce_fp_rsx`
  for fragment.  The compiler under wine is silent on success; errors
  print to stderr.  Always check the produced `.vpo` / `.fpo` size is
  > 0 before linking.

## Related

- `tools/rsx-cg-compiler/docs/REVERSE_ENGINEERING.md` — sce-cgc binary
  format reverse-engineering log.
- `sdk/include/cell/gcm/gcm_cg_bridge.h` — the Cg→RSX bridge.
- `samples/hello-ppu-cellgcm-triangle/` — our own clean-room triangle
  sample, useful as a known-good reference when comparing behavior.
