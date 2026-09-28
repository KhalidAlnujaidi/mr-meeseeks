#!/usr/bin/env python3
"""Hash the manifest evidence: raw speed log, both containers, quality files."""
import glob, hashlib, json, os

def sha(paths):
    h = hashlib.sha256()
    for p in sorted(paths):
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 22), b""):
                h.update(chunk)
    return h.hexdigest()

BASE = os.path.expanduser("~/models/olmoe_merged")
HERE = os.path.dirname(os.path.abspath(__file__))
out = {
    "evidence_paired_speed_raw": sha([f"{HERE}/paired_speed_warm.log"]),
    "container_baseline": sha(glob.glob(f"{BASE}/*.safetensors") + glob.glob(f"{BASE}/*.json")),
    "container_tier220": sha(glob.glob("/tmp/olmoe_tier220/*.safetensors")
                             + glob.glob("/tmp/olmoe_tier220/*.json")),
    "quality_log_tier220": sha([f"{HERE}/tier_eval_results.json"]),
}
json.dump(out, open(f"{HERE}/manifest_evidence_hashes.json", "w"), indent=1)
for k, v in out.items(): print(k, v)
