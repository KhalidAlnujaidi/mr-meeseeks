/**
 * harness/typesafe-judge.ts — calibrated atomicity judge (Jev, TypeSafe SystemOne).
 *
 * judge(task, criterion) POSTs state + 3 parallel questions to
 * https://api.typesafe.ai/v1/systemone (model jev-latest, key from
 * TYPESAFE_API_KEY env only — never logged, never persisted).
 * Returns {route, confidence, probabilities, atomicity_score, spiral}.
 * On API error/timeout/missing key: {route:"do_direct", confidence:0, fallback:true}
 * so the loop degrades to current behavior and never blocks.
 *
 * Erasable-syntax TS only: runs directly with `node` (>=22.18 type stripping).
 * Zero deps: uses global fetch (Node >=18).
 */

export interface JudgeVerdict {
  route: "do_direct" | "split_once";
  confidence: number; // 0..1 (0 + fallback:true on transport failure / missing key)
  probabilities: Record<string, number>;
  atomicity_score: number; // Score 0..2 (0=atomic, 1=2-3 steps, 2=sprawling)
  spiral: number; // noul 0..1 (subset-of-completed-sibling risk)
  fallback?: true;
  usage?: { input_tokens: number; output_tokens: number };
}

export const TYPESAFE_ENDPOINT = "https://api.typesafe.ai/v1/systemone";
/**
 * t3 follow-up: pin the versioned model ID (aliases move when new releases
 * ship, which would silently shift tuned confidence thresholds).
 * Overridable via TYPESAFE_MODEL env for staged migration to new versions.
 */
export const TYPESAFE_MODEL_PIN = "jev-1.13.0";
export const TYPESAFE_MODEL = TYPESAFE_MODEL_PIN;
export function resolveModel(env: Record<string, string | undefined> = process.env): string {
  const m = (env["TYPESAFE_MODEL"] ?? "").trim();
  return m ? m : TYPESAFE_MODEL_PIN;
}

function questions() {
  return {
    route: {
      type: "choice",
      instructions: "How should this task be handled",
      criteria: {
        do_direct: "One tool call produces the result and one check verifies it",
        split_once: "Needs decomposition into smaller sub-tasks first",
      },
    },
    atomicity: {
      type: "score",
      instructions: "How atomic is this task",
      criteria: [
        "Fits one tool call and one verification check",
        "Needs two to three steps but tightly scoped",
        "Sprawling, multi-part, must be decomposed",
      ],
    },
    is_spiral_risk: {
      type: "noul",
      instructions: "This task text is a subset of an already-completed sibling task in this run",
    },
  };
}

/** Fallback verdict: act-safe default, flags itself so callers route to escalate. */
export function fallbackVerdict(): JudgeVerdict {
  return {
    route: "do_direct", confidence: 0, probabilities: {},
    atomicity_score: 1, spiral: 0, fallback: true,
  };
}

/**
 * judge(): one SystemOne call, three parallel questions.
 * Throws only on caller misuse (empty task); transport failures → fallback.
 */
export async function judge(
  task: string,
  criterion: string,
  env: Record<string, string | undefined> = process.env,
  endpoint: string = TYPESAFE_ENDPOINT,
): Promise<JudgeVerdict> {
  if (!task || !task.trim()) throw new Error("judge: task text required");
  const key = env["TYPESAFE_API_KEY"];
  if (!key) return fallbackVerdict(); // no key configured: degrade, don't block
  const state = `Task: ${task}\nCriterion: ${criterion || "(none given)"}`;
  let res: Response;
  try {
    res = await fetch(endpoint, {
      method: "POST",
      headers: { Authorization: `Bearer ${key}`, "Content-Type": "application/json" },
      body: JSON.stringify({ state, model: resolveModel(env), questions: questions() }),
      signal: AbortSignal.timeout(30000),
    });
  } catch {
    return fallbackVerdict();
  }
  if (!res.ok) return fallbackVerdict();
  let data: {
    answers?: {
      route?: { choice?: string; confidence?: number; probabilities?: Record<string, number> };
      atomicity?: { score?: number };
      is_spiral_risk?: { noul?: number };
    };
    usage?: { input_tokens?: number; output_tokens?: number };
  };
  try {
    data = (await res.json()) as typeof data;
  } catch {
    return fallbackVerdict();
  }
  const route = data.answers?.route;
  const choice = route?.choice === "split_once" ? "split_once" : "do_direct";
  return {
    route: choice,
    confidence: typeof route?.confidence === "number" ? route.confidence : 0,
    probabilities: route?.probabilities ?? {},
    atomicity_score: typeof data.answers?.atomicity?.score === "number"
      ? data.answers.atomicity.score : 1,
    spiral: typeof data.answers?.is_spiral_risk?.noul === "number"
      ? data.answers.is_spiral_risk.noul : 0,
    usage: {
      input_tokens: data.usage?.input_tokens ?? 0,
      output_tokens: data.usage?.output_tokens ?? 0,
    },
  };
}

/**
 * routeJudge(): confidence-gated routing (doctrine, code-enforced).
 *  conf>=0.8 → act on route; 0.5–0.8 → do_direct + strict-verify;
 *  <0.5 (incl. fallback) → escalate to captain with probabilities attached.
 *  spiral>0.7 → SPIRAL report, stop lineage (independent of route).
 */
export type JudgeRouting =
  | { action: "act"; route: "do_direct" | "split_once" }
  | { action: "strict_verify" }
  | { action: "escalate"; probabilities: Record<string, number> }
  | { action: "spiral_stop" };

export function routeJudge(v: JudgeVerdict): JudgeRouting {
  if (v.spiral > 0.7) return { action: "spiral_stop" };
  if (v.confidence >= 0.8) return { action: "act", route: v.route };
  if (v.confidence >= 0.5) return { action: "strict_verify" };
  return { action: "escalate", probabilities: v.probabilities };
}
