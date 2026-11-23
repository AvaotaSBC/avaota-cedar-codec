#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

cd "$project_dir"
if [[ $# -ne 0 ]]; then
    echo "Usage: ./build.sh" >&2
    exit 2
fi
for tool in aarch64-linux-gnu-gcc aarch64-linux-gnu-as patchelf xz pkgconf make python3 dpkg-buildpackage; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
python3 scripts/verify.py
dpkg-buildpackage -d -b -us -uc -a arm64
python3 scripts/verify.py debian/tmp
echo "Packages: $project_dir/../avaota-cedar-*.deb"
