# Verifying Stage A

> **Status handover:** `HANDOVER-lg.md` in this directory — profile `lg`.

Two levels, and they are not the same claim.

## 1. In-repo suite (this IS green)

```sh
cd colibri/c
python3 -m unittest tests.test_residency_sim      # 39 tests, OK
```

35 of those are upstream's; 4 are new and cover the Stage-A additions:

- `test_markov_without_a_table_reproduces_half_pinned_exactly` — the control that
  makes any markov-vs-baseline number attributable to the table.
- `test_markov_requires_a_table_to_be_loaded`
- `test_markov_table_round_trips_and_scores_summation` — summing over the
  conditioning set must outrank a single rare predecessor.
- `test_load_markov_table_rejects_a_foreign_format`

`make check` (clean + portable + test) was NOT run: it wipes every build artifact
and rebuilds the whole tree. PR 1's olmoe change is verified separately and its
four test binaries pass.

## 2. Ad-hoc verification of the changed behaviour (NOT suite green)

Two standalone scripts exercising Stage A end to end against the real artifacts
(traces, manifest, table) — they fabricate nothing and refuse to run if an
artifact is missing. 31 + 32 checks, all passing:

```sh
cd bench/prune-spike/stage-a
python3 hermes-verify-stage-a.py          # simulator, markov policy, route_markov.py
python3 hermes-verify-stage-a-drivers.py  # gate driver, felt-cost, capture, corpus
```

`hermes-verify-stage-a.py` groups: A1-A5 simulator ingests OLMoE traces +
manifest; B1-B8 `route_markov.py` sparsity, ordering, uniqueness, and holdout
enforcement; C1-C5 the empty-table control; D1-D6 the negative result
re-derived, plus the measured cause (prediction set ~19.7 experts vs 10 unpinned
slots); E1-E2 the in-repo suite; F budget compliance at all four budgets.

`hermes-verify-stage-a-drivers.py` groups: G1-G9 the gate driver, including
**G6 asserting markov == half-pinned at every budget** and G8 checking
`bytes_read == misses * expert_size` so byte accounting cannot drift from the
manifest; H1-H7 the felt-cost validator, including H6/H7 reproducing the
non-constant, rising cost per miss; I1-I2 `capture_traces.sh` argument handling
and its skip-existing path; J1-J5 the corpus builder's split, quota and
duplicate-content logic.

Kept here rather than only in a temp dir because the numbers in FINDINGS.md
should be re-derivable by a reader. If either drifts from residency_sim.py, that
is itself a finding.

Note on the H5 check: it first anchored on the last column of validate_felt_cost's
table, which is the NLL, and collected 3.837 twice — a false failure caused by the
check, not the code. Fixed to read the µs/miss column by position.

## Reproducing the whole campaign

```sh
cd bench/prune-spike/stage-a
python3 stage_a_build_corpus.py --out traces --per-category 8   # refs + by-session split
./capture_traces.sh /path/to/olmoe traces/refs traces/raw        # ~50 min, one process per ref
python3 route_markov.py markov.table --train $(cat train.txt) \
        --score $(cat heldout.txt) --report --topn 8
python3 run_olmoe_gate.py --traces traces/raw --manifest olmoe-residency.json \
        --markov-table markov.table --budgets-gb 0.5 1 2 4 --read-gbps 4.64
python3 validate_felt_cost.py /path/to/olmoe <ref.json> 8 16 24 32 48 64
```
