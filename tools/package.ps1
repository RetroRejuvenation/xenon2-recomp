# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Windows release package from an existing build (macOS/Linux: tools/package.sh). docs/RELEASING.md.
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-BuildDir DIR] [-OutDir DIR] [-Config Release]
#
# Produces dist\x2recomp-<version>-windows-<arch>.zip with x2recomp.exe, SDL3.dll, LICENSE, README.md,
# THIRD-PARTY-NOTICES.md, PROVENANCE.md, BUILDING.md, ASSET-PACKS.md, ASSET-PACKS-REFERENCE.md, asset-packs\ (tutorials), PORTING.md, DUMPING.md, licenses\ and SHA256SUMS.txt,
# plus a .sha256 file of the zip. The build must be an
# interpreter-only build (a -DX2_RECOMP=ON build links code translated from game data and is refused). Packages
# never contain game data, imported assets, generated/translated code or ROMs (checked below).
# `cmake` is called by name: make sure the one first on PATH is 3.29 or newer (docs/BUILDING.md).
[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build" }
if (-not $OutDir) { $OutDir = Join-Path $Root "dist" }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

$Cache = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Test-Path $Cache)) { throw "$BuildDir is not a configured build directory" }
function Get-CacheValue([string]$Name) {
    $line = Select-String -Path $Cache -Pattern "^${Name}:[A-Z]*=(.*)$" | Select-Object -First 1
    if ($line) { return $line.Matches[0].Groups[1].Value } else { return "" }
}
if ((Get-CacheValue "X2_RECOMP") -match "^(ON|1|TRUE|YES)$") {
    throw "$BuildDir is a -DX2_RECOMP=ON build: it contains code translated from game data and must not be packaged"
}
$Version = Get-CacheValue "CMAKE_PROJECT_VERSION"
$Arch = if ([Environment]::Is64BitOperatingSystem) { "x64" } else { "x86" }
if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { $Arch = "arm64" }
$Name = "x2recomp-$Version-windows-$Arch"

$Stage = Join-Path ([System.IO.Path]::GetTempPath()) ("x2pkg-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Stage | Out-Null
try {
    & cmake --build $BuildDir --config $Config
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    # Windows install layout (CMakeLists.txt): bin\x2recomp.exe, bin\SDL3.dll, documents and licenses\ at the top.
    & cmake --install $BuildDir --config $Config --prefix (Join-Path $Stage "install")
    if ($LASTEXITCODE -ne 0) { throw "install failed" }
    $Install = Join-Path $Stage "install"
    # Every third-party component that is compiled in ships its licence text (packaging\licenses\README.txt lists them).
    foreach ($f in @("README.txt", "Musashi-LICENSE.txt", "puff-LICENSE.txt", "Unscii-LICENSE.txt", "dr_flac-LICENSE.txt", "stb_vorbis-LICENSE.txt")) {
        if (-not (Test-Path (Join-Path $Install "licenses\$f"))) { throw "the install lacks licenses\$f (the install(FILES packaging/licenses/...) list in CMakeLists.txt)" }
    }
    $Pkg = Join-Path $Stage $Name
    New-Item -ItemType Directory -Path $Pkg | Out-Null
    Copy-Item (Join-Path $Install "bin\*") $Pkg -Recurse
    Get-ChildItem $Install -File | Copy-Item -Destination $Pkg
    Copy-Item (Join-Path $Install "licenses") (Join-Path $Pkg "licenses") -Recurse
    if (-not (Test-Path (Join-Path $Pkg "x2recomp.exe"))) { throw "x2recomp.exe missing from the install" }
    if (-not (Test-Path (Join-Path $Pkg "SDL3.dll"))) {
        Write-Warning "SDL3.dll is not in the package (static or missing SDL3): the window mode needs it"
    }

    # Nothing derived from game data, no disk/CD images, audio dumps or translated code.
    $Forbidden = @("*.adf", "*.adz", "*.dms", "*.ipf", "*.zip", "*.iso", "*.bin", "*.cue", "*.rom", "*.wav", "*.pcm",
        "*.ppm", "x2r_*", "x2rc_*", "x2rs_*", "manifest.txt", "settings.cfg", "input.cfg")
    $Bad = Get-ChildItem $Pkg -Recurse -File -Include $Forbidden
    if ($Bad -or (Test-Path (Join-Path $Pkg "generated"))) {
        throw "package would contain forbidden files: $($Bad.FullName -join ', ')"
    }

    # SHA-256 manifest of the contents (sha256sum format, forward slashes).
    $Lines = Get-ChildItem $Pkg -Recurse -File | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($Pkg.Length + 1).Replace("\", "/")
        "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $rel
    }
    [System.IO.File]::WriteAllText((Join-Path $Pkg "SHA256SUMS.txt"), (($Lines -join "`n") + "`n"))

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $Zip = Join-Path $OutDir "$Name.zip"
    if (Test-Path $Zip) { Remove-Item $Zip }
    # Not Compress-Archive: Windows PowerShell 5.1 writes backslash entry names, which the zip format forbids
    # (macOS and Linux unzip warn and flatten the paths). Write the entries with forward slashes ourselves.
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $PkgRoot = (Get-Item $Pkg).FullName
    $PkgName = (Get-Item $Pkg).Name
    $Out = [System.IO.File]::Open($Zip, [System.IO.FileMode]::Create)
    try {
        $Archive = New-Object System.IO.Compression.ZipArchive($Out, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            Get-ChildItem $PkgRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
                $entry = $PkgName + "/" + $_.FullName.Substring($PkgRoot.Length + 1).Replace("\", "/")
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $Archive, $_.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal)
            }
        }
        finally { $Archive.Dispose() }
    }
    finally { $Out.Dispose() }
    $Hash = (Get-FileHash $Zip -Algorithm SHA256).Hash.ToLower()
    [System.IO.File]::WriteAllText("$Zip.sha256", "$Hash  $Name.zip`n")
    Write-Host "package: $Zip"
    Write-Host "$Hash  $Name.zip"
}
finally {
    Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
}
