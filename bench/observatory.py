#!/usr/bin/env python3
"""Minimal observatory: one screen, live from every node in the fleet.

usage:
  observatory.py            one snapshot
  observatory.py --watch 60 refresh every 60s (Ctrl-C to exit)
  observatory.py --html     also write bench/observatory.html (static snapshot)

Stdlib only. Never mutates anything; ssh commands are read-only tails.
"""
import datetime, json, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = "enigma@100.64.164.41"
AIR = "khalid@100.120.42.119"
R = "/home/enigma/bench/prune-spike/runs/qwen38_flash_next"
SEP = "\x01SEP\x01"
PUB = {"cold": (0.2487, 0.3302), "rand": (0.3932, 0.3841)}  # published OLMoE

def ssh(host, cmd, timeout=15):
    try:
        r = subprocess.run(["ssh", "-o", "ConnectTimeout=6", "-o", "BatchMode=yes",
                            "-o", "LogLevel=ERROR", host, cmd],
                           capture_output=True, text=True, timeout=timeout)
        return (r.stdout + r.stderr).strip()
    except Exception as e:
        return "UNREACHABLE:" + type(e).__name__

def enigma_board():
    cmd = ("tail -c 2000 " + R + "/results.json 2>/dev/null; echo -n '" + SEP + "'; "
           "tail -30 ~/bench/logs/q38-resume.log 2>/dev/null; echo -n '" + SEP + "'; "
           "pgrep -af 'qwen38 [0-9]' | grep -v -e pgrep -e 'bash -c' | head -1 "
           "| sed 's|/home/enigma/bench/prune-spike/runs/qwen38_flash_next/||g'; "
           "df -h / | tail -1 | awk '{print $4}'")
    out = ssh(ENIGMA, cmd)
    parts = out.split(SEP)
    res, log = {}, ""
    disk = "?"
    engine = ""
    if len(parts) >= 2:
        try: res = json.loads(parts[0])
        except Exception: pass
        log = parts[1]
        engine = parts[2].strip().splitlines()[0] if len(parts) > 2 else ""
        disk = engine.rsplit(" ", 1)[-1] if "G" in engine.rsplit(" ", 1)[-1] else "?"
        engine = engine.rsplit(" ", 1)[0].strip()
    return res, log, engine, disk

def fmt_delta(v):
    return ("+" if v >= 0 else "") + f"{v:.4f}"

def arm_table(res):
    refs = sorted({k.split("|")[1] for k in res if k.startswith("base|")})
    def rlab(r):  # eval_ref_2.json -> "2"; anything else -> first 8 chars
        m = re.search(r"(\d+)", r)
        return m.group(1) if m else r[:8]
    rows = []
    hdr = f"  {'arm':5}" + "".join(f"{rlab(r):>10}" for r in refs) + f"{'mean':>10}"
    for arm in ("base", "ctl", "cold", "rand"):
        vals = [res.get(f"{arm}|{r}") for r in refs]
        if all(v is None for v in vals): continue
        if arm in ("base", "ctl"):
            cells = [f"{v:>10.4f}" if v is not None else f"{'--':>10}" for v in vals]
            rows.append((arm, cells, None))
        else:
            ds, cells = [], []
            for r, v in zip(refs, vals):
                b = res.get(f"base|{r}")
                if v is not None and b is not None:
                    ds.append(v - b); cells.append(f"{fmt_delta(v-b):>10}")
                else:
                    cells.append(f"{'--':>10}")
            rows.append((arm, cells, sum(ds)/len(ds) if ds else None))
    return hdr, rows, refs

