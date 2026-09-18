/**
 * harness/loop.ts — minimal 4-verb agent loop (approach C + amendment).
 *
 * Splitter lives ONLY in this loop; members never split; depth via re-queueing.
 * Single file, zero plugin boilerplate, no AgentTeams imports.
 * Erasable-syntax TS only: runs directly with `node harness/loop.ts`
 * (Node >=22.18 type stripping) or `npx tsx harness/loop.ts`.
 *
 * Layout:
 *   harness/tasks/<id>.json  task state (status: queued|claimed|done|verified|failed)
 *   harness/queue/<id>        empty marker = needs claiming (O(1) poll)
 *   harness/tasks/<id>.lock   O_EXCL claim lock {owner, ts}
 *   harness/ledger.jsonl      append-only split/report/verify events
 *
 * t5 scope: skeleton + enqueue + atomic claim + ledger append + queue poll.
 * t6 scope: do/report/verify + frozen prompts + allowlists (reviewer = one
 *   criterion / unlimited read-only checks / no writes / no network).
 * t7 adds: atomicity + requeue + guards.
 */

import { openSync, closeSync, mkdirSync, readdirSync, readFileSync, writeFileSync, appendFileSync, rmSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

export type TaskStatus = "queued" | "claimed" | "done" | "verified" | "failed";
export type Role = "researcher" | "engineer" | "reviewer";

export interface Task {
  id: string;            // dotted lineage: t1, t1.1, t1.2
  parent_id: string | null;
  depth: number;         // root = 0, +1 per delegation hop
  resplit_count: number; // spiral counter for this lineage
  role: Role;
  criterion: string;     // acceptance the result must meet
  status: TaskStatus;
  result?: string;
  // R2-B: heterogeneous assignment recorded at split time. Optional
  // (additive: old task JSONs without it still load). Set by splitOnce
  // from the per-child assignment; verify() only confirms it matches.
  workerModel?: string;
}

export interface LedgerLine {
  type: "split" | "report" | "verify";
  task_id: string;
  parent_id: string | null;
  depth: number;
  model: string;
  role: Role | "captain";
  ts: string;
  cost: { input_tokens: number; output_tokens: number };
  criterion: string;
  verdict: "pass" | "fail" | null;
  detail: string;
  resplit_count: number;
}

const HERE = join(dirname(fileURLToPath(import.meta.url)));
export const TASKS_DIR = join(HERE, "tasks");
export const QUEUE_DIR = join(HERE, "queue");
export const LEDGER = join(HERE, "ledger.jsonl");

export function initDirs(): void {
  mkdirSync(TASKS_DIR, { recursive: true });
  mkdirSync(QUEUE_DIR, { recursive: true });
}

const taskPath = (id: string): string => join(TASKS_DIR, `${id}.json`);
const lockPath = (id: string): string => join(TASKS_DIR, `${id}.lock`);
const queuePath = (id: string): string => join(QUEUE_DIR, id);

export function loadTask(id: string): Task {
  return JSON.parse(readFileSync(taskPath(id), "utf8")) as Task;
}

function saveTask(t: Task): void {
  writeFileSync(taskPath(t.id), JSON.stringify(t, null, 2) + "\n");
}

/** Append one event to the ledger (append-only; never rewrite). */
export function ledgerAppend(line: LedgerLine): void {
  appendFileSync(LEDGER, JSON.stringify(line) + "\n");
}

/** Enqueue a new task: state file (status=queued) + queue marker. */
export function enqueue(t: Task): void {
  initDirs();
  t.status = "queued";
  saveTask(t);
  writeFileSync(queuePath(t.id), "");
}

/** Tasks awaiting a claim (queue markers whose task is still queued). */
export function pollQueue(): string[] {
  initDirs();
  return readdirSync(QUEUE_DIR).filter((id) => {
    try {
      return loadTask(id).status === "queued";
    } catch {
      return false;
    }
  });
}

export class ClaimConflict extends Error {
  owner: string;
  constructor(id: string, owner: string) {
    super(`task ${id} already claimed by ${owner}`);
    this.owner = owner;
  }
}

/**
 * Atomic claim: O_EXCL create of tasks/<id>.lock. Exactly one claimant wins;
 * losers read the winner from the lock file and throw ClaimConflict.
 * On success: task status -> claimed, queue marker removed.
 */
export function claim(id: string, owner: string): Task {
  const stamp = JSON.stringify({ owner, ts: new Date().toISOString() });
  let fd: number;
  try {
    fd = openSync(lockPath(id), "wx", 0o644); // O_EXCL: atomic, fails if exists
  } catch {
    const winner = JSON.parse(readFileSync(lockPath(id), "utf8")) as { owner: string };
    throw new ClaimConflict(id, winner.owner);
  }
  try {
    writeFileSync(fd, stamp + "\n");
  } finally {
    closeSync(fd);
  }
  const t = loadTask(id);
  if (t.status !== "queued") {
    // Lock won but task not claimable: leave lock (owner recorded) and abort.
    throw new Error(`task ${id} not queued (status=${t.status})`);
  }
  t.status = "claimed";
  saveTask(t);
  try {
    rmSync(queuePath(id), { force: true });
  } catch { /* marker already gone: harmless */ }
  return t;
}

// --- t6: do/report/verify + frozen role prompts + allowlists ---

export const LEAF_HEADER =
  "You are a leaf worker. You see ONLY your task below. Do exactly one thing: " +
  "either DO it and report back, or (if non-atomic) answer NEEDS_SPLIT plus " +
  "one reason and stop. Never do both. Never re-split someone else's atomic unit. " +
  "You never create subtasks; only the loop splits.";

export const ROLE_PROMPTS: Record<Role, string> = {
  researcher:
    "Find facts. You answer ONE question with evidence (quotes/links/file paths). " +
    "No code changes, no judgments beyond relevance. Report: answer + sources, " +
    "short enough to verify in one check.",
  engineer:
    "Change code or run ONE command sequence toward the stated criterion. Report: " +
    "what changed, exact files/commands, and observed output. " +
    "No refactoring beyond the criterion.",
  // CORRECTION (captain): reviewer runs ONE verification criterion with as many
  // read-only checks as it needs — no writes, no network. t4's "ONE read-only
  // check" was too narrow; a criterion like "3 links resolve" needs 3 reads.
  reviewer:
    "Verify ONE report against its ONE criterion. Output exactly: " +
    "VERDICT: pass|fail + one-sentence reason. You may run as many read-only " +
    "checks (read/grep/glob) as the criterion needs to confirm. No writes, " +
    "no network, never rewrite the work; on fail, state the missing piece " +
    "so the parent re-issues.",
};

/** Tool allowlists, max 3 per role. Reviewer is read-only, no network. */
export const ALLOWLISTS: Record<Role, string[]> = {
  researcher: ["web_search", "web_fetch", "read"],
  engineer: ["read", "edit", "bash"], // bash = run/tests only, scoped to task dir
  reviewer: ["read", "grep", "glob"],
};

export interface LeafInvocation {
  prompt: string; // frozen prompt the driver feeds to exactly one model call
  tools: string[]; // allowlist: the only tools that call may use
  model: string;  // exact model id, never "small model"
}

export const NEEDS_SPLIT = "NEEDS_SPLIT";

/**
 * do(): build the single atomic leaf invocation. loop.ts owns no model client,
 * so do() freezes the prompt + allowlist and the driver performs exactly one
 * model call with them. Members never split: a leaf that judges its task
 * non-atomic answers "NEEDS_SPLIT:<reason>" as its result text; only verify()
 * (t7) may create children, in the loop alone.
 */
export function buildLeafInvocation(task: Task, model: string): LeafInvocation {
  if (task.status !== "claimed") throw new Error(`task ${task.id} must be claimed before do (status=${task.status})`);
  if (!model) throw new Error("model id required (exact id, never 'small model')");
  const prompt =
    `${LEAF_HEADER}\n\nROLE (${task.role}): ${ROLE_PROMPTS[task.role]}\n\n` +
    `TASK ${task.id} (depth ${task.depth}): ${task.criterion}\n` +
    `Answer with your result, or with "${NEEDS_SPLIT}:<reason>" if this needs more than one tool call.`;
  return { prompt, tools: ALLOWLISTS[task.role], model };
}

/** Max result size: a report must fit one verification check. */
export const MAX_REPORT_CHARS = 2000;

export class OversizedReport extends Error {
  constructor(id: string, n: number) {
    super(`task ${id} report too long (${n} > ${MAX_REPORT_CHARS} chars): split the task, don't grow the report`);
  }
}

export interface Cost { input_tokens: number; output_tokens: number; }
const ZERO_COST: Cost = { input_tokens: 0, output_tokens: 0 };

/**
 * report(): claimed -> done. Writes result, appends one report ledger line
 * (verdict=null; the verifier decides). Rejects oversized results.
 */
export function report(id: string, result: string, model: string, cost: Cost = ZERO_COST): Task {
  const t = loadTask(id);
  if (t.status !== "claimed") throw new Error(`task ${id} must be claimed before report (status=${t.status})`);
  if (result.length > MAX_REPORT_CHARS) throw new OversizedReport(id, result.length);
  t.result = result;
  t.status = "done";
  saveTask(t);
  ledgerAppend({
    type: "report", task_id: t.id, parent_id: t.parent_id, depth: t.depth,
    model, role: t.role, ts: new Date().toISOString(), cost,
    criterion: t.criterion, verdict: null, detail: result, resplit_count: t.resplit_count,
  });
  return t;
}

/**
 * R5-B: normalized model-id compare (compare-only, never rewrites stored ids).
 * normModel() strips provider/routing prefixes (kilo/, openrouter/,
 * kilo//openrouter/, org/ — anything before the last "/"), strips a
 * trailing ":free" suffix, trims whitespace, lowercases. Raw ids stay on
 * task records + ledger lines; normalization applies ONLY at compare sites
 * (verify worker-match, correlated-blindness, hetero distinctness).
 * sameModel(a, b) is the normalized equality predicate (1 leaf).
 */
export function normModel(id: string): string {
  let s = (id ?? "").trim().toLowerCase();
  if (s.endsWith(":free")) s = s.slice(0, -":free".length);
  // Strip provider/routing prefix path: compare on the basename so routed
  // equivalents match. Empty segments (kilo//openrouter/) are dropped so
  // the double-slash form collapses to the same basename.
  const parts = s.split("/").filter((p) => p.length > 0);
  return parts.length > 0 ? parts[parts.length - 1] : s;
}

/** Normalized model-id equality: true iff normModel(a) === normModel(b). */
export function sameModel(a: string, b: string): boolean {
  return normModel(a) === normModel(b);
}

/**
 * R5-A: mismatch hint for the strict worker-model compare (additive-only).
 * The compare itself stays byte-exact (silent substitution stays a refusal);
 * errHint() only enriches the refusal message with both ids, a
 * "did you mean X?" pointer at the assigned form, and the likely cause
 * (:free suffix / provider prefix / casing / whitespace) when detectable.
 */
export function errHint(assigned: string, got: string): string {
  const hints: string[] = [];
  const stripFree = (m: string): string => (m.endsWith(":free") ? m.slice(0, -":free".length) : m);
  const tail = (m: string): string => {
    const i = m.lastIndexOf("/");
    return i < 0 ? m : m.slice(i + 1);
  };
  if (assigned !== got && stripFree(assigned) === stripFree(got)) {
    hints.push("check :free suffix");
  }
  if (stripFree(assigned) !== stripFree(got) && tail(stripFree(assigned)) === tail(stripFree(got))) {
    hints.push("check provider prefix");
  }
  if (assigned !== got && assigned.toLowerCase() === got.toLowerCase()) {
    hints.push("check casing (compare is case-sensitive)");
  }
  if (assigned.trim() !== assigned || got.trim() !== got) {
    hints.push("check leading/trailing whitespace");
  }
  const cause = hints.length > 0 ? `likely cause: ${hints.join("; ")}` : "ids differ";
  return `did you mean '${assigned}'? ${cause}. re-run verify with workerModel exactly as assigned.`;
}

/**
 * verify(): done -> verified|failed. Collapse event, one verify ledger line.
 * Enforces cross-model verification (correlated-blindness rule: reviewer model
 * MUST differ from worker model) and reviewer read-only discipline (verify
 * performs no writes and no network — evidence arrives as read-only detail).
 */
export function verify(
  id: string, verdict: "pass" | "fail",
  opts: { reviewerModel: string; workerModel: string; reason: string; cost?: Cost; normalizeModels?: boolean },
): Task {
  const t = loadTask(id);
  if (t.status !== "done") throw new Error(`task ${id} must be reported before verify (status=${t.status})`);
  if (!opts.reason) throw new Error("verify requires a one-sentence reason");
  // R5-B: opt-in normalized compare (default strict — R5-A intact).
  // normalizeModels=true compares via sameModel(); raw ids stay on records/ledger.
  const norm = opts.normalizeModels ?? false;
  const modelsEqual = (a: string, b: string): boolean => (norm ? sameModel(a, b) : a === b);
  if (modelsEqual(opts.reviewerModel, opts.workerModel)) {
    throw new Error(`correlated blindness: reviewer model must differ from worker model (${opts.workerModel})`);
  }
  // R2-B: verify only CONFIRMS the split-time workerModel record (additive:
  // tasks without a record skip this check). Assignment was decided at split.
  if (t.workerModel && !modelsEqual(t.workerModel, opts.workerModel)) {
    throw new Error(`worker model mismatch: split assigned '${t.workerModel}', verify got '${opts.workerModel}'. ${errHint(t.workerModel, opts.workerModel)}`);
  }
  t.status = verdict === "pass" ? "verified" : "failed";
  saveTask(t);
  ledgerAppend({
    type: "verify", task_id: t.id, parent_id: t.parent_id, depth: t.depth,
    model: opts.reviewerModel, role: "reviewer", ts: new Date().toISOString(),
    cost: opts.cost ?? ZERO_COST, criterion: t.criterion, verdict,
    detail: opts.reason, resplit_count: t.resplit_count,
  });
  return t;
}

// --- t7: atomicity check + split-once + requeue + budget/spiral guards ---

export const MAX_RESPLITS = 3; // spiral stop (doctrine rule 7)
export const DEFAULT_BUDGET_MAX_AGENTS = 64; // breadth^depth estimate ceiling

export const SPIRAL_DETAIL = "SPIRAL: decomposition bug, awaiting instructions";

export class SpiralStop extends Error {
  constructor(id: string) {
    super(`task ${id}: resplit_count reached ${MAX_RESPLITS}, lineage stopped`);
  }
}

export class BudgetExceeded extends Error {
  constructor(id: string, estimate: number, max: number) {
    super(`task ${id}: split refused, breadth^depth estimate ${estimate} > budget ${max}`);
  }
}

/**
 * Atomicity check (doctrine rule 6): a task is ATOMIC iff BOTH hold —
 * (a) ONE-TOOL-CALL: producible with a single tool call;
 * (b) ONE-CHECK: verifiable pass/fail with a single check against the criterion.
 * The LOOP asks these two questions at claim time, never the member.
 */
export function isAtomic(criterion: string): boolean {
  const multi = /(and then|then\s+\w+,\s*and|steps?:?\s*\d|phase\s*\d|multi-?step|all of the following|each of the following)/i;
  return !multi.test(criterion);
}

/**
 * Scope-aware atomicity (v2, additive alongside v1 — no replacement yet).
 * Conjecture: keyword-regex isAtomic misses coordination scope. v2 counts
 * distinct files/tools/roles implied by the criterion; >1 in any axis → split.
 * v1 verdicts are preserved: anything v1 calls split, v2 also calls split.
 */
const SCOPE_VERBS = [
  "audit", "add", "verify", "verif", "check", "render", "fix", "run", "test",
  "read", "write", "search", "fetch", "document", "import", "compil", "render",
  "comment",
];
const PLURAL_SCOPE = /\b(every|each|all|multiple|various|several)\b/i;
const PATH_TOKEN = /[\w.-]+\/[\w./-]*|\b[\w-]+\.\w+\b/g;
const ROLE_WORDS = ["researcher", "engineer", "reviewer"];

export interface ScopeCounts { files: number; tools: number; roles: number; }

export function scopeCounts(criterion: string): ScopeCounts {
  const paths = new Set(
    (criterion.match(PATH_TOKEN) ?? []).map((p) => p.toLowerCase()),
  );
  let files = paths.size;
  if (PLURAL_SCOPE.test(criterion)) files = Math.max(files, 2);
  const lower = ` ${criterion.toLowerCase()} `;
  const tools = new Set(
    SCOPE_VERBS.filter((v) => new RegExp(`[^\\w]${v}\\w*[^\\w]`).test(lower)),
  );
  // verif* and render* listed with stems; dedupe stems that double-match.
  const toolsNorm = new Set(
    [...tools].map((v) => v.replace(/^(verif|compil).*/, "$1").replace(/^(render).*/, "rend")),
  );
  const roles = new Set(ROLE_WORDS.filter((r) => lower.includes(` ${r} `)));
  return { files, tools: toolsNorm.size, roles: roles.size };
}

/** v2: atomic iff v1 says atomic AND every scope axis counts ≤ 1. */
export function isAtomicV2(criterion: string): boolean {
  if (!isAtomic(criterion)) return false;
  const s = scopeCounts(criterion);
  return s.files <= 1 && s.tools <= 1 && s.roles <= 1;
}

export interface ChildSpec { role: Role; criterion: string; workerModel?: string; }

export class HomogeneousAssignment extends Error {
  constructor(id: string, models: string[]) {
    super(`task ${id}: split refused, sibling subtasks must differ in workerModel (got [${models.join(", ")}]) — assign 2+ distinct models or refuse explicitly`);
  }
}

/** Distinct non-empty worker models in a sibling set (strict, raw strings). */
export function distinctWorkerModels(models: (string | undefined)[]): string[] {
  return [...new Set(models.filter((m): m is string => !!m && m.trim().length > 0))];
}

/**
 * R5-B: distinct non-empty worker models under normalized compare.
 * Additive alongside strict distinctWorkerModels: normModel each id, then
 * dedupe. Raw strings are preserved by callers (compare-only).
 */
export function distinctWorkerModelsNorm(models: (string | undefined)[]): string[] {
  return [...new Set(models.filter((m): m is string => !!m && m.trim().length > 0).map((m) => normModel(m!)))];
}

/**
 * splitOnce(): the ONLY splitter in the system. Loop-only: members never call
 * this (they answer NEEDS_SPLIT; the loop decides). Splits ONCE into the
 * smallest useful subtasks, writes child JSONs (parent_id, depth+1), one split
 * ledger line, and enqueues children (re-queue = depth emerges from queueing).
 * Guards: spiral (resplit_count==3 → report SPIRAL, stop lineage) then budget
 * (breadth^depth estimate vs run budget before each split).
 */
export function splitOnce(
  id: string, children: ChildSpec[], model: string,
  opts: { budgetMax?: number; cost?: Cost; workerModels?: string[]; requireHetero?: boolean; normalizeModels?: boolean } = {},
): Task[] {
  const t = loadTask(id);
  if (t.status !== "claimed" && t.status !== "done" && t.status !== "failed") {
    throw new Error(`task ${id} must be claimed/done/failed before split (status=${t.status})`);
  }
  if (children.length === 0) throw new Error("splitOnce requires at least one child");
  // R2-B: heterogeneous assignment enforced AT SPLIT TIME (additive).
  // Legacy callers pass no models → old behavior preserved (no workerModel set).
  // New callers pass per-child workerModel (or opts.workerModels array);
  // a multi-child split with <2 distinct models is refused explicitly.
  const models: (string | undefined)[] =
    opts.workerModels ?? children.map((c) => c.workerModel);
  const assigned = models.filter((m): m is string => !!m && m.trim().length > 0);
  if (children.length > 1 && assigned.length > 0) {
    if (assigned.length < children.length) {
      throw new HomogeneousAssignment(id, models.map((m) => m ?? "(unassigned)"));
    }
    if (opts.requireHetero ?? true) {
      // R5-B: normalizeModels=true counts hetero under normModel (compare-only;
      // raw ids stay on child records + ledger). Default strict (R5-A intact).
      const distinct = (opts.normalizeModels ?? false)
        ? distinctWorkerModelsNorm(models)
        : distinctWorkerModels(models);
      if (distinct.length < 2) {
        throw new HomogeneousAssignment(id, models as string[]);
      }
    }
  }
  // Spiral guard first: a lineage re-split 3 times is a decomposition bug.
  if (t.resplit_count >= MAX_RESPLITS) {
    t.status = "failed";
    t.result = SPIRAL_DETAIL;
    saveTask(t);
    ledgerAppend({
      type: "report", task_id: t.id, parent_id: t.parent_id, depth: t.depth,
      model, role: t.role, ts: new Date().toISOString(), cost: opts.cost ?? ZERO_COST,
      criterion: t.criterion, verdict: null, detail: SPIRAL_DETAIL,
      resplit_count: t.resplit_count,
    });
    throw new SpiralStop(id);
  }
  // Budget guard: breadth^depth estimate vs run budget.
  const max = opts.budgetMax ?? DEFAULT_BUDGET_MAX_AGENTS;
  const estimate = Math.pow(children.length, t.depth + 1);
  if (estimate > max) throw new BudgetExceeded(id, estimate, max);
  // Split once: children inherit lineage, depth+1, parent's NEW resplit_count
  // (the count tracks splits along this lineage; at 3 the next split stops).
  const made: Task[] = children.map((c, i) => ({
    id: `${t.id}.${i + 1}`,
    parent_id: t.id,
    depth: t.depth + 1,
    resplit_count: t.resplit_count + 1,
    role: c.role,
    criterion: c.criterion,
    status: "queued" as TaskStatus,
    // R2-B: record heterogeneous assignment at split time (optional field).
    ...(models[i] ? { workerModel: models[i] } : {}),
  }));
  for (const child of made) enqueue(child);
  t.resplit_count += 1;
  t.status = "done";
  t.result = `${NEEDS_SPLIT}: split into ${made.map((c) => c.id).join(",")}`;
  saveTask(t);
  ledgerAppend({
    type: "split", task_id: t.id, parent_id: t.parent_id, depth: t.depth,
    model, role: "captain", ts: new Date().toISOString(), cost: opts.cost ?? ZERO_COST,
    criterion: t.criterion, verdict: null,
    detail: `children=${made.map((c) => c.id).join(",")}`,
    resplit_count: t.resplit_count,
  });
  return made;
}

// --- t8 smoke test drives enqueue→claim→do→report→verify→split→requeue ---

function usage(): never {
  console.error("usage: node harness/loop.ts <enqueue|claim|poll|show|prompt|report|verify|split|atomic|atomic2> [args...]");
  process.exit(1);
}

// Minimal CLI for smoke tests (t8 drives this end-to-end).
const [, , cmd, ...args] = process.argv;
if (cmd === "enqueue") {
  const [id, role, criterion] = args;
  if (!id || !role || !criterion) usage();
  const depth = id.split(".").length - 1;
  const parent_id = depth === 0 ? null : id.split(".").slice(0, -1).join(".");
  enqueue({ id, parent_id, depth, resplit_count: 0, role: role as Role, criterion, status: "queued" });
  console.log(`enqueued ${id}`);
} else if (cmd === "claim") {
  const [id, owner] = args;
  if (!id || !owner) usage();
  try {
    claim(id, owner);
    console.log(`claimed ${id} by ${owner}`);
  } catch (e) {
    if (e instanceof ClaimConflict) {
      console.error(`conflict: ${e.message}`);
      process.exit(2);
    }
    throw e;
  }
} else if (cmd === "poll") {
  console.log(pollQueue().join("\n"));
} else if (cmd === "show") {
  const [id] = args;
  if (!id) usage();
  console.log(JSON.stringify(loadTask(id), null, 2));
} else if (cmd === "prompt") {
  // Print the frozen leaf prompt for a claimed task (driver feeds it to one model call).
  const [id, model] = args;
  if (!id || !model) usage();
  const inv = buildLeafInvocation(loadTask(id), model);
  console.log(inv.prompt);
  console.log(`\n[tools: ${inv.tools.join(", ")} | model: ${inv.model}]`);
} else if (cmd === "report") {
  // report <id> <model> <result...>  (model BEFORE result: result may contain spaces)
  const [id, model, ...rest] = args;
  const result = rest.join(" ");
  if (!id || !model || !result) usage();
  try {
    report(id, result, model);
  } catch (e) {
    if (e instanceof OversizedReport) {
      console.error(`rejected: ${e.message}`);
      process.exit(3);
    }
    throw e;
  }
  console.log(`reported ${id}`);
} else if (cmd === "verify") {
  // verify <id> <pass|fail> <reviewerModel> <workerModel> [--norm] <reason...>
  // R5-B: --norm opts into normalized model compare (sameModel); default strict.
  const [id, verdict, reviewerModel, workerModel, ...rest] = args;
  const norm = rest[0] === "--norm";
  const reason = (norm ? rest.slice(1) : rest).join(" ");
  if (!id || (verdict !== "pass" && verdict !== "fail") || !reviewerModel || !workerModel || !reason) usage();
  verify(id, verdict, { reviewerModel, workerModel, reason, ...(norm ? { normalizeModels: true as const } : {}) });
  console.log(`verified ${id}: ${verdict}${norm ? " (norm)" : ""}`);
} else if (cmd === "atomic") {
  // atomic <criterion...>: loop-side atomicity check (heuristic v1).
  const criterion = args.join(" ");
  if (!criterion) usage();
  console.log(isAtomic(criterion) ? "atomic" : "split");
} else if (cmd === "atomic2") {
  // atomic2 <criterion...>: scope-aware atomicity check (heuristic v2, additive).
  const criterion = args.join(" ");
  if (!criterion) usage();
  const s = scopeCounts(criterion);
  console.log(isAtomicV2(criterion) ? "atomic" : "split");
  console.log(`[scope files=${s.files} tools=${s.tools} roles=${s.roles}]`);
} else if (cmd === "split") {
  // split <id> <model> [--budget N] [--norm] [--models m1,m2,...] <role[@workerModel]:criterion> [...]
  // R2-B: per-child worker model via role@model: prefix or --models list (additive).
  // R5-B: --norm counts hetero under normModel (compare-only; raw ids stored).
  const [id, model, ...rest] = args;
  if (!id || !model || rest.length === 0) usage();
  let budgetMax: number | undefined;
  let cliModels: string[] | undefined;
  let norm = false;
  const specs: string[] = [];
  for (let i = 0; i < rest.length; i++) {
    if (rest[i] === "--budget") {
      budgetMax = Number(rest[++i]);
      if (!Number.isFinite(budgetMax)) usage();
    } else if (rest[i] === "--norm") {
      norm = true;
    } else if (rest[i] === "--models") {
      const raw = rest[++i] ?? "";
      cliModels = raw.split(",").map((m) => m.trim()).filter(Boolean);
      if (cliModels.length === 0) usage();
    } else {
      specs.push(rest[i]);
    }
  }
  const children: ChildSpec[] = specs.map((s, i) => {
    const j = s.indexOf(":");
    if (j < 0) usage();
    const rolePart = s.slice(0, j);
    const criterion = s.slice(j + 1);
    const at = rolePart.indexOf("@");
    const role = (at < 0 ? rolePart : rolePart.slice(0, at)) as Role;
    const perChild = at < 0 ? undefined : rolePart.slice(at + 1).trim() || undefined;
    if ((role !== "researcher" && role !== "engineer" && role !== "reviewer") || !criterion) usage();
    const workerModel = perChild ?? cliModels?.[i];
    return workerModel ? { role, criterion, workerModel } : { role, criterion };
  });
  try {
    const made = splitOnce(id, children, model, {
      ...(budgetMax === undefined ? {} : { budgetMax }),
      ...(norm ? { normalizeModels: true as const } : {}),
    });
    console.log(`split ${id} -> ${made.map((c) => c.id).join(",")}`);
    for (const c of made) console.log(`  ${c.id} workerModel=${c.workerModel ?? "(none)"}`);
  } catch (e) {
    if (e instanceof SpiralStop) {
      console.error(`spiral: ${e.message}`);
      process.exit(4);
    }
    if (e instanceof BudgetExceeded) {
      console.error(`budget: ${e.message}`);
      process.exit(5);
    }
    if (e instanceof HomogeneousAssignment) {
      console.error(`hetero: ${e.message}`);
      process.exit(6);
    }
    throw e;
  }
} else {
  usage();
}
