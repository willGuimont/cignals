#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

clang-format -i "$ROOT_DIR"/src/*.c "$ROOT_DIR"/src/*.h
