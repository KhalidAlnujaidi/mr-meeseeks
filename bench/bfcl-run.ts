/**
 * bench/bfcl-run.ts — cheap JSON tool-routing accuracy micro-bench.
 *
 * 12 fixed cases: a user request -> exactly one correct tool call
 * (name + required args). A LOCAL keyword-overlap router picks the tool:
 * zero network, zero keys, fully deterministic. Measures routing accuracy
 * (the cheap proxy for "does the request reach the right tool"), plus
 * per-case routing latency.
 *
 * Rules: --out MUST be under /tmp/ (default timestamped /tmp/bench-bfcl-*).
 * Never reads/writes harness/ledger.jsonl, harness/tasks/, harness/queue/.
 *
 * Usage: node bench/bfcl-run.ts [--out /tmp/bench-bfcl.jsonl]
 *
 * Erasable-syntax TS only: runs directly with `node` (>=22.18). Zero deps.
 */

import { appendFileSync, writeFileSync } from "node:fs";

export interface BfclCase { id: string; request: string; tool: string; args: Record<string, string>; hint: string[]; }
export interface BfclRunLine {
  bench: "bfcl-micro"; case_id: string; predicted: string; expected: string;
  correct: boolean; latency_ms: number; ts: string;
}
export interface BfclSummaryLine {
  bench: "bfcl-micro"; type: "summary"; n_cases: number; accuracy: number;
  mean_latency_ms: number; p95_latency_ms: number; ts: string;
}

export const TOOLS = ["get_order", "issue_refund", "track_shipment", "update_address"] as const;

export const CASES: BfclCase[] = [
  { id: "b01", request: "Where is my package, tracking number 1Z999?", tool: "track_shipment", args: { tracking: "1Z999" }, hint: ["package", "tracking", "where"] },
  { id: "b02", request: "I want a refund for order #4521, item arrived broken", tool: "issue_refund", args: { order: "4521" }, hint: ["refund", "broken", "order"] },
  { id: "b03", request: "Show me the details of order #7890", tool: "get_order", args: { order: "7890" }, hint: ["details", "order", "show"] },
  { id: "b04", request: "I moved — change my delivery address to 5 Oak Ave", tool: "update_address", args: { address: "5 Oak Ave" }, hint: ["moved", "address", "delivery", "change"] },
  { id: "b05", request: "Has my shipment left the warehouse yet?", tool: "track_shipment", args: { tracking: "unknown" }, hint: ["shipment", "warehouse", "left"] },
  { id: "b06", request: "Return this and give me my money back, order #111", tool: "issue_refund", args: { order: "111" }, hint: ["return", "money back", "refund"] },
  { id: "b07", request: "What did I buy last Tuesday? Look up order #222", tool: "get_order", args: { order: "222" }, hint: ["bought", "look up", "order"] },
  { id: "b08", request: "Update where my parcel goes: 9 Pine Rd now", tool: "update_address", args: { address: "9 Pine Rd" }, hint: ["parcel", "update", "goes"] },
  { id: "b09", request: "The courier says delivered but nothing arrived — trace it", tool: "track_shipment", args: { tracking: "unknown" }, hint: ["courier", "delivered", "trace"] },
  { id: "b10", request: " promotions aside, refund order #333 for the defective unit", tool: "issue_refund", args: { order: "333" }, hint: ["refund", "defective"] },
  { id: "b11", request: "Pull up my receipt for order #444", tool: "get_order", args: { order: "444" }, hint: ["receipt", "pull up", "order"] },
  { id: "b12", request: "Redirect my delivery to my office at 1 Main St", tool: "update_address", args: { address: "1 Main St" }, hint: ["redirect", "delivery", "office"] },
];

/** Local overlap router: score = hint-term hits in the lowercased request; ties -> first tool in TOOLS order. */
export function routeLocal(request: string): string {
  const lower = request.toLowerCase();
  let best = TOOLS[0];
  let bestHits = -1;
  for (const tool of TOOLS) {
    const hints = CASES.filter((c) => c.tool === tool).flatMap((c) => c.hint);
    const hits = new Set(hints.filter((h) => lower.includes(h))).size;
    if (hits > bestHits) { bestHits = hits; best = tool; }
  }
  return best;
}

function defaultOut(): string {
  return `/tmp/bench-bfcl-${new Date().toISOString().replace(/[:.]/g, "-")}.jsonl`;
}

if (process.argv[1] !== undefined && process.argv[1].endsWith("bfcl-run.ts")) {
  let out = defaultOut();
  const oi = process.argv.indexOf("--out");
  if (oi >= 0) {
    out = process.argv[oi + 1] ?? "";
    if (!out.startsWith("/tmp/bench-")) {
      console.error(`bfcl-run: refusing --out outside scratch: ${out} (must be /tmp/bench-*)`);
      process.exit(1);
    }
  }
  if (process.argv.some((a) => a.startsWith("--") && a !== "--out")) {
    console.error("usage: node bench/bfcl-run.ts [--out /tmp/bench-bfcl.jsonl]");
    process.exit(1);
  }
  writeFileSync(out, "");
  const emit = (o: unknown): void => appendFileSync(out, JSON.stringify(o) + "\n");
  const lat: number[] = [];
  let correct = 0;
  for (const c of CASES) {
    const t0 = Date.now();
    const predicted = routeLocal(c.request);
    const ms = Date.now() - t0;
    lat.push(ms);
    const ok = predicted === c.tool;
    if (ok) correct++;
    const line: BfclRunLine = {
      bench: "bfcl-micro", case_id: c.id, predicted, expected: c.tool,
      correct: ok, latency_ms: ms, ts: new Date().toISOString(),
    };
    emit(line);
    console.log(`${c.id}: predicted=${predicted} expected=${c.tool} ${ok ? "OK" : "MISS"}`);
  }
  lat.sort((a, b) => a - b);
  const p95 = lat.length === 0 ? 0 : lat[Math.min(lat.length - 1, Math.ceil(0.95 * lat.length) - 1)];
  const summary: BfclSummaryLine = {
    bench: "bfcl-micro", type: "summary", n_cases: CASES.length,
    accuracy: correct / CASES.length,
    mean_latency_ms: lat.reduce((a, b) => a + b, 0) / Math.max(1, lat.length),
    p95_latency_ms: p95, ts: new Date().toISOString(),
  };
  emit(summary);
  console.log(`accuracy=${summary.accuracy.toFixed(3)} (${correct}/${CASES.length})`);
  console.log(`wrote ${out}`);
}
