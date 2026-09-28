# Golem on enigma — prompt-frame delta + sentinel fix (2026-09-20)

## Question
Can the colibri engine on enigma drive the Golem runtime (dsh-lite-cpp)?
Golem's tool intent travels over `body.response_format` (grammar-forced
drafts), NOT the OpenAI `tools` channel — `src/llm_client.cpp` builds no
`tools` field. enigma serves `qwen3.6-colibri`, whose family is
`grammar_payload=False` (`grammar.hpp` F46), so the grammar request 400s
and `brain.cpp:326` falls back to a plain post. The draft is then
UNCONSTRAINED text that `parseStrictPayload()` must accept cleanly.

## Method
`tests/payload_draft_run.cpp` (new, not a ctest member) drives the real
`solicitToolPayload -> parseStrictPayload -> checkPayload` path against a
live engine, two arms, same engine/tools/gate, only the prompt frame
differs. 10 probes per arm, `maxTokens=384`.

## Result — two independent findings

| arm | frame | pre-fix | post-fix |
|---|---|---|---|
| control | `harness/loop.ts` `buildLeafInvocation` (result-oriented) | 0/10 | **0/10** |
| treatment | `tests/g4_run.cpp:377-384` (shape-declaring) | 6/10 | **10/10** |

### Finding 1 — the prompt frame was the binding constraint
The control frame says *"Answer with your result, or with
NEEDS_SPLIT:<reason>"* — it never asks for a payload and never shows its
shape. Three control replies were literally `NEEDS_SPLIT:<reason>`. That
is near-correct compliance with the prompt as written, which is why the
rate is 0% rather than a model failure.

### Finding 2 — the remaining 4 failures were ONE defect, now fixed
All four pre-fix treatment failures were a trailing `<|im_end|>` template
token on an otherwise byte-perfect payload:

    {"tool":"read","args":{"path":"/tmp/build.log"}}<|im_end|>

- raised `maxTokens` 256 -> 384: **zero truncations** — not a budget issue
- deterministic: probe 1 leaks 2/2, probe 3 clean 2/2 via raw curl
- trigger is criterion LENGTH/COMPLEXITY, not any specific token
- NOT client-side mangling (a bare request via raw curl never leaks)
- mechanism: wire-format artifact of `render_chat_qwen`'s pre-closed
  think block

Fixed in `src/grammar.cpp` `parseStrictPayload()`: a **closed allowlist**
of engine chat-template sentinels (`<|im_end|>`, `<|endoftext|>`, `</s>`)
is stripped **from the tail only**, repeatedly, before the strict parse.
Measured effect: 6/10 -> **10/10** (all four leaks recovered).

F45 INTENT PRESERVED — the original law forbids prose frames, fences,
prefix/suffix *garbage*, and substring extraction/repair heuristics. All
of those still fail: a non-allowlisted token (`<|im_start|>`), a sentinel
followed by junk, a leading sentinel plus prose, and fenced JSON are all
still REJECTED. Stripping the engine's own delimiter is tokenizer hygiene,
not JSON-hunting. `checkPayload` remains the only hard enforcement point.

Regression guard: `tests/test_grammar.cpp` §7b, 11 cases. Verified to
**FAIL against the pre-fix code** (5 recovery cases fail, exit 1) and pass
after (exit 0). Full `ctest`: 13/13 green.

## What this does NOT claim
Not "Golem works/breaks on enigma" — a measured prompt-frame delta and a
sentinel fix for this engine without grammar. Per F45 the colibri grammar
is a speculative DRAFT source, never a sampling constraint, so its absence
costs acceptance rate, not output shape. `checkPayload` validates SHAPE,
never intent (one pre-fix probe returned a gate-valid but semantically
wrong `read` for a criterion that wanted `bash`).

## Reproduce
    cd dsh-lite-cpp
    cmake --build build --target payload-draft-run -j8
    ./build/payload-draft-run http://enigma:8000/v1/chat/completions \\
                              qwen3.6-colibri 10
    ctest --test-dir build --output-on-failure   # §7b regression guard

Raw per-probe evidence: `bench/golem-enigma-prompt-frame.json`.
