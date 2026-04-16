#!/usr/bin/env bash
# ビルド → 書き込み → モニタ (ログ保存付き)
set -e
cd "$(dirname "$0")/.."
mkdir -p logs
LOG=logs/latest.log
: > "$LOG"

echo "Build + upload + monitor (log -> $LOG)"
pio run -t upload 2>&1 | tee -a "$LOG"
pio device monitor 2>&1 | tee -a "$LOG"
