#!/bin/sh
# Build and install snova-lsp with the matching Snovalang compiler sources.
# Usage: curl -fsSL https://raw.githubusercontent.com/snovalang/snova-lsp/master/install.sh | sh
set -eu

REPO=https://codeload.github.com/snovalang
PREFIX=${PREFIX:-"$HOME/.local"}

case "$(uname -s)" in
  Darwin|Linux) ;;
  *) echo "snova-lsp source installer supports macOS and Linux." >&2; exit 1 ;;
esac

for tool in curl tar make cc; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required command not found: $tool" >&2
    exit 1
  fi
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/snova-lsp-install.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

echo "Downloading snova-lsp and snovac sources..."
curl -fsSL "$REPO/snova-lsp/tar.gz/refs/heads/master" -o "$tmp/lsp.tar.gz"
curl -fsSL "$REPO/snovac/tar.gz/refs/heads/master" -o "$tmp/snovac.tar.gz"
tar -xzf "$tmp/lsp.tar.gz" -C "$tmp"
tar -xzf "$tmp/snovac.tar.gz" -C "$tmp"

lsp="$tmp/snova-lsp-master"
snovac="$tmp/snovac-master"
if [ ! -f "$lsp/Makefile" ] || [ ! -f "$snovac/Makefile" ]; then
  echo "Downloaded source archives are incomplete." >&2
  exit 1
fi

echo "Building snovac..."
make -C "$snovac" all
echo "Building and installing snova-lsp..."
make -C "$lsp" SNOVAC_DIR="$snovac" PREFIX="$PREFIX" install

echo "Installed to $PREFIX/bin/snova-lsp"
echo "Add $PREFIX/bin to PATH if it is not already there."
