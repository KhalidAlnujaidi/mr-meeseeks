# Mr. Meeseeks — heterogeneous free-model teams for DeepSeek Harness

*"I'm Mr. Meeseeks, look at me!"* — a swarm of small helpers, each with a
different brain, spawned to do one job.

This repo has three parts:

| Dir | What |
|---|---|
| `plugin/` | **dsh-mr-meeseeks**: a 1:1 functional replica of [`dsh-agent-teams`](https://github.com/NanmiCoder/dsh-agent-teams) v0.1.13 (MIT), with the whale avatars swapped for Meeseeks art. Same 10 `agent_teams_*` tools, same task DAG, scheduler, routes, and slots. |
| `preset/budget-agi/` | **Budget-AGI**: a DeepSeek Harness agent preset. The main session is the brain (thinks, decides, verifies); all labor delegates to Meeseeks teams by default. Copy into `~/.dsh/.agent-presets/budget-agi/`. |
| `scripts/` | `or-swarm` (parallel fan-out over OpenRouter free models), `kilo-subagent` (Kilo Code free-tier one-shot delegate), plus runbooks. |

## Quick start

```sh
# 1. plugin
cd plugin && npm install && npm run build
dsh plugin --profile web add .
dsh --profile web --dump-config | grep mr-meeseeks
# restart DSH, then: /agent-teams review the last commit

# 2. preset
cp -r preset/budget-agi ~/.dsh/.agent-presets/budget-agi
# new session -> preset picker -> Budget-AGI

# 3. scripts (optional, standalone)
export OPENROUTER_API_KEY="sk-or-v1-..."
./scripts/or-swarm --mix -n 6 "your task here"
```

## Relationship to upstream

`plugin/` is derived from
[NanmiCoder/dsh-agent-teams](https://github.com/NanmiCoder/dsh-agent-teams)
v0.1.13 (MIT © 2026 程序员阿江/Relakkes) — see `plugin/NOTICE` for full
attribution. Changed files: `package.json` (name), `cordis.patch.yml` (boot
id), `src/index.ts` (exported name), `assets/agent-teams/*.png` (Meeseeks art;
originals kept in `assets/agent-teams.orig/`). All logic, tools, routes,
slots, and locales are byte-identical to upstream v0.1.13.

## License

MIT. Upstream portions © 程序员阿江 (Relakkes); Meeseeks art and Budget-AGI
preset © Khalid Alnujaidi. See `plugin/LICENSE` and `plugin/NOTICE`.
