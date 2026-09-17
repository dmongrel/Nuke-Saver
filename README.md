# nuke-saver

A Windows screen saver.

Source: https://github.com/dmongrel/Nuke-Saver

## Status

Scaffolding only. The screensaver plumbing is in place — full-screen window, preview
child window, configure dialog, exit on input — and it currently paints a black screen.
The animation is not written yet; the design goes in [`docs/Nuke-Saver-Spec.md`](docs/Nuke-Saver-Spec.md).

## Building

### Prerequisites
- MinGW-w64 (x86_64) for Windows — MSYS2 UCRT64 works
- Make
- windres (part of MinGW-w64)

### Build Commands

```bash
# Build the screensaver
make

# Clean build artifacts
make clean

# Install to Windows System32 directory (requires Administrator)
make install
```

### Manual Build

```bash
windres nuke-saver.rc -O coff -o nuke-saver.res
g++ -std=c++11 -Wall -Wextra -O2 -municode -DUNICODE -D_UNICODE -o nuke-saver.scr main.cpp nuke-saver.res -mwindows -municode -static -lgdi32 -lshell32
```

## Installation

1. Build the project: `make`
2. Copy to Windows directory from an elevated prompt:
   ```cmd
   copy nuke-saver.scr %SystemRoot%\System32\
   ```
   Or right-click `nuke-saver.scr` and select **Install**.
3. Set as screensaver:
   - Right-click Desktop → Personalize → Lock screen → Screen saver
   - Select "Nuke Saver" from the dropdown
   - Set desired wait time
   - Click OK

## Command-Line Arguments

| Argument | Description |
|----------|-------------|
| (none) / `/s` | Run the screensaver in full-screen mode (all monitors) |
| `/p <hwnd>` or `/p:<hwnd>` | Show animation in a child window (preview for settings dialog) |
| `/c` or `/c:<hwnd>` | Show information message box (no configuration available) |
| Any other argument | Exit immediately with code 0 |

Arguments are case-insensitive and accept both `/` and `-` as the prefix.
