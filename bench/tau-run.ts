/**
 * bench/tau-run.ts — retail-subset τ-bench runner (conditions B + C, A + J gated).
 *
 * Measures harness ORCHESTRATION (atomicity + topology + daemon consult),
 * not leaf model quality: leaves are deterministic in-process stubs, so the
 * run makes ZERO model API calls. What varies across conditions is the
 * decision path:
 *
 *   B  Jev-OFF (forced isAtomicV2C) + daemon env scrubbed in-process
 *      (HARNESSD_SOCK/HARNESSD_TOKEN deleted, cache reset, restored after).
 *   C  Jev-OFF (forced isAtomicV2C) + daemon consulted when configured
 *      (consultDaemonSplit; daemon_reached recorded, null -> local fallback).
 *   A  Jev-ON (isAtomicAsync — LOCAL-ONLY since 3c591c8: always source=v2c,
 *      kept as the harness-default control) + daemon consulted. STUBBED
 *      behind TYPESAFE_API_KEY env presence: absent -> the whole condition is
 *      skipped cleanly (exit 0, skip line on stdout + JSONL).
 *   J  Calibrated-Jev atomicity, BENCH-LEVEL variant: calls
 *      checkAtomicityNoul() from harness/jev.ts DIRECTLY with
 *      JEV_DEFAULT_TIMEOUT_MS (2000ms, > the measured ~935ms round-trip),
 *      bypassing isAtomicAsync entirely. Same TYPESAFE_API_KEY gate as A.
 *      J answers "does Jev's own calibrated Noul verdict beat local v2c?"
 *      — it is NOT a harness behavior; the loop's hot path stays local-only.
 *      Per task it records jev_source ("jev" = real Noul answer, "v2c" =
 *      local fallback) plus the Jev atomic-check latency and confidence, so
 *      J-vs-A can be compared on the same tasks with the same stub leaves.
 *
 * Frozen pins: MODEL_WORKER / MODEL_REVIEWER labels + PROMPT_DIGEST (frozen
 * LEAF_HEADER + ROLE_PROMPTS text recorded on summary lines for provenance —
 * labels only, never called). Prompt digest is sha1 of that frozen text.
 *
 * Metrics per run line: pass (stub verdict), turns (1 atomic / 3 split),
 * cost_usd (estimated: stub tokens x stub price — a placeholder scale, not
 * a bill), orch_latency_ms (atomicity+topology decision time only).
 * Condition J adds: jev_source ("jev" | "v2c"), jev_confidence,
 * jev_atomic_latency_ms (the atomic-check round-trip alone).
 * Summary per condition: pass^1 (mean), pass^3 (all-k pass), mean turns,
 * $/task, orch p50/p95, fallback rate, topology distribution; J also adds
 * jev_source_dist, mean_jev_atomic_latency_ms, mean_jev_confidence.
 *
 * Rules: --out MUST be under /tmp/ (default timestamped /tmp/bench-tau-*).
 * Never reads/writes harness/ledger.jsonl, harness/tasks/, harness/queue/.
 * Both conditions reset the daemon ops cache so B/C ordering can't leak.
 *
 * Usage: node bench/tau-run.ts [--k 3] [--conditions B,C] [--out /tmp/x.jsonl]
 *        TYPESAFE_API_KEY=... node bench/tau-run.ts --conditions A,J
 *        TYPESAFE_API_KEY=... node bench/tau-run.ts --conditions A,B,C,J --k 3
 *
 * Erasable-syntax TS only: runs directly with `node` (>=22.18). Zero deps.
 */

import { appendFileSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import {
  isAtomicAsync, isAtomicV2C, consultDaemonSplit,
  daemonConfigured, resetDaemonOpsCache,
} from "../harness/loop.ts";
import {
  FALLBACK_TOPOLOGY, chooseTopology, checkAtomicityNoul,
  JEV_DEFAULT_TIMEOUT_MS, type Topology,
} from "../harness/jev.ts";

const HERE = join(dirname(fileURLToPath(import.meta.url)));

// --- frozen pins (labels only; the runner performs no model calls) ---
export const MODEL_WORKER = "stub-worker/bench-1.0";
export const MODEL_REVIEWER = "stub-reviewer/bench-1.0";
export const FROZEN_PROMPT_TEXT =
  "You are a leaf worker. You see ONLY your task below. Do exactly one thing." +
  "|researcher:Find facts. You answer ONE question with evidence." +
  "|engineer:Change code or run ONE command sequence toward the stated criterion." +
  "|reviewer:Verify ONE report against its ONE criterion. VERDICT: pass|fail.";
export const PROMPT_DIGEST = createHash("sha1").update(FROZEN_PROMPT_TEXT).digest("hex").slice(0, 12);

// --- stub cost model (estimated placeholder scale, NOT a billed price) ---
const STUB_INPUT_TOKENS = 400;
const STUB_OUTPUT_TOKENS = 120;
const STUB_PRICE_PER_1K_USD = 0.0002; // arbitrary bench-internal scale

// --- retail-subset task list (deterministic, in-memory; never touches harness/tasks) ---
export interface BenchTask { id: string; role: "researcher" | "engineer" | "reviewer"; criterion: string; expectPass: boolean; }
export const TASKS: BenchTask[] = [
  { id: "tau-r1", role: "researcher", criterion: "Find the refund policy for opened items under $50", expectPass: true },
  { id: "tau-r2", role: "researcher", criterion: "Find which shipping carriers deliver to PO boxes and quote the help page", expectPass: true },
  { id: "tau-e1", role: "engineer", criterion: "Fix the cart total rounding error in cart.ts", expectPass: true },
  { id: "tau-e2", role: "engineer", criterion: "Add a size-chart link to the product page template", expectPass: false },
  { id: "tau-e3", role: "engineer", criterion: "Migrate the checkout flow to the new tax API and update all callers and then verify each of the following", expectPass: false },
  { id: "tau-v1", role: "reviewer", criterion: "Confirm the return window reads 30 days", expectPass: true },
  { id: "tau-s1", role: "researcher", criterion: "Audit every return reason across all regions and document each of the following", expectPass: false },
  { id: "tau-s2", role: "engineer", criterion: "Render the order confirmation email", expectPass: true },
];

export type Condition = "A" | "B" | "C" | "J";

/** Conditions whose atomicity verdict is gated behind TYPESAFE_API_KEY. */
const KEYED_CONDITIONS: Condition[] = ["A", "J"];

export interface RunLine {
  bench: "tau-retail"; condition: Condition; task_id: string; repeat: number;
  atomic: boolean; atomic_source: "jev" | "v2c"; topology: Topology;
  topology_fallback: boolean; daemon_configured: boolean; daemon_reached: boolean;
  turns: number; pass: boolean; cost_usd: number; orch_latency_ms: number; ts: string;
  /** Condition J only: whether the Jev atomic check itself answered, or v2c fallback won. */
  jev_source?: "jev" | "v2c";
  /** Condition J only: Jev's verdict confidence (distance from the 0.5 boundary). */
  jev_confidence?: number;
  /** Condition J only: the atomic-check latency alone (Jev round-trip or ~0 for fallback). */
  jev_atomic_latency_ms?: number;
}

export interface SummaryLine {
  bench: "tau-retail"; type: "summary"; condition: Condition; k: number; n_tasks: number;
  pass1: number; pass3: number; mean_turns: number; cost_per_task_usd: number;
  orch_p50_ms: number; orch_p95_ms: number; fallback_rate: number;
  topology_dist: Record<string, number>; model_worker: string; model_reviewer: string;
  prompt_digest: string; daemon_configured: boolean; skipped?: boolean; ts: string;
  /** Condition J only: how many verdicts came from Jev vs local v2c fallback. */
  jev_source_dist?: Record<string, number>;
  /** Condition J only: mean Jev atomic-check latency in ms (fallback rows included). */
  mean_jev_atomic_latency_ms?: number;
  /** Condition J only: mean Jev confidence (0 for fallback rows). */
  mean_jev_confidence?: number;
}

/** Deterministic stub verdict: hash(task + repeat + condition) -> pass unless expectPass=false forces fail. */
function stubPass(task: BenchTask, repeat: number, cond: Condition): boolean {
  if (!task.expectPass) return false;
  const h = createHash("sha1").update(`${task.id}:${repeat}:${cond}`).digest();
  return h[0] >= 26; // ~90% pass for expectPass tasks (deterministic jitter)
}

function percentile(sorted: number[], p: number): number {
  if (sorted.length === 0) return 0;
  const i = Math.min(sorted.length - 1, Math.ceil((p / 100) * sorted.length) - 1);
  return sorted[Math.max(0, i)];
}

function defaultOut(): string {
  return `/tmp/bench-tau-${new Date().toISOString().replace(/[:.]/g, "-")}.jsonl`;
}

/**
 * Refuse any --out outside /tmp/bench-*. The path is RESOLVED first so a
 * traversal form such as /tmp/bench-evil/../harness/ledger.jsonl cannot ride
 * the "/tmp/bench-" prefix into prod state — without normalization the raw
 * startsWith check passed and only failed later on a missing parent dir.
 */
function requireScratchOut(p: string): string {
  const resolved = resolve(p);
  if (!resolved.startsWith("/tmp/bench-")) {
    throw new Error(`refusing --out outside scratch: ${p} (resolves to ${resolved}; must be /tmp/bench-*)`);
  }
  return resolved;
}

function parseArgs(argv: string[]): { k: number; conditions: Condition[]; out: string } {
  let k = 3;
  let conditions: Condition[] = ["B", "C"];
  let out = defaultOut();
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--k") { k = Number(argv[++i]); if (!Number.isInteger(k) || k < 1 || k > 9) throw new Error("--k must be an integer 1..9"); }
    else if (argv[i] === "--conditions") {
      const cs = (argv[++i] ?? "").split(",").map((s) => s.trim().toUpperCase());
      const valid = (c: string): c is Condition => c === "A" || c === "B" || c === "C" || c === "J";
      if (cs.length === 0 || !cs.every(valid)) throw new Error("--conditions must be a subset of A,B,C,J");
      conditions = cs;
    }
    else if (argv[i] === "--out") { out = requireScratchOut(argv[++i] ?? ""); }
    else throw new Error(`unknown arg: ${argv[i]}`);
  }
  return { k, conditions, out };
}

