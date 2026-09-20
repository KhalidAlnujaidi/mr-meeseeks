# kilo-subagent — delegate one-shot tasks to Kilo Code's FREE tier

Wrapper: `~/bin/kilo-subagent` (install anywhere on PATH; bash 3.2-compatible, macOS-safe)

Kilo Code CLI: `@kilocode/cli@7.7.2` at `/opt/homebrew/bin/kilo`
Config: `~/.config/kilo/kilo.jsonc`   Auth: `~/.local/share/kilo/auth.json`

## Why a wrapper instead of pinning a model in config

`kilo run` accepts `-m provider/model`. Free-tier models are NOT config
defaults, so each invocation must name one. A single hard-coded model is
fragile (any one can hang or rate-limit), so the wrapper picks a RANDOM
model from a verified pool and retries a different one on failure.

## Verified free models (work with NO sign-in, tested 2026-09-17)

    kilo/nvidia/nemotron-3.5-lightning:free
    kilo/poolside/laguna-s-2.1:free
    kilo/inclusionai/ling-3.0-flash-vl:free
    kilo/nex-agi/nex-n2.5-pro:free

Tested and EXCLUDED:

    kilo/meta/muse-spark-1.3(-contributor)   -> "You need to sign in to use this model."
    kilo/liquid/lfm-2.5-2.6b:free            -> hangs indefinitely
    openrouter/google/gemma-4-31b-it:free    -> hangs indefinitely

`muse-spark-1.3` is real but NOT free — there is no `:free` variant. Using it
bills OpenRouter/DeepInfra credits. Do not add it to the pool if the goal is
zero-cost.

## Usage

    kilo-subagent "Add docstrings to every function in utils.py"
    kilo-subagent -f context.py "Refactor this into smaller functions"
    kilo-subagent -r -f context.py "Suggest improvements; return as text"
    echo "some code" | kilo-subagent "Explain what this does"
    kilo-subagent -n 3 "long task"        # try up to 3 different models

## Flags

    -f FILE      attach context file (repeatable)
    -n N         max attempts across DIFFERENT models (default 3)
    -r           READONLY: forbid file writes, answer as text only
    -h           help

## Env

    KILO_SUBAGENT_TIMEOUT   per-attempt seconds (default 180)
    KILO_SUBAGENT_MODELS    override pool, space-separated
    KILO_SUBAGENT_SEED      fix RNG for reproducible model choice
    KILO_SUBAGENT_DRYRUN=1  print the plan without calling kilo

## Critical pitfalls (all found the hard way)

1. **`-f` is greedy.** `kilo run -m X -f file.py "my task"` treats the task as
   another filename: `Error: File not found: my task`. Always separate with
   `--`: `kilo run -m X -f file.py -- "my task"`. The wrapper does this for you.

2. **Kilo is an agent, not a text function.** By default it EDITS and CREATES
   files to satisfy the task. Running it in a repo will mutate your working
   tree. Use `-r` for analysis-only work.

3. **Free models can hang.** No error, no timeout — just silence. Hence the
   wrapper's portable timeout (background + poll + kill); macOS has no
   `timeout(1)` by default, and `gtimeout` isn't installed either.

4. **bash 3.2 on macOS** — no `mapfile`, no associative arrays. Array
   compaction is done manually.

5. **Logging goes to stderr**, answer to stdout — so `$(kilo-subagent ...)`
   captures only the model's reply.

6. Cerebras appears under `kilo auth list` -> "Environment" because it comes
   from the `CEREBRAS_API_KEY` env var, not `auth.json`. Clearing Kilo's config
   files will NOT remove it; unset the shell variable instead.

## Credentials: current state

`auth.json` still holds deepinfra + openrouter keys. Backups were made at:

    ~/.local/share/kilo/auth.json.bak-*
    ~/.config/kilo/kilo.jsonc.bak-*

Kilo was left otherwise intact (298 MB `kilo.db` session history untouched).
The free pool above is used regardless of those credentials.
