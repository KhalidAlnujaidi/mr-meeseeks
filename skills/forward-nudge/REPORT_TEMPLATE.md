# Forward-Nudge Report Template

Copy this structure verbatim at the end of every task completion, member
report, and reviewer verdict. The last two sections are mandatory — a report
without them is incomplete.

```markdown
## Result

<1–5 lines: what was done, files changed, key evidence. Link files as
`path/to/file`.>

## Next Steps

1. [owner: engineer] <concrete action> — done looks like: <acceptance criteria>
2. [owner: researcher] <concrete action> — done looks like: <acceptance criteria>
3. ...

## Auto-continue proposal

Auto-enqueuing N continuation(s) under guardrail budget
(depth <d>/3, parallel <p>/5, retries <r>/2): <t_next1> (deps: [<t_done>],
assignee: <role>), … Independent paths run in parallel; chained work uses
explicit dependencies. Destructive steps proposed only, never auto-executed.
Interrupt with `stop` / `pause` / `hold` at any time.
```

## Field rules

- **Next Steps**: 2–5 items. Fewer than 2 means the goal is likely done —
  say so and stop (no auto-continue on a met goal). More than 5: keep the
  top 5 by value, list the rest under a `Deferred:` line.
- Each step names an **owner role**, a **concrete action** (verb + object +
  target), and **acceptance criteria** starting with "done looks like:".
- **Auto-continue proposal**: state depth/parallelism/retries consumed vs the
  caps (`MAX_NUDGE_DEPTH=3`, `MAX_PARALLEL_PATHS=5`, `MAX_ROUNDS_PER_TASK=2`),
  the exact task ids created (or `subagent` descriptions outside AgentTeams),
  and the interrupt keywords. Then create them in the same turn.
- On `failed` tasks: same template, but Next Steps are recovery paths and the
  proposal cites the retry count (`retry 1/2`, `retry 2/2 → escalate`).
- On a STOP condition (user interrupt, plan mode, goal met, budget blown,
  destructive-only follow-ups): replace the proposal with a one-line
  `Auto-continue: withheld (<reason>).` and end the turn.
