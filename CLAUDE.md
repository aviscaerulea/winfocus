# winfocus

Win11 上のすべての可視ウィンドウを通常サイズに復元し、プライマリモニタに集約する CLI ツール。

## 目的

DisplayFusion によるウィンドウ再配置の前処理として使用する。

- すべての可視ウィンドウをフォアグラウンド化（最小化解除）
- すべてのウィンドウをプライマリモニタの作業領域に移動
- DisplayFusion が再配置しないウィンドウをプライマリモニタに留める

## 技術仕様

### 言語・ビルド環境

- **言語**: C (Win32 API 直接呼び出し)
- **ビルド**: cl.exe (Visual Studio 2026) によるバッチビルド
- **構成**: `src/winfocus.c` + `scripts/build.bat`
- **ターゲット**: Windows 11 x64
- **DPI 対応**: 不要

### ファイル構成

```
winfocus/
├── src/
│   └── winfocus.c   // メインソース
├── winfocus.toml    // 設定ファイル（exe と同じディレクトリに配置）
└── scripts/
    └── build.bat    // ビルドスクリプト
```

### 動作仕様

#### 実行形態

| コマンド | 動作 |
|----------|------|
| `winfocus` | ウィンドウ配置を保存してからプライマリモニタに集約 |
| `winfocus --restore` | 保存した配置に全ウィンドウを復元して保存ファイルを削除 |

共通仕様：

- CLI 実行のみ（常駐・ホットキーなし）
- サイレント実行（stdout/stderr 出力なし）
- 処理完了時に MessageBeep(MB_OK) でシステムサウンドを鳴らす
- 終了コード: 0

#### ウィンドウ配置の保存・復元

- 保存先：`%TEMP%\winfocus_positions.dat`（バイナリ形式）
- 保存内容：HWND・PID・クラス名・`WINDOWPLACEMENT`（位置 + 最小化・最大化状態）
- 復元時の照合：HWND の有効性 + PID + クラス名の 3 条件を検証
- 同一セッション内での復元を前提とする
- F11 全画面状態は保存のみで復元対象外

#### 対象ウィンドウ

- タスクバーに表示されている可視ウィンドウ（最小化含む）
- `winfocus.toml` のホワイトリストに登録されたツールウィンドウ

#### 除外ウィンドウ

- システムウィンドウ: Shell_TrayWnd, Progman, WorkerW, Button, Shell_SecondaryTrayWnd
- ツールウィンドウ: WS_EX_TOOLWINDOW スタイルを持つもの（ホワイトリスト除外あり）
- 非表示ウィンドウ: トレイ常駐アプリ等はスルーする
- 自プロセスのウィンドウ

#### 設定ファイル（winfocus.toml）

exe と同じディレクトリに配置する。

```toml
[toolwindow_whitelist]
classes = ["SystemMetersWnd"]
```

`WS_EX_TOOLWINDOW` スタイルを持つウィンドウのうち、`classes` に列挙したクラス名のウィンドウは処理対象とする。
ファイルが存在しない場合はホワイトリストなしで動作する。

#### ウィンドウ処理（全対象に一様に適用）

| 状態 | 処理 |
|------|------|
| 最小化・最大化 | `ShowWindow(SW_RESTORE)` で通常サイズに復元 |
| F11 全画面 | モニタ全領域と一致判定 → `SendInput(VK_F11)` で全画面解除 |
| セカンダリモニタ上 | `SetWindowPos()` でプライマリモニタ作業領域左上に移動（サイズ変更なし） |
| プライマリモニタ上 | 移動なし（復元のみ） |
| 各ウィンドウ間 | `Sleep(5)` で安定化 |

#### 移動仕様

- サイズ変更は一切行わない（SWP_NOSIZE）
- プライマリモニタより大きいウィンドウもそのまま配置
- 移動先はプライマリモニタの作業領域左上（重なり許容）
- Z オーダーは変更しない（SWP_NOZORDER）

### ビルド方法

**Developer Command Prompt for VS** から実行する必要がある（通常の cmd.exe では cl.exe にパスが通っていない）。

1. スタートメニューから「Developer Command Prompt for VS 2026」を開く
2. プロジェクトディレクトリに移動して `task build` を実行する

```bat
cd D:\project-private\winfocus
task build
```

成功すると `out\winfocus.exe` が生成される。

> **注意**: build.bat 内に日本語コメントを書かないこと。Windows のバッチファイルは CP932 で解釈されるため、UTF-8 の日本語がコマンドとして誤実行される。

### 使用する Win32 API

- `EnumWindows` - 全トップレベルウィンドウ列挙
- `GetWindowLong` - ウィンドウスタイル取得
- `GetClassName` - クラス名取得
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
- `GetWindowPlacement` / `SetWindowPlacement` - 配置情報（位置・表示状態）の取得・復元
- `IsWindow` - HWND 有効性検証（復元時）
- `GetTempPathA` - 保存ファイルパスの組み立て
- `GetModuleFileNameA` - 設定ファイルパスの組み立て
- `DeleteFileA` - 復元完了後の保存ファイル削除
- `MessageBeep` - システムサウンド再生（処理完了通知）

## 参考

@README.md
