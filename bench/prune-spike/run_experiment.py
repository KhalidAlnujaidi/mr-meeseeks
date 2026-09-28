#!/usr/bin/env python3
"""Profile-driven driver for the colibri expert-pruning experiment (#1621).

Everything model-specific lives in profiles/<name>.json; everything
procedural lives here. The pipeline is exactly what was published, step by
step, so a re-run on a new model is a new profile, not new code.

  steps (subset, in order):
    corpus    extract_corpus.py         -> corpus.txt (+ token report)
    heat      CHAT-loop heat capture    -> usage.txt   (mode chat_loop;
            or ROUTE_TRACE capture      -> route_trace.txt, for engines
            without a chat loop — parse_route_trace.py normalizes it)
    cands     analyze_candidates.py     -> candidates_cold.txt (per-layer
            --pct cold tail + zero-route set)
    uniform   uniform control arm       -> candidates_rand.txt + arm_shares.json
    build     ablate_container.py       -> one container per arm (slow:
            full container rewrite; skipped if the arm dir already exists)
    eval      paired TF-NLL, arms rotated per ref -> results.json
    report    damage-per-expert and damage-per-traffic-unit vs the uniform
            control — the #1621 headline recomputed for this model

Usage:
  run_experiment.py --profile profiles/olmoe_merged.json --steps eval,report
  run_experiment.py --profile profiles/newmodel.json          # all steps
"""
import argparse, collections, json, os, random, re, shutil, statistics, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))

def load_profile(path):
    p = json.load(open(path))
    for k in ("engine","binary","model","layers","experts","tensor_res"):
        if k not in p: sys.exit(f"profile {path}: missing required key '{k}'")
    p.setdefault("args", [])
    p.setdefault("threads", 8)
    p.setdefault("pct", 0.1)
    p.setdefault("seed", 1972)
    p.setdefault("out", os.path.splitext(os.path.basename(path))[0])
    return p

def expand(x): return os.path.expanduser(x)

def run_engine(p, snap, extra_args, env=None, timeout=1800):
    e = dict(os.environ, SNAP=snap, OMP_NUM_THREADS=str(p["threads"]))
    if env: e.update(env)
    r = subprocess.run([expand(p["binary"])] + list(p["args"]) + extra_args,
                       env=e, capture_output=True, text=True, timeout=timeout)
    return r

# ---------------- steps ----------------

def step_corpus(p, work):
    out = os.path.join(work, "corpus.txt")
    subprocess.run([sys.executable, os.path.join(HERE, "extract_corpus.py"),
                    "--model", expand(p["model"]), "--out", out], check=True)
    return out

def step_fixtures(p, work):
    if p.get("fixtures") == "corpus_split":
        # No session logs on this host: derive BOTH workloads from corpus.txt
        # with a line split — heat = first N lines (full length, published
        # scale), eval = next M lines truncated (~210 ids, #1621 window).
        hs = int(p.get("heat_lines", 60)); es = int(p.get("eval_lines", 3))
        mk = os.path.join(HERE, "make_ppl_refs.py")
        subprocess.run([sys.executable, mk, "--model", expand(p["model"]),
                        "--corpus", os.path.join(work, "corpus.txt"),
                        "--prefix", os.path.join(work, "heat_ref"),
                        "--count", str(hs)], check=True)
        subprocess.run([sys.executable, mk, "--model", expand(p["model"]),
                        "--corpus", os.path.join(work, "corpus.txt"),
                        "--prefix", os.path.join(work, "eval_ref"),
                        "--skip", str(hs), "--count", str(es),
                        "--max-tokens", "210"], check=True)
        return
    subprocess.run([sys.executable, os.path.join(HERE, "make_eval_refs.py"),
                    "--model", expand(p["model"]),
                    "--heat-lines", os.path.join(work, "corpus.txt")],
                   cwd=work, check=True)

