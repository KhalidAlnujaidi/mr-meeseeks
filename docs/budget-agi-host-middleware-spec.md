# Budget-AGI Host Middleware Spec (token firewall + depth limits + worktree isolation)

> Assembled from team `budget-agi-middleware-spec` tasks t1 (survey) + t2 (token/depth middleware) + t3 (worktree isolation).
> Convention: every rule is tagged **[PROMPT]** (advisory doctrine text, LLM self-discipline) or **[HOST-ENFORCED]** (deterministic host gate that refuses/blocks regardless of what the prompt says). On conflict, the host gate wins.
> Sources: `preset/budget-agi/agent.cordis.yml` (doctrine), `harness/loop.ts` (`splitOnce()` guards), `skills/forward-nudge/GUARDRAILS.md` (caps), `VISION.md` (atomicity/budget rules).

## §0 Prompt doctrine fallback quotes

Kept verbatim so behavior is defined even where no host gate exists yet. All quotes below are **[PROMPT]**.

- Atomicity stop-rule **[PROMPT]**: "A task is atomic when its result fits in one tool call and one verification check; do it and report back. Otherwise split ONCE into the smallest useful sub-tasks, delegate, verify each return, and stop. Never split an already-atomic task." (`VISION.md`)
- Budget counting **[PROMPT]**: "Before delegating past depth 2, estimate total agents (breadth^depth) against the day's request budget. A spiral (same task re-split three times) is a decomposition bug: stop and report." (`VISION.md`)
- Forward-nudge caps **[PROMPT]**: "MAX_NUDGE_DEPTH=3 chained auto-continuations without a user turn; MAX_PARALLEL_PATHS=5 per report (prune to top-5, defer the rest); MAX_ROUNDS_PER_TASK=2 retries then escalate narrower; never split an already-atomic task; same task re-split >=3x is a decomposition bug — stop and report." (`preset/budget-agi/agent.cordis.yml` §forward-nudge, full contract `skills/forward-nudge/GUARDRAILS.md`)
- Escalate honestly **[PROMPT]**: "If a team fails twice on the same task, say so and propose a narrower retry or a different approach — never silently redo the labor yourself at full scope." (doctrine §5)
- Verify before presenting **[PROMPT]**: "Re-read diffs, re-run key claims, check cited files exist." (doctrine §3)
- User interrupt always wins **[PROMPT]**: "Cancel unclaimed continuations, let claimed atoms finish, report cancelled vs finished. Never auto-resume an interrupted chain — wait for explicit user continue. Interrupt keywords: stop / pause / hold / don't continue." (GUARDRAILS.md)

### §0.1 Why prompt-only is unsafe (t1 survey summary)

| # | Prompt-only rule **[PROMPT]** | Why the LLM cannot self-enforce it | Host invariant **[HOST-ENFORCED]** that replaces it |
|---|---|---|---|
| 1 | Atomicity stop-rule (judge own future tool count, stop recursing) | No backstop once delegation is uncapped; one misjudged "non-atomic" recurses until budget burns | I1 depth firebreak + I8 atomic lock |
| 2 | COUNT THE TREE (breadth^depth vs 1000/day, "before delegating past depth 2") | Needs arithmetic over an invisible tree + a live counter the LLM does not have | I2 budget gate on host-owned daily ledger |
| 3 | Spiral detection (same-task identity across rewordings, cross-turn counting) | Each split looks locally reasonable; LLMs do not track lineage reliably | I4 spiral breaker on normalized goal-hash lineage counter |
| 4 | Nudge caps (self-maintain counters while ordered to "DO it in the same turn") | Fan-out pressure vs self-policing; nothing rejects the 6th path or 3rd retry | I5 fan-out cap, I6 retry cap, I7 nudge-depth cap |
| 5 | Interrupt/stop ("cancel unclaimed, never auto-resume") | Prompt revocation with no host cancellation binding; next auto-continue can re-drive the chain | I9 interrupt binding (scheduler cancels + resume-lock) |
| 6 | Verify-before-presenting | Unattestable prose; nothing records what was re-read/re-run | I10 verification attestation (check artifact required to close) |

Full invariant list (normative IDs): **I1** depth firebreak (childDepth ≤ memberMaxDepth) @ SPAWN GATE **[HOST-ENFORCED]**; **I2** budget gate (breadth^depth ≤ remaining daily budget) @ TASK-CREATE + SPAWN GATES **[HOST-ENFORCED]**; **I3** split-once (one delegation step per parent id) @ TASK-CREATE GATE **[HOST-ENFORCED]**; **I4** spiral breaker (goal-hash re-split ≥3x → block, force stop+report, needs human continue) @ TASK-CREATE GATE **[HOST-ENFORCED]**; **I5** fan-out cap (≤5 children per parent per turn; 6th rejected "prune to top-5") @ TASK-CREATE GATE **[HOST-ENFORCED]**; **I6** retry cap (≤2 attempts per task id, then escalate-only lock) @ TASK-CREATE GATE / scheduler **[HOST-ENFORCED]**; **I7** nudge-depth cap (≤3 chained auto-continuations, reset on user message) @ PER-TURN GATE **[HOST-ENFORCED]**; **I8** atomic lock (atomic-flagged task rejects children) @ TASK-CREATE GATE **[HOST-ENFORCED]**; **I9** interrupt binding (keywords → host cancels unclaimed + resume-lock) @ PER-TURN GATE **[HOST-ENFORCED]**; **I10** verification attestation (close requires check artifact: file re-read hash, command exit code, or explicit waiver) @ PER-TURN / task-close gate **[HOST-ENFORCED]**. Priority: I1+I2 first (bound worst-case cost even if all else fails).

---

## Part A — Token Firewall + Depth Middleware **[HOST-ENFORCED]**

Codifies `harness/loop.ts` (`MAX_RESPLITS=3`, `DEFAULT_BUDGET_MAX_AGENTS=64`, `SpiralStop`, `BudgetExceeded`, `splitOnce()` guards). The uncapped-depth experiment (fork, `memberMaxDepth` default removed) is why this is a host firebreak: prompt rules alone did not bound `breadth^depth`.

### A.1 Purpose

Turn three prompt doctrines into deterministic host gates that run before any spawn/split/turn consumes budget: (1) token firewall — refuse work that cannot fit the day's request budget **[HOST-ENFORCED]**; (2) depth firebreak — re-add `memberMaxDepth` as host enforcement plus nudge caps **[HOST-ENFORCED]**; (3) spiral detector — stop decomposition-bug lineages, never re-split them **[HOST-ENFORCED]**.

### A.2 Config schema **[HOST-ENFORCED]**

```ts
interface BudgetMiddlewareConfig {
  // — token firewall —
  dailyRequestCap: number;        // default 1000 (OpenRouter free tier, SHARED across all sessions/keys)
  perTreeRequestBudget: number;   // default 64 (== DEFAULT_BUDGET_MAX_AGENTS); max agents one tree may ever cost
  perDepthBreadthCap: number;     // default 8; max children any single split may create
  softWarnRatio: number;          // default 0.7; estimate > 70% of per-tree budget → warn
  hardDenyRatio: number;          // default 1.0; estimate > 100% of per-tree budget → deny
  // — depth limits —
  memberMaxDepth: number;         // default 1 (DSH stock default); 0 = members may not delegate at all
  maxNudgeDepth: number;          // default 3 (MAX_NUDGE_DEPTH)
  maxParallelPaths: number;       // default 5 (MAX_PARALLEL_PATHS)
  maxRoundsPerTask: number;       // default 2 (MAX_ROUNDS_PER_TASK, retries; 3rd attempt must be narrower)
  // — spiral detector —
  maxResplits: number;            // default 3 (MAX_RESPLITS / SPIRAL_DETECTOR)
  // — accounting —
  ledgerPath: string;             // JSONL ledger; every gate decision appends a line
}
```

