#!/bin/bash
# Air node (M3/16GB): setup for the #1621 cross-host OLMoE replication.
# Idempotent. The model + pipeline are PUSHED from this Mac (source host does
# not accept incoming ssh, so the Air never rsyncs from it); this script only
# prepares the environment. All output -> ~/bench/logs/setup.log
set -uo pipefail
LOGD=~/bench/logs; mkdir -p $LOGD ~/bench/arms
exec >> $LOGD/setup.log 2>&1
echo "=== SETUP $(date) ==="

# 1) venv + deps (system py3.9 arm64 has torch wheels)
if [ ! -x ~/venv-colibri/bin/python ]; then
  python3 -m venv ~/venv-colibri || { echo "FAIL venv"; exit 1; }
fi
source ~/venv-colibri/bin/activate
python -m pip install -q --upgrade pip 2>&1 | tail -1
python -m pip install -q numpy torch safetensors 2>&1 | tail -2
python -c "import torch, safetensors; print('deps ok', torch.__version__)" || { echo "FAIL deps"; exit 1; }

# 2) colibri clone + olmoe engine (no brew on PATH: Makefile finds libomp in
#    /opt/homebrew/opt/libomp directly; if absent it builds single-threaded)
if [ ! -d ~/colibri ]; then
  git clone --depth 50 https://github.com/JustVugg/colibri ~/colibri || { echo "FAIL clone"; exit 1; }
fi
cd ~/colibri/c && make olmoe 2>&1 | tail -2
[ -x ./olmoe ] || { echo "FAIL build olmoe"; exit 1; }
echo "engine ok: $(ls -l olmoe | awk '{print $5}') bytes"

# 3) expect PUSHED files: pipeline at ~/bench/prune-spike, model at
#    ~/models/olmoe_merged. Fail loudly if absent (never a silent "done").
test -f ~/bench/prune-spike/run_experiment.py || { echo "FAIL pipeline not pushed"; exit 1; }
test -f ~/models/olmoe_merged/config.json    || { echo "FAIL model not pushed"; exit 1; }
echo "=== SETUP OK $(date) ==="
