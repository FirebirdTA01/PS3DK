"""ABI fixture comparator.

Compares an object manifest against a reference fixture manifest on the
strict conformance contract:

- ELF identity (class, endian, machine, flags)
- ``.opd`` (when the role is ``crt0``): entry_size, addralign, entsize,
  entries, ``off_slot_relocs``, and for each descriptor in order:
  head/tail slot count + reloc type, and for the tail — the symbol and
  addend as well (spec §2 rule 4).  The head's symbol is **informative**
  and intentionally NOT compared (spec §2 rule 3).
- ``.sys_proc_prx_param`` (when the role is ``sprx``): size, magic,
  version, ``abi_version``, ``pointer_slots``, and for each of the six
  required offsets — the reloc count + type + addend (spec §3 rule 2 —
  the symbol name is **informative**).  Any reloc outside the six
  required offsets is reported as unexpected and fails the comparison.

Used by ``scripts/abi-conformance.sh`` from the section-7 7.2 gate and
by ``tests/abi/abifx-comparator-test.py``.

Design notes (per card t_47df3504, codex findings 3 & 5):
- The old implementation collapsed a descriptor table into ``set(...)``
  and called ``sorted(...)`` on it.  That let two objects with a
  duplicate descriptor pass (set dedupes) and crashed with ``TypeError``
  when one symbol was a string and the other was ``None`` (sorting
  mixed-type tuples).  This module compares **index-by-index** over the
  descriptor list — count, layout, and addend are strict; the head
  symbol is explicitly excluded from the comparison so a rename of a
  non-contract name cannot fail the gate.
"""
from __future__ import annotations

import json
import sys
from typing import Any, Dict, List, Optional, Tuple


def _get(d: Any, *path: str) -> Any:
    cur = d
    for k in path:
        if not isinstance(cur, dict) or k not in cur:
            return None
        cur = cur[k]
    return cur


def _reloc_tuple(rs: List[Dict[str, Any]], want_symbol: bool, want_addend: bool) -> Tuple[Tuple[Any, ...], ...]:
    out = []
    for r in rs:
        t: List[Any] = [r.get("r_type")]
        if want_symbol:
            t.append(r.get("symbol"))
        if want_addend:
            t.append(r.get("addend"))
        out.append(tuple(t))
    return tuple(out)


def opd_contract(man: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    o = _get(man, "opd")
    if o is None:
        return None
    descs = o.get("descriptors") or []
    return {
        "entry_size": o.get("entry_size"),
        "entries": o.get("entries"),
        "addralign": o.get("addralign"),
        "entsize": o.get("entsize"),
        "off_slot": tuple(
            (r.get("offset"), r.get("r_type"), r.get("addend"))
            for r in (o.get("off_slot_relocs") or [])
        ),
        "descriptors": [
            {
                "offset": d.get("offset"),
                "head_n": len(d.get("head_relocs") or []),
                "head_types": tuple(r.get("r_type") for r in (d.get("head_relocs") or [])),
                # head symbol is INFORMATIVe — not compared
                "tail_n": len(d.get("tail_relocs") or []),
                "tail_types": tuple(r.get("r_type") for r in (d.get("tail_relocs") or [])),
                "tail_symbols": tuple(r.get("symbol") for r in (d.get("tail_relocs") or [])),
                "tail_addends": tuple(r.get("addend") for r in (d.get("tail_relocs") or [])),
            }
            for d in descs
        ],
    }


def prx_contract(man: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    p = _get(man, "sys_proc_prx_param")
    if p is None:
        return None
    required = (0x10, 0x14, 0x18, 0x1c, 0x24, 0x28)
    by_offset: Dict[int, List[Dict[str, Any]]] = {}
    for r in (p.get("relocs") or []):
        by_offset.setdefault(r.get("offset"), []).append(r)
    slots = []
    for off in required:
        rs = by_offset.get(off) or []
        slots.append({
            "n": len(rs),
            "types": tuple(r.get("r_type") for r in rs),
            "addends": tuple(r.get("addend") for r in rs),
            # symbol is INFORMATIVe — not compared
        })
    unexpected = tuple(
        (off, r.get("r_type"))
        for off in sorted(by_offset)
        if off not in required
        for r in by_offset[off]
    )
    return {
        "size": p.get("size"),
        "magic": p.get("magic"),
        "version": p.get("version"),
        "abi_version": p.get("abi_version"),
        "pointer_slots": tuple(p.get("pointer_slots") or ()),
        "slots": slots,
        "unexpected_relocs": unexpected,
    }


def identity_contract(man: Dict[str, Any]) -> Tuple[Any, ...]:
    return tuple(_get(man, "elf", f) for f in ("class", "endian", "machine", "flags"))


def compare(obj: Dict[str, Any], fix: Dict[str, Any], role: str) -> Tuple[bool, List[str]]:
    """Compare two manifests on the strict contract.

    Returns ``(ok, lines)`` where ``lines`` is a list of human-readable
    ``[ok]`` / ``[FAIL]`` prefixed strings.
    """
    fail: List[str] = []

    a_id, b_id = identity_contract(obj), identity_contract(fix)
    if a_id == b_id:
        pass
    else:
        fail.append(f"  [FAIL] elf identity: object={a_id!r} fixture={b_id!r}")

    if role == "crt0":
        a, b = opd_contract(obj), opd_contract(fix)
        if a is None and b is None:
            fail.append("  [FAIL] .opd absent in both object and fixture (unexpected)")
        elif a == b:
            pass
        else:
            fail.append(f"  [FAIL] .opd: object={a!r}")
            fail.append(f"         fixture={b!r}")
    elif role == "sprx":
        a, b = prx_contract(obj), prx_contract(fix)
        if a is None and b is None:
            fail.append("  [FAIL] .sys_proc_prx_param absent in both (unexpected)")
        elif a == b:
            pass
        else:
            fail.append(f"  [FAIL] .sys_proc_prx_param: object={a!r}")
            fail.append(f"         fixture={b!r}")
    else:
        fail.append(f"  [FAIL] unknown role {role!r} (expected 'crt0' or 'sprx')")

    ok = not fail
    lines = []
    if ok:
        lines.append(f"  [ok]   {role}: elf identity + {role} contract match")
    lines.extend(fail)
    return (ok, lines)


def _cli(argv: List[str]) -> int:
    if len(argv) != 5 or argv[1] != "compare":
        print(
            "usage: python3 abifx.py compare OBJ_JSON FIXTURE_JSON ROLE (crt0|sprx)",
            file=sys.stderr,
        )
        return 2
    with open(argv[2]) as f:
        obj = json.load(f)
    with open(argv[3]) as f:
        fix = json.load(f)
    ok, lines = compare(obj, fix, argv[4])
    for line in lines:
        print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(_cli(sys.argv))
