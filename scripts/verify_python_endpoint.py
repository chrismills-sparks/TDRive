#!/usr/bin/env python3
"""Assert a built TDRiveTOP plugin really carries the CPython schema endpoint.

Why this exists
---------------
The Python endpoint is compiled behind `#if defined(TDRIVE_PYTHON)`. When that
define is absent the plugin still builds, still loads, and still renders - it
just has no `pythonGetSets`, so TouchDesigner builds no Python class on the
node and every documented attribute raises:

    tdAttributeError: 'td.RiveTOP' object has no attribute 'propertySchema'

Nothing in a normal build or smoke test distinguishes that DLL from a good one.
A release once shipped exactly that way: CI has no TouchDesigner install, CMake's
SDK lookup missed, the old code warned instead of failing, and the release went
out with the endpoint silently stripped. CMake is now fatal on a missed lookup;
this script is the belt to that braces, checking the *artifact* rather than the
build configuration, so a Python-less DLL can never reach a release again.

What it checks
--------------
Windows (PE):
1. `python3.dll` appears in the PE import table. This is the load-bearing
   signal: the endpoint calls into CPython through the PEP 384 stable ABI, so
   a DLL that does not import python3.dll cannot possibly serve it.

macOS (Mach-O): the plugin links no libpython; its Python symbols are left to
load-time lookup against TouchDesigner's interpreter (per-symbol -U flags).
1. At least one `_Py*` symbol is dynamically looked up - the endpoint is in.
2. NOTHING else is dynamically looked up. A non-Python symbol left to
   load-time lookup would link cleanly and only fail when TouchDesigner loads
   the plugin (e.g. a stray -undefined dynamic_lookup).

Both:
- Every `gPyGetSets` name is present in the binary's data, which catches a
  partially-compiled or renamed endpoint that still links CPython.

Usage
-----
    python scripts/verify_python_endpoint.py build/Release/TDRiveTOP.dll
    python3 scripts/verify_python_endpoint.py build/TDRiveTOP.plugin/Contents/MacOS/TDRiveTOP

Exits 0 when the plugin carries the endpoint, 1 otherwise. The Mach-O check
shells out to `nm` (Xcode command line tools).
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

# Must match gPyGetSets in src/TDRiveTOP.cpp. Add a name here when you add a
# getset there, or the new attribute ships unverified.
EXPECTED_GETSETS = ("schemaVersion", "propertySchema", "tdJSONPars")

REQUIRED_IMPORT = "python3.dll"


class PEError(Exception):
    """The file is not a PE image we can read."""


def _section_table(data: bytes, pe: int, n_sections: int, opt_size: int):
    """Return [(virtual_addr, virtual_size, raw_size, raw_ptr)] for each section."""
    sections = []
    for i in range(n_sections):
        off = pe + 24 + opt_size + i * 40
        if off + 40 > len(data):
            raise PEError("section table runs past end of file")
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, vsize, rsize, rptr))
    return sections


def _rva_to_offset(sections, rva: int):
    """Map a virtual address to a file offset, or None if it lands in no section."""
    for vaddr, vsize, rsize, rptr in sections:
        # A section's on-disk span can be shorter or longer than its virtual
        # span depending on alignment; accept the larger so we never reject a
        # valid RVA.
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return rptr + (rva - vaddr)
    return None


def imported_dlls(path: Path) -> list[str]:
    """Names from the PE import directory, in table order."""
    data = path.read_bytes()

    if data[:2] != b"MZ":
        raise PEError("missing MZ signature - not a PE image")
    (pe,) = struct.unpack_from("<I", data, 0x3C)
    if data[pe : pe + 4] != b"PE\0\0":
        raise PEError("missing PE signature")

    n_sections, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    magic, = struct.unpack_from("<H", data, pe + 24)
    if magic == 0x20B:       # PE32+
        dd_off = pe + 24 + 112
    elif magic == 0x10B:     # PE32
        dd_off = pe + 24 + 96
    else:
        raise PEError(f"unrecognised optional header magic 0x{magic:x}")

    sections = _section_table(data, pe, n_sections, opt_size)

    # Data directory entry 1 is the import table: 8 bytes in, {VA, Size}.
    import_rva, import_size = struct.unpack_from("<II", data, dd_off + 8)
    if not import_rva or not import_size:
        return []   # legitimately imports nothing

    off = _rva_to_offset(sections, import_rva)
    if off is None:
        raise PEError("import directory RVA is outside every section")

    names = []
    while True:
        descriptor = data[off : off + 20]
        if len(descriptor) < 20 or descriptor == b"\0" * 20:
            break   # null descriptor terminates the array
        name_rva, = struct.unpack_from("<I", descriptor, 12)
        name_off = _rva_to_offset(sections, name_rva)
        if name_off is None:
            raise PEError("an import name RVA is outside every section")
        end = data.index(b"\0", name_off)
        names.append(data[name_off:end].decode("ascii", "replace"))
        off += 20
    return names


MACHO_MAGICS = (
    b"\xcf\xfa\xed\xfe",  # MH_MAGIC_64, little-endian
    b"\xca\xfe\xba\xbe",  # FAT_MAGIC (universal binary)
)

_PY_SYMBOL = re.compile(r"^__?Py")


def dynamic_lookup_symbols(path: Path) -> list[str]:
    """Undefined symbols `nm` reports as '(dynamically looked up)'."""
    out = subprocess.run(
        ["nm", "-m", "-u", str(path)], capture_output=True, text=True, check=True
    ).stdout
    syms = []
    for line in out.splitlines():
        if "(dynamically looked up)" in line:
            m = re.search(r"external (\S+)", line)
            if m:
                syms.append(m.group(1))
    return sorted(set(syms))


def check_pe(path: Path) -> list[str]:
    """Return failure reasons for a Windows DLL (empty list = OK)."""
    imports = imported_dlls(path)
    has_python = any(dll.lower() == REQUIRED_IMPORT for dll in imports)
    print(f"  imports: {', '.join(imports) or '(none)'}")
    print(f"  {REQUIRED_IMPORT} imported: {'yes' if has_python else 'NO'}")
    if has_python:
        return []
    return [
        f"No {REQUIRED_IMPORT} import - the plugin was built without "
        "TDRIVE_PYTHON defined.\n"
        "  Configure with -DTD_PYTHON_ROOT=<python sdk> (see CMakeLists.txt) "
        "and rebuild."
    ]


def check_macho(path: Path) -> list[str]:
    """Return failure reasons for a macOS plugin binary (empty list = OK)."""
    syms = dynamic_lookup_symbols(path)
    py = [s for s in syms if _PY_SYMBOL.match(s)]
    other = [s for s in syms if not _PY_SYMBOL.match(s)]
    print(f"  load-time Python symbols: {len(py)}")
    print(f"  load-time non-Python symbols: {len(other)}")
    problems = []
    if not py:
        problems.append(
            "No Python symbol is looked up at load time - the plugin was built "
            "without TDRIVE_PYTHON defined.\n"
            "  Configure with -DTD_PYTHON_ROOT=<python 3.11 root> (see "
            "CMakeLists.txt) and rebuild."
        )
    if other:
        problems.append(
            "Non-Python symbols are left to load-time lookup; nothing at link "
            "time resolved them, so TouchDesigner could fail to load this plugin:\n"
            + "\n".join(f"    {s}" for s in other)
        )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Verify a TDRiveTOP plugin carries the CPython schema endpoint."
    )
    ap.add_argument(
        "binary",
        type=Path,
        help="TDRiveTOP.dll, or the Mach-O inside TDRiveTOP.plugin/Contents/MacOS",
    )
    args = ap.parse_args()
    path: Path = args.binary

    if not path.is_file():
        print(f"FAIL: {path} does not exist", file=sys.stderr)
        return 1

    blob = path.read_bytes()
    print(f"{path}  ({len(blob):,} bytes)")
    try:
        if blob[:2] == b"MZ":
            problems = check_pe(path)
        elif blob[:4] in MACHO_MAGICS:
            problems = check_macho(path)
        else:
            print(f"FAIL: {path} is neither a PE nor a Mach-O image", file=sys.stderr)
            return 1
    except (PEError, struct.error, ValueError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: could not inspect {path}: {exc}", file=sys.stderr)
        return 1

    missing_getsets = [
        name for name in EXPECTED_GETSETS if name.encode("ascii") not in blob
    ]
    for name in EXPECTED_GETSETS:
        present = name not in missing_getsets
        print(f"  getset {name}: {'present' if present else 'MISSING'}")

    if not problems and not missing_getsets:
        print("OK: CPython schema endpoint is present.")
        return 0

    print("", file=sys.stderr)
    print("FAIL: this plugin does not carry the CPython schema endpoint.", file=sys.stderr)
    for p in problems:
        print(f"  {p}", file=sys.stderr)
    if missing_getsets:
        print(
            f"  Missing getset name(s): {', '.join(missing_getsets)}.\n"
            "  Either gPyGetSets in src/TDRiveTOP.cpp changed without updating "
            "EXPECTED_GETSETS in this script, or the endpoint is only partly "
            "compiled.",
            file=sys.stderr,
        )
    return 1


if __name__ == "__main__":
    sys.exit(main())
