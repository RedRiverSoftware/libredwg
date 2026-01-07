<#
.SYNOPSIS
    Builds the native C# wrapper DLL for LibreDWG.

.DESCRIPTION
    Compiles the SWIG-generated libredwg_wrap.c into a native DLL (libredwg_csharp.dll)
    that the C# managed code will P/Invoke into.

.NOTES
    Prerequisites:
    - Visual Studio Build Tools with C++ workload
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
$buildSrcDir = Join-Path $libredwgRoot ".build\src"  # Contains config.h
$srcDir = Join-Path $libredwgRoot "src"
$libDir = Join-Path (Split-Path -Parent $libredwgRoot) "bin\windows"
$outputDir = Join-Path $scriptDir "bin\$Configuration"

Write-Host "LibreDWG C# Native Wrapper Builder" -ForegroundColor Cyan
Write-Host "===================================" -ForegroundColor Cyan

# Verify prerequisites
$wrapperFile = Join-Path $generatedDir "libredwg_wrap.c"
if (-not (Test-Path $wrapperFile)) {
    Write-Host "ERROR: libredwg_wrap.c not found. Run generate-bindings.ps1 first." -ForegroundColor Red
    exit 1
}

$libredwgLib = Join-Path $libDir "libredwg.lib"
if (-not (Test-Path $libredwgLib)) {
    Write-Host "ERROR: libredwg.lib not found at $libredwgLib" -ForegroundColor Red
    Write-Host "Build libredwg first using build-windows.ps1" -ForegroundColor Yellow
    exit 1
}

$configH = Join-Path $buildSrcDir "config.h"
if (-not (Test-Path $configH)) {
    Write-Host "ERROR: config.h not found at $configH" -ForegroundColor Red
    Write-Host "Build libredwg first using build-windows.ps1" -ForegroundColor Yellow
    exit 1
}

# Find Visual Studio vcvars64.bat
function Find-VCVars {
    # Common locations to search
    $searchPaths = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
    
    foreach ($path in $searchPaths) {
        if (Test-Path $path) {
            return $path
        }
    }
    
    # Try vswhere as fallback
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $installations = & $vsWhere -all -property installationPath
        foreach ($vsPath in $installations) {
            $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vcvars) {
                return $vcvars
            }
        }
    }
    
    return $null
}

$vcvarsPath = Find-VCVars
if (-not $vcvarsPath) {
    Write-Host "ERROR: vcvars64.bat not found. Install Visual Studio Build Tools with C++ workload." -ForegroundColor Red
    exit 1
}

Write-Host "Using: $vcvarsPath" -ForegroundColor Green

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

# Build using cl.exe
$buildScript = @"
@echo off
call "$vcvarsPath"
cd /d "$generatedDir"

cl.exe /LD /MD /O2 ^
    /I"$includeDir" ^
    /I"$srcDir" ^
    /I"$buildSrcDir" ^
    "$wrapperFile" ^
    /Fe:"$outputDir\libredwg_csharp.dll" ^
    /link /LIBPATH:"$libDir" libredwg.lib

if errorlevel 1 exit /b 1

:: Copy libredwg.dll to output
copy "$libDir\libredwg.dll" "$outputDir\" >nul
"@

$tempDir = [System.IO.Path]::GetTempPath()
$buildScriptPath = Join-Path $tempDir ("libredwg_build_{0}.bat" -f [guid]::NewGuid().ToString("N"))
$buildScript | Out-File -FilePath $buildScriptPath -Encoding ascii

try {
    & cmd.exe /c $buildScriptPath
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
} finally {
    Remove-Item -Force $buildScriptPath -ErrorAction SilentlyContinue
    # Clean up obj files from generated directory
    Remove-Item -Force (Join-Path $generatedDir "*.obj") -ErrorAction SilentlyContinue
}

Write-Host "`nBuild complete!" -ForegroundColor Green
Write-Host "  Native DLL: $outputDir\libredwg_csharp.dll"
Write-Host "  LibreDWG:   $outputDir\libredwg.dll"
Write-Host "`nNext: dotnet build to create the managed assembly"