Precedence: per-call opts (`budgetMax`, `workerModels`) > tree config > host defaults. `dailyRequestCap` is global (not per-tree, not per-session) because the 1000/day OpenRouter cap is shared.

### A.3 Token firewall **[HOST-ENFORCED]**

Normative estimator (exactly `harness/loop.ts:630`):

```
estimate = breadth^(parentDepth + 1)
```

`breadth` = `children.length` of the proposed split; `parentDepth` = depth of the task being split (root = 0). Worst-case downstream agents if every child re-splits at the same breadth — deliberately pessimistic; it over-blocks fan-out, the failure mode that burns the daily cap.

Worked examples (`perTreeRequestBudget=64`): breadth 3 @ depth 0 → 3, ALLOW; breadth 3 @ depth 2 → 27, ALLOW no warn (27 ≤ 44); breadth 5 @ depth 2 → 125, HARD-DENY; breadth 8 @ depth 1 → 64, ALLOW at exactly budget.

| Condition | Gate action |
|---|---|
| `estimate ≤ softWarnRatio × perTreeRequestBudget` (default ≤ 44) | ALLOW, no annotation |
| `44 < estimate ≤ 64` | ALLOW + `BUDGET_WARN` ledger line + brain-visible warning |
| `estimate > perTreeRequestBudget` | HARD-DENY (`BudgetExceeded`); no children created, parent NOT marked done |
| `breadth > perDepthBreadthCap` (default > 8) | HARD-DENY (`BREADTH_CAP`) regardless of estimate |
| `dayUsed + estimate > dailyRequestCap` | HARD-DENY (`DAILY_CAP`) — shared 1000/day guard |

`dayUsed` = count of request-type ledger lines today (all trees, all sessions). Kilo-lane workers have no token readout — count requests (wall-seconds + chars are proxy telemetry only, never budget currency).

Destructive-action rule **[HOST-ENFORCED]**: splits whose children contain irreversible verbs (push, publish, delete, migrate, credential change) are never auto-executed: gate returns `DESTRUCTIVE_PROPOSE_ONLY`; the brain may propose them in a report but must not enqueue them in the same turn (GUARDRAILS.md stop-condition 5).

### A.4 Depth limits **[HOST-ENFORCED]**

| Cap | Default | Enforcement point | On exceed |
|---|---|---|---|
| `memberMaxDepth` (re-added firebreak; I1) | 1 | **pre-spawn**: member `subagent`/`subagent_fork` with `callerDepth + 1 > memberMaxDepth` | HARD-DENY `DEPTH_FIREBREAK`; 0 forbids all member delegation |
| `maxNudgeDepth` (MAX_NUDGE_DEPTH; I7) | 3 | **per-turn**: chained auto-continuations from one lineage without intervening user turn | Stop, plain report, `Auto-continue: withheld (depth 3/3)` |
| `maxParallelPaths` (MAX_PARALLEL_PATHS; I5) | 5 | **pre-task-create**: one report proposing > 5 parallel continuations | Enqueue top-5 by value, remainder as `Deferred:` lines |
| `maxRoundsPerTask` (MAX_ROUNDS_PER_TASK; I6) | 2 retries | **pre-task-create / per-turn**: same task failing verification twice | No full-scope retry; 3rd attempt must be narrower/different, else escalate |

Depth accounting: `depth = taskId.split('.').length - 1` (root 0, +1 per hop); nudge-depth increments per chained auto-enqueue from the same lineage, resets to 0 on any user turn. `memberMaxDepth` bounds *delegation* depth (who may spawn); `maxNudgeDepth` bounds *auto-continue chain* length (how long without a human). Both must hold.

### A.5 Spiral detector **[HOST-ENFORCED]** (I4)

- Per-task counter `resplit_count`: 0 at enqueue, +1 on the parent at each `splitOnce()`, inherited by children as the parent's NEW count (`loop.ts:634-646`).
- Gate order in `splitOnce()` is normative: **spiral check BEFORE budget check**. Lineage at `resplit_count >= maxResplits` (3) is failed with result `SPIRAL: decomposition bug, awaiting instructions`, a `report`-type ledger line is written, `SpiralStop` thrown — even if budget would also deny.
- Third re-split is never created. Recovery requires brain re-frame (narrower criterion, different approach), never an automatic 4th split of the same criterion.
- `same task re-split ≥ 3×` counts lineage splits (`t.resplit_count`), not verification failures (`maxRoundsPerTask`); both reported as `retries <r>/2, resplits <s>/3`.

### A.6 Enforcement gates **[HOST-ENFORCED]**

1. **GATE-SPAWN (pre-spawn)**: every member `subagent`/`subagent_fork`/team-spawn call. Checks: `memberMaxDepth` firebreak (I1), `DAILY_CAP` (`dayUsed + 1 > dailyRequestCap` → deny, I2), destructive-verb scan → propose-only. Deny writes only a `DENY` ledger line.
2. **GATE-SPLIT (pre-task-create / splitOnce)**: every fan-out. Order: (a) spiral (I4, `resplit_count >= 3` → `SpiralStop`), (b) hetero-assignment (multi-child split with < 2 distinct `workerModel`s → `HomogeneousAssignment`, R2-B preserved), (c) breadth cap, (d) `breadth^depth` vs per-tree budget (warn/deny, I2), (e) daily cap, (f) split-once/atomic-lock (I3/I8: same parent id split twice, or atomic parent → reject). Exactly one `split` ledger line on allow; `DENY` line on refuse.
3. **GATE-TURN (per-turn, report close)**: every member/captain report. Checks: `maxNudgeDepth` (I7), `maxParallelPaths` prune + `Deferred:` (I5), `maxRoundsPerTask` (I6), goal-met → stop, user-interrupt keywords / plan-mode entry / pending `ask_user_question` → `Auto-continue: withheld` (I9), completed status requires check artifact (I10). Report MUST cite `depth <d>/3, parallel <p>/5, retries <r>/2, resplits <s>/3`.

Prompt text contradicting a gate is advisory; the gate wins and says so in its message.

### A.7 Error codes + brain-visible messages **[HOST-ENFORCED]**

| Code | Gate | Brain-visible message (exact) |
|---|---|---|
| `SPIRAL_STOP` | GATE-SPLIT | `task {id}: resplit_count reached 3, lineage stopped — SPIRAL: decomposition bug, awaiting instructions` |
| `BUDGET_EXCEEDED` | GATE-SPLIT | `task {id}: split refused, breadth^depth estimate {estimate} > budget {max}` |
| `BREADTH_CAP` | GATE-SPLIT | `task {id}: split refused, breadth {b} > per-depth cap {cap}` |
| `DAILY_CAP` | GATE-SPAWN / GATE-SPLIT | `day budget exhausted: used {u}/1000 requests, proposed +{e} — stopped, resume tomorrow or narrow scope` |
| `DEPTH_FIREBREAK` | GATE-SPAWN | `spawn refused: depth {d} > memberMaxDepth {m} (task {id})` |
| `HOMO_ASSIGNMENT` | GATE-SPLIT | `task {id}: multi-child split needs ≥2 distinct workerModel (got: {models})` |
| `BUDGET_WARN` (not an error) | GATE-SPLIT | `warning: estimate {e} is {pct}% of tree budget {max} (task {id}) — proceeding, prune follow-ups` |
| `DESTRUCTIVE_PROPOSE_ONLY` | GATE-TURN | `proposed only, not enqueued (destructive): {criterion} — needs explicit user approval` |
| `MAX_ROUNDS` | GATE-TURN | `task {id}: failed verification twice — no full-scope retry; propose narrower attempt or escalate` |

All denies are fail-closed (no partial children, no state change except the `DENY` ledger line) and name the cap, the numbers, and the recovery (narrow scope / prune / wait for user).

