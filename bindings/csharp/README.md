# LibreDWG C# Bindings

This directory contains the C# / .NET bindings for LibreDWG.

## Overview

The bindings consist of:

1. **SWIG-generated wrappers** - Auto-generated from C headers, providing access to all LibreDWG types and functions
2. **Hand-written helpers** - Custom C# code for APIs that need cleaner marshaling (like SVG generation)

## Directory Structure

| Path | Description |
|------|-------------|
| `generated/` | SWIG-generated C# files and native wrapper code (gitignored) |
| `bin/` | Build output - native DLLs and managed assembly (gitignored) |
| `src/` | Contains `config.h` copied during build (gitignored) |
| `obj/` | .NET build intermediates (gitignored) |

## Files

### Scripts

| Script | Description |
|--------|-------------|
| `generate-bindings.ps1` | Run SWIG to generate C# wrappers from `../dwg.i` |
| `build-native.ps1` | Compile native wrapper DLL on Windows (uses MSYS2/gcc) |
| `build-native-linux.sh` | Compile native wrapper .so on Linux |
| `build-native-linux-via-docker.ps1` | Compile Linux .so on Windows using Docker |
| `docker-build-csharp.sh` | Helper script used inside Docker container |

### Source Files

| File | Description |
|------|-------------|
| `LibreDWG.csproj` | .NET project file for the managed assembly |
| `DwgSvgApi.cs` | **Hand-written** C# wrapper for SVG generation functions |

## DwgSvgApi.cs - Why It's Hand-Written

The SVG API functions use `char**` output parameters which SWIG wraps as opaque `SWIGTYPE_p_p_char` types - not usable from C#.

`DwgSvgApi.cs` provides a clean C# API by:
- Using direct P/Invoke with proper marshaling
- Handling memory management (calling `dwg_free_svg`)
- Returning proper `string` types instead of opaque pointers

**Important:** This file P/Invokes directly into `libredwg` (the main library), not `libredwg_csharp` (the SWIG wrapper). The build scripts rename `libredwg-0.dll` to `libredwg.dll` so the P/Invoke name works on both Windows and Linux.

### SVG API Usage

```csharp
using LibreDWGInterop;

// From a file path
string svg = DwgSvgApi.ToSvg("drawing.dwg");

// From an already-loaded Dwg_Data
var dwg = LibreDWG.dwg_read_file("drawing.dwg");
string svg = DwgSvgApi.ToSvg(dwg);

// Write directly to file
DwgSvgApi.WriteSvg("input.dwg", "output.svg");
```

## Build Process

### Full Build (from parent directory)

The easiest way to build everything:

```powershell
# From lib/libredwg/
.\build-csharp-bindings.ps1
```

This runs all steps automatically.

### Manual Build Steps

If you need to build components individually:

```powershell
# 1. Generate SWIG bindings (creates generated/*.cs and generated/libredwg_wrap.c)
.\generate-bindings.ps1

# 2. Build native wrapper DLL (compiles libredwg_wrap.c)
.\build-native.ps1

# 3. Build managed assembly
dotnet build -c Release
```

### Output

After building, `bin/Release/` contains:
- `libredwg_csharp.dll` - Native wrapper (P/Invoke target)
- `libredwg-0.dll` - Main library (copied from `../../bin/windows/`)
- `net8.0/LibreDWG.Net.dll` - Managed assembly