def step_heat(p, work):
    corpus = os.path.join(work, "corpus.txt")
    usage = os.path.join(work, "usage.txt")
    if p["heat"] == "chat_loop":
        # feed one /reset+line pair per corpus line, MAX_NEW=1: prefill bumps
        # the freq counters, stdin close flushes COLI_USAGE (verified recipe).
        lines = []
        for ln in open(corpus):
            ln = ln.replace("\\", "\\\\").rstrip("\n")
            if ln: lines += ["/reset", ln]
        e = dict(os.environ, SNAP=expand(p["model"]), CHAT="1", MAX_NEW="1",
                 PILOT="0", COLI_USAGE=usage, OMP_NUM_THREADS=str(p["threads"]))
        subprocess.run([expand(p["binary"])] + list(p["args"]),
                       input="\n".join(lines) + "\n", env=e, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=14400)
    elif p["heat"] == "route_trace":
        rt = os.path.join(work, "route_trace.txt")
        env = {"ROUTE_TRACE": rt, "COLI_USAGE": usage}
        refs = sorted(f for f in os.listdir(work) if f.startswith("eval_ref_"))
        for ref in refs:
            run_engine(p, expand(p["model"]), [os.path.join(work, ref)],
                       env=dict(env, PPL="1"))
        print("normalize with: python3 parse_route_trace.py", rt, "-> usage-style")
        if not os.path.exists(usage):
            subprocess.run([sys.executable, os.path.join(HERE, "parse_route_trace.py"),
                            rt, "--out", usage], check=True)
    elif p["heat"] == "ppl_usage":
        # engines without a CHAT loop but with a PPL meter that flushes
        # COLI_USAGE (qwen38): heat = prefill of dedicated heat refs (the
        # eval refs stay OUT of the heat workload, mirroring #1621's
        # holdout); counts accumulate across runs (rt_save merges).
        if not any(f.startswith("heat_ref_") for f in os.listdir(work)):
            subprocess.run([sys.executable, os.path.join(HERE, "make_ppl_refs.py"),
                            "--model", expand(p["model"]),
                            "--corpus", os.path.join(work, "corpus.txt"),
                            "--prefix", os.path.join(work, "heat_ref")],
                           check=True)
        refs = sorted(f for f in os.listdir(work) if f.startswith("heat_ref_"))
        for ref in refs:
            r = run_engine(p, expand(p["model"]), [os.path.join(work, ref)],
                           env={"PPL": "1", "COLI_USAGE": usage}, timeout=7200)
            if r.returncode != 0:
                sys.exit(f"heat prefill {ref} failed rc={r.returncode}:\n"
                         + (r.stderr or r.stdout)[-500:])
        if not os.path.exists(usage):
            sys.exit(f"heat finished but {usage} was never written — engine "
                     f"ignores COLI_USAGE or crashed quietly")
    else:
        sys.exit(f"heat mode '{p['heat']}' unknown (chat_loop|ppl_usage|route_trace)")
    return usage

def read_usage(path):
    counts = collections.defaultdict(dict); dims = None
    for ln in open(path):
        l, e, c = map(int, ln.split())
        if l < 0:
            if l == -1: dims = (e, c)
            continue
        counts[l][e] = counts[l].get(e, 0) + c
    if dims is None: sys.exit("usage file has no header record; refusing")
    return counts, dims

def step_cands(p, work):
    usage = os.path.join(work, "usage.txt")
    out = os.path.join(work, "candidates_cold.txt")
    subprocess.run([sys.executable, os.path.join(HERE, "analyze_candidates.py"),
                    usage, "--pct", str(p["pct"]), "--out", out], check=True)
    return out

def step_uniform(p, work):
    counts, (nl, ne) = read_usage(os.path.join(work, "usage.txt"))
    tot = sum(sum(v.values()) for v in counts.values())
    cold = {(int(l), int(e)) for l, e, _ in
            (ln.split() for ln in open(os.path.join(work, "candidates_cold.txt"))
             if not ln.startswith("#"))}
    rnd = random.Random(p["seed"]); rand = set()
    while len(rand) < len(cold):
        rand.add((rnd.randrange(nl), rnd.randrange(ne)))
    share = lambda ps: 100.0 * sum(counts[l].get(e, 0) for l, e in ps) / tot
    with open(os.path.join(work, "candidates_rand.txt"), "w") as f:
        f.write(f"# uniform-random control arm (seed {p['seed']}): layer expert count\n")
        for l, e in sorted(rand):
            f.write(f"{l} {e} {counts[l].get(e, 0)}\n")
    arm = {"cold_share_pct": share(cold), "uniform_share_pct": share(rand),
           "n": len(cold), "overlap": len(cold & rand)}
    json.dump(arm, open(os.path.join(work, "arm_shares.json"), "w"), indent=1)
    print(f"cold {arm['cold_share_pct']:.2f}% vs uniform {arm['uniform_share_pct']:.2f}%"
          f" traffic, overlap {arm['overlap']}")
    return arm

