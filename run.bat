@echo off
REM ServiceScope Launcher
setlocal

set "MINGW_BIN=%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\llvm-mingw-20260505-ucrt-x86_64\bin"
set "PATH=%MINGW_BIN%;%PATH%"

set PORT=%1
if "%PORT%"=="" set PORT=8080

set THREADS=%2
if "%THREADS%"=="" set THREADS=8

echo Starting ServiceScope on port %PORT% with %THREADS% threads...
echo Dashboard: http://localhost:%PORT%/
echo Press Ctrl+C to stop.
echo.

"%~dp0build\servicescope.exe" %PORT% %THREADS%
