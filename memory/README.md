# Layered memory — L0 / L1 / L2

Prototype in `memory/`. Erasable-syntax TS, node stdlib only, `node --test` green.
No DSH fork, no embeddings, no plugins touched.

## Layers

- L0 harness — `$DSH_HOME/AGENTS.md`. Host, OS, keys, budgets. Nothing
  project-specific. Absent file = null, never an error.
- L1 project — `<projectRoot>/.dsh/brain/*.md`, sorted by filename.
  What the project is, layout map, deploy path, contacts, gotchas.
  Shared by every session whose cwd sits under the root.
  Project root = nearest ancestor containing `.git`, else the cwd itself
  (same rule as `dsh-skill-filesystem`).
- L2 session — caller-owned scratch. `buildContext` never reads it, so two
  sessions in one project get deep-equal results. Verified by test
  "two sessions in one project share L1, L2 scratch never leaks".

Render order is broad-to-specific: L0 first, then L1 files. L2 never renders
from here.

## Promotion L2 -> L1

Session scratch NEVER becomes project memory by itself.

- `proposePromotion(text, { projectRoot, author })` only checks + stages.
  It never writes. Secrets (OpenRouter `sk-or-v1-…`, generic `sk-…`,
  `api_key/token/password/secret` assignments) refuse with `ok: false`.
- Only the returned `approve(file?)` writes, appending to
  `<root>/.dsh/brain/<file>` (default `BRAIN.md`) with a promotion comment.
- Blocked proposals: `approve()` is a no-op returning `{ wrote: null }`.

## Files

- `memory/memory.ts` — `nearestProjectRoot`, `buildContext`, `renderContext`
- `memory/promotion.ts` — `proposePromotion`, `findSecrets`
- `memory/memory.test.ts`, `memory/promotion.test.ts` — 5 tests, TDD RED first

## Run

```sh
node --test memory/memory.test.ts memory/promotion.test.ts
```

## Not yet

- No DSH wiring (no `agent-instructions` row, no preset doctrine rule).
  That is the next tracer: read these files at session start.
- No per-file budgets, no merge/conflict policy, no recall ranking.
  Add only when a real session proves it needs them.
