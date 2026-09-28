#!/usr/bin/env python3
"""Turn corpus.txt lines into engine ref.json fixtures (token ids) WITHOUT
the holdout logic of make_eval_refs.py — for ppl_usage heat: the engine's
PPL prefill of these refs is the heat workload; eval refs stay separate.

Usage: make_ppl_refs.py --model <container> --corpus corpus.txt --prefix heat_ref --count 12
Writes <prefix>_N.json per corpus line (first `count`), same schema as
make_eval_refs (prompt/prompt_ids/full_ids/text).
"""
import argparse, json, os, sys
from tokenizers import Tokenizer

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--corpus", default="corpus.txt")
    ap.add_argument("--prefix", default="heat_ref")
    ap.add_argument("--count", type=int, default=12)
    ap.add_argument("--min-tokens", type=int, default=32)
    ap.add_argument("--skip", type=int, default=0, help="skip first N usable lines (holdout)")
    ap.add_argument("--max-tokens", type=int, default=0, help="truncate ids to this length (0 = full line)")
    a = ap.parse_args()
    tok = Tokenizer.from_file(os.path.join(os.path.expanduser(a.model), "tokenizer.json"))
    n = 0; usable = 0
    for ln in open(a.corpus):
        s = ln.rstrip("\n")
        if not s: continue
        ids = tok.encode(s, add_special_tokens=False).ids
        if len(ids) < a.min_tokens: continue
        usable += 1
        if usable <= a.skip: continue
        if a.max_tokens: ids = ids[:a.max_tokens]
        ref = {"schema_version": 1,          # qwen38's ref loader hard-requires
                                            # this (olmoe ignores extra keys)
               "prompt": tok.decode(ids[:8]), "prompt_ids": ids[:8],
               "full_ids": ids, "text": tok.decode(ids)}
        n += 1
        json.dump(ref, open(f"{a.prefix}_{n}.json", "w"))
        if n >= a.count: break
    print(f"wrote {n} {a.prefix}_*.json from {a.corpus} ({sum(1 for _ in open(a.corpus))} lines avail)")

if __name__ == "__main__":
    main()
