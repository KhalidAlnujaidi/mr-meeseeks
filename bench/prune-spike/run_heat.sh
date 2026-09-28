#!/usr/bin/env bash
# Feed bench/prune-spike/corpus.txt through the OLMoE chat loop (one /reset +
# one corpus line per turn, MAX_NEW=1) and flush the routing heat to usage.txt.
# Telemetry channels verified on main@a8f2ca6 (pilot 2026-09-19): m->freq bumps
# at every MoE call incl. prefill; COLI_USAGE=<path> -> rt_save at stdin close.
set -e
cd "$(dirname "$0")"
{ for line in "" $(cat corpus.txt | sed 's/\\/\\\\/g'); do
    [ -z "$line" ] && continue
    printf '/reset\n%s\n' "$line"
  done; } | SNAP="$HOME/models/olmoe_merged" CHAT=1 MAX_NEW=1 PILOT=0 \
    COLI_USAGE=./usage_full.txt OMP_NUM_THREADS=12 \
    ../../colibri/c/olmoe 64 8 2>/dev/null | tail -2
echo "usage file:"; wc -l usage_full.txt
