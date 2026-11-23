#!/usr/bin/env python3
"""Cross-build a private FFmpeg from the pinned upstream source archive."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "vendor/ffmpeg-7.1.5-full.tar.xz"
BUILD = ROOT / "obj-ffmpeg-arm64"
SOURCE = BUILD / "ffmpeg-7.1.5"
X264 = BUILD / "x264-tina"
VPX = BUILD / "libvpx-1.11.0"
STAGE = BUILD / "stage"
PREFIX = "/usr/lib/avaota-multimedia"


def run(*args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def build():
    if not ARCHIVE.is_file():
        raise FileNotFoundError(ARCHIVE)
    BUILD.mkdir(exist_ok=True)
    if not X264.exists():
        run("tar", "-xf", ROOT / "vendor/x264-tina.tar.xz", "-C", BUILD)
    if not VPX.exists():
        run("tar", "-xf", ROOT / "vendor/libvpx-1.11.0.tar.xz", "-C", BUILD)
    if not (STAGE / PREFIX.lstrip("/") / "lib/libx264.so").exists():
        if not (X264 / "config.mak").exists():
            run(X264 / "configure", "--prefix=" + PREFIX,
                "--host=aarch64-linux-gnu", "--cross-prefix=aarch64-linux-gnu-",
                "--enable-shared", "--enable-pic", "--disable-cli", cwd=X264)
        run("make", "-j", str(min(os.cpu_count() or 2, 8)), cwd=X264)
        run("make", "install", "DESTDIR=" + str(STAGE), cwd=X264)
    if not (STAGE / PREFIX.lstrip("/") / "lib/libvpx.so").exists():
        if not (VPX / "config.mk").exists():
            run(VPX / "configure", "--prefix=" + PREFIX,
                "--target=arm64-linux-gcc", "--enable-shared", "--disable-static",
                "--disable-examples", "--disable-tools", "--disable-unit-tests",
                cwd=VPX, env={**os.environ,
                              "CC": "aarch64-linux-gnu-gcc",
                              "CXX": "aarch64-linux-gnu-g++",
                              "AR": "aarch64-linux-gnu-ar",
                              "AS": "aarch64-linux-gnu-as",
                              "LD": "aarch64-linux-gnu-gcc"})
        # The optional standalone vp9rc C++ archive is not used by libvpx.so.
        # Its construction requires a C++ cross compiler even for a C-only
        # shared-library build, so omit that ancillary archive.
        libs_mk = VPX / "libs.mk"
        contents = libs_mk.read_text()
        marker = "ifeq ($(CONFIG_VP9_ENCODER),yes)\n  RC_RTC_OBJS="
        if marker in contents:
            start = contents.index(marker)
            end = contents.index("\nendif", start) + len("\nendif")
            libs_mk.write_text(contents[:start] + contents[end:])
        run("make", "-j", str(min(os.cpu_count() or 2, 8)), cwd=VPX)
        run("make", "install", "DESTDIR=" + str(STAGE), cwd=VPX)
    if not SOURCE.exists():
        run("tar", "-xf", ARCHIVE, "-C", BUILD)
    pkgdir = STAGE / PREFIX.lstrip("/") / "lib/pkgconfig"
    env = {**os.environ, "PKG_CONFIG_PATH": str(pkgdir),
           "PKG_CONFIG_LIBDIR": str(pkgdir),
           "PKG_CONFIG_SYSROOT_DIR": str(STAGE)}
    if not (SOURCE / "config.h").exists():
        run(SOURCE / "configure", "--prefix=" + PREFIX,
            "--arch=aarch64", "--target-os=linux",
            "--cross-prefix=aarch64-linux-gnu-", "--enable-cross-compile",
            "--pkg-config=pkg-config",
            "--enable-shared", "--disable-static", "--disable-autodetect",
            "--disable-doc", "--disable-debug", "--disable-ffplay",
            "--disable-symver", "--enable-gpl", "--enable-libx264",
            "--enable-libvpx", cwd=SOURCE, env=env)
    run("make", "-j", str(min(os.cpu_count() or 2, 8)), cwd=SOURCE)


def install(dest):
    build()
    run("make", "install", "DESTDIR=" + str(Path(dest).resolve()), cwd=SOURCE)
    staged = Path(dest).resolve() / PREFIX.lstrip("/")
    for part in ("lib", "include"):
        shutil.copytree(STAGE / PREFIX.lstrip("/") / part,
                        staged / part, dirs_exist_ok=True, symlinks=True)
    for pattern in ("libx264.so.*", "libvpx.so.*"):
        for lib in (staged / "lib").glob(pattern):
            if not lib.is_symlink():
                run("aarch64-linux-gnu-strip", "--strip-unneeded", lib)
    for lib in (staged / "lib").glob("*.so.*"):
        if lib.is_symlink():
            continue
        run("patchelf", "--set-rpath", "$ORIGIN", lib)
    for program in (staged / "bin").glob("ff*"):
        run("patchelf", "--set-rpath", "$ORIGIN/../lib", program)
    bindir = Path(dest).resolve() / "usr/bin"
    bindir.mkdir(parents=True, exist_ok=True)
    for name in ("ffmpeg", "ffprobe"):
        wrapper = bindir / ("avaota-" + name)
        wrapper.write_text("#!/bin/sh\nexec " + PREFIX + "/bin/" + name + ' "$@"\n')
        wrapper.chmod(0o755)


if __name__ == "__main__":
    if len(sys.argv) == 1:
        build()
    elif len(sys.argv) == 3 and sys.argv[1] == "--install":
        install(sys.argv[2])
    else:
        raise SystemExit("usage: build_ffmpeg.py [--install DESTDIR]")
