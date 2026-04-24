#!/usr/bin/env python3
# vim: set ft=python ff=unix fenc=utf-8 ts=4 sw=4 sts=4 et :
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
#
# ==================================================
# winfocus 保存ファイルダンプ
#
# out\winfocus.dat の内容を人間が読める形式で出力する。
# winfocus --save 後にこのスクリプトを実行して保存内容を確認する。
#
# WindowEntry 構造体レイアウト（x64 MSVC、バージョン 2）:
#   offset  0: HWND hwnd               (8 bytes)
#   offset  8: DWORD pid               (4 bytes)
#   offset 12: char className[256]     (256 bytes)
#   offset 268: WINDOWPLACEMENT        (44 bytes)
#   offset 312: RECT rect              (16 bytes)
#   offset 328: BOOL isTopmost         (4 bytes)
#   合計: 332 bytes（パディングによる差異は actual_entry_size で自動計算）
#
# ファイル形式（バージョン 2）：
#   [uint32 magic=0x57464353]["WFCS"][uint32 version=2][int count][WindowEntry x count]
#
# 使用方法:
#   uv run scripts/dump_positions.py
# ==================================================

import struct
import ctypes
import datetime
from pathlib import Path

SAVE_FILE_MAGIC   = 0x57464353  # "WFCS"
SAVE_FILE_VERSION = 2

SHOW_CMD = {
    0: "SW_HIDE",
    1: "SW_SHOWNORMAL",
    2: "SW_SHOWMINIMIZED",
    3: "SW_SHOWMAXIMIZED",
    4: "SW_SHOWNOACTIVATE",
    5: "SW_SHOW",
    6: "SW_MINIMIZE",
    7: "SW_SHOWMINNOACTIVE",
    8: "SW_SHOWNA",
    9: "SW_RESTORE",
    10: "SW_SHOWDEFAULT",
}

# WindowEntry の各フィールドオフセット
OFF_HWND       = 0
OFF_PID        = 8
OFF_CLASSNAME  = 12
OFF_WP         = 268  # WINDOWPLACEMENT 開始
OFF_RECT       = 312  # RECT (left, top, right, bottom)
OFF_TOPMOST    = 328  # BOOL isTopmost
ENTRY_SIZE     = 332  # sizeof(WindowEntry) の目安（実測値を優先）

# WINDOWPLACEMENT 内の相対オフセット
WP_LENGTH      = 0
WP_FLAGS       = 4
WP_SHOWCMD     = 8
WP_PTMIN_X     = 12
WP_PTMIN_Y     = 16
WP_PTMAX_X     = 20
WP_PTMAX_Y     = 24
WP_RC_LEFT     = 28
WP_RC_TOP      = 32
WP_RC_RIGHT    = 36
WP_RC_BOTTOM   = 40


def get_window_title(hwnd):
    try:
        buf = ctypes.create_unicode_buffer(256)
        ctypes.windll.user32.GetWindowTextW(hwnd, buf, 256)
        return buf.value or "(タイトルなし)"
    except Exception:
        return "(取得失敗)"


def main():
    # out\ はビルド出力ディレクトリ（winfocus.exe と同じ場所）
    path = Path(__file__).parent.parent / "out" / "winfocus.dat"

    if not path.exists():
        print(f"保存ファイルが見つからない: {path}")
        return

    file_mtime = datetime.datetime.fromtimestamp(path.stat().st_mtime)
    mtime_str = file_mtime.strftime("%Y-%m-%d %H:%M:%S")
    data = open(path, "rb").read()

    # ヘッダ検証
    if len(data) < 8:
        print("ファイルが短すぎる（ヘッダなし）")
        return
    magic, version = struct.unpack_from("<II", data, 0)
    if magic != SAVE_FILE_MAGIC:
        print(f"マジック不一致: 0x{magic:08X}（期待値 0x{SAVE_FILE_MAGIC:08X}）- 旧形式または破損")
        return
    if version != SAVE_FILE_VERSION:
        print(f"バージョン不一致: {version}（期待値 {SAVE_FILE_VERSION}）")
        return

    count = struct.unpack_from("<i", data, 8)[0]
    header_size = 12  # magic(4) + version(4) + count(4)
    actual_entry_size = (len(data) - header_size) // count if count > 0 else ENTRY_SIZE

    # ブート時刻を算出して stale 判定を表示
    ctypes.windll.kernel32.GetTickCount64.restype = ctypes.c_uint64
    uptime_ms = ctypes.windll.kernel32.GetTickCount64()
    boot_time = datetime.datetime.now() - datetime.timedelta(milliseconds=uptime_ms)
    is_stale = file_mtime < boot_time - datetime.timedelta(seconds=2)

    print(f"保存ファイル: {path}")
    print(f"更新日時: {mtime_str}  バージョン: {version}  エントリ数: {count}  エントリサイズ: {actual_entry_size} bytes")
    print(f"ブート時刻: {boot_time:%Y-%m-%d %H:%M:%S}  stale: {'Yes (前回ブートのデータ)' if is_stale else 'No'}")
    print("-" * 80)

    for i in range(count):
        base = header_size + i * actual_entry_size

        hwnd = struct.unpack_from("<Q", data, base + OFF_HWND)[0]
        pid  = struct.unpack_from("<I", data, base + OFF_PID)[0]
        class_name = data[base + OFF_CLASSNAME: base + OFF_CLASSNAME + 256].split(b"\x00")[0].decode("mbcs", errors="replace")

        wp = base + OFF_WP
        show_cmd = struct.unpack_from("<I", data, wp + WP_SHOWCMD)[0]
        flags    = struct.unpack_from("<I", data, wp + WP_FLAGS)[0]
        rc_left  = struct.unpack_from("<i", data, wp + WP_RC_LEFT)[0]
        rc_top   = struct.unpack_from("<i", data, wp + WP_RC_TOP)[0]
        rc_right = struct.unpack_from("<i", data, wp + WP_RC_RIGHT)[0]
        rc_bot   = struct.unpack_from("<i", data, wp + WP_RC_BOTTOM)[0]

        rt_left, rt_top, rt_right, rt_bot = struct.unpack_from("<iiii", data, base + OFF_RECT)

        show_str = SHOW_CMD.get(show_cmd, f"unknown({show_cmd})")
        title    = get_window_title(hwnd)

        print(f"[{i+1:3d}] {class_name}  PID={pid}  HWND=0x{hwnd:016X}")
        print(f"       Title  : {title}")
        print(f"       showCmd: {show_cmd} ({show_str})  flags=0x{flags:04X}")
        print(f"       rcNormal (workspace): ({rc_left},{rc_top})-({rc_right},{rc_bot})")
        print(f"       rect    (screen)    : ({rt_left},{rt_top})-({rt_right},{rt_bot})")
        print()


if __name__ == "__main__":
    main()
