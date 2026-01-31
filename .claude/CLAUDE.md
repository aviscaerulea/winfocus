# winfocus

Win11 上のすべてのアプリウィンドウ（最小化・トレイ常駐含む）を通常サイズに復元し、プライマリモニタに集約する CLI ツール。

## 目的

DisplayFusion によるウィンドウ再配置の前処理として使用する。

- すべてのウィンドウをフォアグラウンド化（最小化解除・トレイ常駐復元）
- すべてのウィンドウをプライマリモニタの作業領域に移動
- DisplayFusion が再配置しないウィンドウをプライマリモニタに留める

## 技術仕様

### 言語・ビルド環境

- **言語**: C (Win32 API 直接呼び出し)
- **ビルド**: cl.exe (Visual Studio 2026) によるバッチビルド
- **構成**: `winfocus.c` + `winfocus.toml` + `build.bat`
- **ターゲット**: Windows 11 x64、コンソールアプリ
- **DPI 対応**: 不要

### ファイル構成

```
winfocus/
├── winfocus.c       // メインソース
├── winfocus.toml    // トレイ復元対象ホワイトリスト（オプション）
└── build.bat        // ビルドスクリプト（Developer Command Prompt で実行）
```

### 動作仕様

#### 実行形態

- CLI 実行のみ（常駐・ホットキーなし）
- サイレント実行（stdout/stderr 出力なし）
- 終了コード: 0

#### 対象ウィンドウ

- タスクバーに表示されている可視ウィンドウ（最小化含む）
- winfocus.toml に記載されたプロセスのトレイ常駐（非表示）ウィンドウ

#### 除外ウィンドウ

- システムウィンドウ: Shell_TrayWnd, Progman, WorkerW, Button, Shell_SecondaryTrayWnd
- ツールウィンドウ: WS_EX_TOOLWINDOW スタイルを持つもの
- UWP バックグラウンド: 不可視の ApplicationFrameHost 配下ウィンドウ
- メッセージ専用: タイトルなし かつ 不可視のウィンドウ
- 自プロセスのウィンドウ

#### ウィンドウ処理（全対象に一様に適用）

| 状態 | 処理 |
|------|------|
| 最小化・最大化 | `ShowWindow(SW_RESTORE)` で通常サイズに復元 |
| F11 全画面 | モニタ全領域と一致判定 → `SendInput(VK_F11)` で全画面解除 |
| トレイ常駐（非表示） | `SW_SHOW` → `SW_RESTORE` の順に試行。復元不可は `ShellExecute` で再起動 |
| セカンダリモニタ上 | `SetWindowPos()` でプライマリモニタ作業領域左上に移動（サイズ変更なし） |
| プライマリモニタ上 | 移動なし（復元のみ） |
| 各ウィンドウ間 | `Sleep(5)` で安定化 |

#### 移動仕様

- サイズ変更は一切行わない（SWP_NOSIZE）
- プライマリモニタより大きいウィンドウもそのまま配置
- 移動先はプライマリモニタの作業領域左上（重なり許容）
- Z オーダーは変更しない（SWP_NOZORDER）

#### トレイ常駐アプリの復元（ホワイトリスト + ShellExecute 方式）

`winfocus.toml` に記載されたプロセスのウィンドウのみ復元対象。

**処理フロー:**

1. **早期 ShellExecute（EnumWindows 前）**
   - ホワイトリストの `exe` 指定ありエントリについて、`EnumProcesses` でプロセス存在チェック
   - プロセスが存在しない場合、`ShellExecute` で再起動（バックグラウンドで起動開始）

2. **EnumWindows によるウィンドウ処理**
   - 全ウィンドウを列挙して復元・移動処理（早期発行したアプリはバックグラウンドで起動中）
   - ホワイトリストに一致するウィンドウは `found` フラグを立てる

3. **EnumWindows 後の追加 ShellExecute チェック**
   - プロセスは存在するがウィンドウが見つからないケースをポーリング対象に含める

4. **ポーリング（ShellExecute 発行時のみ）**
   - 20 秒タイムアウト、200ms 間隔でポーリング
   - `enum_callback` でウィンドウ検出・復元・移動を同時実行
   - 全対象ウィンドウが見つかれば早期終了

**WM_COMMAND は使用しない**（破壊的動作リスク排除のため）。

#### winfocus.toml 仕様

exe と同じディレクトリに配置する TOML 形式の設定ファイル。

```toml
# winfocus.toml
# Tray restoration whitelist

[[tray]]
process = "LINE.exe"           # プロセス名（大文字小文字を区別しない）
title = "LINE"                 # ウィンドウタイトルの前方一致パターン（大文字小文字を区別しない）
exe = "C:\\Users\\...\\LINE.exe"  # ShellExecute 用の実行ファイルパス（バックスラッシュは \\\\ でエスケープ）
```

- `[[tray]]` セクションで 1 エントリを定義（複数定義可）
- `process`: プロセス名（必須）
- `title`: ウィンドウタイトルパターン（オプション、空文字列の場合は全ウィンドウ対象）
- `exe`: ShellExecute で起動するパス（オプション、空文字列の場合は ShellExecute しない）
- ファイルが存在しない場合、トレイ復元はスキップ（可視ウィンドウのみ処理）

### ビルド方法

**Developer Command Prompt for VS** から実行する必要がある（通常の cmd.exe では cl.exe にパスが通っていない）。

1. スタートメニューから「Developer Command Prompt for VS 2026」を開く
2. プロジェクトディレクトリに移動して `build.bat` を実行する

```bat
cd D:\project-tmp\新しいフォルダー
build.bat
```

成功すると同ディレクトリに `winfocus.exe` が生成される。

> **注意**: build.bat 内に日本語コメントを書かないこと。Windows のバッチファイルは CP932 で解釈されるため、UTF-8 の日本語がコマンドとして誤実行される。

### 使用する Win32 API

- `EnumWindows` - 全トップレベルウィンドウ列挙
- `EnumProcesses` - 全プロセス列挙（早期 ShellExecute の存在チェック用）
- `GetWindowLong` / `GetWindowLongPtr` - ウィンドウスタイル取得
- `GetClassName` - クラス名取得
- `GetWindowText` - タイトル取得
- `IsWindowVisible` - 可視判定
- `IsIconic` / `IsZoomed` - 最小化・最大化判定
- `ShowWindow` - 表示状態変更
- `SetWindowPos` - 位置変更
- `SetForegroundWindow` - フォアグラウンド化（F11 解除前）
- `SendInput` - キー入力シミュレーション（F11 全画面解除用）
- `MonitorFromWindow` - 所属モニタ判定
- `GetMonitorInfo` - モニタ情報取得
- `GetWindowRect` - ウィンドウ矩形取得（F11 全画面判定用）
- `SystemParametersInfo(SPI_GETWORKAREA)` - プライマリモニタ作業領域取得
- `GetWindowThreadProcessId` / `GetCurrentProcessId` - プロセス判定
- `GetModuleFileName` - exe パス取得（toml 読み込み用）
- `OpenProcess` / `GetModuleBaseName` - プロセス名取得（ホワイトリスト照合・存在チェック用）
- `ShellExecuteA` - アプリ再起動（トレイ復元用）
- `GetTickCount` - タイムアウト計測（ポーリング用）
