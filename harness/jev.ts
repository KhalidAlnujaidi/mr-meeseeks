/**
 * harness/jev.ts — lean Jev SystemOne task helpers (t3).
 *
 * EXTENDS harness/typesafe-judge.ts, does not duplicate it: reuses its
 * endpoint/model constants; adds the per-task primitives the loop needs
 * with their own minimal question shapes (one small call each, global
 * fetch, zero deps):
 *
 *   checkAtomicityNoul(text)  single Noul, tight <150ms budget.
 *   chooseTopology(task)       single Choice, strict 3-enum validation.
 *   verifyGate(output, crit)   single Noul, 0.85 pass / 0.5 escalate bands.
 *   batchPrune(ctx, items)     ONE call, N Noul questions (forward-nudge).
 *
 * Key from TYPESAFE_API_KEY env only — never logged, never persisted.
 * Absent key / timeout / transport error / malformed answer all degrade
 * to {fallback:true} so callers route to the local v2c hybrid and never block.
 *
 * Erasable-syntax TS only: runs directly with `node` (>=22.18 type stripping).
 * Zero deps: uses global fetch (Node >=18).
 */

import { TYPESAFE_ENDPOINT, TYPESAFE_MODEL, resolveModel } from "./typesafe-judge.ts";

/** Jev Noul budget for the claim-time atomicity path (loop tries Jev, else v2c). */
export const JEV_ATOMIC_BUDGET_MS = 150;
/** Default timeout for the non-critical paths (topology / verify / prune). */
export const JEV_DEFAULT_TIMEOUT_MS = 2000;
/** verifyGate bands: pass >= 0.85, escalate < 0.5 (mirrors routeJudge bands). */
export const VERIFY_PASS_THRESHOLD = 0.85;
export const VERIFY_ESCALATE_THRESHOLD = 0.5;
/** batchPrune default: prune an item when P(stale) >= 0.7. */
export const PRUNE_THRESHOLD = 0.7;

export type Topology = "RESEARCHER_ONLY" | "ENGINEER_REVIEWER" | "FULL_SWARM";
export const TOPOLOGIES: Topology[] = ["RESEARCHER_ONLY", "ENGINEER_REVIEWER", "FULL_SWARM"];

/** Strict enum validation: true iff v is exactly one of the three topologies. */
export function isTopology(v: string): v is Topology {
  return (TOPOLOGIES as string[]).includes(v);
}

export interface JevCallOpts {
  env?: Record<string, string | undefined>;
  endpoint?: string;
  model?: string;
  timeoutMs?: number;
}

interface JevCallResult {
  answers: Record<string, {
    noul?: number; choice?: string;
    confidence?: number; probabilities?: Record<string, number>;
  }>;
  latencyMs: number;
}

/**
 * systemOne(): one lean POST. Returns null on missing key / timeout /
 * non-2xx / bad JSON (caller maps null → {fallback:true}).
 * Throws only on caller misuse (empty questions map).
 */
async function systemOne(
  state: string,
  questions: Record<string, unknown>,
  opts: JevCallOpts = {},
): Promise<JevCallResult | null> {
  if (Object.keys(questions).length === 0) throw new Error("jev: questions required");
  const env = opts.env ?? process.env;
  const key = env["TYPESAFE_API_KEY"];
  if (!key) return null; // no key configured: degrade, don't block
  const t0 = Date.now();
  let res: Response;
  try {
    res = await fetch(opts.endpoint ?? TYPESAFE_ENDPOINT, {
      method: "POST",
      headers: { Authorization: `Bearer ${key}`, "Content-Type": "application/json" },
      body: JSON.stringify({ state, model: opts.model ?? resolveModel(env), questions }),
      signal: AbortSignal.timeout(opts.timeoutMs ?? JEV_DEFAULT_TIMEOUT_MS),
    });
  } catch {
    return null;
  }
  if (!res.ok) return null;
  let data: { answers?: JevCallResult["answers"] };
  try {
    data = (await res.json()) as typeof data;
  } catch {
    return null;
  }
  if (!data.answers || typeof data.answers !== "object") return null;
  return { answers: data.answers, latencyMs: Date.now() - t0 };
}

export interface AtomicCheck {
  atomic: boolean;
  /**
   * Confidence IN THE VERDICT: p when atomic, 1-p when split. Noul
   * answers carry NO confidence field (API fact) — so this is the
   * calibrated Noul probability folded into a verdict confidence
   * (distance from the 0.5 decision boundary), documented explicitly
   * rather than read off a field that does not exist.
   */
  confidence: number;
  latencyMs: number;
  fallback?: true;
}

/**
 * checkAtomicityNoul(): "this task fits one tool call and one check?"
 * Single Noul with the claim-path budget (default 150ms). Null/malformed
 * → {fallback:true}; the loop then uses isAtomicV2C.
 */
export async function checkAtomicityNoul(
  text: string,
  opts: JevCallOpts = {},
): Promise<AtomicCheck> {
  if (!text || !text.trim()) throw new Error("checkAtomicityNoul: text required");
  const r = await systemOne(
    `Task: ${text}`,
    {
      atomic: {
        type: "noul",
        instructions: "This task fits one tool call and one verification check",
        criteria: {
          true: "One tool call produces the result and one check verifies it",
          false: "Needs decomposition into smaller sub-tasks first",
        },
      },
    },
    { ...opts, timeoutMs: opts.timeoutMs ?? JEV_ATOMIC_BUDGET_MS },
  );
  if (!r) return { atomic: false, confidence: 0, latencyMs: 0, fallback: true };
  const p = r.answers["atomic"]?.noul;
  if (typeof p !== "number") return { atomic: false, confidence: 0, latencyMs: r.latencyMs, fallback: true };
  const atomic = p >= 0.5;
  return { atomic, confidence: atomic ? p : 1 - p, latencyMs: r.latencyMs };
}

