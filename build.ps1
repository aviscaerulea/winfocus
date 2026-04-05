# vi: ts=4 sw=4 ff=unix fenc=utf-8 bomb
# winfocus ビルドスクリプト
# DevShell モジュール経由で VC++ ビルド環境を初期化し、cl でビルドする。

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "vswhere.exe が見つからない: $vswhere"; exit 1 }
$vsPath = & $vswhere -products '*' -latest -property installationPath
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

New-Item -ItemType Directory -Force -Path out | Out-Null

cl /nologo /O2 /W4 /utf-8 /Fo:out\ /Fe:out\winfocus.exe src\winfocus.c `
    user32.lib /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
if ($LASTEXITCODE) { exit 1 }
