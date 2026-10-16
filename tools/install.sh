#!/bin/sh
# Installs the latest Tundra release (or TUNDRA_VERSION) for Linux or macOS.
#
#   curl -fsSL https://github.com/ingen0s/tundra/releases/latest/download/install.sh | sh
#
# Puts tundra_chat_server and tundra_client in $PREFIX/bin (default
# ~/.local/bin) after checking the download against the release's
# SHA256SUMS. Windows: download the .zip from the releases page instead.
set -eu

REPO="ingen0s/tundra"
PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin"

say() { printf '%s\n' "$*" >&2; }
die() { say "error: $*"; exit 1; }

case "$(uname -s)" in
    Linux)
        case "$(uname -m)" in
            x86_64 | amd64) platform="linux-x64" ;;
            aarch64 | arm64) platform="linux-arm64" ;;
            *) die "no prebuilt binary for Linux $(uname -m); build from source (see README)" ;;
        esac ;;
    Darwin) platform="macos-universal" ;;
    *) die "unsupported system $(uname -s); on Windows download the .zip from https://github.com/$REPO/releases" ;;
esac

if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL "$1" -o "$2"; }
    fetch_stdout() { curl -fsSL "$1"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -q "$1" -O "$2"; }
    fetch_stdout() { wget -q "$1" -O -; }
else
    die "curl or wget is required"
fi

if [ -n "${TUNDRA_VERSION:-}" ]; then
    tag="v${TUNDRA_VERSION#v}"
else
    tag=$(fetch_stdout "https://api.github.com/repos/$REPO/releases/latest" |
        sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n 1)
    [ -n "$tag" ] || die "could not find the latest release"
fi
version="${tag#v}"
name="tundra-$version-$platform"
base="https://github.com/$REPO/releases/download/$tag"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

say "Downloading $name"
fetch "$base/$name.tar.gz" "$tmp/$name.tar.gz" || die "download failed: $base/$name.tar.gz"
fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS" || die "download failed: $base/SHA256SUMS"

expected=$(awk -v f="$name.tar.gz" '$2 == f {print $1}' "$tmp/SHA256SUMS")
[ -n "$expected" ] || die "$name.tar.gz is not listed in SHA256SUMS"
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$tmp/$name.tar.gz" | awk '{print $1}')
else
    actual=$(shasum -a 256 "$tmp/$name.tar.gz" | awk '{print $1}')
fi
[ "$expected" = "$actual" ] || die "checksum mismatch for $name.tar.gz"

tar -xzf "$tmp/$name.tar.gz" -C "$tmp"
mkdir -p "$BIN"
for b in tundra_chat_server tundra_client; do
    cp "$tmp/$name/$b" "$BIN/$b.new"
    chmod 755 "$BIN/$b.new"
    mv -f "$BIN/$b.new" "$BIN/$b"
done

say "Installed Tundra $version to $BIN"
case ":$PATH:" in
    *":$BIN:"*) ;;
    *) say "Add $BIN to your PATH, for example: export PATH=\"$BIN:\$PATH\"" ;;
esac
say "Start a server with: tundra_chat_server -w 8080   (then open http://localhost:8080/)"
