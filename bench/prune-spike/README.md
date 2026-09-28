# Expert-pruning pipeline (repeatable; published as colibri #1621)

The experiment: does routing-count "coldness" predict which MoE experts can be
deleted? Answer, measured on OLMoE int8 (M5 Pro): NO — per unit of routing
traffic the count-cold tail is 22-29x more load-bearing than the average
expert (upstream issue #1621 + control-arm follow-up; maintainer reply: counts
are a placement signal, exactly their role in PIN/auto-pin/cache admission).

Upstream: https://github.com/JustVugg/colibri/issues/1621
PR #1625 (https://github.com/JustVugg/colibri/pull/1625) ports the in-process
ABLATE_SCORE harness to olmoe — the per-cell instrument that makes the 3x
container copies below unnecessary once it lands.

## Layout

    profiles/<model>.json   everything model-specific (engine, tensor names, dims)
    runs/<profile>/         per-model outputs (fixtures, usage, results, report)
    extract_corpus.py       real-workload text from ~/.dsh session logs
    make_eval_refs.py       teacher-forced eval fixtures (holdout from heat corpus)
    run_heat.sh             (manual) CHAT-loop heat capture -> .coli_usage file
    parse_route_trace.py    ROUTE_TRACE -> usage-format counts AND gate mass
    analyze_candidates.py   usage -> per-layer cold tail + zero-route set
    ablate_container.py     container round-trip zeroing N candidate experts
    run_experiment.py       the driver: corpus|fixtures|heat|cands|uniform|build|eval|report

## Run

    cd bench/prune-spike
    python3 run_experiment.py --profile profiles/olmoe_merged.json --steps report
        -> re-derives the published #1621 headline from published evidence
    python3 run_experiment.py --profile profiles/<newmodel>.json
        -> full pipeline for a freshly converted model (~3 container copies,
           hours; disk-heavy: 3x container size under /tmp). Start from
           profiles/olmoe_merged.json and edit; heat mode "route_trace" is for
           glm53/kimi/v4-class engines, "chat_loop" only works on olmoe.

## Adding a model = writing a profile

Required keys: engine, binary, model, layers, experts, tensor_res (regex list
matching `<layer,expert>` tensor names in the converted container), arms
{base,cold,rand,ctl paths}. Optional: args (engine CLI args, e.g. ["64","8"]),
pct (coldness threshold, default 0.1), seed (1972), threads, norm_topk_prob.

Engine capability matrix (measured on the local clone @ a8f2ca6/dev):

    engine        CHAT-loop heat   PPL loss meter   COLI_USAGE   ROUTE_TRACE   abl harness
    colibri(glm)  -                TF-score path    yes          yes           yes (native)
    olmoe         yes              yes              yes          no            yes (PR #1625)
    qwen36        no               yes              no           no            no
    qwen38        no               yes              yes          no            no
    glm53         no               no               yes          yes           no
    kimi_k3       no               no               yes          yes           no
    deepseek_v4   no               no               yes          yes           no

Notes:
- heat: only olmoe has the CHAT=1 feed loop for cheap bulk heat capture.
  glm53/kimi/v4 capture heat via COLI_USAGE on any run that forwards tokens
  (their rt_save fires per serve request). qwen38 saves usage too. qwen36
  saves neither — heat there needs a tiny engine-side rt_init hook (upstream
  contribution candidate) or the ROUTE_TRACE-less gap simply excludes it.
- gate mass: per #1609, .coli_usage records SELECTION COUNTS only; ROUTE_TRACE
  carries the gate weights. parse_route_trace.py emits both files — on engines
  that trace, the cold-tail selection can be redone by mass instead of count,
  which is the sharper experiment the maintainer pointed at. On count-only
  engines the #1621 caveat stays: rare-specialist vs rare-marginal are
  indistinguishable.
- norm_topk_prob=true models: dropping a slot (mode-2-style) and zeroing a
  contribution differ there; the container-zero method is pure zero-out
  regardless — for those models expect deltas to sit BETWEEN modes 1 and 2.
- eval is deterministic teacher-forced NLL (PPL=1 path); deltas per ref are
  causal against the same fixtures; ALWAYS keep a ctl (round-trip, nothing
  zeroed) arm result in the writeup — that control is what made #1621 credible.

## Discipline (what made the published result survive review)

1. Heat corpus must be big: a 24-token pilot "found" 140 dead experts; at
   12.0M picks it was 2. Never rank candidates from a small capture.
2. Holdout: eval fixtures drawn from session lines NOT in the heat corpus.
3. Uniform-random control arm, count-matched (same N experts, seed fixed,
   never consulting counts): without it "the count is the wrong signal" and
   "the count isn't a signal at all" are indistinguishable (maintainer point).
4. Pipeline-control arm (round-trip rewrite, nothing zeroed) proves the
   rewrite itself is loss-free; deltas are then causal.
5. n=refs is small (published: 2 distinct refs, one duplicate disclosed).
   Monotonicity across tiers within each sequence is the supporting evidence.
6. Label negative/no-change results as first-class; never spin them.
