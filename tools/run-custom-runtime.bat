@echo off
setlocal EnableExtensions

rem Starts the self-built COT runtime (runtime\build-smoke\cot-runtime.exe) with
rem the command line and environment the MojoRecomp Launcher uses, against the
rem game library, settings, saves and log folders the launcher already manages.
rem Versions > Install Runtime from ZIP accepts only officially signed packages,
rem so a local build is started this way instead.
rem
rem Usage:
rem   tools\run-custom-runtime.bat "<game folder>" [frame_rate] ["<game overlay>"]
rem     game folder  the installed game, for example
rem                  %USERPROFILE%\Games\MojoRecomp-Games\cot
rem     frame_rate   30, 60, 120, 144, 165, uncapped or 61-300 (default 60)
rem     game overlay only for a Localization Pack profile; same as the launcher
rem
rem Extra MOJORECOMP_* variables set before running (for example
rem MOJORECOMP_HFR_DISCOVERY=1 or MOJORECOMP_SNAPSHOT_HOT_PAGES=1) are passed
rem through. The runtime log is written to the launcher's log folder. Options the
rem launcher has no field for (docs\HFR-PHASE4.md): MOJORECOMP_MOTION_BLUR=0,
rem MOJORECOMP_UPSCALER=fsr1 or rcas, MOJORECOMP_FSR_SHARPNESS=0..1,
rem MOJORECOMP_GUEST_PROFILE=1 or all, and MOJORECOMP_GPU_PROFILE=1.

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "RUNTIME_DIR=%ROOT%\runtime\build-smoke"
set "RUNTIME=%RUNTIME_DIR%\cot-runtime.exe"
set "DXC_DIR=%ROOT%\thirdparty\XenosRecomp\thirdparty\dxc-bin\bin\x64"
set "GAME_ROOT=%~1"
set "FRAME_RATE=%~2"
set "GAME_OVERLAY=%~3"
if "%FRAME_RATE%"=="" set "FRAME_RATE=60"

if "%GAME_ROOT%"=="" goto :usage
call :require "%GAME_ROOT%\default.xex" "Game executable in the game folder"
if errorlevel 1 exit /b 1
call :require "%RUNTIME%" "Self-built runtime (run tools\recompile.bat and build-smoke.bat)"
if errorlevel 1 exit /b 1
call :require "%RUNTIME_DIR%\mojorecomp-lzx.dll" "Runtime LZX library"
if errorlevel 1 exit /b 1
call :require "%RUNTIME_DIR%\mojorecomp-ffmpeg.dll" "Runtime FFmpeg library"
if errorlevel 1 exit /b 1
call :require "%DXC_DIR%\dxcompiler.dll" "DXC shader compiler"
if errorlevel 1 exit /b 1
call :require "%DXC_DIR%\dxil.dll" "DXIL support library"
if errorlevel 1 exit /b 1

rem Same roots as the launcher's storage layout: Local AppData\MojoRecomp and
rem the Windows Saved Games known folder (which can be relocated).
set "LOCAL_ROOT=%LOCALAPPDATA%\MojoRecomp"
set "SAVED_GAMES="
for /f "tokens=2,*" %%A in ('reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders" /v "{4C5C32FF-BB9D-43b0-B5B4-2D72E54EAAA4}" 2^>nul ^| find "REG_"') do set "SAVED_GAMES=%%B"
rem The value is REG_EXPAND_SZ (normally %USERPROFILE%\Saved Games); CALL
rem expands the variables it contains.
if defined SAVED_GAMES call set "SAVED_GAMES=%SAVED_GAMES%"
if not defined SAVED_GAMES set "SAVED_GAMES=%USERPROFILE%\Saved Games"
set "SETTINGS=%LOCAL_ROOT%\titles\cot\settings.toml"
set "LOG_ROOT=%LOCAL_ROOT%\logs\cot"
call :require "%SETTINGS%" "Launcher settings (open Game Settings in the launcher once and Save)"
if errorlevel 1 exit /b 1
if not exist "%LOG_ROOT%" mkdir "%LOG_ROOT%"
if not exist "%LOG_ROOT%" exit /b 1

set "DISABLE_VULKAN_OBS_CAPTURE=1"
set "MOJORECOMP_VULKAN_PRESENT=1"
set "MOJORECOMP_SAVE_ROOT=%SAVED_GAMES%\MojoRecomp\cot"
set "MOJORECOMP_CONTENT_ROOT=%LOCAL_ROOT%\titles\cot\content"
set "MOJORECOMP_CACHE_ROOT=%LOCAL_ROOT%\cache\cot"
set "MOJORECOMP_UTILITY_ROOT=%LOCAL_ROOT%\titles\cot\utility"
set "MOJORECOMP_LOG_ROOT=%LOG_ROOT%"
set "MOJORECOMP_FRAME_RATE=%FRAME_RATE%"
set "MOJORECOMP_FRAME_RATE_TARGET="
set "MOJORECOMP_DXC_LIB=%DXC_DIR%\dxcompiler.dll"
set "PATH=%RUNTIME_DIR%;%DXC_DIR%;%PATH%"

set "RUNTIME_LOG=%LOG_ROOT%\runtime.log"
> "%RUNTIME_LOG%" echo === MojoRecomp Runtime Log v1 ===
if errorlevel 1 exit /b 1
>> "%RUNTIME_LOG%" echo launched_by=tools\run-custom-runtime.bat
>> "%RUNTIME_LOG%" echo runtime=%RUNTIME%
>> "%RUNTIME_LOG%" echo game_root=%GAME_ROOT%
>> "%RUNTIME_LOG%" echo frame_rate=%FRAME_RATE%
rem Empty means unset: settings.toml, custom-settings.toml or the default applies.
>> "%RUNTIME_LOG%" echo env motion_blur=%MOJORECOMP_MOTION_BLUR% depth_of_field=%MOJORECOMP_DEPTH_OF_FIELD% upscaler=%MOJORECOMP_UPSCALER% fsr_sharpness=%MOJORECOMP_FSR_SHARPNESS% guest_profile=%MOJORECOMP_GUEST_PROFILE% gpu_profile=%MOJORECOMP_GPU_PROFILE%
>> "%RUNTIME_LOG%" echo --- runtime output ---

echo Starting the self-built runtime at frame rate %FRAME_RATE%.
echo Log: %RUNTIME_LOG%
pushd "%RUNTIME_DIR%"
if errorlevel 1 exit /b 1
if "%GAME_OVERLAY%"=="" goto :run_without_overlay
"%RUNTIME%" --cpu --config "%SETTINGS%" --game-root "%GAME_ROOT%" --game-overlay "%GAME_OVERLAY%" >> "%RUNTIME_LOG%" 2>&1
goto :finished
:run_without_overlay
"%RUNTIME%" --cpu --config "%SETTINGS%" --game-root "%GAME_ROOT%" >> "%RUNTIME_LOG%" 2>&1
:finished
set "EXIT_CODE=%ERRORLEVEL%"
popd
echo Runtime exited with code %EXIT_CODE%.
exit /b %EXIT_CODE%

:usage
echo Usage: %~nx0 "<game folder>" [frame_rate] ["<game overlay>"]
exit /b 2

:require
if exist "%~1" exit /b 0
echo ERROR: %~2 is missing: %~1
exit /b 1
