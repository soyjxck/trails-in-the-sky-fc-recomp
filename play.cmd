@echo off
rem Play the recompiled game, with your own saves and settings.
rem What it says while it runs goes to run\play.log; if it stops, that file is
rem what to look at. Any options are passed on (build\trails.exe --help).
setlocal
cd /d "%~dp0"
if not exist build\trails.exe (echo build\trails.exe is not built yet: see README.md & exit /b 1)
if not exist run mkdir run
build\trails.exe --run %* > run\play.log 2>&1
echo exit code %errorlevel%; the log is run\play.log
