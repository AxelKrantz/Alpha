#!/bin/sh
set -e

echo "Installing Alpha..."

# Create temp directory
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Clone and build
git clone --depth 1 https://github.com/AxelKrantz/Alpha.git "$TMPDIR/alpha" 2>/dev/null
cd "$TMPDIR/alpha/bootstrap"
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1) > /dev/null 2>&1

# Install
if [ -w /usr/local/bin ]; then
    cp alphac /usr/local/bin/alphac
else
    sudo cp alphac /usr/local/bin/alphac
fi

echo "Done! Run: alphac --help"