### A.8 Metrics / counters **[HOST-ENFORCED]**

Ledger (`ledger.jsonl`) event types (existing, preserved): `split | report | verify`, each with `{task_id, parent_id, depth, model, role, ts, cost, criterion, verdict, detail, resplit_count}`. Middleware adds `DENY` lines (`{gate: SPAWN|SPLIT|TURN, code, estimate?, dayUsed?}` — every refusal auditable) and `BUDGET_WARN` lines on soft-warn allows. Per-report gauges: `dayUsed/1000`, `treeUsed/treeBudget` (sum of estimates enqueued in this tree), `depth d/3, parallel p/5, retries r/2, resplits s/3`. Experiment telemetry (`VISION.md` §What to instrument, unchanged): depth distributions, per-level cost ratios (verifier vs worker), model-by-role performance, re-split counts. Kilo-lane entries record `wall-seconds + chars` as proxy telemetry alongside the request count, never instead of it.

### A.9 Acceptance (dry-run) **[HOST-ENFORCED]**

From `GUARDRAILS.md` §Dry-run self-test plus firewall cases: (1) 3 follow-ups → 3 parallel tasks sharing `deps=[t_done]`, `depth 1/3, parallel 3/5` **[HOST-ENFORCED]**; (2) user `stop` → zero new tasks, `Auto-continue: withheld (user interrupt)` **[HOST-ENFORCED]**; (3) 6 follow-ups → top-5 enqueued + 1 `Deferred:` line **[HOST-ENFORCED]**; (4) same task fails 2× → no full-scope retry **[HOST-ENFORCED]**; (5) destructive follow-up → proposal-only, no execution same turn **[HOST-ENFORCED]**; (6) breadth 5 @ depth 2 (est. 125 > 64) → `BUDGET_EXCEEDED`, zero children **[HOST-ENFORCED]**; (7) 4th split of one lineage → `SPIRAL_STOP`, lineage failed **[HOST-ENFORCED]**.

---

## Part B — Git Worktree Isolation Layer **[HOST-ENFORCED]**

> Status: **[HOST-ENFORCED]**. Docker is explicitly OUT of scope — local `git worktree` only.
> Verified against host: git 2.55.0, repo main worktree clean at `b0986b8`, `.agent-teams/` already covered by `.gitignore:5`, so all worktree paths below are untracked by construction.

### B.1 Goals / non-goals

Goals **[HOST-ENFORCED]**: (1) every sub-agent task executes in its own filesystem checkout — no two writers share a CWD; (2) the main workdir (`main` branch checkout) is write-exclusive to BRAIN/captain — workers can never dirty it; (3) merge-back into main happens only via an explicit diff/patch review gate owned by BRAIN; (4) lifecycle (create → work → propose → release → cleanup) is deterministic host code, not LLM discipline.
Non-goals: containers, VMs, remote runners, cross-repo worktrees, partial-file checkouts (`--sparse` deferred).

### B.2 Naming + layout (normative) **[HOST-ENFORCED]**

- Base ref: pinned at acquire time as `{baseBranch, baseSha}`. Default `baseBranch = main`, `baseSha = $(git rev-parse main)`. Both recorded in the registry entry; the branch is created from exactly `baseSha` (`git worktree add -b <branch> <path> <baseSha>`), never from a moving ref.
- Branch name: `task/<team>/<task-id>` (e.g. `task/budget-agi-middleware-spec/t3`). On collision (branch exists from a prior attempt): `task/<team>/<task-id>-r<attempt>`. Workers never choose branch names — the host mints them.
- Workdir path: `.agent-teams/<team>/worktrees/<task-id>/` relative to repo root (e.g. `.agent-teams/budget-agi-middleware-spec/worktrees/t3/`). Retry reuses the path only after prior worktree is fully released; otherwise `-r<attempt>` suffix mirrors the branch (`worktrees/t3-r2/`).
- Registry (host-owned, alongside team state, never inside a worktree): `.agent-teams/<team>/worktrees.json` — array of `{taskId, attempt, branch, path, baseBranch, baseSha, owner, state, createdAt, updatedAt}`. `state ∈ {active, proposed, merged, discarded, stale}`.
- One worktree per (task, attempt). One branch per worktree. No nested worktrees.

### B.3 Lifecycle state machine (host executes each transition) **[HOST-ENFORCED]**

```
acquire → active → propose → (merged | discarded) → released
              \-> stale (base moved beyond threshold, §B.6) → rebase-or-reacquire
active --(task complete/fail/cancel)--> propose|discarded --> cleanup --> released
```

1. **acquire(taskId, team, base?)**: host checks preconditions (§B.6), mints branch+path, runs `git worktree add -b <branch> <path> <baseSha>`, writes registry entry `active`, returns `{path, branch, baseSha}`. Member CWD is pinned to `path` for the whole attempt (sandbox workdir pin — writes outside `path` are denied).
2. **work (active)**: worker commits freely inside its worktree (or leaves changes uncommitted — both supported). Worker MUST NOT touch the main workdir, MUST NOT `git push`, MUST NOT `git merge` into any other branch. (Must-nots are **[HOST-ENFORCED]** via the sandbox pin + denied git verbs, with the prompt quote kept as **[PROMPT]** fallback.)
3. **propose**: on task terminal state the host runs, inside the worktree: `git add -A -n` (dry list) + `git diff <baseSha>...HEAD --stat` + full `git diff` saved to `.agent-teams/<team>/patches/<task-id>.patch`, plus `git status --porcelain`. Registry → `proposed`. The patch file (not the worktree) is what BRAIN reviews.
4. **review + merge-back (BRAIN only)**: BRAIN inspects the patch, then in the MAIN workdir: `git apply --check patch` → apply (`git apply` or `git am`) or `git merge --no-ff <branch>` at its discretion → run pre-flight (build green, one API regression, `git grep sk-or-` clean). Registry → `merged`. Workers never execute this step.
5. **release + cleanup**: after merge decision (or on fail/cancel with no patch accepted): `git worktree remove --force <path>` → `git worktree prune` → delete task branch (`git branch -D`, unless `keepOnFail=true` for forensics) → registry → `merged|discarded` and entry tombstoned. Worktree paths are gitignored, so even a leaked directory can never pollute a commit; a periodic sweeper prunes entries older than 7 days.

### B.4 Collision avoidance **[HOST-ENFORCED]**

- **No shared CWD writes**: host pins each member's file-sandbox root to its worktree `path`. Main workdir accepts writes only from the captain/BRAIN session. Two tasks never share a path (§B.2).
- **Branch uniqueness**: branch minting is atomic in the host (check-then-create under a lock); same-name retry gets `-r<attempt>`. A branch checked out in any worktree cannot be checked out twice — git itself refuses; host surfaces it as `WT_BRANCH_CHECKED_OUT`.
- **Merge serialization**: only one merge-back into main runs at a time (host mutex); each merge re-validates `git apply --check` against current main tip, so concurrent patches cannot silently stack.
- **`.git` sharing**: worktrees share the object store but have independent indexes — status/diff in one worktree never leaks into another. `node_modules/` is NOT shared: each worktree bootstraps deps itself (`npm ci`); disk cost accepted over cross-talk risk.

### B.5 Cleanup policy **[HOST-ENFORCED]**

| Terminal disposition | Patch kept? | Worktree | Branch |
|---|---|---|---|
| `complete` + patch accepted (merged) | yes (`patches/<task>.patch`) | `remove --force` + `prune` | `-D` |
| `complete` + patch rejected | yes, 7 days (forensics) | `remove --force` | `-D` |
| `fail` | yes if `keepOnFail` else no | `remove --force` | `-D` unless kept |
| `cancel` | `git diff` snapshot kept 24h | `remove --force` | `-D` |
| orphan (`list` shows active but task terminal >1h) | snapshot then discard | sweeper removes | `-D` |

