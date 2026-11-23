#!/usr/bin/env python3
"""Check provenance, ELF architecture, dependency closure and private RPATH."""
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path


def elf_dynamic(path: Path) -> tuple[list[str], str]:
    header = subprocess.check_output(["readelf", "-h", str(path)], text=True)
    if "AArch64" not in header or "DYN (Shared object file)" not in header:
        raise ValueError(f"not an AArch64 shared object: {path}")
    dynamic = subprocess.check_output(["readelf", "-d", str(path)], text=True)
    needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
    paths = re.findall(r"\((?:RPATH|RUNPATH)\).*?\[(.*?)\]", dynamic)
    return needed, paths[0] if paths else ""


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    manifest = json.loads((root / "vendor/manifest.json").read_text())
    for name, expected in manifest["sha256"].items():
        path = root / name
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"checksum mismatch: {name}")
    libs = {p.name.removesuffix(".blob"): p
            for p in (root / "vendor/arm64").glob("*.so*.blob")}
    system = {"libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1",
              "libdl.so.2", "ld-linux-aarch64.so.1", "libstdc++.so.6",
              "libgcc_s.so.1", "libz.so.1", "libatomic.so.1",
              "libasound.so.2"}
    for name, path in sorted(libs.items()):
        needed, _ = elf_dynamic(path)
        missing = set(needed) - libs.keys() - system
        if missing:
            raise ValueError(f"{name}: unbundled dependencies: {sorted(missing)}")
    if len(libs) != 64:
        raise ValueError(f"expected 64 private libraries, got {len(libs)}")
    print("Source checksums, arm64 ELF format and dependency closure: OK")
    if len(sys.argv) == 2:
        staged = Path(sys.argv[1]) / "usr/lib/aarch64-linux-gnu/avaota-cedar"
        for path in staged.glob("*.so*"):
            _, rpath = elf_dynamic(path)
            if rpath != "$ORIGIN":
                raise ValueError(f"{path}: unexpected runtime path {rpath!r}")
        if len(list(staged.glob("*.so*"))) != 65:
            raise ValueError("incomplete staged runtime library set")
        print("Installed private RUNPATH: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
