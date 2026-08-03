# winfocus

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/winfocus)](https://github.com/aviscaerulea/winfocus/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/winfocus)](LICENSE)
[![Build](https://github.com/aviscaerulea/winfocus/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/winfocus/actions/workflows/release.yml)

winfocus is a command-line tool that restores every open window to its normal size and gathers them onto your main monitor.
When windows are scattered across multiple monitors, a single command brings them all back to the screen in front of you.
Save the layout beforehand and you can put everything back the way it was.

```bat
winfocus.exe           # Save the window layout, then gather windows onto the primary monitor
winfocus.exe --save    # Save the current window layout (no windows are moved)
winfocus.exe --restore # Restore the saved layout
winfocus.exe --raise   # Bring the configured executables' windows to the foreground
```

## Features

- Restores minimized and maximized windows to their normal size
- Detects and cancels F11 full-screen mode
- Moves windows on secondary monitors to the main monitor (sizes are left unchanged)
- Minimizes every target window at the end of the gather, keeping later restores stable
- Saves and restores window layouts (with an expiry, 24 hours by default)
- Brings the windows of specified executables to the foreground in one go

winfocus targets visible windows that appear on the taskbar.
System windows such as the desktop and taskbar, hidden windows of tray-resident apps, and winfocus's own window are excluded.

## Installation

### Requirements

- Windows 11 x64

### Steps

You can install it with [Scoop](https://scoop.sh/).

```bat
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install winfocus
```

## Usage

Every command runs silently and prints nothing to the screen.

| Command | Behavior |
| --- | --- |
| `winfocus.exe` | Saves the window layout, then gathers windows onto the main monitor |
| `winfocus.exe --save` | Saves the current window layout (windows are not moved) |
| `winfocus.exe --restore` | Restores the saved layout |
| `winfocus.exe --raise` | Brings the configured executables' windows to the foreground |

### Saving and restoring window layouts

`--save` stores each window's position, display state (normal, minimized, maximized), and stacking order next to the executable.
`--restore` puts the saved layout back. The saved data remains after a restore, so you can restore as often as you like while it is still valid.

### Bringing windows to the foreground

`--raise` brings the windows of the executables listed in the configuration file to the front, in order.
The executable listed last ends up frontmost. Minimized windows are restored first, then brought forward.

## Configuration

Place `winfocus.toml` next to the executable to adjust the behavior.
If the file is missing or a value is invalid, the defaults apply.

```toml
[toolwindow_whitelist]
classes = ["SystemMetersWnd"]

[save_file]
expiry_hours = 24

[raise]
apps = ["WindowsTerminal.exe"]
```

| Setting key | Description |
| --- | --- |
| `[toolwindow_whitelist].classes` | Window class names of tool windows to include, which are otherwise excluded |
| `[save_file].expiry_hours` | How long a saved layout stays valid, in hours. `0` disables the expiry. Default is 24 |
| `[raise].apps` | Executable names that `--raise` brings forward. Defaults to `WindowsTerminal.exe` only |

A layout older than `[save_file].expiry_hours` is not restored, even when you run `--restore`.

## Limitations

- Restoring assumes the applications are still running (restarted applications cannot be matched and are not restored)
- Stacking order cannot be reproduced exactly, due to Windows constraints
- F11 full-screen state is saved only and is never restored
- On restore, windows opened after the save are pushed behind the saved windows

> [!CAUTION]
> `--save` overwrites the saved data every time it runs.
> Running `--save` after gathering windows with a bare `winfocus` stores the gathered state,
> so run `--save` before gathering if you want to get the original layout back.

## Build

[Task](https://taskfile.dev/) and Visual Studio 2026 (or Build Tools) are required.
Run the following in the project directory.

```bat
task build
```

This produces `out\winfocus.exe`.
