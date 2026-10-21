# Runs stages headless (always silent), a few at a time: Story mode is started from the title (Hero,
# no memory card), its events skipped, and the stage it loads replaced by the one under test
# (SA2_STAGE: level, character); the player then holds forward and jumps. Collects per stage: log,
# frames at 300/900/1500/2100 frames after the stage module links, and the audio. Then
# tools/stagereport.py summarises. Usage:
#   tools\stagetest.ps1 [-Only "3,13,22"] [-Parallel 3] [-Out build\stagetest]
param(
    [string]$Only = "",  # comma-separated levels (a string: -File does not parse arrays)
    [int]$Parallel = 3,
    [string]$Out = "build\stagetest",
    [string]$Iso = "C:\Users\hyper\roms\GC\Sonic Adventure 2 - Battle (USA) (En,Ja,Fr,De,Es).iso"
)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)

# level, character (0 Sonic, 1 Shadow, 4 Knuckles, 5 Rouge, 6 Tails mech, 7 Eggman mech), name
$stages = @(
    @(0, 0, "Basic Test", 2), @(3, 0, "Green Forest"), @(4, 1, "White Jungle"), @(5, 4, "Pumpkin Hill"),
    @(6, 1, "Sky Rail"), @(7, 4, "Aquatic Mine"), @(8, 5, "Security Hall"), @(9, 6, "Prison Lane"),
    @(10, 0, "Metal Harbor"), @(11, 7, "Iron Gate"), @(12, 7, "Weapons Bed"), @(13, 0, "City Escape"),
    @(14, 1, "Radical Highway"), @(15, 7, "Weapons Bed 2P"), @(16, 4, "Wild Canyon"), @(17, 6, "Mission Street"),
    @(18, 5, "Dry Lagoon"), @(19, 0, "Sonic vs Shadow 1"), @(20, 6, "Tails vs Eggman 1"), @(21, 7, "Sand Ocean"),
    @(22, 0, "Crazy Gadget"), @(23, 6, "Hidden Base"), @(24, 6, "Eternal Engine"), @(25, 4, "Death Chamber"),
    @(26, 5, "Egg Quarters"), @(27, 7, "Lost Colony"), @(28, 0, "Pyramid Cave"), @(29, 7, "Tails vs Eggman 2"),
    @(30, 0, "Final Rush"), @(31, 0, "Green Hill"), @(32, 4, "Meteor Herd"), @(33, 4, "Knuckles vs Rouge"),
    @(34, 0, "Cannon's Core (Sonic)"), @(35, 7, "Cannon's Core (Eggman)"), @(36, 6, "Cannon's Core (Tails)"),
    @(37, 5, "Cannon's Core (Rouge)"), @(38, 4, "Cannon's Core (Knuckles)"), @(39, 6, "Mission Street 2P"),
    @(40, 1, "Final Chase"), @(41, 4, "Wild Canyon 2P"), @(42, 1, "Sonic vs Shadow 2"), @(43, 7, "Cosmic Wall"),
    @(44, 5, "Mad Space"), @(45, 7, "Sand Ocean 2P"), @(46, 5, "Dry Lagoon 2P"), @(47, 0, "Pyramid Race"),
    @(48, 6, "Hidden Base 2P"), @(49, 4, "Pool Quest"), @(50, 5, "Planet Quest"), @(51, 6, "Deck Race"),
    @(52, 0, "Downtown Race"), @(53, 7, "Cosmic Wall 2P"), @(54, 0, "Grind Race"), @(55, 7, "Lost Colony 2P"),
    @(56, 6, "Eternal Engine 2P"), @(57, 0, "Metal Harbor 2P"), @(58, 7, "Iron Gate 2P"), @(59, 4, "Death Chamber 2P"),
    # bosses and karts: their own modules (4th field: module id)
    @(60, 0, "Big Foot", 82), @(61, 1, "Hot Shot", 83), @(62, 6, "Flying Dog", 84), @(63, 4, "King Boom Boo", 85),
    @(64, 0, "Egg Golem (Sonic)", 87), @(65, 1, "Biolizard", 86), @(66, 9, "Finalhazard", 89),
    @(67, 7, "Egg Golem (Eggman)", 88), @(70, 2, "Route 101", 81), @(71, 5, "Route 280", 81)
)
if ($Only) {  # (a pipeline would flatten a single selected stage into its fields)
    $want = $Only -split ',' | ForEach-Object { [int]$_ }
    $sel = New-Object System.Collections.ArrayList
    foreach ($s in $stages) { if ($want -contains $s[0]) { [void]$sel.Add($s) } }
    $stages = $sel
}

New-Item -ItemType Directory -Force $Out | Out-Null
# memory card warning, title START, then A every 200 frames: 1P PLAY, STORY, HERO, ... and jumps in the stage
$p = @("200:A", "700:START", "2400:START"); for ($f = 2800; $f -le 14000; $f += 200) { $p += "${f}:A" }
$press = $p -join ","
$running = @()
foreach ($s in $stages) {
    while (@($running | Where-Object { -not $_.HasExited }).Count -ge $Parallel) { Start-Sleep -Milliseconds 500 }
    $dir = Join-Path $Out ("stg{0:D2}" -f $s[0])
    $module = if ($s.Count -gt 3) { $s[3] } else { $s[0] }
    New-Item -ItemType Directory -Force $dir | Out-Null
    Set-Content -Path (Join-Path $dir "name.txt") -Value $s[2]
    Set-Content -Path (Join-Path $dir "module.txt") -Value $module
    $env:SA2_STAGE = "$($s[0]):$($s[1])"
    $env:SA2_DUMP_DIR = $dir
    $env:SA2_DUMP_REL = "$module"
    $env:SA2_DUMP_FROM = "300"
    $env:SA2_DUMP_EVERY = "600"
    $env:SA2_STOP_AFTER = "2101"
    $env:SA2_AUDIO_DUMP = (Join-Path $dir "audio.raw")
    $env:SA2_PRESS = $press
    $env:SA2_SKIP_EVENTS = "1"
    $env:SA2_RUN = "1"
    $p = Start-Process -FilePath .\build\windows-clang\sa2b.exe -PassThru -WindowStyle Hidden `
        -ArgumentList '--dol', 'orig/main.dol', '--iso', "`"$Iso`"", '--headless', '--frames', '14000' `
        -RedirectStandardOutput (Join-Path $dir "log.txt") -RedirectStandardError (Join-Path $dir "err.txt")
    $running += $p
    Write-Host ("started {0,2} {1}" -f $s[0], $s[2])
}
foreach ($v in "SA2_STAGE", "SA2_DUMP_DIR", "SA2_DUMP_REL", "SA2_DUMP_FROM", "SA2_DUMP_EVERY", "SA2_STOP_AFTER", "SA2_AUDIO_DUMP", "SA2_PRESS", "SA2_SKIP_EVENTS", "SA2_RUN") {
    Set-Item "Env:$v" -Value $null
}
$running | ForEach-Object { $_.WaitForExit() }
python tools\stagereport.py $Out
