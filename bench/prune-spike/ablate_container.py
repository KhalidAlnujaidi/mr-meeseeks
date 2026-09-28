#!/usr/bin/env python3
"""Build an ablated colibri container: zero the expert weight tensors for the
candidate (l,e) list. Engine-generic since the #1621 pipeline port: which
tensor names count as "expert weights" comes from --tensor-res (JSON list of
regexes with groups (layer, expert); default = olmoe merged int8).

int8 zeros dequantize to exactly 0.0 regardless of group scales (qs
untouched), so the expert contributes nothing to the block sum. On models
with norm_topk_prob=true the surviving weights renormalise accordingly;
olmoe (norm_topk=false) adds a pure zero-out with no side-effects.
Writes a full container copy with shards rewritten.
"""
import argparse, json, os, re, sys
from safetensors import safe_open
from safetensors.torch import save_file

OLMOE_RES = [r"model\.layers\.(\d+)\.mlp\.experts\.(\d+)\.merged_weight$"]
# multi-tensor families (zero gate+up+down together -> expert outputs 0):
# e.g. [r"layers\.(\d+)\.mlp\.experts\.(\d+)\.gate_proj\.weight$",
#       r"...up_proj...", r"...down_proj..."]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default=os.path.expanduser("~/models/olmoe_merged"))
    ap.add_argument("--candidates", default="candidates_full.txt")
    ap.add_argument("--out", default=os.path.expanduser("~/models/olmoe_ablated"))
    ap.add_argument("--tensor-res", default=json.dumps(OLMOE_RES),
                    help="JSON list of (layer,expert) name regexes to zero")
    ap.add_argument("--zero-all", action="store_true",
                    help="control arm: round-trip the container, zero NOTHING")
    ap.add_argument("--link-mode", action="store_true",
                    help="hardlink untouched shards + metadata instead of copying "
                         "(only rewritten shards consume new disk; source must "
                         "stay read-only while arms exist)")
    a = ap.parse_args()

    targets = set()
    if not a.zero_all:
        for ln in open(a.candidates):
            if ln.startswith("#"): continue
            l, e, c = map(int, ln.split())
            targets.add((l, e))
        print(f"{len(targets)} candidate experts to zero", flush=True)
    else:
        print("pipeline-control arm: full round-trip, nothing zeroed", flush=True)
    name_re = [re.compile(rx) for rx in json.loads(a.tensor_res)]

    os.makedirs(a.out, exist_ok=True)
    import shutil
    # copy (or hardlink) metadata files
    for fn in os.listdir(a.model):
        src = os.path.join(a.model, fn)
        dst = os.path.join(a.out, fn)
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True)
        elif not fn.endswith(".safetensors"):
            if a.link_mode: os.link(src, dst) if not os.path.exists(dst) else None
            else: shutil.copy2(src, dst)

    zeroed = 0
    hits = set()          # distinct (layer, expert) actually zeroed
    for fn in sorted(os.listdir(a.model)):
        if not fn.endswith(".safetensors"): continue
        path = os.path.join(a.model, fn)
        with safe_open(path, framework="pt") as f:
            meta = f.metadata()
            tensors = {k: f.get_tensor(k) for k in f.keys()}
        touched = 0
        for k, v in tensors.items():
            for rx in name_re:
                m = rx.match(k)
                if m and (int(m.group(1)), int(m.group(2))) in targets:
                    import torch
                    tensors[k] = torch.zeros_like(v)
                    zeroed += 1; touched += 1; hits.add((int(m.group(1)), int(m.group(2))))
                    break
        if touched == 0 and a.link_mode:
            # untouched shard: hardlink it in instead of rewriting 1-4 GB.
            dst = os.path.join(a.out, fn)
            if not os.path.exists(dst): os.link(path, dst)
        else:
            dst = os.path.join(a.out, fn)
            # a rewrite must never travel through a hardlink into the source
            # (crashed-earlier-run re-runs land here with dst already linked).
            if os.path.exists(dst) and os.path.samefile(dst, path):
                os.unlink(dst)
            save_file(tensors, dst, metadata=meta)
        del tensors
        print(f"  {fn}: zeroed {touched}", flush=True)
    # multi-tensor engines (qwen3: gate+up+down = 3 tensors/expert) zero more
    # tensors than there are candidates; the invariant is DISTINCT experts hit.
    print(f"DONE: zeroed {zeroed} tensors / {len(hits)} experts "
          f"(of {len(targets)} candidates) -> {a.out}")
    if hits != targets:
        missing = len(targets - hits)
        sys.exit(f"FAIL: {missing} candidate experts not found in shards")

if __name__ == "__main__":
    main()
