#!/bin/bash
set -e

echo "Building C# native wrapper for Linux..."

# Create unique temp directory to avoid conflicts with parallel builds
BUILD_DIR="$(mktemp -d /tmp/csharp-build.XXXXXX)"
trap "rm -rf '$BUILD_DIR'" EXIT

# Copy generated source to temp location (to avoid Windows path issues)
# The wrapper includes "config.h", found via -I/buildsrc below
mkdir -p "$BUILD_DIR/generated"
cp /generated/libredwg_wrap.c "$BUILD_DIR/generated/"

# Fix CRLF if needed
sed -i 's/\r$//' "$BUILD_DIR/generated/libredwg_wrap.c"

cd "$BUILD_DIR/generated"

# Compile the wrapper
gcc -shared -fPIC -O2 \
    -I/include \
    -I/src \
    -I/buildsrc \
    -L/nativelib \
    -o libredwg_csharp.so \
    libredwg_wrap.c \
    -lredwg \
    -Wl,-rpath,'$ORIGIN'

# Copy outputs (including versioned .so files)
cp libredwg_csharp.so /output/
cp /nativelib/libredwg.so* /output/

echo "Build complete!"
ls -la /output/
