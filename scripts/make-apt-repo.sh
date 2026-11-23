#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 --packages DIR --site DIR --key-id FINGERPRINT --url PAGES_URL" >&2
}

packages=
site=
key_id=
url=
while (($#)); do
    case "$1" in
        --packages) packages=$2; shift 2 ;;
        --site) site=$2; shift 2 ;;
        --key-id) key_id=$2; shift 2 ;;
        --url) url=$2; shift 2 ;;
        *) usage; exit 2 ;;
    esac
done
if [[ -z $packages || -z $site || -z $key_id || -z $url ]]; then
    usage
    exit 2
fi
for tool in dpkg-parsechangelog dpkg-deb apt-ftparchive gpg gpgv gzip; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
packages=$(realpath -- "$packages")
mkdir -p "$site"
site=$(realpath -- "$site")
version=$(cd "$project_dir" && dpkg-parsechangelog -S Version)
repo=$site/debian
mkdir -p "$repo"
shopt -s nullglob
debs=("$packages"/avaota-cedar-*_"$version"_arm64.deb)
if ((${#debs[@]} != 8)); then
    echo "Expected 8 arm64 packages at version $version, found ${#debs[@]}" >&2
    exit 1
fi
for deb in "${debs[@]}"; do
    [[ $(dpkg-deb -f "$deb" Version) == "$version" ]] || exit 1
    [[ $(dpkg-deb -f "$deb" Architecture) == arm64 ]] || exit 1
    cp -- "$deb" "$repo/"
done
cd "$repo"
apt-ftparchive packages . > Packages
gzip -9 -c Packages > Packages.gz
apt-ftparchive \
    -o 'APT::FTPArchive::Release::Origin=Avaota Sun55i Cedar' \
    -o 'APT::FTPArchive::Release::Label=Avaota Sun55i Cedar' \
    -o 'APT::FTPArchive::Release::Suite=./' \
    -o 'APT::FTPArchive::Release::Codename=./' \
    -o 'APT::FTPArchive::Release::Architectures=arm64' \
    release . > Release
sign_args=(--batch --yes --local-user "$key_id")
if [[ -n ${APT_GPG_PASSPHRASE:-} ]]; then
    sign_args+=(--pinentry-mode loopback --passphrase "$APT_GPG_PASSPHRASE")
fi
gpg "${sign_args[@]}" --clearsign --output InRelease Release
gpg "${sign_args[@]}" --armor --detach-sign --output Release.gpg Release
gpg --batch --export "$key_id" > "$site/KEY.gpg"
[[ -s $site/KEY.gpg ]] || { echo 'Empty public key export' >&2; exit 1; }
gpgv --keyring "$site/KEY.gpg" InRelease
gpgv --keyring "$site/KEY.gpg" Release.gpg Release
cat > "$site/avaota-sun55i-cedar.sources" <<EOF
Types: deb
URIs: ${url%/}/debian
Suites: ./
Architectures: arm64
Signed-By: /usr/share/keyrings/avaota-sun55i-cedar.gpg
EOF
echo "Signed APT repository: $site ($version)"
