# Forward-Nudge Guardrails

Hard limits for auto-continue. Check BEFORE every auto-enqueue. These caps
implement VISION.md's atomicity stop-rule and budget counting for the nudge
loop specifically.

## Caps

| Constant | Default | Rationale |
|---|---|---|
| `MAX_NUDGE_DEPTH` = 3 | 3 chained auto-continuations without a user turn | Bounds `breadth^depth` agent explosion; past depth 3 the brain must re-frame, not the loop re-split. |
| `MAX_PARALLEL_PATHS` = 5 | 5 parallel continuations per report | Matches small fixed role sets (researcher/engineer/reviewer/…); more paths = unfocused decomposition. Prune to top-5 by value, defer the rest in the report. |
| `MAX_ROUNDS_PER_TASK` = 2 retries | same task failing twice → escalate | Per meeseeks-doctrine rule 5: never silently redo full-scope labor. Third attempt must be narrower or a different approach, stated in the report. |
| `ATOMIC_STOP` | result fits one tool call + one check → do it, don't split | From VISION.md: never split an already-atomic task. Depth ends where atomicity begins — the tree stops itself. |
| `SPIRAL_DETECTOR` = 3 re-splits | same task re-split ≥ 3× → decomposition bug | Stop, report upward, do not continue the chain. |

## Stop conditions (any one → plain report, no auto-continue)

1. User interrupt: `stop` / `pause` / `hold` / `don't continue`, or plan-mode entry.
2. Open `ask_user_question` awaiting a user answer.
3. Goal acceptance criteria all met — say so, stop.
4. Any cap above would be exceeded.
5. Next Steps contain destructive/irreversible actions (push, publish, delete,
   migrate, credential change) — propose only, never auto-execute.
6. Same task failed verification twice — escalate honestly.

## User interrupt protocol (always wins over the loop)

1. On interrupt keywords: cancel unclaimed auto-continuations
   (`agent_teams_update_task status=cancelled` or drop unspawned subagents),
   let already-claimed atoms finish, then report what was cancelled vs finished.
2. Never auto-resume an interrupted chain. Resume only on explicit user
   `continue` or a new instruction.
3. Plan-mode entry freezes auto-continue until `exit_plan_mode` succeeds.
4. After interrupt, the next report uses `Auto-continue: withheld (awaiting
   user direction).`

## Budget accounting per report

Each Auto-continue proposal must state consumed vs cap:
`depth <d>/3, parallel <p>/5, retries <r>/2`. Depth increments per chained
auto-enqueue from the same lineage; a user turn resets depth to 0.

## Dry-run self-test (for reviewers)

1. Simulate a completed task with 3 independent follow-ups → expect 3 parallel
   tasks sharing `deps=[t_done]`, proposal cites `depth 1/3, parallel 3/5`.
2. Simulate `stop` from user → expect zero new tasks, report ends with
   `Auto-continue: withheld (user interrupt).`
3. Simulate 6 follow-ups → expect top-5 enqueued + 1 `Deferred:` line.
4. Simulate same task failing 2× → expect no full-scope retry, narrower
   proposal or escalation.
5. Simulate destructive follow-up (e.g. `git push`) → expect proposal-only,
   no execution call in the same turn.