// Daemon env scrub for condition B (deleted in-process, restored after).
function scrubDaemonEnv(): Record<string, string | undefined> {
  const saved = { HARNESSD_SOCK: process.env["HARNESSD_SOCK"], HARNESSD_TOKEN: process.env["HARNESSD_TOKEN"] };
  delete process.env["HARNESSD_SOCK"];
  delete process.env["HARNESSD_TOKEN"];
  resetDaemonOpsCache();
  return saved;
}
function restoreDaemonEnv(saved: Record<string, string | undefined>): void {
  for (const k of ["HARNESSD_SOCK", "HARNESSD_TOKEN"] as const) {
    if (saved[k] === undefined) delete process.env[k];
    else process.env[k] = saved[k];
  }
  resetDaemonOpsCache();
}

async function runCondition(cond: Condition, k: number, emit: (o: unknown) => void): Promise<SummaryLine> {
  const ts0 = new Date().toISOString();
  // Keyed-condition gate (A + J): stubbed behind TYPESAFE_API_KEY presence.
  if (KEYED_CONDITIONS.includes(cond) && !process.env["TYPESAFE_API_KEY"]) {
    console.log(`condition ${cond} skipped: TYPESAFE_API_KEY absent (Jev-backed condition, nothing to run)`);
    const skip: SummaryLine = {
      bench: "tau-retail", type: "summary", condition: cond, k, n_tasks: TASKS.length,
      pass1: 0, pass3: 0, mean_turns: 0, cost_per_task_usd: 0,
      orch_p50_ms: 0, orch_p95_ms: 0, fallback_rate: 0, topology_dist: {},
      model_worker: MODEL_WORKER, model_reviewer: MODEL_REVIEWER,
      prompt_digest: PROMPT_DIGEST, daemon_configured: daemonConfigured(), skipped: true, ts: ts0,
    };
    emit(skip);
    return skip;
  }

  let restore: (() => void) | null = null;
  if (cond === "B") {
    const saved = scrubDaemonEnv();
    restore = () => restoreDaemonEnv(saved);
  } else {
    resetDaemonOpsCache(); // C/A: fresh consult, no leak from B ordering
  }
  try {
    const perTaskPass: Record<string, boolean[]> = {};
    const runs: RunLine[] = [];
    const orchLat: number[] = [];
    let fallbacks = 0;
    const topoDist: Record<string, number> = {};
    let totalTurns = 0;
    let totalCost = 0;
    let jevSum = 0; // J only: sum of per-row Jev atomic-check latency
    let jevConfSum = 0; // J only: sum of per-row Jev confidence
    const jevSrcDist: Record<string, number> = {}; // J only: jev vs v2c

    for (const task of TASKS) {
      perTaskPass[task.id] = [];
      for (let r = 1; r <= k; r++) {
        const t0 = Date.now();
        // Atomicity: A = harness default (isAtomicAsync — LOCAL-ONLY since
        // 3c591c8: always v2c, no Jev call); J = calibrated Jev called
        // DIRECTLY at the generous JEV_DEFAULT_TIMEOUT_MS (bypasses
        // isAtomicAsync, bench-level variant only); B/C = forced local v2c
        // (isAtomicV2C), Jev never consulted.
        let atomic: boolean;
        let atomicSource: "jev" | "v2c";
        let atomicFallback: boolean;
        let jevConfidence: number | undefined;
        let jevAtomicLatencyMs: number | undefined;
        if (cond === "A") {
          const a = await isAtomicAsync(task.criterion);
          atomic = a.atomic; atomicSource = a.source;
          atomicFallback = a.source === "v2c"; // Jev-first fell back to local
        } else if (cond === "J") {
          // checkAtomicityNoul default is JEV_ATOMIC_BUDGET_MS (150ms), which
          // the measured ~935ms round-trip cannot meet — pass the generous
          // non-critical-path budget explicitly, exactly as loop.ts documents.
          const j = await checkAtomicityNoul(task.criterion, { timeoutMs: JEV_DEFAULT_TIMEOUT_MS });
          atomicSource = j.fallback ? "v2c" : "jev";
          // Fallback verdict is local v2c — same answer the harness hot path
          // gives — so J stays comparable to A/B/C even with no Jev reachable.
          atomic = j.fallback ? isAtomicV2C(task.criterion) : j.atomic;
          atomicFallback = !!j.fallback;
          jevConfidence = j.confidence;
          jevAtomicLatencyMs = j.latencyMs;
        } else {
          atomic = isAtomicV2C(task.criterion);
          atomicSource = "v2c"; atomicFallback = true; // Jev-OFF by design
        }
        // Topology:Je v Choice when key allows, else strict-validated fallback.
        const topo = await chooseTopology(task.criterion);
        const orchMs = Date.now() - t0;
        orchLat.push(orchMs);
        const fb = atomicFallback || !!topo.fallback;
        if (fb) fallbacks++;
        topoDist[topo.topology] = (topoDist[topo.topology] ?? 0) + 1;

        // Daemon consult for A/C (advisory; null = local fallback path).
        // For B the env is scrubbed so consultDaemonSplit -> null by construction.
        let daemonReached = false;
        if (cond !== "B" && daemonConfigured()) {
          const verdict = await consultDaemonSplit({
            task_id: task.id, parent_depth: 0, breadth: atomic ? 1 : 2, resplit_count: 0, models: [],
          });
          daemonReached = verdict !== null;
        }

        const pass = stubPass(task, r, cond);
        const turns = atomic ? 1 : 3; // split path: claim + 2 children + verify
        const cost = ((STUB_INPUT_TOKENS + STUB_OUTPUT_TOKENS) / 1000) * STUB_PRICE_PER_1K_USD;
        totalTurns += turns;
        totalCost += cost;
        perTaskPass[task.id]!.push(pass);
        if (cond === "J") {
          jevSum += jevAtomicLatencyMs ?? 0;
          jevConfSum += jevConfidence ?? 0;
          jevSrcDist[atomicSource] = (jevSrcDist[atomicSource] ?? 0) + 1;
        }

        const line: RunLine = {
          bench: "tau-retail", condition: cond, task_id: task.id, repeat: r,
          atomic, atomic_source: atomicSource, topology: topo.topology,
          topology_fallback: !!topo.fallback, daemon_configured: daemonConfigured(),
          daemon_reached: daemonReached, turns, pass, cost_usd: cost,
          orch_latency_ms: orchMs, ts: new Date().toISOString(),
          // J-only fields: absent on A/B/C rows, so their shape is unchanged.
          ...(cond === "J" ? {
            jev_source: atomicSource, jev_confidence: jevConfidence ?? 0,
            jev_atomic_latency_ms: jevAtomicLatencyMs ?? 0,
          } : {}),
        };
        runs.push(line);
        emit(line);
      }
    }

    const n = TASKS.length * k;
    const pass1 = runs.filter((x) => x.pass).length / n;
    const pass3 = TASKS.filter((t) => (perTaskPass[t.id] ?? []).every(Boolean)).length / TASKS.length;
    orchLat.sort((a, b) => a - b);
    const summary: SummaryLine = {
      bench: "tau-retail", type: "summary", condition: cond, k, n_tasks: TASKS.length,
      pass1, pass3, mean_turns: totalTurns / n, cost_per_task_usd: totalCost / n,
      orch_p50_ms: percentile(orchLat, 50), orch_p95_ms: percentile(orchLat, 95),
      fallback_rate: fallbacks / n, topology_dist: topoDist,
      model_worker: MODEL_WORKER, model_reviewer: MODEL_REVIEWER,
      prompt_digest: PROMPT_DIGEST, daemon_configured: daemonConfigured(), ts: new Date().toISOString(),
      // J-only fields: absent on A/B/C summaries.
      ...(cond === "J" ? {
        jev_source_dist: jevSrcDist,
        mean_jev_atomic_latency_ms: jevSum / n,
        mean_jev_confidence: jevConfSum / n,
      } : {}),
    };
    if (topoDist[FALLBACK_TOPOLOGY] === undefined) topoDist[FALLBACK_TOPOLOGY] = 0;
    emit(summary);
    return summary;
  } finally {
    restore?.();
  }
}

