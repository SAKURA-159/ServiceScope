@echo off
REM ServiceScope - Windows build script
REM Prerequisites: CMake 3.16+, Visual Studio 2019+ or MinGW-w64
REM
REM Usage:
REM   build.bat          — Build with default generator
REM   build.bat mingw    — Build with MinGW Makefiles
REM   build.bat ninja    — Build with Ninja

setlocal enabledelayedexpansion

set BUILD_DIR=build
set GENERATOR=

if "%1"=="mingw" (
    set GENERATOR=-G "MinGW Makefiles"
) else if "%1"=="ninja" (
    set GENERATOR=-G "Ninja"
)

echo === ServiceScope Build ===
echo.

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"

echo [1/2] Running CMake...
cmake .. %GENERATOR% -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo CMake configuration failed!
    exit /b 1
)

echo.
echo [2/2] Building...
cmake --build . --config Release --parallel
if errorlevel 1 (
    echo Build failed!
    exit /b 1
)

echo.
echo === Build complete ===
echo Binary: build\Release\servicescope.exe   (MSVC)
echo      or: build\servicescope.exe          (MinGW/Ninja)
echo.
echo Run: servicescope.exe [port] [threads]
echo.
