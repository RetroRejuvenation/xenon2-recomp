# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Windows PowerShell version of tools/build_all.sh: (a) build x2recomp, (b) import your own Xenon 2 disk images,
# (c) optionally generate the statically recompiled code from the imported image and build the recompiled executable.
#
#   powershell -ExecutionPolicy Bypass -File tools\build_all.ps1 [-Recomp] [-Test] [-BuildDir DIR] [-DataDir DIR]
#              [DISK_IMAGE_OR_ZIP_OR_FOLDER ...]
#
#   (a) always: cmake configure + build into DIR (default build\), Visual Studio 2022 x64 unless -Generator is given;
#       SDL3 is downloaded and built (-DX2_FETCH_SDL3=ON) unless -SystemSdl3 is given (then set CMAKE_PREFIX_PATH)
#   (b) with images given: x2recomp --import ...  (into the per-user data directory, or -DataDir)
#   (c) -Recomp: needs Python 3 and a GCC-style C compiler for tools/recomp/recomp.py (set CC=clang or CC=gcc).
#       The translated code is derived from your game data: it stays in DIR (never commit or share it).
#   -Test: run ctest on the builds made.
# docs/BUILDING.md describes each step. This script has not been run on Windows by the project yet
# (docs/PORTING.md).
[CmdletBinding()]
param(
    [switch]$Recomp,
    [switch]$Test,
    [string]$BuildDir = "",
    [string]$DataDir = "",
    [string]$Generator = "",
    [switch]$SystemSdl3,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Images = @()
)
$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build" }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$Config = "Release"

function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

# Python 3: the py launcher (python.org installs), else python3 / python on PATH.
function Get-Python {
    foreach ($c in @(@("py", "-3"), @("python3"), @("python"))) {
        if (Get-Command $c[0] -ErrorAction SilentlyContinue) { return ,$c }
    }
    throw "Python 3 not found (install it from python.org or the Microsoft Store)"
}

function Get-Exe([string]$Dir) {
    foreach ($p in @((Join-Path $Dir "$Config\x2recomp.exe"), (Join-Path $Dir "x2recomp.exe"))) {
        if (Test-Path $p) { return $p }
    }
    throw "x2recomp.exe not found in $Dir"
}

function Invoke-Configure([string]$Dir, [string[]]$Extra) {
    $cfg = @("-S", $Root, "-B", $Dir)
    if ($Generator) { $cfg += @("-G", $Generator) } else { $cfg += @("-G", "Visual Studio 17 2022", "-A", "x64") }
    if (-not $SystemSdl3) { $cfg += "-DX2_FETCH_SDL3=ON" }
    $cfg += "-DCMAKE_BUILD_TYPE=$Config"
    Invoke-Checked "cmake" ($cfg + $Extra)
    Invoke-Checked "cmake" @("--build", $Dir, "--config", $Config, "--parallel")
}

Write-Host "== (a) build: $BuildDir"
Invoke-Configure $BuildDir @()
$Exe = Get-Exe $BuildDir

$DataArgs = @()
if ($DataDir) { $DataArgs = @("--data-dir", $DataDir) }
if ($Images.Count -gt 0) {
    Write-Host "== (b) import"
    & $Exe @DataArgs --import @Images
    if ($LASTEXITCODE -ne 0) {
        throw "import failed: see the messages above (exit code 2 = bad or missing disc, 3 = unsupported release/format)"
    }
}

# Where the imported assets are: x2recomp itself knows its per-user data directory (SDL_GetPrefPath).
if ($DataDir) { $Assets = Join-Path $DataDir "assets" }
else { $Assets = ((& $Exe --print-data-dir) | Select-Object -First 1) + "assets" }

$RecompDir = "$BuildDir-recomp"
if ($Recomp) {
    Write-Host "== (c) recompiled build from $Assets"
    if (-not (Test-Path (Join-Path $Assets "manifest.txt"))) {
        throw "no imported game data in ${Assets}: give your disk images to this script first"
    }
    $Py = Get-Python
    $PyArgs = @($Py | Select-Object -Skip 1)
    Invoke-Checked $Py[0] ($PyArgs + @("-I", (Join-Path $Root "tools\trace\run_corpus.py"), "--exe", $Exe,
        "--assets", $Assets, "--out", (Join-Path $BuildDir "x2-trace")))
    Invoke-Checked $Py[0] ($PyArgs + @("-I", (Join-Path $Root "tools\recomp\recomp.py"), "--no-lane", "--assets",
        $Assets, "--trace", (Join-Path $BuildDir "x2-trace"), "--out", (Join-Path $BuildDir "x2-recomp-src")))
    Invoke-Configure $RecompDir @("-DX2_RECOMP=ON", "-DX2_RECOMP_DIR=$(Join-Path $BuildDir 'x2-recomp-src')",
        "-DX2_ASSETS_DIR=$Assets")
}

if ($Test) {
    Write-Host "== tests"
    $env:SDL_VIDEO_DRIVER = "dummy"
    $env:SDL_AUDIO_DRIVER = "dummy"
    # Game-data tests use X2_ASSETS_DIR (default generated\assets); point them at the import if that is absent.
    if (-not (Test-Path (Join-Path $Root "generated\assets\game_0400.bin")) -and
        (Test-Path (Join-Path $Assets "game_0400.bin"))) {
        Invoke-Checked "cmake" @("-S", $Root, "-B", $BuildDir, "-DX2_ASSETS_DIR=$Assets")
    }
    Invoke-Checked "ctest" @("--test-dir", $BuildDir, "-C", $Config, "--output-on-failure")
    if ($Recomp) { Invoke-Checked "ctest" @("--test-dir", $RecompDir, "-C", $Config, "--output-on-failure") }
}

Write-Host "== done"
Write-Host "run: $Exe $($DataArgs -join ' ')"
if ($Recomp) { Write-Host "recompiled: $(Get-Exe $RecompDir) $($DataArgs -join ' ')" }
