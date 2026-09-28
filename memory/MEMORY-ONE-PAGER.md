# Layered Memory for DSH — One Pager

Date: 2026-09-18. Owner: Khalid. Status: prototype live in 2 projects.

## The problem

DSH sessions share nothing. Two chats in the same project start from zero.
Workspace registry only groups sessions, it teaches nothing. Lingshu memory
is global per profile, not per project. Result: you paste the same deploy
facts over and over, and agents re-derive what another session already learned.

## The idea — 3 boxes

L0 — one notebook for the whole machine.
Host, OS, deploy pattern, secrets rules, budgets. Nothing project-specific.
File: ~/.dsh/AGENTS.md (4.3K). Every session reads it.

L1 — one notebook per project.
What is this project, layout map, how to deploy it, gotchas proven twice.
Files: <project>/.dsh/brain/BRAIN.md, linked as <project>/AGENTS.md so DSH
auto-loads it. Shared by all sessions in that project, invisible elsewhere.

L2 — sticky notes for one chat only.
Current task, scratch, debug output. Never auto-saved. Sibling sessions
never see each other. Verified: two sessions in one project get identical L0+L1.

Sharing rule: think-fast may READ leastgen-demo L1 for the shared Alibaba
pattern (same box 8.213.85.203, same tunnel discipline). Read-only. Never
write to another project.

## Promotion rule — how L2 becomes L1

Nothing promotes by itself. proposePromotion() checks and stages only, never
writes. Secrets (keys, tokens, passwords) are blocked. Only an explicit
approve() appends to .dsh/brain/. Durable facts only: deploy paths, layout,
gotchas. Session IDs and debug output stay L2.

## What is live today

- L0: ~/.dsh/AGENTS.md — box, tunnel dashboard-only rule, secrets handling,
  model slug, Budget-AGI contract, promotion rule.
- L1 demo: ~/dev/leastgen-demo — demo.leastgen.com, port 8100, admin sidecar.
- L1 think-fast: ~/Desktop/think-fast — leastgen-idea server, port 8756,
  nginx + certbot + UFW, borrows demo pattern, notes differences.
- Wiring: AGENTS.md symlinks + Budget-AGI doctrine rule 8 (read L0+L1,
  keep L2 private, never promote secrets). Backup: /tmp/budget-agi-backup-20260918.yml
- Prototype: ~/dev/mr-meeseeks/memory/ — memory.ts, promotion.ts, 5 tests green.

## What it does NOT do

Memory files are tiny (under 15K total). They do not shrink live context.
Session logs are the blowup: 466M on disk, think-fast 71M, biggest single
session 782K compressed. Member reports land full-text in the main loop via
steer, plus a settle notice — no cap. Tool-result pruner (8192 chars) does
not touch reports. Compaction fires at 80 percent, then breaks cache.

Fixes for blowup are separate: fresh sessions for a clean L2, report size cap,
status-over-inbox reading, earlier compaction. Memory shares knowledge; it
does not compress live history.

## Next steps

1. Restart DSH once so rule 8 loads in new chats.
2. Open one new chat per project, ask what L0 and this project are.
3. Add report cap in mr-meeseeks fork when blowup bites again.
