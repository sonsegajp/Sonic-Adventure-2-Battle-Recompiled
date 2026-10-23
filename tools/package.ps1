# Assembles a release from the build: dist\SA2BRecompiled\ (the runtime, the launcher, their
# libraries, the bundled compiler, readme and licences) and
# dist\SA2BRecompiled-<version>-windows-x64.zip.
# Nothing of the game goes in - neither its data nor its code, translated or not: the launcher builds
# sa2b_game.dll from the player's own disc at first run (docs/SETUP.md), and the game reads the disc.
#   tools\package.ps1 -Version 1.1.0
# Needs build\llvm\clang.exe, lld-link.exe and LICENSE.TXT: from the official LLVM 19.1.5 release
# (LLVM-19.1.5-win64.exe, unpacked with 7-Zip: bin\clang.exe, bin\lld-link.exe; llvm/LICENSE.TXT).
param([string]$Version = "1.1.0")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\windows-clang'
$llvm = Join-Path $root 'build\llvm'
$dist = Join-Path $root 'dist'
$out = Join-Path $dist 'SA2BRecompiled'
foreach ($f in 'clang.exe', 'lld-link.exe', 'LICENSE.TXT') {
    if (-not (Test-Path (Join-Path $llvm $f))) { throw "build\llvm\$f is missing (see the top of this script)" }
}
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force (Join-Path $out 'tools\llvm') | Out-Null

# the runtime and its import library (the game module links against it), the launcher
foreach ($f in 'sa2b.exe', 'sa2b.lib', 'SA2BLauncher.exe') { Copy-Item (Join-Path $bin $f) $out }
# their libraries - never the developer build's game module
Get-ChildItem $bin -Filter *.dll | Where-Object { $_.Name -ne 'sa2b_game.dll' } | ForEach-Object { Copy-Item $_.FullName $out }
foreach ($f in 'clang.exe', 'lld-link.exe', 'LICENSE.TXT') { Copy-Item (Join-Path $llvm $f) (Join-Path $out 'tools\llvm') }
Copy-Item (Join-Path $root 'tools\prs.py') (Join-Path $out 'tools')
Copy-Item (Join-Path $root 'LICENSE.md') $out
Copy-Item (Join-Path $root 'res\THIRD_PARTY_NOTICES.txt') $out
Copy-Item (Join-Path $root 'res\README.txt') $out

# nothing translated from the game, nothing generated from it
$bad = Get-ChildItem $out -Recurse -File | Where-Object { $_.Name -match '^sa2b_game\.|\.obj$|^dol_|^rel_' }
if ($bad) { throw "game code in the package: $($bad.Name -join ', ')" }

$zip = Join-Path $dist "SA2BRecompiled-$Version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# (Windows' tar writes a standard zip; PowerShell 5.1's Compress-Archive stores backslashes in
# entry names, which other unzip tools turn into flat files named 'SA2BRecompiled\sa2b.exe')
& (Join-Path $env:SystemRoot 'System32\tar.exe') -a -c -f $zip -C $dist SA2BRecompiled
if ($LASTEXITCODE) { throw 'tar failed' }
Get-Item $zip | ForEach-Object { "{0} ({1:N0} MB)" -f $_.FullName, ($_.Length / 1MB) }
