# cignals

Small Unix daemon to remind you to move.

## Commands

```bash
cignals start [--interval N (in minutes, defaults to 60)]
cignals status
cignals quit
cignals stop  # same as quit
cignals get interval
cignals set interval N
```

The daemon stores its runtime files under `$XDG_RUNTIME_DIR/cignals` when available,
or `/tmp/cignals-<uid>` as a fallback.

## Build

Debug:

```bash
cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug -- -j$(nproc)
```

Release build:

```bash
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -- -j$(nproc)
```

Notes:
- This project uses `pkg-config` to find `libnotify`. Install the system packages (example for Arch Linux):

```bash
sudo pacman -Syu libnotify pkgconf
```

## Dev tools

clang-format:
```bash
bash tools/format.sh
```
