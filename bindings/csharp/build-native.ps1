<#
.SYNOPSIS
    Builds the native C# wrapper DLL for LibreDWG.

.DESCRIPTION
    Compiles the SWIG-generated libredwg_wrap.c into a native DLL (libredwg_csharp.dll)
    that the C# managed code will P/Invoke into.

.NOTES
    Prerequisites:
    - MSYS2 with MINGW64 toolchain (same as build-windows.ps1)
    - LibreDWG already built (libredwg.dll in bin/windows/)
    - SWIG bindings generated (run generate-bindings.ps1 first)
#>

param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$generatedDir = Join-Path $scriptDir "generated"
$libredwgRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$includeDir = Join-Path $libredwgRoot "include"
$buildSrcDir = Join-Path $libredwgRoot ".build-autotools\src"  # Contains config.h from autotools build
$srcDir = Join-Path $libredwgRoot "src"
$libDir = Join-Path (Split-Path -Parent $libredwgRoot) "bin\windows"
$outputDir = Join-Path $scriptDir "bin\$Configuration"

Write-Host "LibreDWG C# Native Wrapper Builder (MINGW64)" -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan

# Find MSYS2 installation
$Msys2Paths = @(
    "C:\msys64",
    "C:\tools\msys2",
    "$env:USERPROFILE\scoop\apps\msys2\current"
)

$Msys2Root = $null
foreach ($path in $Msys2Paths) {
    if (Test-Path "$path\mingw64\bin\gcc.exe") {
        $Msys2Root = $path
        break
    }
}

if (-not $Msys2Root) {
    Write-Host "ERROR: MSYS2 with MINGW64 toolchain not found." -ForegroundColor Red
    Write-Host "Install MSYS2 and run: pacman -S mingw-w64-x86_64-toolchain" -ForegroundColor Yellow
    exit 1
}

$Mingw64Bin = "$Msys2Root\mingw64\bin"
Write-Host "Using MSYS2: $Msys2Root" -ForegroundColor Green

# Verify prerequisites
$wrapperFile = Join-Path $generatedDir "libredwg_wrap.c"
if (-not (Test-Path $wrapperFile)) {
    Write-Host "ERROR: libredwg_wrap.c not found. Run generate-bindings.ps1 first." -ForegroundColor Red
    exit 1
}

$libredwgDll = Join-Path $libDir "libredwg-0.dll"
if (-not (Test-Path $libredwgDll)) {
    Write-Host "ERROR: libredwg-0.dll not found at $libredwgDll" -ForegroundColor Red
    Write-Host "Build libredwg first using build-windows.ps1" -ForegroundColor Yellow
    exit 1
}

$libredwgA = Join-Path $libDir "libredwg.a"
if (-not (Test-Path $libredwgA)) {
    Write-Host "ERROR: libredwg.a not found at $libredwgA" -ForegroundColor Red
    Write-Host "Build libredwg first using build-windows.ps1" -ForegroundColor Yellow
    exit 1
}

$configH = Join-Path $buildSrcDir "config.h"
if (-not (Test-Path $configH)) {
    Write-Host "ERROR: config.h not found at $configH" -ForegroundColor Red
    Write-Host "Build libredwg first using build-windows.ps1" -ForegroundColor Yellow
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

Write-Host "`nBuilding native wrapper..." -ForegroundColor Yellow
Write-Host "  Source:  $wrapperFile"
Write-Host "  Include: $includeDir"
Write-Host "  Lib:     $libDir"
Write-Host "  Output:  $outputDir"

# The wrapper includes "../src/config.h" - relative to the generated folder
# Create a src folder as sibling to generated and copy config.h there
$localSrcDir = Join-Path $scriptDir "src"
if (-not (Test-Path $localSrcDir)) {
    New-Item -ItemType Directory -Path $localSrcDir | Out-Null
}
Copy-Item $configH $localSrcDir -Force
Write-Host "  Config:  $localSrcDir\config.h (copied from build)"

# Convert paths to MSYS2 format
function ConvertTo-MsysPath($winPath) {
    $winPath -replace '\\', '/' -replace '^([A-Za-z]):', '/$1'
}

$wrapperFileUnix = ConvertTo-MsysPath $wrapperFile
$includeDirUnix = ConvertTo-MsysPath $includeDir
$srcDirUnix = ConvertTo-MsysPath $srcDir
$buildSrcDirUnix = ConvertTo-MsysPath $buildSrcDir
$libDirUnix = ConvertTo-MsysPath $libDir
$outputDirUnix = ConvertTo-MsysPath $outputDir
$scriptDirUnix = ConvertTo-MsysPath $scriptDir

# Optimization flags
$optFlags = if ($Configuration -eq "Debug") { "-g -O0" } else { "-O2" }

# Build using gcc
$buildScript = @"
set -e
cd "$scriptDirUnix/generated"

gcc -shared $optFlags \
    -I"$includeDirUnix" \
    -I"$srcDirUnix" \
    -I"$buildSrcDirUnix" \
    -I"$scriptDirUnix" \
    "$wrapperFileUnix" \
    -L"$libDirUnix" \
    -lredwg \
    -o "$outputDirUnix/libredwg_csharp.dll"

# Copy libredwg-0.dll and runtime dependencies to output
cp "$libDirUnix/libredwg-0.dll" "$outputDirUnix/"
"@

$env:PATH = "$Mingw64Bin;$env:PATH"
$env:MSYSTEM = "MINGW64"

$MsysBin = "$Msys2Root\usr\bin"

Write-Host "`nRunning gcc..." -ForegroundColor Yellow
$buildScript | & "$MsysBin\bash.exe" -l

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "`nBuild complete!" -ForegroundColor Green
Write-Host "  Native DLL: $outputDir\libredwg_csharp.dll"
Write-Host "  LibreDWG:   $outputDir\libredwg.dll"
Write-Host "`nNext: dotnet build to create the managed assembly"
