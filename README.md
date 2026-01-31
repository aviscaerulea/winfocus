# winfocus

Win11 上のすべてのアプリウィンドウ（最小化・トレイ常駐含む）を通常サイズに復元し、プライマリモニタに集約する CLI ツール。

## 概要

DisplayFusion によるウィンドウ再配置の前処理として使用する軽量ツール。

- すべてのウィンドウをフォアグラウンド化（最小化解除・トレイ常駐復元）
- すべてのウィンドウをプライマリモニタの作業領域に移動
- DisplayFusion が再配置しないウィンドウをプライマリモニタに留める

## 主な機能

- **最小化・最大化ウィンドウの復元**: `ShowWindow(SW_RESTORE)` で通常サイズに復元
- **F11 全画面ウィンドウの解除**: モニタ全領域と一致判定 → `SendInput(VK_F11)` で全画面解除
- **トレイ常駐アプリの復元**: ホワイトリスト方式で `ShellExecute` による再起動
- **早期アプリ起動最適化**: `EnumWindows` の前にプロセス存在チェック + `ShellExecute` で並列起動
- **ポーリング処理**: 20 秒タイムアウト、200ms 間隔、全ウィンドウ検出で早期終了

## ビルド方法

**Developer Command Prompt for VS** から実行する必要がある。

```bat
# 1. Developer Command Prompt for VS 2026 を開く
# 2. プロジェクトディレクトリに移動
cd D:\project-private\winfocus

# 3. ビルド
build.bat
```

成功すると `winfocus.exe` が生成される（約 149KB）。

## 使用方法

```bat
# 実行（サイレント、出力なし）
winfocus.exe
```

## 設定ファイル（オプション）

`winfocus.toml` を exe と同じディレクトリに配置することで、トレイ常駐アプリの復元対象を指定できる。

```toml
# winfocus.toml
# Tray restoration whitelist

[[tray]]
process = "LINE.exe"                                    # プロセス名
title = "LINE"                                          # ウィンドウタイトルの前方一致パターン
exe = "C:\\Users\\...\\LINE\\bin\\current\\LINE.exe"  # ShellExecute 用のパス（\\\\ でエスケープ）
```

- `process`: プロセス名（必須、大文字小文字を区別しない）
- `title`: ウィンドウタイトルパターン（オプション、前方一致、大文字小文字を区別しない）
- `exe`: ShellExecute で起動するパス（オプション、バックスラッシュは `\\\\` でエスケープ）

ファイルが存在しない場合、トレイ復元はスキップされ、可視ウィンドウのみ処理される。

## 技術仕様

- **言語**: C (Win32 API 直接呼び出し)
- **ビルド**: cl.exe (Visual Studio 2026)
- **ターゲット**: Windows 11 x64、コンソールアプリ
- **主要 API**: EnumWindows, EnumProcesses, ShellExecuteA, SendInput, MonitorFromWindow 等

詳細な仕様は `.claude/CLAUDE.md` を参照。

## ライセンス

Private
