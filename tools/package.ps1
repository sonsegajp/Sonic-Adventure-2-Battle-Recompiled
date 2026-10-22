# Assembles a release from the build: dist\SA2BRecompiled\ (the game, the launcher, their libraries,
# the HD texture builder's scripts, readme and licences) and dist\SA2BRecompiled-<version>-windows-x64.zip.
# Nothing of the game's data goes in: the launcher and the game read the player's own disc.
#   tools\package.ps1 -Version 1.0.0
param([string]$Version = "1.0.0")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\windows-clang'
$dist = Join-Path $root 'dist'
$out = Join-Path $dist 'SA2BRecompiled'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force (Join-Path $out 'tools\hd') | Out-Null

foreach ($f in 'sa2b.exe', 'SA2BLauncher.exe') { Copy-Item (Join-Path $bin $f) $out }
Get-ChildItem $bin -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $out }
Copy-Item (Join-Path $root 'tools\prs.py') (Join-Path $out 'tools')
foreach ($f in 'build_pack.py', 'upscale.py', 'gvr.py', 'setup.ps1') { Copy-Item (Join-Path $root "tools\hd\$f") (Join-Path $out 'tools\hd') }
Copy-Item (Join-Path $root 'LICENSE.md') $out
Copy-Item (Join-Path $root 'res\THIRD_PARTY_NOTICES.txt') $out
Copy-Item (Join-Path $root 'res\README.txt') $out

$zip = Join-Path $dist "SA2BRecompiled-$Version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $out -DestinationPath $zip -CompressionLevel Optimal
Get-Item $zip | ForEach-Object { "{0} ({1:N0} MB)" -f $_.FullName, ($_.Length / 1MB) }
