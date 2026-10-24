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

# Portable SHA-256 tool (coreutils on Linux, shasum on macOS)
if command -v sha256sum >/dev/null 2>&1; then
    SHA256="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    SHA256="shasum -a 256"
else
    echo "No SHA-256 tool found (sha256sum or shasum); refusing to install an unverified binary." >&2
    exit 1
fi

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
SUMS_TMP="$TMP.sha256"
trap 'rm -f "$TMP" "$SUMS_TMP"' EXIT
curl -fsSL "$URL" -o "$TMP"
chmod +x "$TMP"

# Verification is mandatory: a missing sums file or an unlisted asset must not
# silently install an unverified binary.
if ! curl -fsSL "$SUMS_URL" -o "$SUMS_TMP"; then
    echo "Failed to download SHA256SUMS; refusing to install an unverified binary." >&2
    exit 1
fi
EXPECTED=$(awk -v asset="$ASSET" '$2 == asset || $2 == "*" asset { print $1; exit }' "$SUMS_TMP")
if [ -z "$EXPECTED" ]; then
    echo "Asset $ASSET is not listed in SHA256SUMS; refusing to install an unverified binary." >&2
    exit 1
fi
ACTUAL=$($SHA256 "$TMP" | awk '{print $1}')
if [ "$EXPECTED" != "$ACTUAL" ]; then
    echo "Checksum mismatch for $ASSET (expected $EXPECTED, got $ACTUAL)!" >&2
    exit 1
fi
echo "Checksum verified"

# Install
mv "$TMP" "$INSTALL_DIR/skipbuild"
echo "Installed skipbuild to $INSTALL_DIR/skipbuild"

# Make it usable immediately in this shell, and explain how to persist it
case ":$PATH:" in
    *":$INSTALL_DIR:"*) ;;
    *)
        export PATH="$INSTALL_DIR:$PATH"
        echo ""
        echo "NOTE: '$INSTALL_DIR' is not on your PATH yet."
        echo "      This window can use skipbuild now. To make it permanent, add:"
        echo "        export PATH=\"$INSTALL_DIR:\$PATH\""
        echo "      to your ~/.bashrc or ~/.zshrc, then open a new terminal."
        ;;
esac

echo ""
echo "skipbuild is ready."
echo "  Verify:  skipbuild --version"
echo "  CMake:   paste the 4-line snippet from the README 'Integration' section once."