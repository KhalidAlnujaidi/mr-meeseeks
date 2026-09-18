---
name: forward-nudge
description: Use when a task report, member report, or reviewer verdict lands with open follow-ups and the session is about to idle awaiting user input.
metadata:
  version: "0.1.0"
  date: "2026-09-18"
  team: "forward-nudge-skill/t2"
---

# Forward Nudge

After every completed unit of work, push the session forward instead of
stopping for user input. Advise next steps, then auto-delegate them.

## When this fires

- An `agent_teams_update_task` with `status=completed` (or `failed` with a
  recoverable error).
- A subagent/member `report` arrives.
- A reviewer verdict (`pass` with follow-ups, or `reject` with a fix list).
- The main loop is about to end a turn with "done, awaiting instructions"
  while open questions remain.

If none of these hold, do nothing. This skill never fires mid-task.

## The contract (every report MUST end this way)

1. **Result** — what was done, files changed, key evidence (1–5 lines).
2. **Next Steps** — 2–5 concrete follow-ups, each with:
   - a one-line action,
   - acceptance criteria ("done looks like X"),
   - a suggested owner/role (`researcher`, `engineer`, `reviewer`, …).
3. **Auto-continue proposal** — one paragraph stating which steps are being
   auto-enqueued, with what dependencies and parallelism, and under which
   guardrail budget (depth/rounds remaining). Then DO it — create the tasks
   (or spawn the subagents) in the same turn. Never present Next Steps and
   stop to ask permission.

A report template with exact headings lives in `REPORT_TEMPLATE.md`.
Copy its structure verbatim.

## How to auto-continue

### Inside an AgentTeams session (preferred)

The scheduler already auto-claims ready tasks on every idle edge and task-graph
mutation — the skill only supplies the *content* plus the *discipline* of never
ending a report without enqueued continuations:

```text
agent_teams_create_task(id=t_next1, deps=[t_done], assignee=engineer, ...)
agent_teams_create_task(id=t_next2, deps=[t_done], assignee=researcher, ...)
# independent paths share the same deps so the scheduler fans out in parallel;
# sequential chains use deps to order.
```

- Fan out independent paths in parallel (same `deps`, different tasks).
- Chain dependent work with explicit `dependencies`.
- One task per path — never bundle two parallel explorations into one task.
- Always set `dependencies: [<just-finished-task>]` so the graph stays causal.

### Outside AgentTeams (single session, no team)

Use `subagent` / `subagent_fork` fan-out, one call per Next Step, all in the
same assistant turn:

```text
subagent(description="explore path A", prompt="<goal + acceptance criteria>")
subagent(description="explore path B", prompt="<goal + acceptance criteria>")
```

Do NOT use `ask_user_question` to confirm auto-continuation. The proposal IS
the notice.

## Guardrails (hard limits, see GUARDRAILS.md for rationale)

| Limit | Default | Meaning |
|---|---|---|
| `MAX_NUDGE_DEPTH` | 3 | Auto-continue chains longer than 3 without a user turn stop and report. |
| `MAX_PARALLEL_PATHS` | 5 | Never fan out more than 5 parallel continuations per report. |
| `MAX_ROUNDS_PER_TASK` | 2 retries | Same task failing twice → stop, report, propose a narrower retry. Do not silently redo full-scope labor. |
| `ATOMIC_STOP` | atomic = one tool call + one check | Never split an already-atomic task (per VISION.md atomicity stop-rule). |
| `SPIRAL_DETECTOR` | same task re-split ≥ 3× | Stop. File as decomposition bug, report upward. |

### Stop conditions (check before every auto-enqueue)

STOP and end the turn with a plain report (no auto-continue) when ANY holds:

1. User said `stop`, `pause`, `hold`, or switched to plan mode.
2. `ask_user_question` has an open question the user has not answered.
3. No open questions remain — the goal's acceptance criteria are all met.
4. Budget blown: depth/rounds/parallelism would exceed the table above.
5. Destructive or irreversible action (push, publish, delete, migrate,
   credential change) is in the Next Steps — propose, do NOT auto-execute.
6. Verification failed twice on the same task (escalate honestly per doctrine).

### User interrupt (always wins)

- The words `stop` / `pause` / `hold` / `don't continue` from the user abort
  all pending auto-continuations immediately. Cancel unclaimed tasks
  (`status=cancelled`), leave claimed ones to finish their atom, report.
- After an interrupt, never auto-resume that chain. Wait for an explicit
  user `continue` / new instruction.
- Plan-mode entry also freezes auto-continue until `exit_plan_mode`.

## Anti-patterns

- Ending a report with "let me know what you'd like next" — forbidden.
  Enqueue the continuations instead.
- Auto-executing destructive steps — forbidden. Propose only.
- Re-splitting the same failed task at full scope — forbidden after 2 tries.
- Firing mid-task or on a partial result — wait for the report boundary.
- More than 5 parallel paths — prune to the 5 highest-value, list the rest
  as deferred in the report.
