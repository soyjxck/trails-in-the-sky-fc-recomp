# Unattended test run of the recompiled game.
#
# The window is created off screen and never activated, the mouse pointer the
# game moves is a private one, and Direct3D stays windowed (add -Visible to
# watch it instead). Each run gets a fresh copy of run\saves_base as its Saved
# Games folder, so the player's own saves and settings are never touched.
# Frames are saved from the back buffer to run\shots, and the log to
# run\trails.log.
#
#   tools\run_test.ps1 -Seconds 60
#   tools\run_test.ps1 -Seconds 120 -ShotEvery 60 -Extra '--calltrace run\calls.txt'
param(
    [int]$Seconds = 60,
    [int]$ShotEvery = 120,
    [switch]$Visible,
    [string]$Build = 'build',
    [string]$Extra = '',
    [int]$Tail = 40
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Exe = Join-Path $Root "$Build\trails.exe"
if (-not (Test-Path -LiteralPath $Exe)) { throw "no $Exe -- run build.cmd first" }
$Run = Join-Path $Root 'run'
$Saves = Join-Path $Run 'saves'
$Shots = Join-Path $Run 'shots'
$Log = Join-Path $Run 'trails.log'
New-Item -ItemType Directory -Force $Run | Out-Null
Remove-Item -LiteralPath $Saves, $Shots -Recurse -Force -ErrorAction SilentlyContinue
$base = Join-Path $Run 'saves_base'
if (Test-Path -LiteralPath $base) { Copy-Item -LiteralPath $base -Destination $Saves -Recurse }
else { New-Item -ItemType Directory -Force $Saves | Out-Null }

$argv = @('--run', '--active', '--savedir', $Saves, '--shots', $Shots, '--shot-every', $ShotEvery,
          '--watchdog', $Seconds, '--game', (Join-Path $Root 'Trails in the Sky FC'))
if (-not $Visible) { $argv += '--unattended' }
if ($Extra) { $argv += ($Extra -split ' ') }

# Native stderr through PowerShell 5.1 turns into error records, so the
# redirect is cmd's.
$quoted = ($argv | ForEach-Object { if ("$_" -match '\s') { '"' + $_ + '"' } else { "$_" } }) -join ' '
$started = Get-Date
cmd /c "`"$Exe`" $quoted > `"$Log`" 2>&1"
$code = $LASTEXITCODE
$took = [int]((Get-Date) - $started).TotalSeconds
$meaning = switch ($code) { 0 { 'the game left by itself' } 2 { 'a function that was not lifted' }
                            3 { 'fault' } 4 { 'watchdog: still running at the time limit' } default { '' } }
"exit code $code ($meaning) after $took s; log $Log"
$shots = @(Get-ChildItem -LiteralPath $Shots -Filter *.png -ErrorAction SilentlyContinue)
"$($shots.Count) frame(s) saved in $Shots"
Get-Content -LiteralPath $Log -Tail $Tail
