#!/usr/bin/env bash
set -euo pipefail

REPO="rayyan0x/skipbuild"
INSTALL_DIR="${SKIPBUILD_INSTALL_DIR:-$HOME/.local/bin}"
VERSION="${1:-latest}"

# Detect OS and arch
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64) ARCH="x64" ;;
    aarch64|arm64) ARCH="arm64" ;;
    *) echo "Unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

# Map OS to asset name
case "$OS" in
    linux) ASSET="skipbuild-linux-${ARCH}" ;;
    darwin) ASSET="skipbuild-macos-${ARCH}" ;;
    *) echo "Unsupported OS: $OS" >&2; exit 1 ;;
esac

# Resolve version
if [ "$VERSION" = "latest" ]; then
    VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | grep '"tag_name":' | sed -E 's/.*"v([^"]+)".*/\1/')
    if [ -z "$VERSION" ]; then
        echo "Failed to determine latest version" >&2
        exit 1
    fi
fi

URL="https://github.com/$REPO/releases/download/v$VERSION/$ASSET"
SUMS_URL="https://github.com/$REPO/releases/download/v$VERSION/SHA256SUMS"

echo "Installing skipbuild v$VERSION ($ASSET) to $INSTALL_DIR"

# Create install directory
mkdir -p "$INSTALL_DIR"

# Download binary
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
curl -fsSL "$URL" -o "$TMP"
chmod +x "$TMP"

# Verify checksum if available
if curl -fsSL "$SUMS_URL" -o "$TMP.sha256" 2>/dev/null; then
    EXPECTED=$(grep "$ASSET" "$TMP.sha256" | awk '{print $1}')
    ACTUAL=$(sha256sum "$TMP" | awk '{print $1}')
    if [ "$EXPECTED" != "$ACTUAL" ]; then
        echo "Checksum mismatch!" >&2
        exit 1
    fi
    echo "Checksum verified"
fi

# Install
mv "$TMP" "$INSTALL_DIR/skipbuild"
echo "Installed skipbuild to $INSTALL_DIR/skipbuild"

# Check PATH
case ":$PATH:" in
    *":$INSTALL_DIR:"*) ;;
    *) echo "Note: Add $INSTALL_DIR to your PATH" ;;
esac