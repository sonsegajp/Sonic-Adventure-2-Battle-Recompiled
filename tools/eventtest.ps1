# Plays events (cutscenes) headless (always silent) with SA2_EVENT in place of the boot intro, a few
# at a time, until each ends (or -MaxFrames after eventD links). Per event: log, frames every
# -Every frames after eventD linked, and the audio. Then tools/stagereport.py --events summarises.
#   tools\eventtest.ps1 [-Only "0,2,100"] [-Parallel 3] [-Out build\eventtest] [-Every 300] [-MaxFrames 12000]
param(
    [string]$Only = "",
    [int]$Parallel = 3,
    [string]$Out = "build\eventtest",
    [int]$Every = 300,
    [int]$MaxFrames = 12000,
    [string]$Iso = "C:\Users\hyper\roms\GC\Sonic Adventure 2 - Battle (USA) (En,Ja,Fr,De,Es).iso"
)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)

# Real-time events on the disc (eNNNN.prs).
$events = @(0, 2, 3, 4, 5, 6, 11, 14, 15, 16, 17, 19, 21, 22, 24, 25, 26, 27, 28, 50, 51, 52, 53, 54,
    100, 101, 102, 103, 105, 106, 107, 109, 111, 112, 113, 116, 118, 119, 120, 122, 123, 124, 126, 127, 128,
    129, 130, 131, 150, 151, 152, 153, 154, 200, 201, 203, 204, 205, 206, 207, 208, 210, 211, 350, 360, 361, 399)
if ($Only) { $want = $Only -split ',' | ForEach-Object { [int]$_ }; $events = @($events | Where-Object { $want -contains $_ }) }

New-Item -ItemType Directory -Force $Out | Out-Null
$running = @()
foreach ($e in $events) {
    while (@($running | Where-Object { -not $_.HasExited }).Count -ge $Parallel) { Start-Sleep -Milliseconds 500 }
    $dir = Join-Path $Out ("ev{0:D4}" -f $e)
    New-Item -ItemType Directory -Force $dir | Out-Null
    Set-Content -Path (Join-Path $dir "name.txt") -Value ("event {0}" -f $e)
    $env:SA2_EVENT = "$e"
    $env:SA2_DUMP_DIR = $dir
    $env:SA2_DUMP_REL = "80"
    $env:SA2_DUMP_FROM = "120"
    $env:SA2_DUMP_EVERY = "$Every"
    $env:SA2_STOP_AFTER = "$MaxFrames"
    $env:SA2_STOP_ON_UNLINK = "80"
    $env:SA2_AUDIO_DUMP = (Join-Path $dir "audio.raw")
    $env:SA2_PRESS = "200:A,700:START"
    $p = Start-Process -FilePath .\build\windows-clang\sa2b.exe -PassThru -WindowStyle Hidden `
        -ArgumentList '--dol', 'orig/main.dol', '--iso', "`"$Iso`"", '--headless', '--frames', '30000' `
        -RedirectStandardOutput (Join-Path $dir "log.txt") -RedirectStandardError (Join-Path $dir "err.txt")
    $running += $p
    Write-Host ("started event {0}" -f $e)
}
foreach ($v in "SA2_EVENT", "SA2_DUMP_DIR", "SA2_DUMP_REL", "SA2_DUMP_FROM", "SA2_DUMP_EVERY", "SA2_STOP_AFTER", "SA2_STOP_ON_UNLINK", "SA2_AUDIO_DUMP", "SA2_PRESS") {
    Set-Item "Env:$v" -Value $null
}
$running | ForEach-Object { $_.WaitForExit() }
python tools\stagereport.py $Out --events
