# Divide and Conquer to the Atomic Level

A vision for a minimal agent harness built around one philosophy: **split
context until every task is atomic, then let small models shine.**

## The core bet

Small models don't mostly fail at *thinking*. They fail at *context
management*: losing the goal over long contexts, getting distracted by
irrelevant history, mishandling multi-step tool protocols, picking the wrong
tool from a large catalog.

An atomic task — one tool call, one checkable result — removes all four
failure modes at once. What remains is short-horizon generation plus
classification-style verification ("does this output satisfy the criterion?"),
which is exactly where small models are strongest. Entailment judgments are
far easier than open-ended planning.

Decomposition helps weak models *disproportionately*. A frontier model can
hold a whole plan in its head; a 3B model can't, but it never needs to — it
only ever sees its atom. And verification is cheaper than generation at every
level, so the structure degrades gracefully: even mediocre verifiers catch
most errors if each claim is small enough to check mechanically.

## Two load-bearing constraints

1. **Someone has to split well.** Splitting is itself a capability, and
   garbage decomposition propagates downward. Argument for a strong brain at
   the top and small leaves — not small models all the way down. (This is
   the Budget-AGI shape: frontier captain, heterogeneous free-model members.)
2. **Correlated blindness.** If worker and verifier are the same small model,
   they share failure modes. Mitigation: a *heterogeneous* pool — different
   brains checking each other. Variety is load-bearing, not decorative.

## The minimal harness (necessary, not just elegant)

The AgentTeams protocol is too heavy for small-model captains: seven-rule
captain protocol, attempt_ids, mailbox semantics, DAG discipline. A small
model drowns in the protocol before it starts the work.

A minimal harness for this philosophy needs:

- **Four verbs:** claim, do, report, verify. No DAG, no mailboxes, no
  attempt capabilities. A task is claimed, done, reported, and checked.
- **Tiny tool surface per role.** Tool selection from 3 tools vs 30 is a
  different task. Each role sees only its tools.
- **Atomicity stop-rule as the terminus.** A task is atomic when its result
  fits in one tool call and one verification check; do it and report back.
  Otherwise split ONCE into the smallest useful sub-tasks, delegate, verify
  each return, and stop. Never split an already-atomic task. Depth ends
  where atomicity begins — the tree stops itself.
- **Budget counting.** Before delegating past depth 2, estimate total agents
  (breadth^depth) against the day's request budget. A spiral (same task
  re-split three times) is a decomposition bug: stop and report.

## Relation to Context Computing

Context Computing
([essay](https://khalidalnujaidi.github.io/essays/context-computing.html))
models computation as operations over learned semantic state: contexts are
distributions over candidate interpretations, maintained non-deterministically
during computation and collapsed to single outputs at endpoints
(c_byte / c_gate / c_bit).

The agent tree is that theory running:

- A running team is a maintained **(V, P)** — parallel members exploring
  different approaches are multiple candidates alive with mass spread across
  them. "Never converge early" is non-determinism maintenance as engineering.
- The atomicity stop-rule is an **operational collapse criterion** — a
  concrete, implementable answer to the theory's central open question (how
  and when non-determinism resolves) for agent computation.
- **Verification junctions are c_bit; everything else is c_gate.** Member
  reports, reviewer verdicts, captain consolidation — each is a collapse
  event. Splits are expansion.
- The uncapped-depth experiment *generates the interaction logs the theory
  asks for*: depth distributions, residence times, branching factors.
  Falsifiable prediction: tree depths will be heavy-tailed (Zipfian), and
  atomicity judgments will show power-law residence times. If the data
  disagrees, that's interesting too.
- The Zipfian substrate justifies **small fixed role sets**:
  researcher/engineer/reviewer/verifier covers nearly everything if subtask
  shapes are heavy-tailed — no open-ended role invention, which also keeps
  small-model role prompts simple.
- **Semantic verification, not token verification**: verifiers check
  task-relative acceptance (does it satisfy the criterion?), never surface
  similarity. Shannon deliberately excluded semantics; agent verification
  lives entirely in the semantic regime.

## What to instrument (when the harness exists)

Log every split as expansion and every verification as collapse, with depth,
model, token cost, and timestamps, to a JSONL ledger. From day one the
experiment then produces: depth distributions, per-level cost ratios
(verifier cost vs worker cost — the point where a level subtracts value),
model-by-role performance, and spiral detection (re-split counts per task).

## Status

Vision only. Nothing here changes what exists today:

- `~/dev/mr-meeseeks` (public repo): the working plugin + preset + scripts.
- `~/mr-meeseeks-fork` (branch `meeseeks-v0.1.13`): local dev fork.
- `~/.dsh/.agent-presets/budget-agi/`: live preset with doctrine rules 6/7
  (atomicity stop-rule + budget counting).
- The uncapped-depth experiment (`memberMaxDepth` default removed) is
  implemented and verified in the fork, awaiting a live run.

Next step when ready: build the minimal 4-verb harness as a separate
prototype — do not retrofit it onto the working AgentTeams replica.