def step_build(p, work):
    made = {}
    for name in ("cold", "rand", "ctl"):
        cand = {"cold": "candidates_cold.txt", "rand": "candidates_rand.txt",
                "ctl": None}[name]
        out = expand(p["arms"][name])
        if os.path.exists(os.path.join(out, "config.json")):
            print(f"  arm {name}: {out} exists, skip build"); made[name] = out; continue
        cmd = [sys.executable, os.path.join(HERE, "ablate_container.py"),
               "--model", expand(p["model"]), "--out", out,
               "--tensor-res", json.dumps(p["tensor_res"])]
        if cand: cmd += ["--candidates", os.path.join(work, cand)]
        else:    cmd += ["--zero-all"]
        if p.get("link_mode"): cmd += ["--link-mode"]
        subprocess.run(cmd, check=True)
        made[name] = out
    return made

def step_eval(p, work):
    refs = sorted(f for f in os.listdir(work) if re.match(r"eval_ref_\d+\.json$", f))
    if len(refs) < 2:
        sys.exit("need >=2 eval_ref_*.json in the work dir (run make_eval_refs-style "
                 "fixtures for THIS model's tokenizer first); see README")
    nll_re = re.compile(r"TF-NLL: ([\d.]+) nats/token over (\d+) tokens")
    arms = {k: expand(v) for k, v in p["arms"].items() if k in ("base", "cold", "rand")}
    results = {}

    eval_to = int(p.get("eval_timeout", 1800))   # shared hosts can starve CPU:
                                                # big-model profiles raise this
    def eval_arm(a, snap):
        for r in refs:
            out = run_engine(p, snap, [os.path.join(work, r)], env={"PPL": "1"},
                             timeout=eval_to).stdout
            m = nll_re.search(out)
            if not m: raise RuntimeError(f"no TF-NLL for {a}|{r}:\n{out[-300:]}")
            results[f"{a}|{r}"] = float(m.group(1))
            print(f"  {r} {a:>4}: {m.group(1)}", flush=True)

    if p.get("seq_arms"):
        # Big containers: the disk cannot hold base + 3 full copies. Build one
        # arm, eval it, delete it, next arm. TF-NLL is deterministic, so the
        # protocol is unchanged — arms never needed to coexist logically,
        # only the naive pipeline made them coexist physically. ctl (the
        # --zero-all round-trip) is the FIRST arm built: its delta must be
        # 0.0000, which is the proof the container rewrite itself is lossless.
        eval_arm("base", arms["base"])
        import shutil as _sh
        for a in ("ctl", "cold", "rand"):
            out = expand(p["arms"][a])
            if not os.path.exists(os.path.join(out, "config.json")):
                cmd = [sys.executable, os.path.join(HERE, "ablate_container.py"),
                       "--model", expand(p["model"]), "--out", out,
                       "--tensor-res", json.dumps(p["tensor_res"])]
                cand = {"cold": "candidates_cold.txt", "rand": "candidates_rand.txt"}.get(a)
                if cand: cmd += ["--candidates", os.path.join(work, cand)]
                else:    cmd += ["--zero-all"]
                # ctl MUST be a full save_file round-trip — its whole point is
                # proving the rewrite is lossless (linking would trivially be
                # identical and prove nothing). Only the real arms may link.
                if p.get("link_mode") and a != "ctl": cmd += ["--link-mode"]
                subprocess.run(cmd, check=True)
            eval_arm(a, out)
            _sh.rmtree(out, ignore_errors=True)
        json.dump(results, open(os.path.join(work, "results.json"), "w"), indent=1)
        return results
    order = list(arms)
    for r in refs:
        for a in order:                      # deterministic TF-NLL: one pass
            out = run_engine(p, arms[a], [os.path.join(work, r)],
                             env={"PPL": "1"}, timeout=eval_to).stdout
            m = nll_re.search(out)
            if not m: raise RuntimeError(f"no TF-NLL for {a}|{r}:\n{out[-300:]}")
            results[f"{a}|{r}"] = float(m.group(1))
            print(f"  {r} {a:>4}: {m.group(1)}", flush=True)
    json.dump(results, open(os.path.join(work, "results.json"), "w"), indent=1)
    return results