Sweeper: `git worktree prune` + delete registry entries in terminal state older than 7 days. Leaked (unregistered) directories under `worktrees/` are safe to `rm -rf` — gitignored, no unique objects (object store lives in main `.git/`).

### B.6 Failure modes (detection → host action → error code) **[HOST-ENFORCED]**

| # | Mode | Detection (host, pre-acquire or pre-merge) | Action | Code |
|---|---|---|---|---|
| F1 | Dirty MAIN tree | `git status --porcelain` non-empty in main at acquire/merge | refuse acquire/merge; BRAIN stashes/commits first | `WT_MAIN_DIRTY` |
| F2 | Stale base | `baseSha != $(git rev-parse baseBranch)` AND main moved > threshold (default: any move) | mark `stale`; rebase worktree or re-acquire from new tip; patch regenerates | `WT_STALE_BASE` |
| F3 | Worktree lock | `git worktree list --porcelain` shows `locked`, or `path/.git` missing/broken | `git worktree repair` once; else `remove --force` + re-acquire | `WT_LOCKED` / `WT_BROKEN` |
| F4 | Branch collision | `git show-ref --verify refs/heads/<branch>` exists and is checked out elsewhere | mint `-r<attempt>` name (§B.2) | `WT_BRANCH_CHECKED_OUT` |
| F5 | Path collision | `path` exists on disk but unregistered | refuse; BRAIN inspects, `rm -rf` only after confirm it is gitignored scratch | `WT_PATH_EXISTS` |
| F6 | Merge conflict | `git apply --check` or `merge --no-ff` fails | patch returned to BRAIN with conflict hunks; worktree KEPT until BRAIN decides (no auto-resolve) | `WT_MERGE_CONFLICT` |
| F7 | Untracked-only work | `diff base...HEAD` empty but `status --porcelain` shows only `??` | include untracked via `git add -N` + diff; never silently drop | `WT_UNTRACKED_ONLY` |

All failures reported to BRAIN with code + exact command output. Workers receive only "worktree unavailable, waiting for host" — they never self-heal git state.

### B.7 Host API surface **[HOST-ENFORCED]**

```ts
acquire(input: { team: string; taskId: string; attempt: number; baseBranch?: string /* default main */ })
  => { path: string; branch: string; baseSha: string }
  // git worktree add -b <branch> <path> <baseSha>; registry insert active

propose(input: { team: string; taskId: string })
  => { patchFile: string; stat: string; statusPorcelain: string }
  // git diff <baseSha>...HEAD > patches/<task>.patch

release(input: { team: string; taskId: string; disposition: 'merged'|'discarded'; keepBranch?: boolean })
  => void
  // git worktree remove --force <path>; git worktree prune; git branch -D (unless kept)

list(input: { team?: string })
  => Array<{ taskId; branch; path; state; baseSha; updatedAt }>
  // git worktree list --porcelain joined with registry

mergeBack(input: { team: string; taskId: string; strategy: 'apply'|'merge-ff'|'merge-no-ff' })
  => { mergedSha: string }   // BRAIN/captain sessions only; enforces WT_MAIN_DIRTY + WT_MERGE_CONFLICT gates
```

Shared gates with Part A: `acquire` is a pre-task-create gate (denied when budget/depth firewalls trip — no worktree, no spend); `propose`/`mergeBack` run at task-terminal time so patch review is itself a counted, depth-limited BRAIN action, not free recursion.

### B.8 Config schema fragment **[HOST-ENFORCED]**

```yaml
worktree:
  root: ".agent-teams/<team>/worktrees"
  branchPrefix: "task/<team>"
  baseBranch: "main"
  stalePolicy: "mark-and-reacquire"   # or: rebase
  keepOnFail: false
  patchDir: ".agent-teams/<team>/patches"
  sweepAfterDays: 7
  mergeStrategy: "merge-no-ff"        # apply | merge-ff | merge-no-ff
  mergeMutex: true
```

---

## §C Rule index (every rule tagged)

| ID | Rule | Type | Gate |
|---|---|---|---|
| §0 quotes (atomicity, budget, nudge caps, escalate, verify, interrupt) | Doctrine fallback | **[PROMPT]** | none (advisory) |
| I1 memberMaxDepth firebreak | childDepth ≤ memberMaxDepth | **[HOST-ENFORCED]** | GATE-SPAWN |
| I2 token firewall (per-tree + daily) | breadth^depth vs budgets | **[HOST-ENFORCED]** | GATE-SPLIT + GATE-SPAWN |
| I3 split-once | one delegation step per parent id | **[HOST-ENFORCED]** | GATE-SPLIT |
| I4 spiral breaker | goal-hash re-split ≥3x → block | **[HOST-ENFORCED]** | GATE-SPLIT |
| I5 fan-out cap | ≤5 children/parent/turn | **[HOST-ENFORCED]** | GATE-SPLIT / GATE-TURN |
| I6 retry cap | ≤2 attempts/task, then escalate-only | **[HOST-ENFORCED]** | GATE-SPLIT / scheduler |
| I7 nudge-depth cap | ≤3 auto-continues, reset on user turn | **[HOST-ENFORCED]** | GATE-TURN |
| I8 atomic lock | atomic task rejects children | **[HOST-ENFORCED]** | GATE-SPLIT |
| I9 interrupt binding | keywords → cancel + resume-lock | **[HOST-ENFORCED]** | GATE-TURN / scheduler |
| I10 verification attestation | close requires check artifact | **[HOST-ENFORCED]** | task-close gate |
| B lifecycle/collision/cleanup/failures/API | Worktree isolation | **[HOST-ENFORCED]** | acquire/propose/mergeBack/release |

---

## §D Vision-delta appendix (new architecture vision reconciliation)

> Status: deltas vs new architecture vision. Normative additions tagged **[HOST-ENFORCED]**; rationale/advisory tagged **[PROMPT]**. On conflict with Parts A–C, this appendix wins for the named rule and cites the superseded line.
> Reviewed: verifier verdict CHANGES-NEEDED applied (stall-count alignment, flat-layout collision caveat, network-isolation best-effort tag).

### D.0 Verdict table

| # | Vision item | Verdict | One-line delta |
|---|---|---|---|
| 1 | Host Token Guard as pre-call proxy B^D | EQUIVALENT formula, DELTA enforcement point | `estimate = breadth^(parentDepth+1)` is exactly `harness/loop.ts:630`; vision moves it to a shared pre-call proxy covering GATE-SPAWN + GATE-SPLIT (A.3/A.6 only gate fan-out ≥2). Adopt shared check. |
| 2 | Runtime Auto-Nudge Loop in host event loop | MISSING — spec it | GATE-TURN (A.4/A.6.3, I7 `maxNudgeDepth=3`) is a passive per-turn counter. Vision adds active loop: stall/yield detection → re-prompt → fallback routing. New §D.2. |
| 3 | Worktree Sandbox `agent/<task-id>` + `.worktrees/<task-id>` | DELTA naming/layout | Spec §B.2 canonical is team-scoped `task/<team>/<task-id>` + `.agent-teams/<team>/worktrees/`. Adopt dual-support mapping, canonical stays team-scoped. New §D.3. |
| 4 | Verification Pipeline (host runs tests/linters in worktree pre-review) | MISSING — add section | I10 (A.6.3) attests a check artifact but never executes it. New §D.4: host executes `npm test` / `pytest` / linters inside the worktree at `propose` time; exit codes gate `proposed` vs `discarded`; rationale = Programmatic Gatekeeper + sycophancy immunity. |

### D.1 Token Guard: pre-call proxy B^D **[HOST-ENFORCED]**

