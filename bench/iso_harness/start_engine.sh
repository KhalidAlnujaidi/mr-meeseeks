#!/usr/bin/env bash
# start_engine.sh — launch the locked colibri OLMoE engine for the iso-harness.
#
# Why a launcher: the engine must outlive the invoking shell. On this host
# `setsid` does not exist (macOS) and a plain nohup child dies when the parent
# shell is reset, which silently turns every subsequent harness run into
# "engine not ready" (or, worse for the audit, a proxy whose upstream is
# refused while the run still writes rows).
#
# Locked identity matters for comparability: model-id `olmoe-leaf`, port 8081,
# --no-think (F93). Benign conda Traceback noise on stderr is expected.
set -uo pipefail
COLIBRI="${COLIBRI_DIR:-$HOME/dev/mr-meeseeks/colibri/c}"
PORT="${ENGINE_PORT:-8081}"
LOG="${ENGINE_LOG:-/tmp/colmoe-engine.log}"
PIDFILE="${ENGINE_PIDFILE:-/tmp/colmoe-engine.pid}"

cd "$COLIBRI" || exit 1
if curl -s -m 3 "http://127.0.0.1:$PORT/v1/models" -o /dev/null; then
  echo "[engine] already listening on :$PORT"; exit 0
fi
nohup env -u PYTHONPATH CONDA_NO_PLUGINS=true python3.12 ./coli serve \
  --model "$HOME/models/olmoe_merged" --model-id olmoe-leaf \
  --port "$PORT" --host 127.0.0.1 --no-think > "$LOG" 2>&1 < /dev/null &
echo $! > "$PIDFILE"
echo "[engine] launched pid=$(cat "$PIDFILE") log=$LOG"
