#!/usr/bin/env bash
# pio device monitor を動かしつつ logs/latest.log にも書き出す。
# 画面表示と同時にファイル保存されるので、Claude に「ログ見て」と頼めばよい。

set -e
cd "$(dirname "$0")/.."
mkdir -p logs
LOG=logs/latest.log
: > "$LOG"   # 毎回クリア (追記したければこの行を消す)

echo "Writing log to $LOG (Ctrl+C to stop)"
pio device monitor 2>&1 | tee "$LOG"