- Formula is EQUIVALENT — normative estimator unchanged (`harness/loop.ts:630`):
  `estimate = breadth^(parentDepth + 1)`, `breadth` = proposed children count, `parentDepth` = splitting task depth (root 0). Worked examples in A.3 stand.
- DELTA is the enforcement point. Current A.6 checks B^D only at GATE-SPLIT (fan-out of ≥2 children). A breadth-1 chain (`subagent` per turn, depth crawling 0→1→2…) never trips GATE-SPLIT yet consumes `depth+1` requests against the shared 1000/day cap — the exact leak the vision's pre-call proxy closes.
- Normative change **[HOST-ENFORCED]**:
  1. Extract `estimatePreCall(breadth, parentDepth)` as the single shared function used by BOTH GATE-SPAWN (with `breadth=1` for a single spawn: `estimate = 1^(d+1) = 1`, i.e. pure daily-cap + depth-firebreak check) and GATE-SPLIT (existing A.3 table).
  2. GATE-SPAWN pre-call order: (a) `memberMaxDepth` firebreak (I1), (b) shared pre-call: `dayUsed + estimate > dailyRequestCap` → `DAILY_CAP` deny (I2), (c) destructive-verb scan → propose-only. This makes every model call pass one proxy, matching the vision's "pre-call proxy" language, while preserving the pessimistic fan-out estimator for splits.
  3. Ledger: every pre-call refusal writes the existing `DENY {gate, code, estimate, dayUsed}` line (A.8); no new event type.
- Migration: `splitOnce()` guard order (spiral BEFORE budget, A.5) is unchanged; the pre-call proxy wraps it, it does not replace it. Supersedes A.6.1's implication that spawn checks only `dayUsed + 1` — now spawn and split share one function with different `breadth` inputs.

### D.2 Runtime Auto-Nudge Loop **[HOST-ENFORCED]** (extends I7 / A.4 / GUARDRAILS.md)

- Current state is a passive counter: GATE-TURN refuses the 4th chained auto-continuation (`maxNudgeDepth=3`, reset on user turn). Nothing detects a stalled worker, nothing re-prompts it, nothing routes around it — the vision's active loop is missing.
- Normative loop **[HOST-ENFORCED]**, runs in the host event loop per active (task, attempt):
  1. **Stall detection**: no ledger line (`split|report|verify|DENY`) from the owner for `stallAfterMs` (default 120_000; configurable per `BudgetMiddlewareConfig`, cf. A.2) → mark `suspected-stall`, emit ledger `report {detail: 'NUDGE: suspected-stall'}`. Heartbeat rule: any tool call / turn completion resets the timer — wall-clock only, never LLM self-report.
  2. **Yield detection**: owner emits `report` with `Auto-continue: withheld` short of goal, or `verify` verdict `false` with `retries < 2` remaining → eligible for re-prompt, not auto-close.
  3. **Re-prompt (≤ maxNudgeDepth=3, shares the I7 budget)**: host re-queues the SAME (task, attempt) with an injected `nudge {nudgeDepth, reason: stall|yield-fail, lastDetail}` context line. Nudge-depth increments on each host re-prompt AND each member auto-continue; either reaching 3 → stop, plain report, `Auto-continue: withheld (depth 3/3)`. Nudge-depth resets to 0 on any user turn (same rule as A.4).
  4. **Fallback routing / escalation**: stalls 1–2 re-prompt within the nudge budget; on the 3rd consecutive stall, on nudge-budget exhaustion (`depth 3/3`), or on `MAX_ROUNDS` (2 verification failures, I6), the host does NOT retry full scope — it escalates: (a) re-scope to narrower criterion, or (b) reassign to a different `workerModel` (hetero rule R2-B still applies), or (c) `SPIRAL`-style stop+report to BRAIN. A 3rd full-scope attempt is rejected at GATE-SPLIT/GATE-TURN exactly as A.4/A.6.3 require.
  5. **Interrupt supremacy**: user `stop/pause/hold/don't continue`, plan-mode entry, or pending `ask_user_question` cancels all pending nudges for the lineage + sets resume-lock (I9). A stalled task interrupted while `suspected-stall` reports `cancelled vs finished` per GUARDRAILS.md interrupt protocol — never auto-resumes.
- Config additions (extend A.2 `BudgetMiddlewareConfig`): `stallAfterMs: number /* default 120000 */; nudgeOnYield: boolean /* default true */; fallbackModel?: string /* hetero re-route target */`. All three **[HOST-ENFORCED]** defaults; per-call opts may tighten, never loosen, `stallAfterMs`.
- Ledger (extend A.8): nudge re-prompts write `report {detail: 'NUDGE n/d reason'}` lines; fallback re-routes write `split` lines with `criterion: 'narrowed: …'` or changed `model`. Per-report gauges unchanged (`depth d/3, parallel p/5, retries r/2, resplits s/3`) — nudge-depth IS the reported `depth`.
- Acceptance (extend A.9): (8) worker silent 120s → 1 re-prompt with `NUDGE 1/3 suspected-stall`, timer reset on next ledger line; (9) 1st–2nd consecutive stalls → re-prompt within nudge budget; 3rd consecutive stall or nudge-budget exhausted (`depth 3/3`) → no re-prompt, escalate narrower/different-model or stop+report; (10) user `stop` during `suspected-stall` → zero further nudges, `Auto-continue: withheld (user interrupt)`.

### D.3 Worktree Sandbox naming: canonical mapping **[HOST-ENFORCED]**

- DELTA confirmed. Vision: `git worktree add -b agent/<task-id> .worktrees/<task-id> main`. Spec §B.2: `-b task/<team>/<task-id>` at `.agent-teams/<team>/worktrees/<task-id>/` from pinned `baseSha`.
- Decision: canonical stays team-scoped (spec form). Rationale **[PROMPT]**: team scope is load-bearing — registry (`worktrees.json`), patch dir (`patches/`), merge mutex, and sweeper are all per-team; flat `.worktrees/<task-id>` collides across teams and orphans the registry join in `list()` (B.7). Vision form is supported as an alias, not a replacement.
- Normative mapping **[HOST-ENFORCED]**:
  1. `acquire()` accepts optional `layout?: 'team' | 'flat'` (default `'team'`). `'flat'` mints branch `agent/<task-id>` (collision → `agent/<task-id>-r<attempt>`) at path `.worktrees/<task-id>/` (collision → `-r<attempt>` suffix, same rule as B.2) — but STILL writes the registry entry under `.agent-teams/<team>/worktrees.json` with the real `branch`+`path` recorded. All lifecycle/collision/cleanup/failure semantics (B.3–B.6) apply verbatim; only the minted strings differ.
  2. `list()` joins `git worktree list --porcelain` with the registry exactly as B.7 regardless of layout; `mergeBack()`/`release()` take paths from the registry, never reconstructed — so mixed-layout teams merge safely.
  3. Base ref rule unchanged: branch is created from pinned `baseSha` (`git worktree add -b <branch> <path> <baseSha>`), never a moving ref — the vision's `main` argument is read as `baseBranch=main` resolved to `baseSha` at acquire time per B.2.
- Cross-team collision caveat **[HOST-ENFORCED]**: flat `.worktrees/` is repo-global, not team-scoped — two teams reusing the same task-id (e.g. both minting `t1`) target the same path/branch. The `-r<attempt>` suffix disambiguates mechanically, but attempt-numbering is per-team so collisions are likely under parallel teams, not rare. Flat layout is therefore recommended for single-team-per-repo use only; multi-team repos MUST either stay on canonical `team` layout or resolve every path/branch exclusively via `list()`/registry (never by string concatenation). Cross-team flat collision surfaces as `WT_BRANCH_CHECKED_OUT` / `WT_PATH_EXISTS` per B.6, fail-closed.
- Config (extend B.8): `layout: 'team' /* default */ | 'flat'` **[HOST-ENFORCED]** default `team`.
- Migration note: existing checkouts under `.agent-teams/<team>/worktrees/` keep working — no moves. New teams that prefer the vision's shorter paths set `layout: 'flat'`; cross-team tooling MUST resolve paths via `list()`/registry, never by string concatenation. `.gitignore` already covers `.agent-teams/` (§B preamble); adding layout `flat` REQUIRES adding `.worktrees/` to `.gitignore` at adoption time (host refuses first `flat` acquire until `git check-ignore .worktrees/` succeeds → else `WT_GITIGNORE_MISSING`).

