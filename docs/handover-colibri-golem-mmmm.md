# Handover — colibri/qwen36 + Golem tool-payload path

**Profile:** `mmmm`
**Date:** 2026-09-20
**Repo:** `/Users/khalid/dev/mr-meeseeks` (branch `main`, nothing committed)
**Engine:** `enigma` → `http://enigma:8000/v1` model `qwen3.6-colibri`
**Status:** fix complete + verified; one follow-up item open

---

## 1. The original question

> Can we use the model hosted on enigma (colibri engine) through a Hermes
> agent instance?

**Answer: NO — not as a Hermes agent instance.** Verified with live calls,
not inference:

- Engine is up and healthy: `/v1/models` → `qwen3.6-colibri`; `/health` ok.
- Plain completions work over HTTP.
- **Every Hermes agent turn fails**: `HTTP 400: Tool use is not wired up for
  the qwen36 engine yet.` — regardless of toolset, `--ignore-user-config`, etc.
- Hermes cannot opt out: `hermes prompt-size --json` reports
  `tools.count = 20`, `json_bytes = 62073` — ~62 KB of tool schemas on every
  turn. There is no tool-less agent mode.

**Root cause** — a guard clause, not a runtime failure
(`openai_server.py:1551-1556`):

```python
if tools or tool_choice not in (None, "none"):
    raise APIError(400, "Tool use is not wired up for the qwen36 engine yet.",
                   "tools", "unsupported_parameter")
```

**Not a general colibri limitation:** `openai_server.py:2754` routes
per-arch renderers, and `render_chat_qwen38` (:1710) + a Qwen3.8 XML
tool-call parser (:1662) already exist. Tool calling is implemented for
**qwen38** — it's specifically the **qwen36** arch that lacks it. Enigma has
only `qwen36_i4_gs64` (22 GB) and no qwen38 binary.

---

## 2. Follow-up question: "what about the Golem harness?"

"Golem" = the C++20 runtime **in this repo**, `dsh-lite-cpp/` (CMake project
`golem`, "LeastGen's Native C++ Agent Runtime").

**Golem does NOT send a `tools` array.** `src/llm_client.cpp` `postImpl()`
builds only: `model`, `max_tokens`, `messages`, optional
`stream`/`stream_options`, optional `response_format`. Tool intent travels
over `response_format` grammars instead (`BrainLoop::solicitToolPayload`,
`src/brain.cpp:313-346`).

**But qwen36 rejects `response_format` too** — live test, both channels 400:

| channel | HTTP | error |
|---|---|---|
| `tools` | 400 | `unsupported_parameter`, param `tools` |
| `response_format` | 400 | `unsupported_parameter`, param `response_format` |

Golem's own `grammar.hpp` F46 already documents this:
`grammar_payload: glm=True; olmoe/qwen/deepseek/kimi/inkling=False`.
**`qwen` is explicitly False.**

---

## 3. The measurement (option A, executed)

Built `dsh-lite-cpp/tests/payload_draft_run.cpp` — drives the REAL path
`solicitToolPayload → parseStrictPayload → checkPayload` against live enigma.
Two arms, same engine/tools/gate; only the prompt frame differs.

### Finding 1 — the prompt frame was the binding constraint

| arm | frame | gate-valid |
|---|---|---|
| control | `harness/loop.ts` `buildLeafInvocation` | **0/10** |
| treatment | `tests/g4_run.cpp:377-384` shape-declaring | **6/10** (pre-fix) |

The control frame (`loop.ts:316-320`) says *"Answer with your result, or with
NEEDS_SPLIT:<reason>"* — it never asks for a payload and never shows its
shape. Three control replies were literally `NEEDS_SPLIT:<reason>`.

**This was MY bench error initially**: I fed `solicitToolPayload` the
*report* prompt from the wrong consumer. The production drivers
(`g4_run.cpp:377-384`, `mixed_lane_run.cpp:137-143`) already build
shape-declaring prompts — `g4_run.cpp:369-375` records it as **F58**:
*"the model imitates surface form, so the prompt must model the surface
form."* I should have read the call-path before running the arm.

### Finding 2 — the 4 remaining failures were ONE defect (FIXED)

All four treatment failures were a trailing `<|im_end|>` template token on an
otherwise byte-perfect payload:

```
{"tool":"read","args":{"path":"/tmp/build.log"}}<|im_end|>
```

Established by measurement, ruling out alternatives:

- **Not truncation**: raising `maxTokens` 256 → 384 gave **zero** truncations.
- **Deterministic**: probe 1 leaks 2/2, probe 3 clean 2/2 via raw curl.
- **Not client mangling**: a bare request via raw curl never leaks.
- **Trigger is criterion length/complexity**, not any specific token.
- **Mechanism**: wire-format artifact of `render_chat_qwen`'s pre-closed
  think block.
- **Recoverable**: stripping the token yields valid JSON.

---

## 4. The fix

`src/grammar.cpp` — `parseStrictPayload()` now strips a **closed allowlist**
of engine chat-template sentinels (`<|im_end|>`, `<|endoftext|>`, `</s>`)
**from the tail only**, repeatedly, before the strict parse.

**Measured effect: 6/10 → 10/10** (all four leaks recovered).

### F45 intent preserved (this was the design constraint)

