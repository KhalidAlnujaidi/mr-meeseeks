# Forward-Nudge Install Path

Source of truth: `skills/forward-nudge/` (repo root). Discovered in-repo via
`skill-filesystem` local-root discovery — verified live (skill auto-registered
in the building session with zero install).

For use in other workspaces, copy to user level:

```sh
cp -r skills/forward-nudge "$DSH_HOME/skills/forward-nudge"
```

`$DSH_HOME/skills/` is auto-discovered by `skill-filesystem`; no config needed.

Do NOT install under `plugin/skills/`: `plugin/` is a byte-parity replica of
upstream `dsh-agent-teams` (per VISION.md) and must stay clean of experiments.
