# Free-model swarm for DSH AgentTeams

Heterogeneous free-model members for `@nanmicoder/dsh-agent-teams`. Instead of
every member being a subagent of your main model, members are routed to
different FREE models — variety of reasoning, zero cost.

## Architecture (no fork required)

`dsh-agent-teams` already supports per-member model routing. From the installed
`lib/tools.js`:

    agent_teams_add_member(..., provider, model, reasoning_effort)
      provider: 'Optional LLM provider route... requires model.'
      model:    'Optional model override. Omit for the captain's current model.'

    lib/members.js: 'an explicit member LLM provider requires an explicit member model'
    'a member routed to a different provider or model automatically uses that
     target model's default effort'

So the captain can already spawn members on ANY model in the DSH host catalog.
The work is therefore CONFIGURATION, not code:

1. Register free models in the DSH host catalog  (`~/.dsh/settings.yaml`)
2. Tell the captain to route members to them  (natural language, per its protocol)

The plugin's own system prompt says to pass provider/model "only when the user
explicitly requests a different route for that role" — so you must ASK for it.

## Step 1 — provider config (DONE, verified)

`~/.dsh/settings.yaml` -> `llm-pi-ai.providers.openrounter` (id keeps its
historic typo; the baseURL is what matters):

    apiKeyEnv: OPENROUTER_API_KEY      <- was OPENROUNTER_API_KEY (missing T = no key!)
    api: openai-completions
    baseURL: https://openrouter.ai/api/v1

Backup: `~/.dsh/settings.yaml.bak-<ts>`

## Step 2 — prompt the captain for heterogeneous members

Example that works with the plugin's protocol:

    Use AgentTeams. Create 4 members, each routed to a DIFFERENT free model
    from the openrounter provider:
      - reviewer  -> nex-agi/nex-n2.5-pro:free
      - engineer  -> cohere/north-mini-code:free
      - analyst   -> stealth/union-alpha
      - verifier  -> inclusionai/ling-3.0-flash-vl:free
    Review the last commit from correctness, security and performance angles.
    Return one consolidated report.

## Verified model pool (probed live 2026-09-17, cost $0)

| model | ctx | notes |
|---|---|---|
| stealth/union-alpha | 262K | GPQA-D 90.9%, tool-err 0.95%. Best raw quality. Slow ~20 tok/s. |
| nex-agi/nex-n2.5-pro:free | 262K | reasoning |
| nex-agi/nex-n2.5-mini:free | 262K | reasoning |
| cohere/north-mini-code:free | 256K | agentic coding, 30B/3B, Apache-2.0 |
| dots-studio/dots-3-note-preview:free | 512K | reasoning, needs >=3000 tok |
| inclusionai/ling-3.0-flash-vl:free | 262K | multimodal |
| inclusionai/ling-3.0-flash-sante:free | 262K | reasoning |
| poolside/laguna-s-2.1:free | 262K | BLOCKED pending privacy toggle |

Rejected: `thinkingmachines/inkling-small:free` ("only available on agentic
harnesses"), `nvidia/nemotron-3-*:free` (needs training opt-in),
`google/gemma-4-26b-a4b-it:free` (HTTP 429).

## CRITICAL PITFALLS

1. **Reasoning models return EMPTY content when max_tokens is too small.**
   They spend the whole budget on hidden reasoning first. `max_tokens: 16`
   returned empty for cohere/dots/ling; with 400-3000 they answered fine.
   This looks like a model failure but is a budget bug. Always allow >=1024,
   ideally 3000 for dots-studio.

2. **"Guardrail restrictions" = your privacy setting, not a broken model.**
   Full error: `Free model training violation (account settings): 1 endpoint
   excluded; configurable at https://openrouter.ai/settings/privacy`.
   Fix: Privacy -> Data Training -> enable
   "Allow free endpoints that train on request data".
   Leave OFF "Allow free endpoints that publish prompts" (public datasets =
   strictly worse) and "Allow 1% data discount" (trades data for 1% off).

3. **Catalog $0 does NOT mean callable.** Of 24 zero-price models, several are
   harness-only, guardrail-blocked, or rate-limited. Always probe.

4. **Account cap: 1000 free-model requests/day** (`free_model_daily_requests`).
   Check with: `curl -s https://openrouter.ai/api/v1/key -H "Authorization: Bearer $KEY"`.
   An 8-agent swarm doing multi-turn work can exhaust this.

5. **Stealth models may RETAIN prompts** (not train). union-alpha's page says so.
   Do not route secrets through it.

6. **Env var name.** Shell exports `OPENROUTER_API_KEY`; the DSH config said
   `OPENROUNTER_API_KEY` (missing T), so the provider had no credentials.

## Tools built

- `~/.local/bin/or-swarm` — parallel swarm CLI over the free pool.
  `or-swarm --mix -n 6 "task"`, `or-swarm --list`, `--max-tokens N`, `--json`.
  Reports REAL cost from the API (always $0.0000), never fabricated.
- `~/.local/bin/kilo-subagent` — kilo free-tier agent lane (see its .md).

## Proven run

    or-swarm --mix -n 6 "biggest risk of free-tier LLM APIs for code review"
    -> 5/6 succeeded in 18.44s wall, cost $0.0000
    -> models disagreed productively (data exposure vs reliability vs confidentiality)
