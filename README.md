# Avaota Sun55i Cedar Codec

Standalone Debian/Ubuntu arm64 packages for T527 CedarC/CedarX and FFmpeg. All build inputs are bundled; the project occupies about 28 MiB.

## Packages

| Package | Contents |
| --- | --- |
| `avaota-cedar-runtime` | VE, memory libraries, configuration |
| `avaota-cedar-encoder` | H.264, H.265, JPEG hardware encoders; H.264 bridge |
| `avaota-cedar-decoder` | Video decoder and format plugins |
| `avaota-cedar-media` | CedarX audio, parser, muxer, player, recorder |
| `avaota-cedar-ffmpeg` | FFmpeg 7.1.5, x264, libvpx |
| `avaota-cedar-*-dev` | CedarC headers and pkg-config files |

FFmpeg's H.264, VP8, and VP9 encoders are software codecs; CedarC provides the hardware codec API.

## Build

Install `debhelper`, `dpkg-dev`, `patchelf`, `gcc-aarch64-linux-gnu`, `binutils-aarch64-linux-gnu`, `xz-utils`, `pkgconf`, `make`, and `python3`. Run:

```sh
./build.sh
```

The eight arm64 `.deb` files appear in the parent directory. Building needs neither the Tina SDK nor network access.

## GitHub Actions and APT

Push this directory as a GitHub repository with a `main` branch. Add an Actions secret named `APT_GPG_PRIVATE_KEY` containing an exported ASCII-armored GPG secret key. For a protected key, also add `APT_GPG_PASSPHRASE`. Set **Pages → Build and deployment → Source** to **GitHub Actions**. Pull requests build the packages; pushes to `main` and manual workflow runs publish a signed arm64 APT repository on GitHub Pages. Keep the signing key for later releases so installed systems continue to trust updates.

On an arm64 Debian or Ubuntu system:

```sh
base=https://avaotasbc.github.io/avaota-cedar-codec
curl -fsSL "$base/KEY.gpg" | sudo tee /usr/share/keyrings/avaota-sun55i-cedar.gpg >/dev/null
curl -fsSL "$base/avaota-sun55i-cedar.sources" | sudo tee /etc/apt/sources.list.d/avaota-sun55i-cedar.sources >/dev/null
sudo apt update
sudo apt install avaota-cedar-encoder avaota-cedar-decoder avaota-cedar-ffmpeg
```

The workflow also provides the eight `.deb` files as a build artifact. Repository metadata can be generated locally with `scripts/make-apt-repo.sh` after `./build.sh`.

## Use and test

Cedar libraries install under `/usr/lib/aarch64-linux-gnu/avaota-cedar`. FFmpeg installs under `/usr/lib/avaota-multimedia`; its commands are `avaota-ffmpeg` and `avaota-ffprobe`. Distribution FFmpeg updates do not replace these files.

The encoder package includes `libavaota-cedar-bridge.so`, a small NV12-to-H.264 API for external applications.

Install the encoder development package and compile the included hardware test on the target:

```sh
gcc -O2 tests/cedar_h264_smoke.c $(pkg-config --cflags --libs avaota-cedar-encoder) -o cedar-h264-smoke
./cedar-h264-smoke frame.nv12 frame.h264 1920 1080
```