### D.4 Verification Pipeline (new Part D section) **[HOST-ENFORCED]**

- MISSING confirmed: I10 + B.3/B.4 require a check artifact and a BRAIN pre-flight (`build green, one API regression, git grep sk-or- clean`) but no host step ever EXECUTES tests/linters inside the worktree before the patch reaches BRAIN. Workers self-attest; BRAIN re-runs from scratch. The vision's pipeline closes this gap.
- Normative pipeline **[HOST-ENFORCED]** — host runs it inside the worktree at `propose` time, BEFORE the patch file is written (inserts between B.3 steps 3-pre and 3-post):
  1. **Detect**: `package.json` scripts → `npm test` (or `npm run verify` if `test` absent); `pytest.ini`/`pyproject.toml`/`conftest.py` → `pytest -q`; `eslint.config.*`/`.eslintrc*` → `npx eslint .`; `pyproject` ruff/black config → `ruff check .` / `black --check .`. Absent harness → record `verify.json {harness: 'none', verdict: 'waived-harness-absent'}` (waiver path, see 4).
  2. **Execute**: host spawns with CWD pinned to the worktree path, default timeout 300s (`verifyTimeoutMs`, extend A.2/B.8 config), exact argv recorded. Network isolation during verification is best-effort **[PROMPT]** (advisory, not a gate): where the runner supports sandboxing (e.g. `sandbox-exec`, bubblewrap, containers in a future revision) the host SHOULD deny egress so tests cannot exfiltrate or depend on live services; where unsupported, the host runs unisolated and records `verify.json {isolated: false}`. Isolation failure never blocks `proposed` — only exit codes gate.
  3. **Gate**: exit 0 across ALL detected harnesses → `propose` proceeds, `verify.json {verdict: 'pass', exitCodes, durationMs}` ships alongside the patch. ANY non-zero exit → registry stays `active` (NOT `proposed`), task annotated `verify-failed`, worker gets exactly the I6 budget (≤2 retries, 3rd attempt narrower/different) — then `discarded` with logs kept per B.5 forensics. Patch is never written for failing trees, so BRAIN never reviews red code.
  4. **Waiver**: docs-only patches (`diff` touches `*.md` only, zero code hunks) or harness-absent trees may close with `verify.json {verdict: 'waived-docs-only' | 'waived-harness-absent', approvedBy: 'brain'}` — waiver requires a BRAIN-authored line in the close report (I10 attestation), never worker self-waiver.
- Sycophancy-immunity rationale **[PROMPT]** (why host-executed, not worker-reported): a worker grading its own diff is incentivized to report green ("tests pass ✅") without running anything — prose attestation cannot distinguish. Exit codes from a host-owned runner inside the isolated worktree are unforgeable by the worker: the Programmatic Gatekeeper pattern (machine checks machine output before any human/LLM judge sees it) removes the "please agree with me" gradient entirely — BRAIN reviews a patch that already survived contact with the compiler, so its judgment is spent on semantics (does this fix the criterion?) rather than syntax (does it even run?). This is I10 made executable: the check artifact is no longer cited, it is produced.
- Config (extend B.8): `verify: { enabled: true, timeoutMs: 300000, failLoud: true }` **[HOST-ENFORCED]**.
- Ledger + registry (extend A.8/B.2): `verify` ledger lines gain `{exitCodes, durationMs, verdict}`; registry entries gain `verify: {verdict, verifyJsonPath}`. `propose()` return (B.7) gains `verifyJson`.
- Failure codes (extend B.6): `V_VERIFY_FAIL` (non-zero exit → worker retry budget, patch withheld); `V_VERIFY_TIMEOUT` (killed at `timeoutMs` → same as fail + `durationMs: timeoutMs`); `V_HARNESS_ABSENT` (no harness → waiver path, never silent-pass).
- Acceptance: (11) red `npm test` in worktree → `propose` refused, `V_VERIFY_FAIL`, patch file absent, worker retry 1/2; (12) green suite → `proposed` with `verify.json {verdict: pass}` + patch; (13) md-only diff, no harness → `waived-docs-only` close requires BRAIN waiver line, worker alone cannot close.

### D.5 Rule-index additions (extend §C)

| ID | Rule | Type | Gate |
|---|---|---|---|
| D.1 shared pre-call proxy (`estimatePreCall`, breadth=1 at spawn) | Token Guard on every call | **[HOST-ENFORCED]** | GATE-SPAWN + GATE-SPLIT |
| D.2 stall/yield detection + re-prompt + fallback routing (shares I7 nudge budget) | Auto-Nudge Loop | **[HOST-ENFORCED]** | host event loop + GATE-TURN |
| D.3 `layout: team\|flat` dual naming, registry-joined resolution | Worktree alias mapping | **[HOST-ENFORCED]** | acquire/list/mergeBack/release |
| D.4 host-executed test/lint suite at `propose`, exit-code gate | Verification Pipeline | **[HOST-ENFORCED]** | propose (pre-patch) |
| `V_VERIFY_FAIL` / `V_VERIFY_TIMEOUT` / `V_HARNESS_ABSENT` / `WT_GITIGNORE_MISSING` | New error codes w/ brain-visible messages | **[HOST-ENFORCED]** | propose / acquire |

Evidence: spec quotes verified against harness/loop.ts:630 (`Math.pow(children.length, t.depth+1)`), loop.ts:616 spiral-before-budget order, GUARDRAILS.md caps (MAX_NUDGE_DEPTH=3, MAX_PARALLEL_PATHS=5, MAX_ROUNDS_PER_TASK=2). Vision worktree command + Token Guard B^D + auto-nudge + verification-pipeline items have zero matches in-repo (grep over *, .worktrees/, agent/ branch prefix, stall/yield, sycophancy, Programmatic Gatekeeper) — confirmed greenfield additions except D.1's formula.
---

## §E Sister-leaf KV bus + host compaction/bubbling pipeline

> Status: deltas vs new architecture vision (source: L2 memory memo `SisterLeafMemoryEntry`/`SisterLeafBus` + 5-step pipeline). Normative additions tagged **[HOST-ENFORCED]**; rationale/advisory tagged **[PROMPT]**. On conflict with Parts A–D, this appendix wins for the named rule and cites the superseded line. t2 review applied: `gitDiffHash` conditional (E.2.3), status-enum case mapping + `exportedState` deny action (E.2.3 nits).

### E.1 Sister-leaf KV bus **[HOST-ENFORCED]**

Task-scoped IPC state layer for sister leaves ($L_{n,a} \leftrightarrow L_{n,b}$) managed by the host runtime middleware. Sisters coordinate through the bus; the L2 brain token window never carries raw exchange data.

#### E.1.1 Interfaces (normative, as given) **[HOST-ENFORCED]**

```ts
interface SisterLeafMemoryEntry {
  taskId: string;            // Sub-task ID within current DAG
  parentSessionId: string;   // L2 Brain Session ID (partition key, §E.1.2)
  namespace: string;         // E.g. "types", "api_schema", "build_outputs"
  key: string;
  value: Record<string, unknown> | string;
  visibility: 'sister_only' | 'promotable_to_l1';
  updatedBy: string;         // Worker agent instance ID
  timestamp: number;         // Host-assigned ms epoch (see E.1.3)
}

interface SisterLeafBus {
  publish(entry: SisterLeafMemoryEntry): void;
  read(namespace: string, key: string): SisterLeafMemoryEntry | null;
  listNamespace(namespace: string): Record<string, SisterLeafMemoryEntry>;
}
```

