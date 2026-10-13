# LV2 virtual memory (sys/vm.h, sys/memory.h)

| Property | Value |
|---|---|
| Headers | `<sys/vm.h>`, `<sys/memory.h>` |
| Syscalls | 300 301 302 303 304 305 306 307 308 309 310 311 312 (sys/vm.h); 343, 351 (sys/memory.h) |
| Imports | `sys_mmapper_allocate_memory`, `sys_mmapper_allocate_memory_from_container`, `sys_mmapper_free_memory`, `sys_mmapper_map_memory`, `sys_mmapper_unmap_memory` (sysPrxForUser) |
| Sample | `samples/lv2/hello-ppu-lv2-vm/` |

## What sys_vm_memory_map is

`sys_vm_memory_map(vsize, psize, container, flag, policy, &addr)` reserves
`vsize` bytes of address space backed by only `psize` bytes of physical
memory taken from `container`.  The kernel pages between the two on demand.
`vsize` may be larger than `psize`: the address space is the full window,
the physical pool is the smaller backing.  `policy` is
`SYS_VM_POLICY_AUTO_RECOMMENDED`.

The remaining calls operate on a range `[addr, addr + size)` inside that
area.

## The range calls

| Call | Syscall | Description |
|---|---|---|
| `sys_vm_memory_map` | 300 | Reserve `vsize` of address space over `psize` of physical memory |
| `sys_vm_unmap` | 301 | Release the reserved area |
| `sys_vm_append_memory` | 302 | Add physical memory to the range |
| `sys_vm_return_memory` | 303 | Return physical memory from the range |
| `sys_vm_lock` | 304 | Lock the pages |
| `sys_vm_unlock` | 305 | Unlock the pages |
| `sys_vm_touch` | 306 | Page in |
| `sys_vm_flush` | 307 | — |
| `sys_vm_invalidate` | 308 | — |
| `sys_vm_store` | 309 | Page out |
| `sys_vm_sync` | 310 | — |

## sys_vm_test

`sys_vm_test(addr, size, &result)` reports the state of each page in the
range as a set of `SYS_VM_STATE_*` bits:

| Constant | Value | Meaning |
|---|---|---|
| `SYS_VM_STATE_INVALID`    | 0x0000 | not a valid state |
| `SYS_VM_STATE_UNUSED`     | 0x0001 | reserved, no physical backing |
| `SYS_VM_STATE_ON_MEMORY`  | 0x0002 | has physical backing |
| `SYS_VM_STATE_STORED`     | 0x0004 | paged out (stored) |

## Statistics

`sys_vm_get_statistics(addr, &stat)` fills `sys_vm_statistics_t`:

| Field | Type | Meaning |
|---|---|---|
| `page_fault_ppu` | `uint64_t` | PPU page faults in the area |
| `page_fault_spu` | `uint64_t` | SPU page faults in the area |
| `page_in`        | `uint64_t` | pages paged in |
| `page_out`       | `uint64_t` | pages paged out |
| `pmem_total`     | `uint32_t` | total physical memory (32-bit, layout-stable) |
| `pmem_used`      | `uint32_t` | physical memory used (32-bit, layout-stable) |
| `time`           | `uint64_t` | not described by the header |

The two `uint32_t` fields are written as 32 bits by the kernel, so the
struct has one layout in both ILP32 and LP64 userland.  RPCS3 reports zero
paging statistics in this structure.

## The mmapper path

The `sys_mmapper_*` calls build and tear down a physical-memory object —
allocate an address range, back it with a memory object, map it, then
release it.  Unlike the `sys_vm_*` functions these five are not inline
syscalls; they are `sysPrxForUser` exports (`liblv2_stub`) and appear in the
object as a call, not a `sc`.

    sys_mmapper_allocate_address
    -> sys_mmapper_allocate_memory
    -> sys_mmapper_map_memory
    -> sys_mmapper_unmap_memory   (writes the object id back)
    -> sys_mmapper_free_memory
    -> sys_mmapper_free_address

| Call | Description |
|---|---|
| `sys_mmapper_allocate_address(size, flags, alignment, &addr)` | reserve an address range |
| `sys_mmapper_allocate_memory(size, flags, &mem_id)` | allocate a memory object |
| `sys_mmapper_allocate_memory_from_container(size, container, flags, &mem_id)` | allocate a memory object from a specific container |
| `sys_mmapper_map_memory(start_addr, mem_id, flags)` | map the object at `start_addr` |
| `sys_mmapper_unmap_memory(start_addr, &mem_id)` | unmap; writes the object's id to `mem_id` |
| `sys_mmapper_free_memory(mem_id)` | free the memory object |
| `sys_mmapper_free_address(start_addr)` | release the address range |

Protection and granularity flags for the `flags` arguments come from
`<sys/memory.h>`: `SYS_MEMORY_PROT_READ_ONLY`, `SYS_MEMORY_PROT_READ_WRITE`
(`SYS_MEMORY_PROT_MASK`), and `SYS_MEMORY_GRANULARITY_1M`,
`SYS_MEMORY_GRANULARITY_64K` (`SYS_MEMORY_GRANULARITY_MASK`).

## Sample

`samples/lv2/hello-ppu-lv2-vm/` — a PPU program exercising `sys_vm_*` and
the `sys_mmapper_*` path.
