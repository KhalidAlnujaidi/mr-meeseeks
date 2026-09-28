# Validation evidence for PR #1611 / issue #1601 — macOS 26, Apple M5 Pro, 24 GB

Third repro + before/after, from a machine the fix targets (macOS, olmoe engine).

## Host / versions
- Apple M5 Pro (18 logical cores), 24 GB unified, internal APFS SSD
- macOS 26.5.1 (Darwin 25.5.1), Apple clang 21, Homebrew libomp, arm64
- Arms: `main` @ `a8f2ca6` (v1.11.0, the release the reporter ran on Linux) vs
  PR branch @ `74aeb28`. Engine built per-worktree with
  `make -C c olmoe` (default flags, no ARCH=native). Model:
  OLMoE-1B-7B int8 merged container, 6.9 GB (converted via
  `c/tools/convert_olmoe_merged.py`).
- Gateway: `coli serve` from each checkout's own tree, fresh process per arm,
  no `--ram` / no `--cap` (the exact silent-fallback path).

## The silent collapse reproduces on main, at exactly the sizes the fix predicts

baseline main@a8f2ca6 (engine built here, 17:48 local):
```
[cache] 1 slots/layer of 64 experts: 1.6 GB budget (88% of what the OS still offers), 1.6 GB dense resident, 1.1 GB projected KV, 6 MB per expert
```
/health: `"hwinfo":{"cores":18,"ram_total_gb":0.0,"ram_avail_gb":0.0,"cpu":"unknown",...}`

PR #1611 @ 74aeb28:
```
[cache] 37 slots/layer of 64 experts: 7.3 GB budget (88% of what the OS still offers), 2.0 GB dense resident, 1.1 GB projected KV, 6 MB per expert
```
/health: `"hwinfo":{"cores":18,"ram_total_gb":25.8,"ram_avail_gb":6.1,"cpu":"Apple M5 Pro",...}`

So the whole failure mode — 1 slot/layer, RAM 0.0, cpu unknown — is fully fixed
by the branch on this platform. The `[cache]` line's "88% of what the OS still
offers" text was the tell on our side too: it reads plausible while the number
itself is the degenerate fallback.

## Note: dev already carries the engine half (7ed6084); what #1611 adds over dev

`dev` @ HEAD here, same no-flag serve:
```
[cache] 26 slots/layer of 64 experts: 6.0 GB budget ...
/health hwinfo: ram_total 25.8 / avail 4.9, cpu "unknown"
```
i.e. dev sizes correctly (its probe + half-physical fallback), so the remaining
delta PR #1611 vs dev on macOS is:
1. the plausibility floor (<2% of physical treated as unmeasured + warned) —
   dev's `compat_mem_available_gb()` can still return a tiny-but-real number
   under memory pressure and size the cache off it silently; our arm-to-arm
   26 vs 37 slots difference is consistent with avail-RAM fluctuating, and we
   saw no warning in either case;
2. the `/health` CPU-brand + RAM fill (`cpu: "unknown"` persists on dev);
3. the #1050 recency-list eviction (see below).

## #1050 acceptance data (paired, interleaved, medians of 3 fresh processes)

TF-NLL bit-identical across ALL runs of all caps across both arms: 0.5137
nats/token over 12 scored tokens (this repo's ref.json: prompt_ids 5 +
full_ids 17).

| cap | main@a8f2ca6 tok/s | PR@74aeb28 tok/s |
|---|---|---|
| 4  | 5.86 | 5.64 |
| 8  | 5.85 | 5.74 |
| 16 | 6.00 | 6.18 |
| 32 | 4.96 | 4.82 |
| 64 | 5.24 | 5.03 |

Finding: the cap=32 dip (non-monotonic in BOTH arms, PR not worse) plus the
general flatness means the bookkeeping term is invisible on this host
(Apple SSD ~7 GB/s read; the scan cost that shows on 24-core Windows boxes
does not bite here). PR parity, not PR win, on macOS. Raw samples:
/tmp/colibri-cap-sweep.json (preserved as bench/pr1611-cap-sweep.json);
sweep script at bench/colibri-cap-sweep.py.

## Review observation (not blocking)

The branch bundles 21 files across four concerns (#1601 fix, #1050 eviction, a
web dark-mode toggle, per-issue docs writeups) on one PR. Given the repo's
one-changed-variable manifest culture, splitting the #1050 engine work from the
#1601 launcher/probe work would let the latter merge without re-litigating the
former (which has two other open PRs on #1050: #1571, #1591).
