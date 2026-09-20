# Contributing to Golem

Golem is a C++20 runtime with a hard quality bar: the harness executes
model-proposed commands, so defects are safety incidents, not
inconveniences. Please read this before opening a PR.

## Local build requirements

- C++20 compiler (clang 15+ or GCC 12+; CI uses Apple clang on arm64)
- CMake >= 3.20
- OpenSSL (Homebrew `openssl@3` on macOS)
- nlohmann/json and cpp-httplib — resolved via `find_package` first,
  FetchContent fallback, so nothing to install manually
- Optional, for the in-process C ABI lane: a Colibri checkout with
  `make -C colibri/c segment-edge-library` (targets auto-disable when
  the static lib is absent)
- Optional, for live runs: a local Colibri engine + model weights

```sh
cd dsh-lite-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

## The build law

**Zero warnings. `-Wall -Wextra -Werror` is not aspirational — a build
with one new warning fails.** Do not suppress warnings locally to make
your branch pass; fix the code. If a third-party header forces a
suppression, scope it as narrowly as possible and explain in the PR.

## Test suite expectations

```sh
ctest --test-dir build --output-on-failure
```

- **All 13 suites must pass, offline, hermetically.** No suite may
  require a running engine, network access, or a model checkpoint.
  Live-engine sections are opt-in via environment variables (e.g.
  `ABI_MODEL_DIR`) and must print an explicit SKIP and exit 0 when unset.
- Sanitizer discipline: threaded/concurrency changes must also pass an
  ASan+UBSan build:
  ```sh
  cmake -S . -B build-asan \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build build-asan -j8 && ctest --test-dir build-asan
  ```
  (macOS note: LeakSanitizer is unsupported — do not set
  `ASAN_OPTIONS=detect_leaks=1`.)
- **Regression tests must fail against the old code.** A test that
  passes both before and after your fix proves nothing; show the
  before-failure in the PR description.
- Tests that encode behavior the project has deliberately changed
  should be rewritten, not preserved — but say so explicitly in the PR.

## PR standards

1. **Flaw audit before execution.** If your PR touches execution,
   gating, telemetry, or the ABI lane, list the flaws your change could
   introduce (location / defect / impact) and how each is mitigated —
   in the PR description, before review. This is the project's standing
   rule; reviewers will ask.
2. **Honest telemetry only.** Never fabricate or interpolate token
   counts, latencies, or verification results. Estimates must be
   flagged as estimates (`tokens_estimated`); unmeasured values stay
   null/-1. A benchmark number in a PR description must come from a
   reproducible artifact, not a single lucky run — include the log.
3. **Contract deltas get documented, not hidden.** If your change makes
   Golem behave differently from a documented contract (wire shape,
   ledger schema, gate policy), update the docs and the flaw register
   in the same PR.
4. **Ledger schema changes are breaking changes.** `ledger.jsonl` v2 is
   an audit surface; additive fields are fine, semantic changes to
   existing fields are not. Bump the schema version and justify it.
5. Keep diffs scoped. No drive-by refactors mixed with behavior
   changes; split them into separate commits.
6. Commits follow the existing convention:
   `feat(scope): ...`, `fix(scope): ...`, `bench(scope): ...`,
   `docs: ...`, with the flaw IDs (F-numbers) referenced when relevant.

## Benchmarks

`bench/h2h/` is referee-judged: verdicts come from the proxy log +
filesystem ground truth, never a harness self-report. If you change
anything the bench measures, re-run both arms sequentially (the local
engine is single-slot — concurrent runs void the timing axis) and
attach `out/RESULTS.md`. Contaminated runs are labeled VOID, not
quietly deleted.

## Code of conduct / licensing

MIT (see LICENSE). Contributions are licensed under the same terms.
By submitting a PR you certify you have the right to license the code.

Security vulnerabilities: **do not open a public issue** — follow
[SECURITY.md](SECURITY.md).
