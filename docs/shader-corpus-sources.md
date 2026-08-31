# Shader Corpus Sources

This table records the external Cg shader sources used by
`scripts/fetch-shader-corpus.sh`.  The script is the machine-readable source of
truth; update it and this report together.

Public artifacts must not name private tools or paths.  The corpus is only
test input: `fetch-run-only` sources are downloaded into the ignored
`build/shader-corpus/` tree and are never committed.

| Source | Pinned ref | Path | License | Usage | Count |
|---|---|---|---|---|---:|
| PS3 OpenGraphics Toolkit | `77532781e45f0d7734edf094e33d03fbc97c7024` | `testsuite/shaders` | GPL-2.0 | `fetch-run-only` | 87 tracked files: 86 `.fcg`, 1 `.vcg` |
| IoQuake3-PS3 | `4725af4aa4c179e516010eebcf32b75428c0ae40` | `code/gl/shaders` | GPL-2.0 | `fetch-run-only` | 8 tracked files: 7 `.fcg`, 1 `.vcg` |
| th06_ps3 | `a58159b9928254bc9a59fd8028c929f12c86b02c` | `ps3/shaders` | GPL-3.0 | `fetch-run-only` | 11 tracked files: 10 `.fcg`, 1 `.vcg` |
| ClassiCube | `d7ee58bdbccb1f9f72750bdf79f96550dfdd3ea0` | `misc/ps3` | BSD-3-Clause-style file with GitHub `NOASSERTION` | `fetch-run-only` | 5 tracked files: 2 `.fcg`, 3 `.vcg` |
| crystalct/PSL1GHT | `f7eda8960670cf67a63fcc22e11c9b4e485b6e9d` | `samples/graphics` | MIT | `vendor-eligible` | 14 tracked files, 6 distinct shader blobs: 7 `.fcg`, 7 `.vcg` |
| RSXGL | `835ecd3b39b0fc96bd09a31b5fe1e93c090bf5f3` | `src/samples/rsxgltest` | BSD-2-Clause-style | `vendor-eligible` | 10 tracked files: 5 `.fcg`, 5 `.vcg` |

Current totals from the script metadata:

| Usage | Sources | Tracked files | Distinct shader blobs |
|---|---:|---:|---:|
| `fetch-run-only` | 4 | 111 | 111 |
| `vendor-eligible` | 2 | 24 | 16 |
| Total | 6 | 135 | 127 |

Notes:

- ClassiCube stays `fetch-run-only` unless the director explicitly accepts the
  repository metadata discrepancy for vendoring, or upstream resolves it.
- `vendor-eligible` means the license permits future committed corpus import
  after per-file verification and notice preservation; it does not mean the
  source has been vendored here.
- xash3d-fwgs is a validation target, not a corpus source: current measured
  count is zero `.cg`/`.fcg`/`.vcg` files.
- The SM64 PS3 port and Demon's Souls shader-modding repo were found in a broad
  pass but are not in this acceptance set.  The SM64 port needs a reliable count
  and policy decision because its repository license metadata is null in a game
  decompilation fork.  The Demon's Souls repo contains one MIT `.fcg` shader but
  is a game-specific modding workflow; include it only if the corpus policy is
  widened beyond PSL1GHT ecosystem homebrew shaders.
