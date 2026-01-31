@echo off
where cl.exe >nul 2>&1
if not errorlevel 1 goto :build

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

call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

:build
cl.exe /O2 /W4 /utf-8 /Fe:winfocus.exe winfocus.c user32.lib psapi.lib shell32.lib
