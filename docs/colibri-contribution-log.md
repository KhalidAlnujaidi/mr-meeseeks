# Colibri contribution ledger — session 2026-09-19

Upstream: https://github.com/JustVugg/colibri  (clone: ~/dev/mr-meeseeks/colibri, gitignored)
All published items verified on GitHub via API read-back after posting.

## Published

- **2026-09-20 q38 replication + G3 correction POSTED to #1621:**
  https://github.com/JustVugg/colibri/issues/1621#issuecomment-5749562438
  (KhalidAlnujaidi, 2026-09-20T11:39:34Z; verified via API read-back —
  author/created/sections/gate/G3/rand/ratio lines, length 7888 chars ==
  local file char-count exactly). Every headline number independently
  re-derived from enigma results.json before posting (23/23 ad-hoc checks).
  Headline: ctl gate +0.0000 twice (3+5 refs); G3 strict never-routed (1,694,
  0% traffic) mean +0.0060 = free within noise; matched-count rand +0.0539;
  tier-14022 (7.85% traffic) +0.0517; rand-14022 (56.8%) +0.538. Damage
  tracks TRAFFIC-REMOVED, not count; OLMoE's 63-86% per-expert band does
  NOT transfer at 48x512 (q38 = ~10-11%). Body source:
  bench/prune-spike/drafts/issue1621-qwen38-replication.md.
  Local cleanup same day: 5 olmoe /tmp containers removed (198->233Gi free;
  evidence files + manifest intact, regenerable via pipeline).

1. PR #1611 validation comment (issue side: #1601 RAM auto-detect)
   https://github.com/JustVugg/colibri/pull/1611#issuecomment-5743393260
   - third macOS repro of the silent 1-slot/layer collapse, before/after on
     M5 Pro (main@a8f2ca6 vs PR branch), /health captures
   - gate finding: make check broken at tip (expert_ffn.h in Makefile, not in
     the branch; GitHub mergeable_state=dirty independently confirmed)
   - #1050 paired cap sweep included with spread caveats stated
   - local records: bench/pr1611-comment.md, bench/pr1611-cap-sweep.json,
     bench/pr1611-validation-notes.md, bench/colibri-cap-sweep.py

2. Experiment issue: count-cold ≠ removable (the pruning spike)
   https://github.com/JustVugg/colibri/issues/1621
   - 12.0M routing picks from 10,579 tokens of real agentic logs -> 2/1024
     count-dead experts
   - engine-true zero-ablation tiers: cold-tail 220 costs +30% ppl (0.66% of
     traffic); uniform control arm (220 experts, 22.51% traffic) costs +39%
   - headline: per unit of traffic, cold experts 22-29x more load-bearing
     than average -> counts are placement signal, not deletion signal
   - manifest passes upstream's c/experiment_manifest.py validator:
     "ok (ablated_expert_set, no-change)"
   - follow-up comment posted the control arm and struck an unmeasured claim
     from the first post:
     https://github.com/JustVugg/colibri/issues/1621#issuecomment-5744944819
   - evidence dir: bench/prune-spike/ (usage_full.txt, candidates_*.txt,
     tier_eval_results.json, armc_results.json, arm_shares.json,
     paired_speed_warm.log, manifest.json, FINDINGS.md, all scripts)

