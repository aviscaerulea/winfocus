# winfocus

Win11 上のすべての可視ウィンドウを通常サイズに復元し、プライマリモニタに集約する CLI ツール。

## 目的

DisplayFusion によるウィンドウ再配置の前処理として使用する。

- すべての可視ウィンドウをフォアグラウンド化（最小化解除）
- すべてのウィンドウをプライマリモニタの作業領域に移動
- DisplayFusion が再配置しないウィンドウをプライマリモニタに留める

## 技術仕様

### 言語・ビルド環境

- 言語：C（Win32 API 直接呼び出し）
- ビルド：cl.exe（Visual Studio 2026）によるバッチビルド
- 構成：`src/winfocus.c` + `build.ps1` + `Taskfile.yml`
- ターゲット：Windows 11 x64
- DPI 対応：不要

### ファイル構成

```
winfocus/
├── src/
│   └── winfocus.c           // メインソース
├── scripts/
│   └── dump_positions.py    // 保存ファイルダンプスクリプト（デバッグ用）
├── winfocus.toml            // 設定ファイル（exe と同じディレクトリに配置）
├── build.ps1                // ビルドスクリプト（DevShell モジュール経由）
└── Taskfile.yml             // タスクランナー定義
```

### 動作仕様

#### 実行形態

| コマンド | 動作 |
|----------|------|
| `winfocus` | ウィンドウ配置を保存してからプライマリモニタに集約 |
| `winfocus --save` | 現在のウィンドウ配置を保存する（移動なし） |
| `winfocus --restore` | 保存した配置に全ウィンドウを復元する（保存ファイルは削除しない） |

共通仕様：

- CLI 実行のみ（常駐・ホットキーなし）
- サイレント実行（stdout/stderr 出力なし）
- 終了コード：0

#### ウィンドウ配置の保存・復元

- 保存先：exe と同じディレクトリの `winfocus.dat`（バイナリ形式、先頭 8 byte にマジック `WFCS` + バージョン番号）
- 保存内容：HWND・PID・クラス名・`WINDOWPLACEMENT`（表示状態取得用）・`GetWindowRect` のスクリーン座標・`WS_EX_TOPMOST` フラグ
- `--save` および引数なし実行では、実行時点のウィンドウ配置を即座に保存する
- 画面外補正：`--restore` で復元後にウィンドウが全モニタ範囲外にある場合、プライマリモニタ作業領域に移動する
- 判定順序：`--restore` は以下の順に評価し、いずれかにヒットした時点で後続処理をスキップする
    1. stale 判定：保存ファイルの更新日時がシステム起動時刻より古い場合、前回ブートのデータとして削除して終了する
    2. 有効期限判定：保存ファイルの更新日時から `expiry_hours` 時間以上経過している場合、ファイル不在として扱い何もせずに終了する（削除はしない）
- 保存失敗時の挙動：`--save` および引数なし実行で書き込みに失敗した場合、または対象ウィンドウが存在しない場合は不完全な保存ファイルを削除する
- 復元時の照合：HWND の有効性 + PID + クラス名の 3 条件を検証
- Z オーダーの復元：`--restore` 時にベストエフォートで重なり順を再現する（`EnumWindows` の列挙順を逆順に `SetWindowPos` で適用。OS 制約により完全保証はできない）。副作用として、保存後に新たに開いたウィンドウが保存対象ウィンドウの背面に移動する
- F11 全画面状態は保存のみで復元対象外

#### 対象ウィンドウ

- タスクバーに表示されている可視ウィンドウ（最小化含む）
- `winfocus.toml` のホワイトリストに登録されたツールウィンドウ

#### 除外ウィンドウ

- システムウィンドウ：Shell_TrayWnd, Progman, WorkerW, Button, Shell_SecondaryTrayWnd
- ツールウィンドウ：WS_EX_TOOLWINDOW スタイルを持つもの（ホワイトリスト除外あり）
- 非表示ウィンドウ：トレイ常駐アプリ等はスルーする
- 自プロセスのウィンドウ

#### 設定ファイル（winfocus.toml）

exe と同じディレクトリに配置する。

```toml
[toolwindow_whitelist]
classes = ["SystemMetersWnd"]

[save_file]
expiry_hours = 24
```

- `[toolwindow_whitelist].classes`：`WS_EX_TOOLWINDOW` スタイルを持つウィンドウのうち、列挙したクラス名のウィンドウは処理対象とする
- `[save_file].expiry_hours`：保存ファイルの有効期限（単位：時間）。`--restore` 時にこの時間以上経過していれば、ファイル不在として扱い何もせずに終了する。0 以上の整数を指定する。0 を指定すると有効期限判定を無効化する。負値・非数値はデフォルト値で動作する。デフォルト 24（= 24 時間）

ファイルが存在しない場合、または値が不正な場合はデフォルト値で動作する。

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
- 集約時の Z オーダーは変更しない（SWP_NOZORDER）

### ビルド方法

通常のターミナルや Claude Code から `task build` で実行できる。`build.ps1` が DevShell モジュール経由で VS 環境を自動初期化するため、Developer Command Prompt を使う必要はない。

```bat
cd D:\project-private\winfocus
task build
```

成功すると `out\winfocus.exe` が生成される。

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
- `GetWindowRect` - ウィンドウ矩形取得（F11 全画面判定・保存時のスクリーン座標取得）
- `SystemParametersInfo(SPI_GETWORKAREA)` - プライマリモニタ作業領域取得
- `GetWindowThreadProcessId` / `GetCurrentProcessId` - プロセス判定
- `GetWindowPlacement` - 表示状態（showCmd：最大化・最小化・通常）の取得（位置は GetWindowRect を使う）
- `SetWindowPlacement` - 不使用（削除済み）。GetWindowRect↔SetWindowPlacement のラウンドトリップがカスタムタイトルバーを持つアプリ（WindowsTerminal 等）で冪等にならない問題があるため SetWindowPos に統一
- `IsWindow` - HWND 有効性検証（復元時）
- `GetModuleFileNameA` - 設定ファイル・保存ファイルパスの組み立て
- `DeleteFileA` - 前回ブート判定時の stale ファイル削除・保存失敗時の不完全ファイル削除
- `GetFileAttributesExA` - 保存ファイルの更新日時取得（stale 判定用）
- `GetSystemTimeAsFileTime` - 現在時刻取得（stale 判定でブート時刻算出に使用）
- `GetTickCount64` - システム起動からの経過ミリ秒取得（stale 判定用）

## 参考

@README.md
