# Installed-copy map (t_cab4a49d)

Every artifact the SDK build installs that **replaces** a copy the vendored
PSL1GHT also provides.  Where the vendored PSL1GHT would have written the
same path, it does not reach — our build overwrites it (via `build-sdk.sh`
running after `install-psl1ght.sh`) or uses the `install-psl1ght.sh` `owned`
exclusion list to skip the vendored version in the first place.

> **Scope note.**  The original card names five categories: `librt.a`,
> `librsx.a`, `libgcm_cmd.a`, the overriding headers, and sprxlinker.  This
> page is the canonical map for the four artifact-class rows above; the
> remaining categories (stub symlinks like `libsysutil.a → libsysutil_stub.a`,
> `libnet.a → libnet_stub.a`, and `bin/sprxlinker`) have their own per-site
> `say "(replaces PSL1GHT's)"` logs and can be extended into this table as
> `ps3tc_replacing` call sites are added for them.

## Table

| Installed path (relative to `$PS3DK`) | Our source | Builder | Vendored PSL1GHT copy at that path? |
|---------------------------------------|-----------|---------|-------------------------------------|
| `ppu/lib/librt.a`          | `runtime/lv2/librt` | `scripts/build-runtime-lv2.sh` | yes — PSL1GHT ships its own `librt.a` |
| `ppu/lib/lp64/librt.a`     | `runtime/lv2/librt` | `scripts/build-runtime-lv2.sh` | yes — LP64 variant |
| `ppu/lib/librsx.a`         | `sdk/librsx`    | `sdk/librsx/Makefile`            | yes — PSL1GHT ships `librsx.a` |
| `ppu/lib/lp64/librsx.a`    | `sdk/librsx`    | `sdk/librsx/Makefile`            | yes — LP64 variant |
| `ppu/lib/libgcm_cmd.a`     | `sdk/libgcm_cmd` | `sdk/libgcm_cmd/Makefile`        | yes — PSL1GHT ships `libgcm_cmd.a` |
| `ppu/lib/lp64/libgcm_cmd.a`| `sdk/libgcm_cmd` | `sdk/libgcm_cmd/Makefile`        | yes — LP64 variant |
| `ppu/include/rsx/rsx_function_macros.h` | `sdk/include/rsx` | `sdk/Makefile install-headers` | yes — PSL1GHT ships the same-named header |
| `ppu/include/rsx/gcm_sys.h` | `sdk/include/rsx` | `sdk/Makefile install-headers`  | yes — PSL1GHT ships `rsx/gcm_sys.h` |
| `ppu/include/net/socket.h` | `sdk/include/net` | `sdk/Makefile install-headers`  | yes — PSL1GHT ships `net/socket.h` |

## How to verify

```bash
# From the SDK source root:

# 1.  Which builder wrote a given installed path?
scripts/which-copy.sh ppu/lib/librsx.a
scripts/which-copy.sh librt.a            # bare filename also works
scripts/which-copy.sh --list             # dump the whole table

# 2.  Does the build log contain all the expected REPLACING lines?
#     The emitted line is:  REPLACING <path> with <source> (vendored copy at <path> is NOT installed)
grep  'REPLACING ppu/lib/librt.a with runtime/lv2/librt ' build.log
grep  'REPLACING ppu/lib/lp64/librt.a with runtime/lv2/librt ' build.log
grep  'REPLACING ppu/lib/librsx.a with sdk/librsx ' build.log
grep  'REPLACING ppu/lib/lp64/librsx.a with sdk/librsx ' build.log
grep  'REPLACING ppu/lib/libgcm_cmd.a with sdk/libgcm_cmd ' build.log
grep  'REPLACING ppu/lib/lp64/libgcm_cmd.a with sdk/libgcm_cmd ' build.log
grep  'REPLACING ppu/include/rsx/rsx_function_macros.h with sdk/include/rsx ' build.log
grep  'REPLACING ppu/include/rsx/gcm_sys.h with sdk/include/rsx ' build.log
grep  'REPLACING ppu/include/net/socket.h with sdk/include/net ' build.log

# 3.  Cross-check: manifest / per-site call sites / docs all list the same
#     installed-path strings:
scripts/installed-copy-map-consistency-test.sh   # (tests/)
```

## Source of truth

The authoritative set is `scripts/installed-copy-map.tsv` (tab-separated,
three columns: `installed_path<TAB>source<TAB>builder`).  The per-site
`ps3tc_replacing` calls in the build scripts and the table above must
reference the same installed-path strings as that manifest.  A regression
that drifts the strings is caught by
`tests/sdk/installed-copy-map-consistency-test.sh`.
