@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Place this file in the dumped game's USRDIR beside the standalone release
rem executable and launch it with Vulkan at the monitor's refresh rate.
rem Usage: run-taiko-high-refresh.bat [refresh-rate]
rem Example: run-taiko-high-refresh.bat 144

set "REFRESH_RATE=%~1"
if not defined REFRESH_RATE set "REFRESH_RATE=120"

for /f "delims=0123456789" %%D in ("!REFRESH_RATE!") do goto invalid_rate
if not "!REFRESH_RATE:~4,1!"=="" goto invalid_rate
if !REFRESH_RATE! LSS 60 goto invalid_rate
if !REFRESH_RATE! GTR 1000 goto invalid_rate

rem Argument validation is complete; disabling delayed expansion keeps paths
rem containing an exclamation mark intact.
setlocal DisableDelayedExpansion
set "SCRIPT_DIR=%~dp0"
set "TAIKO_EXE=%SCRIPT_DIR%taiko_boot.exe"
if not exist "%TAIKO_EXE%" set "TAIKO_EXE=%SCRIPT_DIR%TaikoRecomp.exe"

if not exist "%TAIKO_EXE%" (
    echo Missing taiko_boot.exe or TaikoRecomp.exe beside this launcher.
    exit /b 2
)

if not exist "%SCRIPT_DIR%dxcompiler.dll" (
    echo The SDL_GPU Windows runtime is missing.
    echo Copy dxcompiler.dll into this USRDIR directory.
    exit /b 2
)

if not exist "%SCRIPT_DIR%dxil.dll" (
    echo The SDL_GPU Windows runtime is missing.
    echo Copy dxil.dll into this USRDIR directory.
    exit /b 2
)

rem SDL_GPU settings. Vsync selects the monitor's active refresh mode.
set "TAIKO_GPU_DRIVER=vulkan"
set "TAIKO_PRESENT_MODE=vsync"
set "TAIKO_FRAMES_IN_FLIGHT=2"

rem Tick gameplay at the real display rate and correct frame-authored Lumen
rem animations so they retain their intended wall-clock duration above 60 Hz.
set "TAIKO_VBLANK_HZ=%REFRESH_RATE%"
set "TAIKO_ANIMATION_TIMING=1"

rem Preserve the normal accelerated boot. The runtime raises this to the play
rem rate automatically if a refresh rate above 240 Hz is requested.
set "TAIKO_BOOT_VBLANK_HZ=240"

echo Launching Taiko at %REFRESH_RATE% Hz using Vulkan...
pushd "%SCRIPT_DIR%"
"%TAIKO_EXE%"
set "EXIT_CODE=%ERRORLEVEL%"
popd

if not "%EXIT_CODE%"=="0" echo Taiko exited with code %EXIT_CODE%.
exit /b %EXIT_CODE%

:invalid_rate
echo Invalid refresh rate: "!REFRESH_RATE!"
echo Enter the monitor's active refresh rate from 60 through 1000 Hz.
echo Usage: %~nx0 [refresh-rate]
exit /b 2
