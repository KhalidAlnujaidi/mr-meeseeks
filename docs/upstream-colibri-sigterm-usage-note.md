# Upstream issue note (DRAFT — for JustVugg/colibri)

> Status: draft for review, not yet submitted. Facts below were observed
> on macOS 26.5.1 (Apple Silicon, arm64), colibri checkout a8f2ca6
> (v1.11.0), OLMoE-1B-7B int8 merged container, python3.12 gateway.

## Title

`coli serve`: expert history (.coli_usage) is not flushed when the server is stopped with SIGTERM

## Summary

`rt_save()` for the `COLI_USAGE` history runs only on the graceful exit
path after `serve_loop()` returns (`olmoe.c`, serve branch: `const char
*up = getenv("COLI_USAGE"); if (up && *up) rt_save(up, 0);`). Stopping
`coli serve` with SIGTERM — the normal way to end a background server
(`kill <pid>`, systemd/launchd stop, Ctrl-C on a wrapper script) — exits
without that save, so a long-running serving session contributes nothing
to the learned cache. The "colibri gets faster the more you use it"
property silently does not accumulate in serve mode, which is the mode
most agent harnesses and long-lived deployments use.

## Repro

```sh
# engine + gateway
COLI_USAGE=/path/model/.coli_usage ./coli serve --model /path/model \
  --model-id olmoe-colibri --port 8081 --host 127.0.0.1 &
SRV=$!

# generate traffic (3 chat completions, all HTTP 200)
for i in 1 2 3; do
  curl -s http://127.0.0.1:8081/v1/chat/completions \
    -H 'Content-Type: application/json' \
    -d '{"model":"olmoe-colibri","messages":[{"role":"user","content":"Count to three."}],"max_tokens":20}' \
    -o /dev/null
done

ls /path/model/.coli_usage   # -> No such file or directory

kill $SRV                    # SIGTERM (exit -15 observed on the child engine)
ls /path/model/.coli_usage   # -> still absent
```

Contrast: one-shot chat mode (`CHAT=1`, stdin EOF) exits gracefully and
DOES save — `[STATS] 2944 selections across 666 distinct experts ->
/path/model/.coli_usage` was observed with the identical model and
`COLI_USAGE` value. So routing data is being collected in serve mode;
only the flush is missing.

## Observed details

- Gateway log shows the requests served (`[api] 127.0.0.1 - "POST
  /v1/chat/completions HTTP/1.1" 200 -` x3), engine child exits with
  signal 15, no `[STATS]`/save line.
- The in-memory history IS loaded at startup when the file exists
  (`[USAGE] expert history: 2944 selections (...)` observed), confirming
  read-side works; the gap is write-side on signal exit.
- `USAGE_SAVE=0` semantics (read-only runs, #1039) are respected on the
  graceful path; the SIGTERM path bypasses both save and the opt-out —
  it simply never reaches `rt_save`.

## Suggested direction (not prescriptive)

A SIGTERM/SIGINT handler in the serve loop that performs one final
`rt_save(up, 0)` before exit (the temp-file + rename publish in
`route_trace.h` is already crash-safe, so a handler-triggered save
cannot corrupt an existing history). Alternatively/additionally, a
periodic save every N turns (the file is small: 4.9 KB for 666 records)
so long sessions are not all-or-nothing on shutdown.

## Why it matters downstream

Agent harnesses (e.g. our dsh-lite Budget-AGI stack) run `coli serve` as
a long-lived local engine and probe `.coli_usage` for warm-cache
telemetry (mtime recency + expert heat). With serve-mode SIGTERM exits —
the only exit a supervised server ever gets — the heat file either never
appears or goes stale after the first chat-mode run, so cache-warmth
telemetry and any future heat-aware scheduling silently degrade to
"no data" for server deployments.
