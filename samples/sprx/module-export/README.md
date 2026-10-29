# sample-sprx-module-export

Demonstrates CellOS PRX/SPRX module creation and loading using inline source declarations:
- `<sys/moduleexport.h>`: `SYS_MODULE_INFO`, `SYS_MODULE_START`, `SYS_MODULE_STOP`.
- Automatic identity extraction by `prx-gen build` from the non-loaded `.ps3dk.module_identity` section.
- Function export definitions via `exports.S` and `exports.yml`, validated with `ps3_prx_export_roundtrip`.
- Client stub archive generation via `ps3_prx_stub_library`.
- Host loader application exercising runtime load, start handler, export call, stop handler, and unload.

All build artifacts (`.prx`, `.sprx`, `.fake.sprx`, `.self`, `.fake.self`) are written under `CMAKE_CURRENT_BINARY_DIR`, keeping the source directory untouched.
