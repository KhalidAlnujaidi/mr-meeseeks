#!/usr/bin/env python3
"""Extract a real-workload text corpus from DSH Desktop session logs
(session.jsonl.zstd) for OLMoE expert-heat feeding.

Reads N session files, pulls user/assistant/tool text out of the typed
records, normalizes to single lines (engine chat reads fgets 8192 max),
and writes corpus.txt + a size report measured against the model's OWN
tokenizer (tokenizers lib, HF Fast tokenizer from the converted container).

Usage: extract_corpus.py [--sessions GLOB] [--limit 20] [--max-tokens 20000]
                         [--out corpus.txt] [--model ~/models/olmoe_merged]
"""
import argparse, glob, json, os, subprocess, sys

TEXT_TYPES = {"user/message", "assistant/message", "agent/inbox/spliced", "tool/result"}

def iter_records(path):
    """Stream one record dict per line, zstd-decompressing transparently."""
    if path.endswith(".zstd") or path.endswith(".zst"):
        p = subprocess.run(["zstd", "-dc", path], capture_output=True)
        data = p.stdout
    else:
        data = open(path, "rb").read()
    for ln in data.decode("utf-8", "replace").splitlines():
        try:
            yield json.loads(ln)
        except json.JSONDecodeError:
            continue

def parts_text(content):
    """content may be str or a list of typed parts; return joined text parts."""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return " ".join(p.get("text", "") for p in content
                        if isinstance(p, dict) and p.get("type") == "text")
    return ""

def text_of(rec):
    t = rec.get("type")
    if t not in TEXT_TYPES:
        return ""
    d = rec.get("data", rec)
    if not isinstance(d, dict):
        return ""
    if t == "agent/inbox/spliced":                    # inserted: [{content: parts}]
        ins = d.get("inserted", [])
        return " ".join(parts_text(x.get("content")) for x in ins
                       if isinstance(x, dict))
    msg = d.get("message", d)                         # assistant/tool wrap in .message
    if isinstance(msg, dict):
        s = parts_text(msg.get("content"))
        if s:
            return s
    return parts_text(d.get("content"))

def clean(s):
    s = "".join(ch if (ch >= " " or ch == "\t") else " " for ch in s)
    return " ".join(s.split())

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sessions", default=os.path.expanduser(
        "~/.dsh/sessions/**/session.jsonl.zstd"), help="glob of session files")
    ap.add_argument("--limit", type=int, default=20, help="max session files")
    ap.add_argument("--per-file", type=int, default=4, help="max text records per file (breadth over depth)")
    ap.add_argument("--max-tokens", type=int, default=20000)
    ap.add_argument("--line-bytes", type=int, default=7000)   # fgets cap is 8192
    ap.add_argument("--out", default="corpus.txt")
    ap.add_argument("--model", default=os.path.expanduser("~/models/olmoe_merged"))
    a = ap.parse_args()

    files = sorted(glob.glob(a.sessions, recursive=True))
    if not files:
        sys.exit(f"no session files match {a.sessions}")
    try:
        from tokenizers import Tokenizer
        tok = Tokenizer.from_file(os.path.join(a.model, "tokenizer.json"))
    except Exception as e:
        sys.exit(f"tokenizers lib needed (pip install tokenizers in .venv): {e}")

    lines, ntok, nfiles = [], 0, 0
    seen = set()
    import random
    random.Random(1972).shuffle(files)          # deterministic shuffle across projects
    for f in files:
        if nfiles >= a.limit or ntok >= a.max_tokens:
            break
        nfiles += 1
        per = 0
        for rec in iter_records(f):
            if per >= a.per_file or ntok >= a.max_tokens:
                break
            s = clean(text_of(rec))
            if len(s) < 40 or s in seen:        # dedup boilerplate (join msgs, tool banners)
                continue
            seen.add(s); per += 1
            ids = tok.encode(s, add_special_tokens=False).ids
            # chunk token-exactly so prefill counts match the report
            for i in range(0, len(ids), 900):
                chunk = ids[i:i+900]
                text = tok.decode(chunk)
                for j in range(0, len(text), a.line_bytes):
                    piece = text[j:j+a.line_bytes]
                    if piece.strip():
                        lines.append(piece)
                        ntok += len(tok.encode(piece, add_special_tokens=False).ids)
                if ntok >= a.max_tokens:
                    break
            if ntok >= a.max_tokens:
                break
    with open(a.out, "w") as fh:
        fh.write("\n".join(l.replace("\n", " ") for l in lines) + "\n")
    print(json.dumps({
        "session_files_used": nfiles,
        "lines": len(lines),
        "model_tokens": ntok,
        "est_prefill_minutes_at_4toks": round(ntok/4/60, 1),
        "out": os.path.abspath(a.out),
    }, indent=1))

if __name__ == "__main__":
    main()