function usage(): never {
  console.error("usage: node bench/tau-run.ts [--k 3] [--conditions B,C] [--out /tmp/bench-tau.jsonl]");
  console.error("  --conditions: subset of A,B,C,J (A and J additionally require TYPESAFE_API_KEY)");
  process.exit(1);
}

if (process.argv[1] !== undefined && process.argv[1].endsWith("tau-run.ts")) {
  let args: { k: number; conditions: Condition[]; out: string };
  try {
    args = parseArgs(process.argv.slice(2));
  } catch (e) {
    console.error(`tau-run: ${(e as Error).message}`);
    usage();
  }
  writeFileSync(args.out, "");
  const emit = (o: unknown): void => appendFileSync(args.out, JSON.stringify(o) + "\n");
  for (const c of args.conditions) {
    const s = await runCondition(c, args.k, emit);
    if (s.skipped) console.log(`condition ${c}: skipped`);
    else {
      console.log(`condition ${c}: pass^1=${s.pass1.toFixed(3)} pass^3=${s.pass3.toFixed(3)} ` +
        `turns=${s.mean_turns.toFixed(2)} $/task=${s.cost_per_task_usd.toExponential(2)} ` +
        `orch_p50=${s.orch_p50_ms}ms p95=${s.orch_p95_ms}ms fallback=${s.fallback_rate.toFixed(2)}`);
    }
  }
  console.log(`wrote ${args.out}`);
}