def step_report(p, work, arm_shares=None):
    results = json.load(open(os.path.join(work, "results.json")))
    results = {k: (statistics.median(v) if isinstance(v, list) else v)
               for k, v in results.items()}
    if arm_shares is None:
        ap = os.path.join(work, "arm_shares.json")
        arm_shares = json.load(open(ap)) if os.path.exists(ap) else {}
    refs = sorted({k.split("|")[1] for k in results if "|" in k})
    rows = {}
    for r in refs:
        base = results[f"base|{r}"]
        for a in ("ctl", "cold", "rand"):
            if f"{a}|{r}" in results:
                rows.setdefault(a, []).append(results[f"{a}|{r}"] - base)
    print(f"\n=== {p['name']}: delta nats/token vs baseline (refs {len(refs)}) ===")
    mean = lambda v: sum(v)/len(v)
    for a in ("ctl", "cold", "rand"):
        if a in rows:
            tag = "  <- pipeline control, must be 0.0000" if a == "ctl" else ""
            print(f"  {a:>4}: per-ref {[f'{x:+.4f}' for x in rows[a]]}  mean {mean(rows[a]):+.4f}{tag}")
    if "cold" in rows and "rand" in rows and arm_shares:
        c, u = mean(rows["cold"]), mean(rows["rand"])
        cs, us = arm_shares["cold_share_pct"], arm_shares["uniform_share_pct"]
        print(f"\n  per-expert cold/uniform damage: {100*c/u:.0f}%  "
              f"(published #1621 OLMoE value: 63-86%)")
        if cs > 0:
            print(f"  per-traffic-unit: cold is {(c/cs)/(u/us):.1f}x more load-bearing "
                  f"than the average expert  (OLMoE: 22-29x)")
        else:
            print(f"  per-traffic-unit: UNDEFINED — zero-count cold arm carries no "
                  f"traffic by construction; the comparison IS delta(cold) vs 0 "
                  f"(mean {c:+.4f}); matched-count rand mean {u:+.4f}.")
        if not p.get("norm_topk_prob", True):
            print("  note: norm_topk_prob=false on this model — cold/rank arms are "
                  "zero-contribution-equivalent; deltas remain valid.")

# ---------------- cli ----------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True)
    ap.add_argument("--steps", default="corpus,fixtures,heat,cands,uniform,build,eval,report")
    a = ap.parse_args()
    p = load_profile(os.path.join(HERE, a.profile) if not os.path.isabs(a.profile) else a.profile)
    work = os.path.join(HERE, "runs", p["out"])
    os.makedirs(work, exist_ok=True)
    # reuse the olmoe fixtures when re-running the published profile untouched
    if p["out"] == "olmoe_merged":
        for f in os.listdir(HERE):
            if re.match(r"(corpus\.txt|eval_ref_\d+\.json|usage_full\.txt|"
                        r"armc_results\.json|arm_shares\.json)$", f):
                dst = os.path.join(work, f)
                if not os.path.exists(dst): shutil.copy2(os.path.join(HERE, f), dst)
        if os.path.exists(os.path.join(work, "usage_full.txt")) and \
           not os.path.exists(os.path.join(work, "usage.txt")):
            os.rename(os.path.join(work, "usage_full.txt"), os.path.join(work, "usage.txt"))
        # the published eval lives in armc_results.json; report reads results.json
        pub = os.path.join(work, "armc_results.json")
        res = os.path.join(work, "results.json")
        if os.path.exists(pub) and not os.path.exists(res):
            shutil.copy2(pub, res)
    steps = a.steps.split(",")
    arm_shares = None
    if "corpus" in steps and not os.path.exists(os.path.join(work, "corpus.txt")): step_corpus(p, work)
    if "fixtures" in steps and not any(f.startswith("eval_ref_") for f in os.listdir(work)):
        step_fixtures(p, work)
    if "heat" in steps: step_heat(p, work)
    if "cands" in steps: step_cands(p, work)
    if "uniform" in steps: arm_shares = step_uniform(p, work)
    if "build" in steps: step_build(p, work)
    if "eval" in steps: step_eval(p, work)
    if "report" in steps: step_report(p, work, arm_shares)

if __name__ == "__main__":
    main()
