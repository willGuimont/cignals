# cignals

Small Unix daemon to remind you to move.

## Commands

```bash
cignals start [--interval N]
cignals status
cignals quit
cignals stop  # same as quit
cignals get interval
cignals set interval N
```

The daemon stores its runtime files under `$XDG_RUNTIME_DIR/cignals` when available,
or `/tmp/cignals-<uid>` as a fallback.

## Dev tools

clang-format:
```bash
bash tools/format.sh
```