#### E.1.2 Partitioning **[HOST-ENFORCED]**

- Entries are partitioned by `parentSessionId`: a `read`/`listNamespace` call resolves **only** within the caller's own `parentSessionId` partition. Cross-partition reads return null/empty — fail-closed, no error-code change needed.
- Rationale **[PROMPT]**: sister leaves coordinate without cross-contaminating unrelated worker trees; partition isolation is what lets parallel teams share one host bus implementation.

#### E.1.3 Host-layer R/W via tool hooks **[HOST-ENFORCED]**

- All bus R/W executes at the host layer via tool hooks, not via worker-issued text. `timestamp` is host-assigned at `publish` (worker-supplied values ignored); `updatedBy` is host-stamped from the calling worker instance ID.
- `publish` overwrites per `(parentSessionId, namespace, key)` — last-writer-wins within a partition. No cross-key transactions in this revision (deferred).
- `value` size cap: 64 KiB serialized per entry (host refuses larger with `BUS_VALUE_TOO_LARGE`, entry unchanged). Keeps the bus an IPC state layer, not a blob store.

#### E.1.4 Visibility vs L0/L1/L2 promotion rule **[HOST-ENFORCED]**

- `sister_only`: visible only inside its `parentSessionId` partition. NEVER promotable, never surfaces in L2 vectors (§E.2.3), never reaches `proposePromotion()`. Partition dies with worktree release (§E.2.4).
- `promotable_to_l1`: marks a **candidate** for L1 promotion — nothing more. RECONCILIATION with the L0/L1/L2 promotion rule (L2→L1 EXPLICIT only; `proposePromotion()` gates secrets and never writes; only `approve(file)` appends to `.dsh/brain/`; blocked on secrets — keys, tokens, passwords — while session IDs, scratch paths, one-off debug output stay L2):
  1. `promotable_to_l1` entries MUST route through `proposePromotion()` + `approve(file)` — the secret-gate and explicit-append steps are never skipped, shortened, or batched-implicit.
  2. **Auto-merge is FORBIDDEN.** This explicitly supersedes the source memo line *"Items flagged as `promotable_to_l1` are automatically merged into persistent project memory upon task completion"* — that line is REJECTED and does not enter the spec. Completion may surface candidates to BRAIN; only BRAIN's explicit `approve(file)` writes L1.
  3. `proposePromotion()` secret screening applies to bus candidates exactly as to any L2 content: candidates containing secrets are blocked, never promoted.
- Enforcement point: promotion-gate at task-terminal time (alongside `propose`, cf. D.4/B.3). Bus holds zero L1-write authority.

#### E.1.5 Overlap verdict: bus **[HOST-ENFORCED]**

- **NEW**: partitioned KV R/W (`SisterLeafMemoryEntry` + `SisterLeafBus`, `parentSessionId` partitioning, host tool-hook R/W, 64 KiB cap) has zero matches in Parts A–D — greenfield addition. Sister-to-sister IPC exists nowhere else in the spec (A–D cover brain↔worker delegation, never leaf↔leaf).
- **REUSE (pointer, no new semantics)**: `parentSessionId` identity reuses the existing session-header parent linkage (`header.parentSession`); visibility reconciliation reuses the L0/L1/L2 promotion rule verbatim.

---

### E.2 Host compaction & bubbling pipeline **[HOST-ENFORCED]**

Runs in the host runtime when a sub-agent completes or hits the atomicity stop-rule. The host intercepts execution and compacts BEFORE passing control back to L2 — L2 receives only the result vector (§E.2.3), never raw leaf context.

#### E.2.1 Step 1 — Gatekeeper pass (REUSE → D.4) **[HOST-ENFORCED]**

- Reuses the D.4 verification pipeline verbatim: host executes `npm test` / linters / validation scripts inside the leaf's Git worktree at `propose` time; exit codes gate `proposed` vs `discarded`; waiver path (`waived-docs-only` / `waived-harness-absent`) requires a BRAIN-authored line, never worker self-waiver.
- Local re-prompt on verify-fail: host re-prompts the worker locally up to `MAX_ROUNDS_PER_TASK=2` (I6, A.4) — 3rd attempt must be narrower/different or escalate. L2 is uninterrupted: no L2 ledger lines, no L2 re-queue, no nudge-budget consumed until the retry budget is exhausted or verification passes.
- No new semantics; D.4 is the normative reference.

#### E.2.2 Step 2 — Structural pruning (REUSE → `agent.cordis.yml` compaction) **[HOST-ENFORCED]**

- Reuses the `tool-result-pruner` thresholds from `preset/budget-agi/agent.cordis.yml` §compaction verbatim: `thresholdChars: 8192` → prune; retain `headChars: 4096` + `tailChars: 1024`. Host strips intermediate tool calls and voluminous output dumps, applying head/tail extraction to raw logs.
- Pruning is lossy-by-design for transport only: the patch file (`patches/<task>.patch`, B.3/D.4) and `verify.json` are written BEFORE pruning and are never pruned. What is pruned is the leaf session transcript handed back toward L2.
- No new semantics; the cordis `tool-result-pruner` config is the normative reference.

#### E.2.3 Step 3 — Result vector compaction (NEW) **[HOST-ENFORCED]**

- The host forces the leaf session context into a strict 1–5 line structured result vector. Normative JSON schema:

```json
{
  "taskId": "task-8042",
  "status": "SUCCESS",
  "summary": "Implemented JWT authentication middleware and added unit test coverage.",
  "changedFiles": ["src/middleware/auth.ts", "tests/auth.test.ts"],
  "gitDiffHash": "a1b2c3d4",
  "exportedState": { "jwtHeaderKey": "X-Auth-Token" }
}
```

- Field bounds: `taskId` (string, required, == originating task id); `status` (canonical enum `SUCCESS | FAILED | DISCARDED`, required — host normalizes lowercase input `success|failed|discarded` to canonical; anything else is rejected); `summary` (string, required, 1–5 lines, no stack traces / no raw logs — those live in the pruned transcript + patch); `changedFiles` (array of repo-relative paths, required, may be empty); `gitDiffHash` (string, conditional — REQUIRED iff `patches/<task>.patch` exists: SUCCESS/`proposed` vectors MUST carry the short hash of the patch file, binding vector to patch; on FAILED/DISCARDED with no patch — B.5 fail-without-patch (`keepOnFail=false`) or D.4 patch-withheld-on-verify-fail (`V_VERIFY_FAIL`) — the field is the empty string `""` and the no-patch reason is carried in `summary`, no new field needed); `exportedState` (object, required, may be `{}` — leaf-declared contract keys for downstream sisters, e.g. header names, schema anchors; values are strings/numbers/booleans only, ≤ 4 KiB serialized).
- Status→disposition mapping (vector `status` vs B.3/B.5 registry dispositions): `SUCCESS` = `proposed` then `merged` (patch accepted); `FAILED` = `discarded` with `fail` terminal (patch rejected/absent, forensics per B.5); `DISCARDED` = `discarded` via cancel/stale path. The vector enum is uppercase by convention (L2-facing contract); the registry keeps its lowercase dispositions — the mapping above is the single bridge, no dual spelling inside either layer.
- `exportedState` enforcement (fail-closed, consistent with the bus `BUS_VALUE_TOO_LARGE` style — truncate-with-flag was considered and REJECTED because silent truncation corrupts downstream contract keys): oversize (> 4 KiB serialized) or non-string/number/boolean values → host refuses the vector with `BUS_VALUE_TOO_LARGE`, vector unchanged, leaf must resubmit narrower. No truncation, no flag bit.
- `sister_only` bus content MUST NOT appear in `summary` or `exportedState` (cf. E.1.4). `exportedState` carrying secrets is blocked at the promotion gate exactly as L1 candidates are.
- **NEW**: this schema exists nowhere in Parts A–D (A.8 ledgers record gate decisions, D.4 records `verify.json` — neither defines the L2-facing result vector). Greenfield addition.