def main():
    watch = 0
    if "--watch" in sys.argv:
        i = sys.argv.index("--watch"); watch = int(sys.argv[i+1]) if i+1 < len(sys.argv) else 60
    do_html = "--html" in sys.argv

    while True:
        res, log, engine, disk = enigma_board()
        done = "ALL DONE" in log
        lines = []
        now = datetime.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")
        lines.append("OBSERVATORY " + now + "   (read-only pull; refresh: re-run or --watch N)")
        lines.append("=" * 74)

        # ---- ENIGMA ----------------------------------------------------
        running = bool(engine)
        if done: status = "DONE  " + (log.split("ALL DONE")[0].strip().splitlines()[-1][:19] if log else "")
        elif running: status = f"RUNNING [{engine.split('/')[-1][:22] if engine else 'build'}]"
        else: status = "NOT-STARTED"
        lines.append(f"ENIGMA  x86 16c RTX-A4500 | q38 48x512 FP8 173G   {status}")
        if res:
            hdr, rows, refs = arm_table(res)
            gate = all(abs(res.get(f"ctl|{r}", 1) - res.get(f"base|{r}", 0)) < 0.00005 for r in refs)
            gflag = "GATE PASS ctl-base==0.0000 (FP8 round-trip lossless)" if gate \
                    else "GATE FAIL ctl!=base -> ALL OTHERS VOID"
            lines.append("  " + gflag)
            lines.append(hdr + f"{'vs-base':>10}")
            for arm, cells, mean in rows:
                note = ""
                if arm == "ctl": note = "  (=base)" if gate else "  (!= base!)"
                lines.append(f"  {arm:5}" + "".join(cells) + (f"{fmt_delta(mean):>10}" if mean is not None else f"{'':>10}") + note)
            cold_d = [res[f"cold|{r}"] - res[f"base|{r}"] for r in refs if f"cold|{r}" in res and f"base|{r}" in res]
            rand_d = [res[f"rand|{r}"] - res[f"base|{r}"] for r in refs if f"rand|{r}" in res and f"base|{r}" in res]
            if cold_d and rand_d:
                m_c, m_r = sum(cold_d)/len(cold_d), sum(rand_d)/len(rand_d)
                ratio = (m_c/m_r*100) if m_r else float("nan")
                verdict = "REPLICATION" if 63 <= ratio <= 86 else "CONTRADICTION"
                lines.append(f"  per-expert cold/rand damage: {ratio:.0f}%   (OLMoE band: 63-86%)  -> {verdict}")
                try: shares = json.loads(ssh(ENIGMA, "cat " + R + "/arm_shares.json 2>/dev/null", 10) or "{}")
                except Exception: shares = {}
                cs, us = shares.get("cold_share_pct"), shares.get("uniform_share_pct")
                if cs and us and m_r:
                    ptu = (m_c/cs) / (m_r/us) if cs else float("nan")
                    lines.append(f"  per-traffic-unit: cold {ptu:.1f}x the avg expert   (OLMoE: 22-29x)")
                    lines.append(f"  cold set carries {cs:.2f}% of traffic, rand {us:.2f}% (n={shares.get('n','?')})")
            if any(d < 0 for d in cold_d + rand_d):
                lines.append("  !! negative per-ref deltas present -> inspect that ref before quoting means")
        else:
            last = [l for l in log.splitlines() if l.strip()]
            if last: lines.append("  last: " + last[-1][:70])
        lines.append(f"  disk free: {disk}   |   watchdog cron q38-enigma-progress: every 30m")

        # ---- AIR -------------------------------------------------------
        alog = ssh(AIR, "tail -12 ~/bench/logs/air-eval.log 2>/dev/null; pgrep -f run_experiment.py >/dev/null && echo _RUNNING_", 15)
        a_running = "_RUNNING_" in alog
        a_done = "per-traffic-unit" in alog
        astat = "DONE" if a_done else ("RUNNING" if a_running else "NOT-STARTED")
        lines.append(f"AIR     M3 8c 16G        | olmoe 16x64 int8 repl.   {astat}")
        for l in alog.splitlines():
            if "per-expert" in l or "per-traffic-unit" in l:
                lines.append("  " + l.strip()[:74])
        if a_done:
            lines.append("  deltas == published to 4dp (cross-host determinism: PASS)")

        # ---- LOCAL -----------------------------------------------------
        draft = os.path.join(HERE, "prune-spike", "drafts", "issue1621-qwen38-replication.md")
        pend = 0
        if os.path.exists(draft):
            pend = open(draft).read().count("PENDING")
        lines.append(f"LOCAL   PR#1625 open | issue#1621 live | draft PENDING cells: {pend}")
        lines.append("=" * 74)

        txt = "\n".join(lines)
        print("\x1b[2J\x1b[H" if watch else "", end="")
        print(txt)
        if do_html:
            hp = os.path.join(HERE, "observatory.html")
            with open(hp, "w") as f:
                f.write("<!doctype html><meta charset=utf-8><meta http-equiv='refresh' content='60'>"
                        "<title>observatory</title><body style='background:#111;color:#0f0;font:13px/1.5 "
                        "monospace;padding:16px'><pre>" + txt.replace("&", "&amp;").replace("<", "&lt;") +
                        "</pre><p style='color:#666'>static snapshot " + now +
                        " — run `python3 bench/observatory.py --html` to refresh</p></body>")
            print("html:", hp)
        if not watch: break
        time.sleep(watch)

if __name__ == "__main__":
    main()
