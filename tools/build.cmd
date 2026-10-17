@echo off
rem Configure and build the 32-bit host with MSVC + Ninja. The compiler itself is
rem the 64-bit-hosted one (amd64_x86): the 32-bit cl.exe runs out of heap on a
rem chunk of the full lift.
rem Needs Visual Studio 2022 or later (any edition, or the Build Tools) with the C++ x86 tools.
rem Run from anywhere: the build folder (BUILD_DIR, default build) is in the checkout.
setlocal
cd /d "%~dp0.."
rem vcvarsall.bat looks for vswhere.exe on PATH too, and says so when it is not.
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio was not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" amd64_x86 >nul || exit /b 1
if not defined BUILD_DIR set "BUILD_DIR=build"
if not defined BUILD_TYPE set "BUILD_TYPE=RelWithDebInfo"
if not exist %BUILD_DIR%\build.ninja cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %CMAKE_ARGS% || exit /b 1
cmake --build %BUILD_DIR% %*
