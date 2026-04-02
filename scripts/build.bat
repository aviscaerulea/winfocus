@echo off
setlocal

REM Check if cl.exe is already available
where cl.exe >nul 2>&1
if not errorlevel 1 goto :build

REM Find Visual Studio installation
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: cl.exe not found and vswhere.exe not available.
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo ERROR: Visual Studio installation not found.
    exit /b 1
)

REM Setup Visual Studio environment
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo ERROR: Failed to set up Visual Studio environment.
    exit /b 1
)

:build
REM Create output directory
if not exist out mkdir out

REM Build
cl.exe /O2 /W4 /utf-8 /Fo:out\ /Fe:out\winfocus.exe src\winfocus.c user32.lib /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
