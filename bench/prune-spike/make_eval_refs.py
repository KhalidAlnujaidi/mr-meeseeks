#!/usr/bin/env python3
"""Build N teacher-forced eval refs (~seq_tokens each) from corpus lines that
were NOT in the heat-run first 12 lines (mild holdout), in the engine's ref.json
schema: {prompt, prompt_ids, full_ids, text}. prompt = first 8 ids."""
import json, os, sys
from tokenizers import Tokenizer

dsh = os.path.expanduser("~/.dsh/sessions")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_corpus import iter_records, text_of, clean
import glob, random

import argparse
ap = argparse.ArgumentParser(); ap.add_argument("--model",
    default=os.path.expanduser("~/models/olmoe_merged"))
ap.add_argument("--heat-lines", default="corpus.txt")
a = ap.parse_args()
MODEL = os.path.expanduser(a.model)
tok = Tokenizer.from_file(os.path.join(MODEL, "tokenizer.json"))
heat_lines = set(open(a.heat_lines).read().splitlines()[:12])   # in-sample ban list

texts = []
files = sorted(glob.glob(os.path.join(dsh, "**/session.jsonl.zstd"), recursive=True))
random.Random(99).shuffle(files)
for f in files:
    if len(texts) >= 3: break
    for rec in iter_records(f):
        s = clean(text_of(rec))
        if len(s) < 2000 or s in heat_lines: continue
        ids = tok.encode(s, add_special_tokens=False).ids
        if len(ids) < 210: continue
        texts.append(ids[:210])
        if len(texts) >= 3: break

for i, ids in enumerate(texts, 1):
    ref = {"prompt": tok.decode(ids[:8]), "prompt_ids": ids[:8],
           "full_ids": ids, "text": tok.decode(ids)}
    json.dump(ref, open(f"eval_ref_{i}.json", "w"))
    print(f"eval_ref_{i}.json: {len(ids)} tokens, scored={len(ids)-8}")