#### E.2.4 Step 4 — Scratchpad & worktree release (REUSE → B.5/B.7) **[HOST-ENFORCED]**

- Reuses the B.5 cleanup table + B.7 `release()` verbatim: `git worktree remove --force <path>` → `git worktree prune` → `git branch -D` (unless `keepOnFail`), registry → terminal state, patch retained per B.5 disposition rows.
- Scratchpad purge: the raw Lₙ session scratchpad (transcript, tool-result dumps, pruned logs) is reclaimed/destroyed at release — ephemeral tokens are purged entirely, not archived. What survives a task is exactly: patch file (when one exists — B.5 fail-without-patch and D.4 verify-fail-withhold excepted, cf. E.2.3) + `verify.json` + result vector + ledger lines.
- Ordering constraint: release runs AFTER patch (if any) + `verify.json` + result vector are durably written (B.3 `proposed` + D.4 gate + E.2.3). Release with unwritten artifacts is refused (`E_ARTIFACTS_PENDING`).
- No new semantics except the ordering constraint + `E_ARTIFACTS_PENDING` code (see E.3).

#### E.2.5 Step 5 — L2 vector post + DAG update + forward-nudge trigger (REUSE → D.2) **[HOST-ENFORCED]**

- The compacted result vector (§E.2.3) is posted to L2 as the task's `report`-type ledger line (extends A.8, same event type — no new ledger type). The host updates the main task DAG (terminal marking per B.3 disposition), then evaluates remaining token/depth caps (`breadth^depth` vs per-tree budget, `dayUsed` vs 1000/day, I2) BEFORE advancing anything.
- Forward advance reuses the D.2 auto-nudge loop verbatim: next queued task is re-queued with nudge context within the shared I7 budget (`maxNudgeDepth=3`, reset on user turn); budget-exhausted or `MAX_ROUNDS`/`SPIRAL` lineages escalate narrower/different-model or stop+report — never auto-retried full-scope.
- Interrupt supremacy (I9/D.2.5) applies end-to-end across all five steps: user `stop/pause/hold/don't continue` halts the pipeline at the current step boundary, cancels pending nudges, sets resume-lock. A pipeline interrupted mid-compaction reports `cancelled vs finished` per the GUARDRAILS.md interrupt protocol.
- No new semantics; D.2 + A.3/A.6 gates are the normative references.

---

### E.3 Deltas summary + rule-index additions

#### Overlap table

| # | §E item | Verdict | Delta |
|---|---|---|---|
| 1 | Sister-leaf KV bus (E.1.1–E.1.3, partitioned R/W) | NEW | Greenfield: leaf↔leaf IPC exists nowhere in A–D |
| 2 | Visibility reconciliation (E.1.4, auto-merge FORBIDDEN) | NEW (rule) / REUSE (mechanism) | New normative prohibition superseding source-memo auto-merge line; mechanism reuses `proposePromotion()`/`approve(file)` verbatim |
| 3 | Gatekeeper pass (E.2.1) | REUSE → D.4 | Pointer only; adds L2-uninterrupted scoping note (local retries consume I6, not I7) |
| 4 | Structural pruning (E.2.2) | REUSE → cordis `tool-result-pruner` (8192/4096/1024) | Pointer only; adds patch/`verify.json`-exempt-before-prune ordering note |
| 5 | Result vector schema (E.2.3, conditional `gitDiffHash`) | NEW | Greenfield: L2-facing vector schema + field bounds + `gitDiffHash`↔patch binding enforced when patch exists; `""` + reason-in-`summary` when B.5/D.4 withhold the patch |
| 6 | Worktree release + purge (E.2.4) | REUSE → B.5/B.7 | Pointer only; adds artifacts-before-release ordering + `E_ARTIFACTS_PENDING` |
| 7 | L2 post + DAG + nudge (E.2.5) | REUSE → D.2 + A.3/A.6 | Pointer only; adds budget re-check-before-advance + I9-across-pipeline notes |

#### Rule-index additions (extend §C / D.5)

| ID | Rule | Type | Gate |
|---|---|---|---|
| E.1 bus (`SisterLeafMemoryEntry`/`SisterLeafBus`, partition, 64 KiB cap) | Sister-leaf IPC | **[HOST-ENFORCED]** | tool-hook R/W |
| E.1.4 auto-merge FORBIDDEN (`promotable_to_l1` → propose/approve only) | Promotion reconciliation | **[HOST-ENFORCED]** | task-terminal promotion gate |
| E.2.3 result vector schema + conditional patch binding | L2-facing vector | **[HOST-ENFORCED]** | pipeline step 3 |
| E.2.3 status-enum normalization + `exportedState` deny | Vector validation | **[HOST-ENFORCED]** | pipeline step 3 |
| E.2.4 ordering (artifacts before release) | Release safety | **[HOST-ENFORCED]** | release |
| `BUS_VALUE_TOO_LARGE` / `E_ARTIFACTS_PENDING` | New error codes w/ brain-visible messages | **[HOST-ENFORCED]** | publish / vector-submit / release |

#### Config fragments (extend A.2/B.8)

```yaml
sisterBus:
  valueMaxBytes: 65536          # per-entry serialized cap → BUS_VALUE_TOO_LARGE
  promotionGate: "propose-approve-only"  # no other value permitted
resultVector:
  summaryMaxLines: 5
  exportedStateMaxBytes: 4096   # oversize/non-scalar → BUS_VALUE_TOO_LARGE deny (no truncation)
  bindPatchHash: true           # gitDiffHash MUST match patches/<task>.patch — enforced when patch exists
```

#### Acceptance (extend A.9/D.2/D.4)

- (14) sister A publishes `promotable_to_l1` candidate; task completion surfaces it to BRAIN but L1 is unchanged until BRAIN `approve(file)` — auto-merge path absent (no ledger line writes L1).
- (15) `sister_only` entry never appears in result vector `summary`/`exportedState`; cross-partition `read` returns null.
- (16) green verify → pruned transcript + vector matching schema (validator passes, `gitDiffHash` matches patch) → release purges scratchpad, patch + `verify.json` + vector survive.
- (16b) FAILED/DISCARDED with no patch (B.5 `keepOnFail=false` / D.4 `V_VERIFY_FAIL`) → vector posts with `gitDiffHash: ""` and the no-patch reason in `summary`; validator accepts.
- (17) user `stop` mid-pipeline → halts at step boundary, zero further steps/nudges, `Auto-continue: withheld (user interrupt)`.
- (18) lowercase `status: "success"` normalizes to `SUCCESS`; `exportedState` with nested object value → `BUS_VALUE_TOO_LARGE` deny, vector unchanged.

Evidence: interfaces + 5 steps quoted from source memo (L2 contextual memory); pruner thresholds verified in `preset/budget-agi/agent.cordis.yml:257-262` (`thresholdChars 8192 / headChars 4096 / tailChars 1024`); promotion rule quoted from L0 baseline (L2→L1 EXPLICIT only, proposePromotion gates secrets, approve appends); no `SisterLeaf*` / `promotable_to_l1` / result-vector schema matches in-repo (grep over `SisterLeaf|sister_only|promotable_to_l1` hit only `parentSessionId` usages in `plugin/src/*` — session-header linkage, reused as partition identity, not a bus).
