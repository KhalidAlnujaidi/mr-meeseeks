## Follow-up: ran the uniform-random control arm I'd flagged as missing

The first post ended with "a uniform-random-drop control arm would sharpen the per-byte comparison; not run here." Now run — and it both confirms the direction and **corrects a number I should not have quantified before measuring**.

Setup: 220 experts drawn uniformly at random (seed 1972, `random.Random(1972)`, never consulting the counts), zeroed through the same safetensors round-trip (container audited: exactly 220/220 candidates zeroed, nothing else). Same held-out refs, same `PPL=1` protocol. The two arms overlap on 50 experts; irrelevant for same-base deltas.

| arm | experts | traffic carried | ΔNLL ref A | ΔNLL ref B |
|---|---|---|---|---|
| cold tail (<0.1%/layer) | 220 | 0.66% | +0.2487 | +0.3302 |
| uniform random | 220 | 22.51% | +0.3932 | +0.3841 |

Two things fall out:

1. **Per expert removed, a cold-tail expert is *cheaper* than a random one** — 63–86% of its damage. My earlier hand-wave ("1–6× more damage per byte than a uniform drop") was the wrong shape: absolute damage is lower, as it must be (the router picks them less often). That sentence was a guess dressed as a measurement; strike it.
2. **Per unit of routing traffic, the tail is 22–29× more load-bearing than the average expert** (0.66% of traffic doing 63–86% of random's damage). That's the number that survives measurement, and it's the whole argument: hit-count coldness massively overestimates removability because rare picks are specialists, not noise — a runbook-heavy sequence pays +0.33 for the tail where an email-heavy one pays +0.25, so even the "cheap" experts have domains.

Consequence for pruning policy unchanged: if you want bytes back, remove them via measured importance *reconstruction* (the REAP route), not via routing counts; counts tell you where to place, not what to delete. Raw: `armc_results.json`, `arm_shares.json` (sha-pinned in the manifest's evidence trail).
