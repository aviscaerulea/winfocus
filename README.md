# winfocus

Win11 上のすべての可視ウィンドウを通常サイズに復元し、プライマリモニタに集約する CLI ツール。

## 概要

DisplayFusion によるウィンドウ再配置の前処理として使用する軽量 CLI ツール。

- 最小化・最大化ウィンドウを通常サイズに復元
- F11 全画面ウィンドウを検出して解除
- セカンダリモニタ上のウィンドウをプライマリモニタに移動
- 集約後に全ウィンドウを最小化

## ビルド方法

**Developer Command Prompt for VS 2026** から実行する必要がある。

```bat
cd D:\project-private\winfocus
task build
```

成功すると `out\winfocus.exe`（約 105KB）が生成される。

## 使用方法

```bat
winfocus.exe           # 全ウィンドウをプライマリモニタに集約
winfocus.exe --save    # 現在のウィンドウ配置を保存（移動なし）
winfocus.exe --restore # 保存した配置に復元
```

サイレント実行（出力なし）。処理完了時にシステムサウンドが鳴る。

`--save` は定期実行やスリープ時フックと組み合わせて使用し、`--restore` で直前の状態に戻す。

## 技術仕様

- **言語**: C（Win32 API 直接呼び出し）
- **ビルド**: cl.exe（Visual Studio 2026）
- **ターゲット**: Windows 11 x64、コンソールアプリ
- **主要 API**: EnumWindows, SendInput, MonitorFromWindow, SetWindowPos 等

詳細な仕様は `CLAUDE.md` を参照。

## ライセンス

Private