`grammar.cpp` originally stated: *"any prose frame, fence, prefix/suffix
garbage => PayloadFormatError … no substring extraction, no repair
heuristics."* That law targets **model sloppiness**. What we found is the
engine's **own delimiter** — a wire-format artifact, not sloppiness.
Rejecting a valid payload for carrying the engine's own sentinel was the
actual defect.

All the original rejections still hold (asserted):

- non-allowlisted token (`<|im_start|>`) → REJECTED
- sentinel followed by junk → REJECTED
- leading sentinel + prose → REJECTED *(no prefix repair)*
- fenced JSON (+ sentinel) → REJECTED *(no fence strip)*
- mid-string sentinel inside `args` → preserved as **content**
- sentinel-only input → **empty-content** error, not opaque parse error

`checkPayload` remains the only hard enforcement point — this is upstream
tolerance, not a gate change.

### Regression guard

`tests/test_grammar.cpp` §7b — 11 cases. **Provably fails against pre-fix
code** (5 recovery cases `[FAIL]`, exit 1, reports legibly rather than
aborting) and passes after (exit 0).

---

## 5. Verification evidence

Ad-hoc verification, **not** suite green. Last run: **27 passed, 0 failed,
0 skipped**.

Pinned hashes:

```
grammar.cpp       16ed83a98bf8c58b4c2d4e30b2b0d77d
test_grammar.cpp  3fd7971264a9cc26a641cafd3f9b224e
payload_draft_run 1d920b3d3843c96ebae70e261ece315c
CMakeLists.txt    6a4fa8cfa33ad088ad31088dcc162f3b
```

- Full `ctest`: **13/13 green** (nothing else broke)
- Live engine post-fix: **10/10** gate-valid
- Old-code arm stashed/rebuilt/restored to prove the guard binds

### Honest limits

1. **Fix and guard are circular** — §7b was written by me against this fix.
   The non-circular anchors are the *old-code failure* and the *live 10/10*.
2. **`bench/` artifacts are not covered by any assertion** in the verify run.
   The JSON was regenerated fresh via tooling (not hand-edited), but no check
   validates it.
3. The verify script was **cleaned up** after each run per convention — so the
   evidence is not re-playable from the temp dir. Reproduce via `ctest` +
   the invocation below.

---

## 6. Files changed (uncommitted)

```
 M dsh-lite-cpp/CMakeLists.txt            (+10  payload-draft-run target)
 M dsh-lite-cpp/src/grammar.cpp           (+64  sentinel tail-trim)
 M dsh-lite-cpp/tests/test_grammar.cpp    (+74  §7b regression guard)
?? dsh-lite-cpp/tests/payload_draft_run.cpp        (new two-arm bench)
?? bench/golem-enigma-prompt-frame.json            (raw per-probe evidence)
?? bench/golem-enigma-prompt-frame-notes.md        (findings writeup)
```

Nothing committed. `git stash list` empty.

---

## 7. OPEN — the unfixed half

**`harness/loop.ts` `buildLeafInvocation()` still uses the report frame**
(the 0/10 control arm). Two conflicting readings resolve against each other:

- `buildLeafInvocation` says *"Answer with your result, or with
  NEEDS_SPLIT:<reason>"* — a **report** contract (loop.ts:319).
- But `solicitToolPayload` expects a **payload** `{"tool":…,"args":…}`.

So either (a) the production TS loop needs a shape-declaring solicitation
prompt, or (b) the two are different consumers (report/verify vs solicit) and
a **prompt builder that doesn't exist yet** is the missing piece.

**Fixing it is not free**: `NEEDS_SPLIT` is load-bearing for the split logic
(loop.ts:308-321, *"only the loop splits"*). Rewriting the frame can break
leaf non-atomicity signalling. Needs a call-path read before touching.

---

## 8. Reproduce

```bash
cd /Users/khalid/dev/mr-meeseeks/dsh-lite-cpp

# two-arm bench (live engine required)
cmake --build build --target payload-draft-run -j8
./build/payload-draft-run http://enigma:8000/v1/chat/completions \
                          qwen3.6-colibri 10

# regression guard + full suite
ctest --test-dir build --output-on-failure
```

Negative-control check (the measurement's premise — both channels must still
400; if these start passing, the bench premise is invalid):

```bash
curl -s -m 30 http://enigma:8000/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen3.6-colibri","messages":[{"role":"user","content":"hi"}],
       "max_tokens":8,"response_format":{"type":"json_object"}}'
# expect: 400 unsupported_parameter
```

---

## 9. Related context

- Skills: `colibri-engine-operations` (engine ops, auth, static-path trap,
  the two telemetry lies), `home-server-operations` (enigma access).
- **Hermes provider registered but unusable**: `providers.colibri` in
  `~/.hermes/profiles/mmmm/config.yaml` (base_url `http://enigma:8000/v1`,
  `key_env HERMES_CUSTOM_COLIBRI_API_KEY`, sentinel appended to `.env`).
  Harmless — nothing selects it; default model unchanged
  (`deepseek-ai/DeepSeek-V4.1-Flash` via deepinfra). Delete it if you want
  the config clean.
- **Hardware ceiling**: RTX A4500, 20 GB VRAM, ~17.9 GB already consumed by
  qwen36. A second GLM/DeepSeek-class giant does **not** fit — so
  "serve a tool+grammar-capable arch" is a model-acquisition / VRAM question,
  not a code question.
- **Higher-leverage alternative** (deferred, not rejected): serve `glm53` or
  `qwen38`, both of which already have renderers + grammar support in
  `openai_server.py`. That removes the need for the workaround entirely.
