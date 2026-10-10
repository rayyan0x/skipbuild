#!/bin/bash
set -e

REPO="yourname/remake"
VERSION="${1:-latest}"
INSTALL_DIR="${2:-/usr/local/bin}"

echo "Installing remake..."

# Detect OS and arch
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
case $ARCH in
    x86_64) ARCH="x86_64" ;;
    aarch64|arm64) ARCH="aarch64" ;;
    *) echo "Unsupported arch: $ARCH"; exit 1 ;;
esac

if [ "$VERSION" = "latest" ]; then
    VERSION=$(curl -s "https://api.github.com/repos/$REPO/releases/latest" | grep '"tag_name"' | cut -d'"' -f4)
fi

URL="https://github.com/$REPO/releases/download/$VERSION/remake-${OS}-${ARCH}.tar.gz"
echo "Downloading $URL..."

curl -fL "$URL" | tar -xz -C /tmp
sudo install -m 755 /tmp/remake "$INSTALL_DIR/remake"

echo "Installed to $INSTALL_DIR/remake"
remake --version