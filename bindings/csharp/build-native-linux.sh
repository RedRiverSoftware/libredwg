#!/bin/bash
#
# Builds the native C# wrapper shared library for LibreDWG on Linux.
# Produces libredwg_csharp.so that the .NET assembly P/Invokes into.
#
# Prerequisites:
#   - GCC
#   - LibreDWG already built (libredwg.so in bin/linux/)
#   - SWIG bindings generated (run generate-bindings.ps1 first, or on Windows)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GENERATED_DIR="$SCRIPT_DIR/generated"
LIBREDWG_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
INCLUDE_DIR="$LIBREDWG_ROOT/include"
BUILD_SRC_DIR="$LIBREDWG_ROOT/.build-linux/src"
SRC_DIR="$LIBREDWG_ROOT/src"
LIB_DIR="$(dirname "$LIBREDWG_ROOT")/bin/linux"
OUTPUT_DIR="$SCRIPT_DIR/bin/linux"

echo "LibreDWG C# Native Wrapper Builder (Linux)"
echo "==========================================="

# Verify prerequisites
WRAPPER_FILE="$GENERATED_DIR/libredwg_wrap.c"
if [ ! -f "$WRAPPER_FILE" ]; then
    echo "ERROR: libredwg_wrap.c not found at $WRAPPER_FILE"
    echo "Generate bindings first (on Windows: .\\generate-bindings.ps1)"
    exit 1
fi

LIBREDWG_SO="$LIB_DIR/libredwg.so"
if [ ! -f "$LIBREDWG_SO" ]; then
    echo "ERROR: libredwg.so not found at $LIBREDWG_SO"
    echo "Build libredwg first using build-linux.sh"
    exit 1
fi

CONFIG_H="$BUILD_SRC_DIR/config.h"
if [ ! -f "$CONFIG_H" ]; then
    echo "ERROR: config.h not found at $CONFIG_H"
    echo "Build libredwg first using build-linux.sh"
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Create local src directory with config.h (wrapper includes ../src/config.h)
LOCAL_SRC_DIR="$SCRIPT_DIR/src"
mkdir -p "$LOCAL_SRC_DIR"
cp "$CONFIG_H" "$LOCAL_SRC_DIR/"

echo ""
echo "Building native wrapper..."
echo "  Source:  $WRAPPER_FILE"
echo "  Include: $INCLUDE_DIR"
echo "  Lib:     $LIB_DIR"
echo "  Output:  $OUTPUT_DIR"

# Compile the wrapper into a shared library
# Build from within the generated directory so ../src/config.h resolves correctly
cd "$GENERATED_DIR"

gcc -shared -fPIC -O2 \
    -I"$INCLUDE_DIR" \
    -I"$SRC_DIR" \
    -I"$BUILD_SRC_DIR" \
    -L"$LIB_DIR" \
    -o "$OUTPUT_DIR/libredwg_csharp.so" \
    libredwg_wrap.c \
    -lredwg \
    -Wl,-rpath,'$ORIGIN'

# Copy libredwg.so to output
cp "$LIBREDWG_SO" "$OUTPUT_DIR/"

echo ""
echo "Build complete!"
echo "  Native wrapper: $OUTPUT_DIR/libredwg_csharp.so"
echo "  LibreDWG:       $OUTPUT_DIR/libredwg.so"
