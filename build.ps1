# vim: set ft=ps1 fenc=utf-8 ff=unix sw=4 ts=4 et :
# ==================================================
# winfocus ビルドスクリプト
# rc.exe で src/winfocus.rc をコンパイルし、MSVC cl.exe で src/winfocus.c と共に
# リンクして out/winfocus.exe を生成する。最後に埋め込んだバージョン情報を検証する
#
# 引数:
#   -Version  : バージョン文字列（VERSIONINFO の生成元。例: 1.0.0、2.6.2-3-gabc1234-dirty）
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

# ==================================================
# バージョン情報リソースのコンパイル
# rc.exe の /d では文字列リテラルを渡せないため、定義ヘッダを生成して .rc から include する
# ==================================================

# $Version は「2.6.2-3-gabc1234-dirty」等の形も取るため、数値版は先頭の major.minor.patch のみ採る
$verMatch = [regex]::Match($Version, '^(\d+)\.(\d+)\.(\d+)')
$verNum = if ($verMatch.Success) {
    "$($verMatch.Groups[1].Value),$($verMatch.Groups[2].Value),$($verMatch.Groups[3].Value),0"
}
else {
    "0,0,0,0"
}
@(
    "#define APP_VERSION_NUM $verNum",
    "#define APP_VERSION_STR `"$Version`""
) | Set-Content -Path "out\version.rc.h" -Encoding ascii

$outRes = "out\winfocus.res"

# /c65001 は .rc の日本語コメント対策。無指定だと rc.exe が CP932 で MBCS 走査し、
# 行末バイトがリードバイト範囲に当たると改行を食って次行が消える。
& rc.exe /nologo /c65001 /I out /fo $outRes src\winfocus.rc
if ($LASTEXITCODE -ne 0) { Write-Error "リソースコンパイル失敗（終了コード：$LASTEXITCODE）"; exit $LASTEXITCODE }

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
          @("/link") + $linkFlags + $libs + @($outRes)

& cl.exe @clArgs
if ($LASTEXITCODE -ne 0) { Write-Error "ビルド失敗（終了コード：$LASTEXITCODE）"; exit $LASTEXITCODE }

# バージョン情報リソースの埋め込み結果を検証する
$vi = [System.Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path $outExe))
if (-not $vi.FileDescription) { Write-Error "バージョン情報リソース未埋め込み"; exit 1 }
Write-Host "Version info: $($vi.ProductName) $($vi.FileVersion) - $($vi.FileDescription)"

Write-Host "Build succeeded: $outExe"
