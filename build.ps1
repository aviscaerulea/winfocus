# vim: set ft=ps1 fenc=utf-8 ff=unix sw=4 ts=4 et :
# ==================================================
# winfocus ビルドスクリプト
# MSVC cl.exe で src/winfocus.c をコンパイルし out/winfocus.exe を生成する
#
# 引数:
#   -Version  : バージョン文字列（例: 1.0.0）
#   -Config   : Debug | Release（デフォルト: Debug）
# ==================================================
param(
    [string]$Version = "0.0.0",
    [string]$Config  = "Debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# VS 開発環境をロード（公式 DLL モジュール方式、Build Tools 対応）
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "vswhere.exe が見つからない: $vswhere"; exit 1 }
$vsPath = & $vswhere -products '*' -latest -property installationPath
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path $devShellDll)) { Write-Error "DevShell.dll が見つからない: $devShellDll"; exit 1 }
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

# 出力ディレクトリ作成
if (-not (Test-Path "out")) { New-Item -ItemType Directory -Path "out" | Out-Null }

# コンパイルオプション
$commonFlags = @(
    "/nologo", "/utf-8",
    "/W4",
    "/D_WIN32_WINNT=0x0A00"  # Windows 10/11 対象
)

$debugFlags   = @("/Zi", "/Od", "/DDEBUG", "/MTd")
$releaseFlags = @("/O2", "/DNDEBUG", "/MT")

$configFlags = if ($Config -eq "Release") { $releaseFlags } else { $debugFlags }

# リンクライブラリ
$libs = @("user32.lib")

# リンクオプション
$linkFlags = @(
    "/SUBSYSTEM:WINDOWS",
    "/ENTRY:mainCRTStartup"
)
if ($Config -eq "Debug") { $linkFlags += "/DEBUG" }

$outExe = "out\winfocus.exe"

Write-Host "Building $outExe ($Config, v$Version)..."

$clArgs = $commonFlags + $configFlags + @("src\winfocus.c", "/Fe:$outExe", "/Fo:out\") + `
          @("/link") + $linkFlags + $libs

& cl.exe @clArgs
if ($LASTEXITCODE -ne 0) { Write-Error "ビルド失敗 (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

Write-Host "Build succeeded: $outExe"
