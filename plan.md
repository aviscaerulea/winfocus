# winfocus: 全ウィンドウをプライマリモニタに集約するツール

## 概要

Win11 上のすべてのアプリウィンドウ（最小化・トレイ常駐含む）を通常サイズに復元し、プライマリモニタの作業領域に移動する CLI ツール。
DisplayFusion の前処理として使用し、フォアグラウンド化 + プライマリモニタ集約を目的とする。

## 技術仕様

### 言語・ビルド

- **言語**: C (Win32 API 直接呼び出し)
- **ビルド**: cl.exe によるバッチビルド（build.bat）
- **構成**: `winfocus.c` + `winfocus.cfg` + `build.bat`
- **ターゲット**: Windows 11 x64、コンソールアプリ（SUBSYSTEM:CONSOLE）
- **DPI 対応**: 不要

### 処理フロー

1. `GetModuleFileName()` で exe パスを取得し、同ディレクトリの `winfocus.cfg` を読み込む
2. `SystemParametersInfo(SPI_GETWORKAREA)` でプライマリモニタの作業領域を取得
3. `EnumWindows()` で全トップレベルウィンドウを列挙（非表示含む）
4. 各ウィンドウに対してフィルタリング（除外判定）
5. 対象ウィンドウに対して一様に処理:
   - 最小化・最大化されている場合: `ShowWindow(hwnd, SW_RESTORE)`
   - 非表示（トレイ常駐、cfg ホワイトリスト該当）の場合: `ShowWindow(hwnd, SW_SHOW)` → `ShowWindow(hwnd, SW_RESTORE)`
   - プライマリモニタ外にある場合: `SetWindowPos()` で作業領域左上に移動（サイズ変更なし）
6. ウィンドウ間に `Sleep(5)` を挿入（メッセージキュー安定化）

### ウィンドウフィルタリング詳細

#### 除外するクラス名（ハードコード）

```
Shell_TrayWnd              // タスクバー
Progman                    // デスクトップ
WorkerW                    // デスクトップ背景
Button                     // デスクトップの「表示」ボタン
Shell_SecondaryTrayWnd     // セカンダリモニタのタスクバー
```

#### 除外条件

- `GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW` → 除外
- タイトルが空文字列 かつ `!IsWindowVisible(hwnd)` → 除外
- `GetWindowThreadProcessId()` で自プロセスのウィンドウ → 除外
- ApplicationFrameHost 配下で `IsWindowVisible() == FALSE` → 除外

#### 対象条件

- **可視ウィンドウ**: `IsWindowVisible(hwnd) == TRUE` かつ上記除外に非該当
- **非表示ウィンドウ**: 以下の条件をすべて満たす
  - `winfocus.cfg` に記載されたプロセスに属する
  - タイトルが空文字列でない
  - cfg でタイトルパターンが指定されている場合、タイトルがパターンに前方一致する（大文字小文字無視）

### トレイ常駐アプリの復元戦略（ホワイトリスト方式）

`winfocus.cfg` に記載されたプロセス名とタイトルパターンでフィルタリングし、該当ウィンドウのみ復元対象とする。

1. `ShowWindow(hwnd, SW_SHOW)` を試行
2. `ShowWindow(hwnd, SW_RESTORE)` を試行
3. 復元できないものはサイレントにスキップ

**WM_COMMAND は使用しない**（破壊的動作リスク排除のため）。

### winfocus.cfg 仕様

exe と同じディレクトリに配置するプレーンテキストファイル。

```
# トレイ復元対象プロセスとタイトルパターン
# 形式: プロセス名|タイトルパターン (パイプ区切り、タイトルパターンは省略可)
# 大文字小文字を区別しない

LINE.exe|LINE
Slack.exe|Slack
Discord.exe
```

#### 設定形式

- **2 カラム形式**: `プロセス名|タイトルパターン`
  - 例: `LINE.exe|LINE` → LINE.exe プロセス内で、タイトルが "LINE" で始まるウィンドウのみ復元
  - タイトルマッチングは**前方一致**（大文字小文字無視）
  - "LINE" で "LINE" と "LINE - 設定" の両方にマッチ
- **1 カラム形式**（後方互換性）: `プロセス名`
  - 例: `Discord.exe` → Discord.exe プロセス内のすべての非表示ウィンドウを復元
  - タイトルフィルタは適用されない

#### その他の規則

- `#` で始まる行はコメント
- 空行は無視
- プロセス名、タイトルパターンともに大文字小文字を区別しない（_stricmp / _strnicmp）
- ファイルが存在しない場合、トレイ復元はスキップ（可視ウィンドウのみ処理）

### プライマリモニタへの移動

- `MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)` で現在のモニタを確認
- プライマリモニタでなければ `SetWindowPos()` で作業領域左上に移動
- サイズ変更は行わない（SWP_NOSIZE）
- Z オーダーは変更しない（SWP_NOZORDER）
- プライマリモニタより大きいウィンドウもそのまま配置（DisplayFusion が後で処理）

### 出力

- サイレント実行（stdout/stderr への出力なし）
- 終了コード: 0（成功）

### ビルド方法

```bat
@echo off
REM build.bat - Developer Command Prompt for VS で実行
cl.exe /O2 /W4 /Fe:winfocus.exe winfocus.c user32.lib
```

## ファイル構成

```
winfocus/
├── winfocus.c      // メインソース（単一ファイル）
├── winfocus.cfg    // トレイ復元対象ホワイトリスト（オプション）
└── build.bat       // ビルドスクリプト
```
