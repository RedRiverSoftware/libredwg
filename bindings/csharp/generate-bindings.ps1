<#
.SYNOPSIS
    Generates C# bindings for LibreDWG using SWIG.

.DESCRIPTION
    This script runs SWIG to generate C# wrapper code from the dwg.i interface file.
    It produces:
    - libredwg_wrap.c: Native C wrapper to be compiled into a DLL
    - *.cs files: C# proxy classes for the managed assembly

.NOTES
    Prerequisites:
    - SWIG 4.0+ installed (winget install SWIG.SWIG)
#>

param(
    [string]$SwigPath = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$bindingsDir = Split-Path -Parent $scriptDir
$includeDir = Join-Path (Split-Path -Parent $bindingsDir) "include"
$interfaceFile = Join-Path $bindingsDir "dwg.i"
$outputDir = Join-Path $scriptDir "generated"

Write-Host "LibreDWG C# Bindings Generator" -ForegroundColor Cyan
Write-Host "===============================" -ForegroundColor Cyan

# Find SWIG executable
function Find-Swig {
    # Check if provided path works
    if ($SwigPath -and (Test-Path $SwigPath)) {
        return $SwigPath
    }
    
    # Check PATH
    $inPath = Get-Command swig -ErrorAction SilentlyContinue
    if ($inPath) {
        return $inPath.Source
    }
    
    # Check WinGet installation location
    $wingetPath = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages"
    $swigDirs = Get-ChildItem -Path $wingetPath -Filter "SWIG*" -Directory -ErrorAction SilentlyContinue
    foreach ($dir in $swigDirs) {
        $swigExe = Get-ChildItem -Path $dir.FullName -Filter "swig.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($swigExe) {
            return $swigExe.FullName
        }
    }
    
    return $null
}

$SwigPath = Find-Swig
if (-not $SwigPath) {
    Write-Host "ERROR: SWIG not found. Install with: winget install SWIG.SWIG" -ForegroundColor Red
    exit 1
}

# Check SWIG version
$swigVersion = & $SwigPath -version 2>&1 | Select-String "SWIG Version"
Write-Host "Using: $swigVersion" -ForegroundColor Green

# Clean if requested
if ($Clean -and (Test-Path $outputDir)) {
    Write-Host "Cleaning generated files..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $outputDir
}

# Create output directory
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir | Out-Null
}

Write-Host "Generating C# bindings..." -ForegroundColor Yellow
Write-Host "  Interface: $interfaceFile"
Write-Host "  Include:   $includeDir"
Write-Host "  Output:    $outputDir"

# Run SWIG
# -csharp: Target language
# -namespace LibreDWG: Put all classes in this namespace
# -dllimport libredwg_csharp: Name of the native DLL to P/Invoke
# -outdir: Where to put .cs files
# -o: The C wrapper file
$swigArgs = @(
    "-csharp",
    "-namespace", "LibreDWGInterop",
    "-dllimport", "libredwg_csharp",
    "-I`"$includeDir`"",
    "-outdir", $outputDir,
    "-o", (Join-Path $outputDir "libredwg_wrap.c"),
    $interfaceFile
)

Write-Host "`nRunning: $SwigPath $($swigArgs -join ' ')" -ForegroundColor Gray

& $SwigPath @swigArgs

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: SWIG failed with exit code $LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
}

# Count generated files
$csFiles = Get-ChildItem -Path $outputDir -Filter "*.cs" | Measure-Object
$cFiles = Get-ChildItem -Path $outputDir -Filter "*.c" | Measure-Object

Write-Host "`nGeneration complete!" -ForegroundColor Green
Write-Host "  C# files:     $($csFiles.Count)"
Write-Host "  C wrapper:    $($cFiles.Count)"
Write-Host "`nNext steps:"
Write-Host "  1. Build native wrapper: .\build-native.ps1"
Write-Host "  2. Build managed DLL:    dotnet build"
