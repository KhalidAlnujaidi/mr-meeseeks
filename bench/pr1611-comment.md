Third repro + validation on a macOS host this branch targets — and two gate findings.

## #1601: the silent collapse reproduces on `main` @ a8f2ca6 and this branch fixes it

Apple M5 Pro, 24 GB unified, macOS 26.5.1 (Darwin 25.5.1), Apple clang 21 + Homebrew libomp, arm64. OLMoE-1B-7B int8 merged container, 6.9 GB (`convert_olmoe_merged.py`). Engine built per-worktree with `make -C c olmoe`, default flags. `coli serve --model <dir>`, fresh process per arm, **no `--ram`, no `--cap`** — the exact silent path from #1601.

Baseline `main` @ a8f2ca6:
```
[cache] 1 slots/layer of 64 experts: 1.6 GB budget (88% of what the OS still offers), 1.6 GB dense resident, 1.1 GB projected KV, 6 MB per expert
```
`/health`: `"hwinfo":{"cores":18,"ram_total_gb":0.0,"ram_avail_gb":0.0,"cpu":"unknown",...}`

This branch @ 74aeb28:
```
[cache] 37 slots/layer of 64 experts: 7.3 GB budget (88% of what the OS still offers), 2.0 GB dense resident, 1.1 GB projected KV, 6 MB per expert
```
`/health`: `"hwinfo":{"cores":18,"ram_total_gb":25.8,"ram_avail_gb":6.1,"cpu":"Apple M5 Pro",...}`

Same failure signature as the reporter's M3 Max (1 slot/layer, 0.0 GB, "unknown" cpu) on a second macOS machine class, and every field the commit message claims to fix is measurably fixed. The `[cache]` line's "88% of what the OS still offers" wording is what made this invisible on our side too — it reads plausible while quoting the degenerate fallback.

One scoping note: `dev` @ HEAD already carries the engine half via 7ed6084 (`[cache] 26 slots/layer`, RAM 25.8/4.9), but still reports `cpu: "unknown"` and no plausibility warning. So the PR's remaining delta over dev on macOS is the floor+warn, the `/health` brand/RAM fill, and the #1050 eviction work.

## Gate findings (not the author's design, the branch's state)

1. `make check` fails at tip: `No rule to make target 'expert_ffn.h', needed by 'tests/test_qwen36_cache_index'`. The conflict-resolution commit 74aeb28 brought in dev's new Makefile dependency (c/Makefile:1333) but not the header itself — `c/expert_ffn.h` exists on `dev`, not on this branch. GitHub also reports `mergeable_state: dirty`. A rebase onto current `dev` should clear both.
2. Partial checklist status, for the record: the branch's targeted validation passes on macOS (`tests/test_olmoe_victim_index: ok`; `tests/test_olmoe_differential`: 5000+5000 steps OK). `make check` as a whole cannot run until (1) is fixed.

## #1050 datapoint on this host (underpowered — labeled as such)

Paired cap sweep, main@a8f2ca6 vs this branch, `PPL=1`, 12 scored tokens from the repo's `ref.json`, 3 fresh processes per arm per cap, arms interleaved within each cap block, OMP_NUM_THREADS=12:

| cap | main tok/s | PR tok/s |
|---|---|---|
| 4 | 5.86 | 5.64 |
| 8 | 5.85 | 5.74 |
| 16 | 6.00 | 6.18 |
| 32 | 4.96 | 4.82 |
| 64 | 5.24 | 5.03 |

TF-NLL bit-identical across all 30 runs and both arms: `0.5137` nats/token (string-compared; the acceptance guard). So the eviction change is output-exact here. On the perf half: within-arm spread reaches 26% (cap 4: 4.62–6.12) and the arm-to-arm gap is 2–4%, so this sweep **neither confirms nor refutes** the win — and because the branch conflates the probe fix, the eviction work, and web/UI files in one diff, it is not a one-variable comparison by the manifest rules. Reporting it as parity-not-proven rather than a result. If the #1050 part gets split out, a dev@HEAD-vs-that-branch rerun of this exact protocol is single-concern and I'll publish it properly.
