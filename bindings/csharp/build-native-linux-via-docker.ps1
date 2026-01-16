<#
.SYNOPSIS
    Builds the native C# wrapper for Linux using Docker (from Windows).

.DESCRIPTION
    Compiles the SWIG-generated libredwg_wrap.c into a Linux shared library
    (libredwg_csharp.so) using Docker with GCC.

.NOTES
    Prerequisites:
    - Docker Desktop running
    - LibreDWG already built for Linux (libredwg.so in bin/linux/)
    - SWIG bindings generated (run generate-bindings.ps1 first)
#>

param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$generatedDir = Join-Path $scriptDir "generated"
$libredwgRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$libredwgParent = Split-Path -Parent $libredwgRoot
$includeDir = Join-Path $libredwgRoot "include"
$buildSrcDir = Join-Path $libredwgRoot ".build-linux\src"
$srcDir = Join-Path $libredwgRoot "src"
$libDir = Join-Path $libredwgParent "bin\linux"
$outputDir = Join-Path $scriptDir "bin\linux"

Write-Host "LibreDWG C# Native Wrapper Builder (Linux via Docker)" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan

# Verify prerequisites
$wrapperFile = Join-Path $generatedDir "libredwg_wrap.c"
if (-not (Test-Path $wrapperFile)) {
    Write-Host "ERROR: libredwg_wrap.c not found. Run generate-bindings.ps1 first." -ForegroundColor Red
    exit 1
}

$libredwgSo = Join-Path $libDir "libredwg.so"
if (-not (Test-Path $libredwgSo)) {
    Write-Host "ERROR: libredwg.so not found at $libredwgSo" -ForegroundColor Red
    Write-Host "Build libredwg for Linux first using build-linux-via-docker.ps1" -ForegroundColor Yellow
    exit 1
}

$configH = Join-Path $buildSrcDir "config.h"
if (-not (Test-Path $configH)) {
    Write-Host "ERROR: config.h not found at $configH" -ForegroundColor Red
    Write-Host "Build libredwg for Linux first using build-linux-via-docker.ps1" -ForegroundColor Yellow
    exit 1
}

# Clean if requested
if ($Clean -and (Test-Path $outputDir)) {
    Write-Host "Cleaning output directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $outputDir
}

# Create output directory
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir | Out-Null
}

Write-Host ""
Write-Host "Building native wrapper in Docker..." -ForegroundColor Yellow
Write-Host "  Source:  $wrapperFile"
Write-Host "  Include: $includeDir"
Write-Host "  Lib:     $libDir"
Write-Host "  Output:  $outputDir"

# Build script path
$buildScript = Join-Path $scriptDir "docker-build-csharp.sh"

# Run Docker
# Use Debian Bullseye (glibc 2.31) for compatibility with dotnet/sdk:8.0 runtime
docker run --rm `
    -v "${generatedDir}:/generated:ro" `
    -v "${includeDir}:/include:ro" `
    -v "${srcDir}:/src:ro" `
    -v "${buildSrcDir}:/buildsrc:ro" `
    -v "${libDir}:/nativelib:ro" `
    -v "${outputDir}:/output" `
    -v "${buildScript}:/build.sh:ro" `
    debian:bullseye `
    /bin/bash -c "apt-get update && apt-get install -y build-essential && /bin/bash /build.sh"

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Docker build failed" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Build complete!" -ForegroundColor Green
Write-Host "  Native wrapper: $outputDir\libredwg_csharp.so"
Write-Host "  LibreDWG:       $outputDir\libredwg.so"