export interface TopologyChoice {
  topology: Topology;
  confidence: number;
  probabilities: Record<string, number>;
  latencyMs: number;
  fallback?: true;
}

/** Fallback topology: verified pair — safest default when Jev is unreachable. */
export const FALLBACK_TOPOLOGY: Topology = "ENGINEER_REVIEWER";

/**
 * chooseTopology(): single 3-option Choice. The answer is STRICTLY
 * validated against the Topology enum — any other string (or missing
 * choice) → {fallback:true} with FALLBACK_TOPOLOGY, never a cast.
 */
export async function chooseTopology(
  task: string,
  opts: JevCallOpts = {},
): Promise<TopologyChoice> {
  if (!task || !task.trim()) throw new Error("chooseTopology: task required");
  const r = await systemOne(
    `Task: ${task}`,
    {
      topology: {
        type: "choice",
        instructions: "Which team topology should handle this task",
        criteria: {
          RESEARCHER_ONLY: "Fact-finding with evidence, no code changes needed",
          ENGINEER_REVIEWER: "One scoped code change plus independent verification",
          FULL_SWARM: "Multi-part work needing researcher, engineer and reviewer",
        },
      },
    },
    opts,
  );
  if (!r) return { topology: FALLBACK_TOPOLOGY, confidence: 0, probabilities: {}, latencyMs: 0, fallback: true };
  const a = r.answers["topology"];
  if (!a || typeof a.choice !== "string" || !isTopology(a.choice)) {
    return { topology: FALLBACK_TOPOLOGY, confidence: 0, probabilities: {}, latencyMs: r.latencyMs, fallback: true };
  }
  return {
    topology: a.choice,
    confidence: typeof a.confidence === "number" ? a.confidence : 0,
    probabilities: a.probabilities ?? {},
    latencyMs: r.latencyMs,
  };
}

export interface VerifyGate {
  pass: boolean;
  /**
   * Noul probability P(meets). NOTE: Noul answers carry NO confidence
   * field (API fact) — so `confidence` here is the calibrated probability
   * itself, i.e. distance from 0.5 IS the uncertainty signal. The 0.85/0.5
   * bands were tuned against this p (mirroring routeJudge's 0.8/0.5 bands
   * on Choice confidence), not against a separate confidence statistic.
   * jev_confidence on the ledger line records this same p.
   */
  confidence: number;
  escalate: boolean;
  latencyMs: number;
  fallback?: true;
}

/**
 * verifyGate(): "does this output meet the criterion?" Single Noul.
 * pass = p >= 0.85; escalate = p < 0.5. Fallback (no signal) is
 * fail-closed: {pass:false, escalate:true, fallback:true} so the loop
 * routes to human review instead of auto-passing blind.
 */
export async function verifyGate(
  output: string,
  criterion: string,
  opts: JevCallOpts = {},
): Promise<VerifyGate> {
  if (!criterion || !criterion.trim()) throw new Error("verifyGate: criterion required");
  if (!output || !output.trim()) {
    return { pass: false, confidence: 0, escalate: true, latencyMs: 0, fallback: true };
  }
  const r = await systemOne(
    `Criterion: ${criterion}\nOutput: ${output}`,
    {
      meets: {
        type: "noul",
        instructions: "The output meets the stated criterion",
        criteria: {
          true: "Output satisfies the criterion as written",
          false: "Output misses, contradicts, or ignores the criterion",
        },
      },
    },
    opts,
  );
  if (!r) return { pass: false, confidence: 0, escalate: true, latencyMs: 0, fallback: true };
  const p = r.answers["meets"]?.noul;
  if (typeof p !== "number") return { pass: false, confidence: 0, escalate: true, latencyMs: r.latencyMs, fallback: true };
  return {
    pass: p >= VERIFY_PASS_THRESHOLD,
    confidence: p,
    escalate: p < VERIFY_ESCALATE_THRESHOLD,
    latencyMs: r.latencyMs,
  };
}

export interface PruneItem { id: string; text: string; }
export interface PruneResult {
  keep: string[];
  drop: { id: string; p: number }[];
  latencyMs: number;
  fallback?: true;
}

/**
 * batchPrune(): forward-nudge helper. ONE SystemOne call with N Noul
 * questions (state = shared run context, sent once) — batching adds no
 * noise (each question scores independently). Fail-open: with no signal
 * nothing is pruned (keep all, {fallback:true}).
 */
export async function batchPrune(
  context: string,
  items: PruneItem[],
  opts: JevCallOpts & { threshold?: number } = {},
): Promise<PruneResult> {
  if (items.length === 0) return { keep: [], drop: [], latencyMs: 0 };
  const threshold = opts.threshold ?? PRUNE_THRESHOLD;
  const questions: Record<string, unknown> = {};
  for (const it of items) {
    questions[it.id] = {
      type: "noul",
      instructions: `This queued nudge is stale or superseded and should be pruned: ${it.text}`,
      criteria: {
        true: "Stale, duplicate, or superseded by newer information",
        false: "Still live and worth delivering",
      },
    };
  }
  const r = await systemOne(`Run context: ${context}`, questions, opts);
  if (!r) return { keep: items.map((i) => i.id), drop: [], latencyMs: 0, fallback: true };
  const keep: string[] = [];
  const drop: { id: string; p: number }[] = [];
  for (const it of items) {
    const p = r.answers[it.id]?.noul;
    if (typeof p === "number" && p >= threshold) drop.push({ id: it.id, p });
    else keep.push(it.id); // malformed answer for one item keeps that item
  }
  return { keep, drop, latencyMs: r.latencyMs };
}
