/**
 * bench/report.ts — bench JSONL results -> markdown summary.
 *
 * Reads one JSONL file produced by bench/tau-run.ts or bench/bfcl-run.ts
 * (run lines + type:"summary" lines) and prints a markdown report to stdout,
 * or to --out (which MUST be under /tmp/). Includes cost-per-reliability-point:
 * USD per percentage point of pass^3 (tau-retail) — i.e. how much estimated
 * stub cost buys each point of all-k reliability. bfcl-micro carries no cost
 * model, so its section reports accuracy + latency only.
 *
 * Rule: reads the given JSONL, never touches harness/ledger.jsonl. The input
 * path itself should be scratch (/tmp/bench-*); anything else gets a warning
 * on stderr but still reads (reporter never writes prod state).
 *
 * Usage: node bench/report.ts <results.jsonl> [--out /tmp/bench-summary.md]
 *
 * Erasable-syntax TS only: runs directly with `node` (>=22.18). Zero deps.
 */

import { readFileSync, writeFileSync } from "node:fs";

interface TauSummary {
  bench?: string; type?: string; condition?: string; k?: number; n_tasks?: number;
  pass1?: number; pass3?: number; mean_turns?: number; cost_per_task_usd?: number;
  orch_p50_ms?: number; orch_p95_ms?: number; fallback_rate?: number;
  topology_dist?: Record<string, number>; model_worker?: string; model_reviewer?: string;
  prompt_digest?: string; daemon_configured?: boolean; skipped?: boolean;
}
interface BfclSummary {
  bench?: string; type?: string; n_cases?: number; accuracy?: number;
  mean_latency_ms?: number; p95_latency_ms?: number;
}

const f3 = (n: unknown): string => typeof n === "number" ? n.toFixed(3) : "n/a";
const f2 = (n: unknown): string => typeof n === "number" ? n.toFixed(2) : "n/a";
const usd = (n: unknown): string => typeof n === "number" ? `$${n.toExponential(2)}` : "n/a";

/** USD per percentage point of pass^3. null when pass3 is 0 (infinite) or inputs missing. */
export function costPerReliabilityPoint(costPerTask: number, pass3: number): number | null {
  if (!Number.isFinite(costPerTask) || !Number.isFinite(pass3) || pass3 <= 0) return null;
  return costPerTask / (pass3 * 100);
}

function tauSection(s: TauSummary): string[] {
  const L: string[] = [];
  const skipped = !!s.skipped;
  L.push(`### Condition ${s.condition ?? "?"}${skipped ? " — SKIPPED" : ""}`);
  if (skipped) {
    L.push("");
    L.push("- skipped (condition A requires TYPESAFE_API_KEY; absent at run time)");
    L.push("");
    return L;
  }
  const cprp = costPerReliabilityPoint(s.cost_per_task_usd ?? NaN, s.pass3 ?? NaN);
  const topo = s.topology_dist ?? {};
  const topoStr = Object.keys(topo).sort().map((k) => `${k}=${topo[k]}`).join(", ") || "n/a";
  L.push("");
  L.push(`- tasks=${s.n_tasks ?? "n/a"} k=${s.k ?? "n/a"} models=${s.model_worker ?? "?"} / ${s.model_reviewer ?? "?"} prompt=${s.prompt_digest ?? "?"} daemon_configured=${s.daemon_configured ?? "n/a"}`);
  L.push(`- pass^1=${f3(s.pass1)} pass^3=${f3(s.pass3)} mean_turns=${f2(s.mean_turns)}`);
  L.push(`- $/task=${usd(s.cost_per_task_usd)} **cost-per-reliability-point=${cprp === null ? "n/a (pass^3 = 0)" : `$${cprp.toExponential(2)}/pt`}**`);
  L.push(`- orch_latency p50=${s.orch_p50_ms ?? "n/a"}ms p95=${s.orch_p95_ms ?? "n/a"}ms fallback_rate=${f2(s.fallback_rate)}`);
  L.push(`- topology: ${topoStr}`);
  L.push("");
  return L;
}

function buildReport(path: string, lines: Record<string, unknown>[]): string {
  const out: string[] = [];
  out.push(`# Bench report — \`${path}\``);
  out.push("");
  out.push(`_generated ${new Date().toISOString()}_`);
  out.push("");
  const tau = lines.filter((l) => l["bench"] === "tau-retail" && l["type"] === "summary") as unknown as TauSummary[];
  const bfcl = lines.filter((l) => l["bench"] === "bfcl-micro" && l["type"] === "summary") as unknown as BfclSummary[];
  if (tau.length > 0) {
    out.push("## tau-retail (orchestration subset)");
    out.push("");
    for (const s of tau) out.push(...tauSection(s));
  }
  if (bfcl.length > 0) {
    out.push("## bfcl-micro (tool-routing accuracy)");
    out.push("");
    for (const s of bfcl) {
      out.push(`- cases=${s.n_cases ?? "n/a"} accuracy=${f3(s.accuracy)} mean_latency=${f2(s.mean_latency_ms)}ms p95=${f2(s.p95_latency_ms)}ms`);
      out.push("");
    }
  }
  if (tau.length === 0 && bfcl.length === 0) {
    out.push("_no summary lines found (empty or run-lines-only input)_");
    out.push("");
  }
  return out.join("\n");
}

if (process.argv[1] !== undefined && process.argv[1].endsWith("report.ts")) {
  const [input, ...rest] = process.argv.slice(2);
  if (!input || input.startsWith("--")) {
    console.error("usage: node bench/report.ts <results.jsonl> [--out /tmp/bench-summary.md]");
    process.exit(1);
  }
  let out: string | null = null;
  const oi = rest.indexOf("--out");
  if (oi >= 0) {
    out = rest[oi + 1] ?? "";
    if (!out.startsWith("/tmp/bench-")) {
      console.error(`report: refusing --out outside scratch: ${out} (must be /tmp/bench-*)`);
      process.exit(1);
    }
  }
  if (!input.startsWith("/tmp/")) {
    console.error(`report: warning: reading non-scratch input ${input} (read-only; prod state untouched)`);
  }
  let raw: string;
  try {
    raw = readFileSync(input, "utf8");
  } catch {
    console.error(`report: cannot read ${input}`);
    process.exit(1);
  }
  const lines: Record<string, unknown>[] = [];
  for (const ln of raw.split("\n")) {
    const t = ln.trim();
    if (!t) continue;
    try {
      lines.push(JSON.parse(t) as Record<string, unknown>);
    } catch {
      console.error("report: warning: skipping malformed JSONL line");
    }
  }
  const md = buildReport(input, lines);
  if (out) { writeFileSync(out, md); console.log(`wrote ${out}`); }
  else console.log(md);
}
