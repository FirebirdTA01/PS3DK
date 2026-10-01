"""Target errno for host builds of librt code.

On the PS3 target the E* names are the LV2 status codes (newlib patch 0018
adds <sys/_lv2_errno.h>).  A host test that compiles real librt sources must
see the same values in every unit, or a code that librt returns (0x8001000D)
will not compare equal to the test's EFAULT.  write_fixture(dir) extracts the
header from the newlib patch itself, so there is no second copy of the table,
and adds an errno.h that layers it over the host's errno.h.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "patches/ppu/newlib-4.x/0018-lv2-errno-values.patch"
NEW_FILE = "+++ b/newlib/libc/include/sys/_lv2_errno.h"


def lv2_errno_header() -> str:
    lines = PATCH.read_text().splitlines()
    start = lines.index(NEW_FILE)
    body = []
    for line in lines[start + 1:]:
        if line.startswith("@@"):
            continue
        if line.startswith("--- ") or line.startswith("diff "):
            break
        if not line.startswith("+"):
            raise ValueError("unexpected line in new-file hunk: %r" % line)
        body.append(line[1:])
    text = "\n".join(body) + "\n"
    if text.count("#define E") != 61:
        raise ValueError("expected 61 LV2 errno names")
    return text


def write_fixture(directory) -> None:
    d = Path(directory)
    (d / "sys").mkdir(parents=True, exist_ok=True)
    (d / "sys" / "_lv2_errno.h").write_text(lv2_errno_header())
    (d / "errno.h").write_text(
        "/* Host errno.h, then the PS3 target's LV2 errno values. */\n"
        "#include_next <errno.h>\n#include <sys/_lv2_errno.h>\n")


if __name__ == "__main__":
    import sys
    write_fixture(sys.argv[1])
    print("wrote", sys.argv[1])