3. PR #1625 (CLOSED — first code contribution, superseded upstream): olmoe port
   of the ABLATE_SCORE causal-ablation harness from colibri.c
   https://github.com/JustVugg/colibri/pull/1625
   - LEDGER CORRECTION 2026-09-28 (verified against the live page, not assumed):
     this item was recorded OPEN; the page shows CLOSED, self-closed by
     KhalidAlnujaidi 2026-09-21. This reads as redundancy, not rejection —
     upstream PR #1356 (monotophic) replaced the same ABLATE_SCORE /
     run_ablate_score mode and bumped its artifact schema coli-ablate/1 -> /2;
     #1356's own body documents the identical defect our old-code gate
     demonstrated ("it exits 0 after writing nothing but a header"). #1625 was
     closed two days after #1356 was cross-linked in its thread. Independently
     confirmed: upstream dev's c/olmoe.c carries 0 ABLATE_SCORE references, so
     the port did not land by another route.
   - base dev, head KhalidAlnujaidi:feat/olmoe-causal-ablation @ 20741dd,
     3 files +165/-6 (c/olmoe.c hooks+driver+dispatch, c/Makefile dep,
     docs/ENVIRONMENT.md two table rows)
   - evidence: make check EXIT=0 clean at tip (first run's 21 fails were MY
     concurrent 7GB probes racing fixture writers — race, not code); old-code
     gate: stock dev silently ignores ABLATE_SCORE (exit 0, 0 JSONL records)
     vs patched (hdr + 3 ah + 120 lg); OFF-state PPL line byte-identical
     patched vs stock (5.1030/164.52); driver mode-0 mean 5.103009 ==
     independent step()-based meter; modes 1/2/3 deltas measured on hot cells
   - MODEL FACT for future ablation design: OLMoE norm_topk_prob=false =>
     modes 1 and 2 are numerically identical (drop-slot == zero-contribution
     when nothing renormalises); they differ only in freq counters. Use GLM
     for the renormalisation arm to matter.
   - artifacts: /tmp/colibri-abl worktree (fork branch pushed),
     /tmp/pr_body_ablate.md, /tmp/abl48.json + /tmp/mh3.txt probe manifests
   - ARTIFACT CORRECTION 2026-09-28: the "(fork branch pushed)" note is no
     longer true. `git ls-remote fork` returns only feat/markov-transition-tables
     and feat/olmoe-route-trace; feat/olmoe-causal-ablation @ 20741dd is on NO
     remote ref (`git branch -r --contains 20741dd` is empty). The local branch
     and the /private/tmp/colibri-abl worktree survive, but this PR's head is
     clone-local only — see "Unpublished / at-risk local work" below. The
     /tmp/*.json and /tmp/pr_body_ablate.md probe manifests are gone (regenerable).

4. Issue #1629 (CLOSED as completed, filed 2026-09-20): `coli serve` expert
   history (.coli_usage) not flushed on SIGTERM
   https://github.com/JustVugg/colibri/issues/1629
   - LEDGER CORRECTION 2026-09-28: recorded OPEN; live page shows CLOSED as
     completed. It was FIXED upstream, by JustVugg's #1631 on `dev`, via the
     mechanism this report suggested — a handler installed for SIGTERM and
     SIGINT, deliberately WITHOUT SA_RESTART so the blocking read the server is
     parked on returns EINTR and the loop reaches the save. Maintainer's closing
     comment credits the report directly: the Ctrl-C-works / systemctl-stop-does-
     not detail "is what pointed straight at the handler rather than at the
     save". A landing win — retire from the watch list.
   - approved contents of docs/upstream-colibri-sigterm-usage-note.md
     submitted verbatim (draft header/Title scaffolding stripped); API
     read-back at filing time confirmed OPEN, title exact
   - serve mode saves only on the graceful serve_loop() return path;
     SIGTERM — the only exit a supervised server gets — skips rt_save, so
     heat never accumulates for long-lived deployments; chat-mode graceful
     exit DOES save (2944 selections / 666 experts observed), read-side
     loads fine at startup; USAGE_SAVE=0 opt-out also bypassed on signal
   - suggested direction (non-prescriptive): signal handler doing one final
     rt_save (temp+rename already crash-safe) and/or periodic save every N
     turns (4.9 KB for 666 records)
   - downstream impact: our cache.warm ledger telemetry (mtime+heat probe)
     degrades to "no data" for serve-mode deployments

5. PR #1630 (MERGED — MISSING from this ledger until 2026-09-28): olmoe emits
   ROUTE_TRACE records (it announced the stream and wrote nothing)
   https://github.com/JustVugg/colibri/pull/1630
   - ADDED 2026-09-28: this was a published code contribution that the ledger
     never recorded. Merged into JustVugg:dev as 32d513d, 2026-09-21, 28 checks
     passed; maintainer retargeted the base main -> dev for us on 2026-09-20.
   - head KhalidAlnujaidi:feat/olmoe-route-trace @ 9b64a0a, 2 files +49/-9
     (c/olmoe.c, docs/experiments/cnre-offline-simulator.md)
   - defect: rt_init() opens the ROUTE_TRACE file and olmoe has called it since
     the shared telemetry header landed, but nothing in moe() ever called
     rt_trace() — the engine printed "[ROUTE_TRACE] logging routing to <path>",
     ran, and left a ZERO-BYTE file. Every consumer saw "no data" instead of an
     error: route_pairs.py wrote an empty table, route_coupling_report.py and
     residency_sim.py had nothing to read. olmoe was the only MoE engine in that
     state (colibri.c, kimi_k3.c, glm53.c all trace).
   - fix: the activation-heatmap loop becomes one rt_route(layer, s, idx, val, K)
     (rt_count is exactly the loop it replaces, including its idx >= 0 guard, so
     the .coli_usage counters stay byte-identical), plus rt_trace_end() once per
     moe() invocation OUTSIDE the row loop — an S==0 batch traces no rows yet must
     still consume a call id, or the ids stop being consecutive and
     residency_sim.py rejects the whole trace. Both no-ops with ROUTE_TRACE unset.
   - evidence: before 0 bytes traced; after 3344 lines / 292917 bytes, 3232
     consecutive call ids, 16 layers cycling 0..15 per forward. TF-NLL 3.8370
     nats/token, ppl 46.39, hit 96.3% — identical to the pre-change binary and to
     the same run with ROUTE_TRACE unset (measurement only; no token moves).
   - CONFIRMED LANDED 2026-09-28: upstream origin/dev's c/olmoe.c now contains
     rt_route and rt_trace_end at the positions this PR added.

6. Branch feat/markov-transition-tables (FORK-ONLY, NO upstream PR — MISSING from
   this ledger until 2026-09-28): order-1 Markov table for cross-layer expert
   prediction (+ gate policy class), @ 8e03dd3
   - ADDED 2026-09-28. Three verified facts:
     - IS pushed to the fork: `git ls-remote fork` returns
       8e03dd33de385cb68a1502466cddd4641ed56d34 refs/heads/feat/markov-transition-tables;
       local branch, fork-tracking ref and live remote all agree at 8e03dd3.
     - NO upstream PR exists: a PR search by author returns only #1625 and #1630;
       a search for "markov" returns 4 unrelated PRs. NOT superseded either —
       upstream dev carries c/tools/residency_sim.py but NOT route_markov.py or
       stage_a_build_corpus.py.
     - ledger-visible content: 4 files +636/-1, ADDITIVE RESEARCH TOOLING, no
       engine code (c/tools/route_markov.py, c/tools/stage_a_build_corpus.py,
       c/tools/residency_sim.py `markov` policy class, c/tests/test_residency_sim.py)
   - route_markov.py builds a COLIMARKOV order-1 table from ROUTE_TRACE captures,
     and --score REFUSES a file passed to --train, so the held-out split is
     enforced rather than trusted. stage_a_build_corpus.py splits by SOURCE
     SESSION because routing is autocorrelated within a run (a line-level split
     leaks). The `markov` class reproduces half-pinned byte-for-byte on an empty
     table — asserted in tests, so an A/B is attributable to the table and not to
     a second implementation of the pinning rule.
   - HONEST NEGATIVE RESULT, reported as one: on 32 real held-out OLMoE traces
     (107,008 records, 4 workload categories, 24 held-out / 8 train, 601,920
     next-layer selections, table 960 entries / 51 KB), next-layer recall at
     budget 8/layer goes marginal heat 39.80% -> order-1 table 82.53% (+42.7pp)
     and transfers across held-out sessions and categories — BUT the Phase-0 gate
     is UNMOVED: `markov` and `half-pinned` post identical misses and identical
     felt wait at every budget (0.5/1/2/4 GB), so the PASS at 2 GB and 4 GB is the
     pinning rule's and the table contributes zero. Structural reason, recorded in
     the write-up: the simulator does not model new prefetch reads, and prefetch
     recall is the lever this table moves — a demand-residency gate cannot score
     it. No runtime path was added on the strength of it. Tests: 39 pass.

## Unpublished / at-risk local work (added 2026-09-28)

Verified by direct inspection of the clone at ~/dev/mr-meeseeks/colibri. The
record is stronger upstream than the ledger claimed; the only real exposure is
LOCAL. Both items below are on NO remote ref at all.

- **HIGH — uncommitted working-tree edits on feat/markov-transition-tables**
  (c/qwen36.c and c/route_trace.h, +40/-4). Present ONLY in the working tree of
  the clone; `git status` shows them as modified-not-staged, and there are no
  stashes. What they do, read in full (measurement/instrumentation only — no
  compute path is touched, and both are inert without env vars):
    - c/route_trace.h: adds "qwen36" to rt_engine_names[], so the engine-id table
      can name qwen36 on a mismatch.
    - c/qwen36.c ROUTE_TRACE wiring — the SAME defect #1630 fixed in olmoe.c.
      Adds #include "route_trace.h", rt_init("qwen36", ...) + rt_drop_row(n_layers)
      in model_init_range, rt_route(layer, s, idx, val, K) after top-k selection
      and HF renormalization, and rt_trace_end() per moe() call outside the row
      loop. The in-source comment states it plainly: "Before this call qwen36
      called neither rt_route nor rt_trace, so ROUTE_TRACE produced a 0-byte file."
      CONFIRMED against upstream dev: c/qwen36.c there has ZERO route_trace.h
      references, so the defect is real and unfixed upstream — this is the same
      repair, one engine over, that merged as #1630.
    - c/qwen36.c PILOT_S_MAX knob — replaces the hard-coded `S <= 8` at the three
      dispatch sites with `S <= g_pilot_s_max` (default 8, hence byte-identical by
      default; S<=0 disables entirely, kept as a knob so an A/B is a one-variable
      change). Comment records the measurement: "prefetch cuts TTFT ~20% but
      leaves decode flat, because decode is I/O-hidden and prefill is not."
      CONFIRMED against upstream dev: three literal `S <= 8`, no PILOT_S_MAX.
  => Higher upstream value than its size suggests: it is the #1630 repair applied
     to qwen36 and would extend trace coverage to that engine.

- **HIGH — feat/olmoe-causal-ablation @ 20741dd (PR #1625's head) is on NO remote
  ref.** `git branch -r --contains 20741dd` is empty; `git ls-remote fork` shows
  only the two route-trace/markov heads. The local branch exists and the
  /private/tmp/colibri-abl worktree (marked `prunable`) holds the tree, but if
  this clone is lost the entire ABLATE_SCORE olmoe port is lost. The ledger's
  "fork branch pushed" claim for this branch is stale (corrected above).

- LOW / disposable: untracked .venv (901M), c/qwen36_tiny_hf (2.4M), c/qwen36_tiny_ok
  (932K) — local test fixtures; four `prunable` worktrees under /private/tmp
  (colibri-abl @20741dd, colibri-baseline, colibri-dev, colibri-pr1611). Pruning
  worktrees would not destroy the commits (they survive via branch refs) but would
  discard the dirty trees.

- SAFE: the bench/prune-spike/ evidence set is intact on disk (manifest.json,
  armc_results.json, arm_shares.json, paired_speed_warm.log, tier_eval_results.json,
  candidates_cold/uniform/full.txt, all scripts + drafts). The /tmp/*.json probe
  manifests are gone but every published number is re-derivable via the packaged
  pipeline (`run_experiment.py --profile profiles/olmoe_merged.json --steps report`).

## Watch-list correction (2026-09-28)

The previous watch list was overstated — two of its three "open, awaiting reply"
items are CLOSED AND LANDED upstream, and the third is closed as redundant. Current:
  - #1629 — RETIRED. Fixed upstream by #1631 using the mechanism we suggested.
  - #1625 — CLOSED (redundant vs upstream #1356, which replaced the same mode and
    bumped the schema coli-ablate/1 -> /2). Our norm_topk_prob=false finding stands.
  - #1630 — MERGED (32d513d). No longer "awaiting review".
  - markov branch — genuinely open, 0 upstream PRs, fork-only.
  - #1621 — still OPEN, maintainer positive (analyzed our count-vs-causal finding
    as confirming colibri's existing placement-not-deletion design).
  - #1611, #1050 — not re-verified in this pass.
A ledger that overstates open items is as misleading as one that understates them:
the published record is stronger than this document previously claimed, and now says so.

## Known self-improvements noted (methodology, worth keeping)

- EXPERIMENT SAVED AS REPEATABLE PIPELINE (2026-09-19, after maintainer
  positive reply + LinkedIn ask "implement for all LLMs?"):
  bench/prune-spike/{README.md, run_experiment.py, parse_route_trace.py,
  profiles/olmoe_merged.json}; ablate_container.py now takes --tensor-res +
  --zero-all; make_eval_refs.py takes --model. `run_experiment.py --profile
  profiles/olmoe_merged.json --steps report` reproduces the published deltas
  (+0.2487/+0.3302 cold, +0.3932/+0.3841 rand, 74% per-expert, 25.3x
  per-traffic — all inside published ranges). New model = new profile JSON.
  Verified 11/11 ad-hoc (not suite green).
- Maintainer reply on #1621 (JustVugg, 20:41Z): confirms placement-vs-deletion
  reading; asks for exactly the uniform control we'd already run (thread
  predates the follow-up); pulls the #1609 thread — traces record IDs,
  discard gate mass — hence parse_route_trace.py emits BOTH counts and mass.

- Small-sample trap: a 24-token pilot "found" 140 dead experts; at 12M picks
  it was 2. Any heat statistic needs corpus scale before it means anything.
- Fixture slips caught in-review: duplicate eval ref (ref1==ref2) disclosed in
  the doc rather than hidden; overclaim ("1-6x") replaced by a real control
  arm instead of defended.
- Verification harness failures were triaged each time by debugging the
  fixture (per-file cap collision, log-header substring, macOS /var vs
  /private/var realpath aliasing) — scripts under test were never edited to
  chase a green harness.
- coli bench is not a usable harness for olmoe (eval_glm.py hard-wires the GLM
  binary + template); our PPL-based harness filled that gap.

## Not done / open

- #1050 has 2 competing PRs already; nothing else in the tree needed a fix
  from this session's findings (the abl.h port became PR #1625, see above).
- Original issue #1621 body still carries "not run here" text where the
  follow-up supersedes it — left as honest history; can be edited to a single
  canonical version if preferred.
- Colibri serve on :8123 (main@a8f2ca6, OLMoE) may still be running; the five
  /tmp/olmoe_* 6.9GB ablation containers (control, tier51/102/220, uniform)
  persist for re-runs, delete to reclaim ~34GB.
- Watch items for replies: #1629 (SIGTERM usage-flush — maintainer/metadata
  reaction), #1625 (PR review — maintainer reaction to the
  port + the norm_topk_prob note), #1621 (maintainer response on the
  count-vs-causal finding — GOT one 20:41Z, positive), PR #1611 (does the
  author rebase; the split-PR suggestion), #1050 (if the maintainer takes
  it, moot for us).
  -> SUPERSEDED 2026-09-28: #1629 CLOSED/landed (#1631), #1625 CLOSED as
     redundant (#1356 replaced the same mode, schema /1 -> /2), #1621 still OPEN
     and positive. See "Watch-list correction" above for the live list.
- CROSS-MODEL RE-RUN in flight (2026-09-19 night): Qwen3.8-Flash-Next FP8
  (48L x 512E top-10, 185.5GB) on enigma via the packaged pipeline.
  Detached driver ~/bench/run_q38.sh, logs ~/bench/logs/q38-run.log; local
  cron job q38-enigma-progress checks every 30m. Profile
  profiles/qwen38_flash_next.enigma.json (seq_arms + link_mode for disk).
  Heat = ppl_usage of 12 corpus lines (published 18-line corpus, line-split
  holdout 12/3/3 — disclose if published). Watch for: ctl delta == 0.0000
  (FP8 rewrite lossless?) before believing any cold/rand delta.
