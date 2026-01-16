#!/bin/bash
set -e

echo "Building C# native wrapper for Linux..."

# Create unique temp directory to avoid conflicts with parallel builds
BUILD_DIR="$(mktemp -d /tmp/csharp-build.XXXXXX)"
trap "rm -rf '$BUILD_DIR'" EXIT

# Copy generated source to temp location (to avoid Windows path issues)
# The wrapper includes "../src/config.h" relative to its location
# So we create: $BUILD_DIR/generated/libredwg_wrap.c
#               $BUILD_DIR/src/config.h
mkdir -p "$BUILD_DIR/generated"
mkdir -p "$BUILD_DIR/src"
cp /generated/libredwg_wrap.c "$BUILD_DIR/generated/"
cp /buildsrc/config.h "$BUILD_DIR/src/"

# Fix CRLF if needed
sed -i 's/\r$//' "$BUILD_DIR/generated/libredwg_wrap.c"
sed -i 's/\r$//' "$BUILD_DIR/src/config.h"

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
