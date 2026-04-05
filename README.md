# winfocus

Win11 上のすべての可視ウィンドウを通常サイズに復元し、プライマリモニタに集約する CLI ツール。

## 機能

- 最小化・最大化ウィンドウを通常サイズに復元
- F11 全画面ウィンドウを検出して解除
- セカンダリモニタ上のウィンドウをプライマリモニタに移動

## 動作要件

- Windows 11 x64

## インストール方法

[Scoop](https://scoop.sh/) でインストールできる。

```bat
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install winfocus
```

## 使用方法

```bat
winfocus.exe           # ウィンドウ配置を保存してプライマリモニタに集約
winfocus.exe --save    # 現在のウィンドウ配置を保存（移動なし）
winfocus.exe --restore # 保存した配置に復元
```

サイレント実行（出力なし）。

### ウィンドウ配置の保存・復元

`--save` でウィンドウの位置・表示状態（通常・最小化・最大化）を `%TEMP%\winfocus_positions.dat` に保存する。
前回の保存以降にマウス/キーボード操作がない場合は保存をスキップする（タスクスケジューラでの定期実行時に不要な上書きを防ぐ）。
`--restore` で保存した配置に復元し、保存ファイルを削除する。

復元の照合は HWND・PID・クラス名の 3 条件で行うため、同一セッション内（アプリを再起動していない状態）での使用を想定している。
F11 全画面状態は保存のみで復元対象外となる。

> **注意：** 引数なしの `winfocus`（集約）実行後に `--save` を実行すると、集約後のウィンドウ状態（最小化・プライマリモニタ）が保存される。`--restore` での元の配置への復元を意図する場合は、集約前に `--save` を実行してください。

典型的な用途：

- タスクスケジューラや PC スリープ時フックで `--save` を定期実行しておき、作業再開時に `--restore` で直前の状態に戻す

タスクスケジューラへの登録（ログオン時から 6 分おきに `--save` を自動実行）は以下のスクリプトで行う。

```pwsh
pwsh -ExecutionPolicy Bypass -File register-task.ps1
```

## 設定

`winfocus.toml`（exe と同じディレクトリに配置）で動作をカスタマイズできる。

```toml
[toolwindow_whitelist]
classes = ["SystemMetersWnd"]
```

`WS_EX_TOOLWINDOW` スタイルを持つウィンドウはデフォルトでスキップされるが、`classes` に列挙したクラス名のウィンドウは処理対象に含める。

## ビルド方法

プロジェクトディレクトリで以下を実行する。

```bat
task build
```

成功すると `out\winfocus.exe`（約 105KB）が生成される。

## 技術仕様

- 言語：C（Win32 API 直接呼び出し）
- ビルド：cl.exe（Visual Studio 2026）
- ターゲット：Windows 11 x64、コンソールアプリ
- 主要 API：EnumWindows, SendInput, MonitorFromWindow, SetWindowPos 等
