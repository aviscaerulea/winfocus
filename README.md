# winfocus

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/winfocus)](https://github.com/aviscaerulea/winfocus/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/winfocus)](LICENSE)
[![Build](https://github.com/aviscaerulea/winfocus/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/winfocus/actions/workflows/release.yml)

winfocus は、開いているすべてのウィンドウを通常サイズに戻し、メインのモニタへ集めるコマンドラインツールです。
複数のモニタにウィンドウが散らばってしまったときに、コマンド 1 つで手元の画面へまとめ直せます。
集める前の配置を保存しておけば、後から元どおりに戻すこともできます。

```bat
winfocus.exe           # ウィンドウ配置を保存してプライマリモニタに集約
winfocus.exe --save    # 現在のウィンドウ配置を保存（移動なし）
winfocus.exe --restore # 保存した配置に復元
winfocus.exe --raise   # 設定した exe のウィンドウを前面化
```

## 機能

- 最小化・最大化されたウィンドウを通常サイズに復元
- F11 による全画面表示を検出して解除
- サブモニタ上のウィンドウをメインモニタへ移動（サイズは変えない）
- 集約の最後に対象ウィンドウをすべて最小化し、復元時の動作を安定させる
- ウィンドウ配置の保存と復元（有効期限つき、初期値 24 時間）
- 指定した実行ファイルのウィンドウをまとめて前面化

処理の対象はタスクバーに表示される可視ウィンドウです。
デスクトップやタスクバーなどのシステムウィンドウ、常駐アプリの非表示ウィンドウ、winfocus 自身のウィンドウは対象外です。

## インストール

### 動作要件

- Windows 11 x64

### 手順

[Scoop](https://scoop.sh/) でインストールできます。

```bat
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install winfocus
```

## 使い方

いずれのコマンドも画面には何も出力せず、静かに終了します。

| コマンド | 動作 |
| --- | --- |
| `winfocus.exe` | ウィンドウ配置を保存してから、メインモニタへ集約する |
| `winfocus.exe --save` | 現在のウィンドウ配置を保存する（ウィンドウは動かさない） |
| `winfocus.exe --restore` | 保存した配置に復元する |
| `winfocus.exe --raise` | 設定した実行ファイルのウィンドウを前面化する |

### ウィンドウ配置の保存と復元

`--save` は、各ウィンドウの位置、表示状態（通常・最小化・最大化）、重なり順を実行ファイルと同じ場所に保存します。
`--restore` で保存した配置に戻せます。保存した内容は復元後も残るため、有効期限内なら何度でも復元できます。

### ウィンドウの前面化

`--raise` は、設定ファイルに並べた実行ファイルのウィンドウを順に前面へ出します。
最後に書いた実行ファイルが最前面になります。最小化されているウィンドウは元に戻してから前面化します。

## 設定

実行ファイルと同じ場所に置く `winfocus.toml` で動作を調整できます。
ファイルがない場合や値が不正な場合は、初期値で動作します。

```toml
[toolwindow_whitelist]
classes = ["SystemMetersWnd"]

[save_file]
expiry_hours = 24

[raise]
apps = ["WindowsTerminal.exe"]
```

| 設定キー | 説明 |
| --- | --- |
| `[toolwindow_whitelist].classes` | 通常は対象外となるツールウィンドウのうち、処理対象に含めるウィンドウクラス名 |
| `[save_file].expiry_hours` | 保存した配置の有効期限を時間単位で指定する。`0` で有効期限の判定を無効にする。初期値は 24 |
| `[raise].apps` | `--raise` で前面化する実行ファイル名。初期値は `WindowsTerminal.exe` のみ |

`[save_file].expiry_hours` を過ぎた配置は、`--restore` しても復元されません。

## 制限事項

- 復元はアプリを起動したままの状態を前提とする（アプリを再起動すると照合できず復元されない）
- 重なり順の復元は Windows の制約により完全には再現できない
- F11 の全画面状態は保存のみで、復元の対象外
- 復元時、保存後に新しく開いたウィンドウは保存対象のウィンドウより後ろへ回る

> [!CAUTION]
> `--save` は実行のたびに保存内容を上書きします。
> 引数なしの `winfocus` で集約した後に `--save` すると集約後の状態が保存されるため、
> 元の配置へ戻したい場合は集約する前に `--save` してください。

## ビルド

[Task](https://taskfile.dev/) と Visual Studio 2026（または Build Tools）が必要です。
プロジェクトのディレクトリで以下を実行します。

```bat
task build
```

`out\winfocus.exe` が生成されます。
