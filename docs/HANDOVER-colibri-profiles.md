# HANDOVER — colibri pruning experiments (session 2026-09-20)

Profile-centric status. A new agent/owner: read this first, then
`docs/colibri-contribution-log.md` (ledger), then `bench/prune-spike/README.md`
(pipeline design). Do not re-run anything marked DONE.

Repo: /Users/khalid/dev/mr-meeseeks   (bench/, docs/ledger are untracked —
commit is the user's call). Upstream: github.com/JustVugg/colibri (fork:
KhalidAlnujaidi/colibri).

## Profiles and their state

### profiles/olmoe_merged.json  — DONE, PUBLISHED
- The original #1621 experiment (16L x 64E top-8 int8, this Mac).
- Published: issue #1621 + correction comment 5744944819.
- Evidence dir: bench/prune-spike/ (usage_full.txt, tier_eval_results.json,
  armc_results.json, manifest.json — validator says "ok (ablated_expert_set,
  no-change)"). DO NOT DELETE these.
- /tmp/olmoe_* arm containers were deleted 2026-09-20 (35 GB reclaimed);
  regenerable via `--steps build`.

### profiles/olmoe_merged.air.json  — DONE, RESULT IN #1621 COMMENT
- Cross-host replication on the M3 MacBook Air (khalid@100.120.42.119,
  ~/bench/prune-spike). Every delta identical to 4 decimals vs published.
- Air is otherwise NOT a compute node for this project; it hosts live
  embeddings/hallucination/hero servers — leave it.

### profiles/qwen38_flash_next.enigma.json  — DONE ("run A")
- 48L x 512E top-10 block-FP8 on enigma (enigma@100.64.164.41,
  ~/bench/prune-spike, model ~/models/Qwen3.8-Flash-Next-FP8 185.5 GB/131 shards).
- Results: runs/qwen38_flash_next/results.json; log ~/bench/logs/q38-run.log.
- ctl gate +0.0000 (FP8 round-trip lossless). Tier(14022@7.85% traffic)
  +0.0517 w/ one NEGATIVE ref; rand(14022@56.81%) +0.538.
- Known caveats that motivated G3: tier mixes never-routed with low-traffic;
  heat only 22,883 rows.

### profiles/qwen38_zeroonly.enigma.json  — DONE ("G3", the headline run)
- Strict never-routed (1,694 @ 0.0% traffic) vs matched-count uniform
  (seed 1972, 6.91%), 5 eval refs. Candidates from gen_zeroonly_cands.py.
- Results: runs/qwen38_zeroonly/results.json; log ~/bench/logs/g3-run.log.
- Gate re-passed 5/5 x +0.0000. cold +0.0060 (noise), rand +0.0539.
- FINDING: damage tracks traffic-removed, not expert count; OLMoE's
  63-86%/22-29x are NOT scale-invariant.
- ALL PUBLISHED 2026-09-20T11:39:34Z:
  https://github.com/JustVugg/colibri/issues/1621#issuecomment-5749562438
  (body source: bench/prune-spike/drafts/issue1621-qwen38-replication.md)

## If a new profile is added (the recurring playbook)
1. Copy an existing profile; set "out" to a FRESH workdir name (never reuse
   a published profile's out — main() re-seeds olmoe_merged fixtures).
2. Big models: seq_arms+link_mode=true, eval_timeout=7200 (enigma is SHARED;
   a load spike at ~31 run-queue killed a 1800 s hard timeout once).
3. Multi-tensor experts (qwen3 gate/up/down): ablate_container.py now counts
   DISTINCT experts hit — keep tensor_res groups = (layer, expert).
4. Engines without a CHAT loop: heat=ppl_usage; refs MUST carry
   "schema_version": 1 (make_ppl_refs.py emits it; qwen38 exit(1)s without).
5. Driver scripts: set -euo pipefail, assert outputs exist, never print
   ALL DONE unconditionally. md5 local==remote before launch.
6. Launch detached: ssh -n host 'setsid bash -c "cmd > ~/log 2>&1 &"', poll
   in a separate short call.
7. Verify with an ad-hoc harness (regression must fail on OLD code), report
   "N passed, 0 failed, not suite green", delete harness.

## Open upstream items (waiting on others, nothing to do)
- PR #1625 (abl.h port to olmoe): OPEN, base dev, mergeable_state=unstable.
  Worktree /tmp/colibri-abl (branch feat/olmoe-causal-ablation, commit
  20741dd). If CI turns red on our diff, fix there.
- #1621: watch for replies to the new comment 5749562438 (post-mortem of
  run A vs G3 is already conceded honestly in it).
- zhengkid/Dream-RSI: no code released yet (README+PDF only); relevant to
  replay-style experiment selection, NOT to fine-tuning. Re-check if "Full
  codebase" flips to available.

## Fleet
- enigma: experiment files intact (~186 GB model + runs/), arms deleted,
  369G free. No process running. Re-check with: bench/observatory.py
- Air: replication done, servers untouched. No cron running (watchdog
  removed after publication).
- This Mac: no colibri serve assumed running; port 8123 = coli serve,
  8000 = ssh tunnel (dsh-lite default collides — known).

## Never
- leastgen-prod (100.104.44.89): production box, no batch work.
- dsh-lite-cpp/*: sibling agent's workspace.
- Published artifacts listed above: immutable evidence.
