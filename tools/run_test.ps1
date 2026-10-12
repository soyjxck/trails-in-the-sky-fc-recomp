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
#       let the game run that long on its title screen
#   tools\run_test.ps1 -Play tests\new_game.txt
#       play a script of button presses (host.c has the format); its own
#       `quit` ends the run, -Seconds is then only the limit
#   tools\run_test.ps1 -Play tests\new_game.txt -Sound -Movies
#       with music, sound effects and the logo and opening movies switched on
#       in the run's config.ini. Nothing is heard: an unattended run mutes
#       its own audio session (--silent).
#   tools\run_test.ps1 -Seconds 120 -Extra '--calltrace run\calls.txt'
param(
    [int]$Seconds = 60,
    [string]$Play = '',
    [int]$ShotEvery = 0,
    [switch]$Sound,
    [switch]$Movies,
    [switch]$Visible,
    [string]$Build = 'build',
    [string]$Extra = '',
    [int]$Tail = 25
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
# The base config has sound and the movies off; a run can ask for them.
$ini = Join-Path $Saves 'FALCOM\ED6\config.ini'
if (($Sound -or $Movies) -and (Test-Path -LiteralPath $ini)) {
    $lines = Get-Content -LiteralPath $ini
    if ($Sound) { $lines = $lines -replace '^(BgmOff|SeOff)=.*$', '$1=0' }
    if ($Movies) { $lines = $lines -replace '^(LogoPlayOff|OpeningPlayOff)=.*$', '$1=0' }
    Set-Content -LiteralPath $ini -Value $lines -Encoding ascii
}

if ($Play -and $Seconds -eq 60) { $Seconds = 600 }
if (-not $ShotEvery) { $ShotEvery = if ($Play) { 1000000 } else { 300 } }
$argv = @('--run', '--active', '--savedir', $Saves, '--shots', $Shots, '--shot-every', $ShotEvery,
          '--watchdog', $Seconds, '--game', (Join-Path $Root 'Trails in the Sky FC'))
if ($Play) { $argv += @('--script', (Resolve-Path -LiteralPath $Play).Path) }
if (-not $Visible) { $argv += @('--unattended', '--silent') }
if ($Extra) { $argv += ($Extra -split ' ') }

# Native stderr through PowerShell 5.1 turns into error records, so the
# redirect is cmd's.
$quoted = ($argv | ForEach-Object { if ("$_" -match '\s') { '"' + $_ + '"' } else { "$_" } }) -join ' '
$started = Get-Date
cmd /c "`"$Exe`" $quoted > `"$Log`" 2>&1"
$code = $LASTEXITCODE
$took = [int]((Get-Date) - $started).TotalSeconds
$text = Get-Content -LiteralPath $Log -Raw
$verdict = if ($text -match '=== fault') { 'FAULT' }
           elseif ($text -match '\[not-lifted\]') { 'NOT LIFTED' }
           elseif ($text -match 'nothing in the guest catches') { 'UNCAUGHT THROW' }
           elseif ($text -match 'I(CALL|TAIL): unresolved') { 'UNRESOLVED CALL' }
           elseif ($Play -and $text -match '\[script\] quit' -and $text -match '\[exit\] ExitProcess') { 'ok: the script played to its end and the game left' }
           elseif ($Play) { 'the script did not finish' }
           elseif ($code -eq 4) { 'ok: still running at the time limit' }
           else { 'the game left early' }
"$verdict (exit code $code after $took s); log $Log"
$shots = @(Get-ChildItem -LiteralPath $Shots -Filter *.png -ErrorAction SilentlyContinue)
"$($shots.Count) frame(s) saved in $Shots"
Get-Content -LiteralPath $Log -Tail $Tail
