# Security Policy

## Scope

Golem is an agent runtime that executes LLM-proposed commands in
sandboxed worker processes. Its security boundary is the host contract:
payload gates before spawn, scrubbed environments, ephemeral workspaces,
and host-enforced verification. A defect that lets a model-proposed
payload bypass the gate, escape the workspace, leak host credentials
into a worker, or corrupt the ledger is a security vulnerability.

## Reporting a Vulnerability

**Please do NOT open a public GitHub issue for security vulnerabilities.**

Report privately via one of:

- GitHub Private Vulnerability Reporting: enable on
  https://github.com/LeastGen/golem/security/advisories (preferred)
- Email: security@leastgen.ai with subject prefix `[golem]`

Include:

1. What you observed and why it crosses the host contract (gate
   bypass, workspace escape, credential leak, ledger forgery, memory
   safety).
2. Minimal reproduction: Golem version/commit, platform, engine
   (model + colibri commit), and the exact payload/task sequence.
3. Whether the finding needs a running engine to reproduce.

## What to Expect

- Acknowledgement within 5 business days.
- Triage and a fix-or-dispute decision within 14 days.
- Coordinated disclosure: we ask for 90 days before public disclosure,
  and we will credit reporters who want to be credited.

## Out of Scope

- Model outputs themselves (a model proposing `rm -rf /` is the
  expected input, not a vulnerability — the gate refusing it is the
  contract; the gate *executing* it is in scope).
- Attacks requiring local shell access as the Golem user (you can
  already do anything the workers can).
- Third-party engine vulnerabilities (report to the engine project;
  we will happily relay).

## Security-Relevant Design Notes

- Workers are hostile by design: fork/execve with an allowlisted
  `envp`, CLOEXEC pipes, watchdog SIGKILL, and output sanitization
  before the Brain sees a byte.
- Destructive verbs are propose-only: the gate records the proposal in
  the append-only ledger and never auto-spawns.
- The ledger is the audit surface: one serialized line per event,
  mutex-guarded, single `write(2)` with `O_APPEND`, host-stamped UTC.
  Fabricated telemetry is treated as a security defect.
- Memory safety is CI-verified: the suite builds and passes under
  ASan/UBSan (see CONTRIBUTING.md).
