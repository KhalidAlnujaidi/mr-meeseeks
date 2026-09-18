/**
 * memory/memory.ts — layered memory L0/L1/L2 (tracer bullet).
 *
 * L0 harness:  $DSH_HOME/AGENTS.md (host, OS, keys, budgets — nothing project-specific)
 * L1 project:  <projectRoot>/.dsh/brain/*.md (what the project is, layout, deploy,
 *              contacts — shared by every session whose cwd sits under the root)
 * L2 session:  NEVER on disk here. Caller-owned scratch; buildContext never reads
 *              it, so sibling sessions cannot leak into each other.
 *
 * Project root = nearest ancestor containing .git, else the cwd itself
 * (same rule as dsh-skill-filesystem). Erasable-syntax TS only, node stdlib only.
 */

import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { join, dirname, resolve } from "node:path";
import { homedir } from "node:os";

export interface MemoryDoc {
  scope: "l0" | "l1";
  path: string;
  content: string;
}

export interface LayeredContext {
  l0: MemoryDoc | null; // harness-global, may be absent
  l1: MemoryDoc[]; // project brain, sorted by filename, shared across sessions
  /** L2 is intentionally absent: session scratch is caller-owned, never loaded here. */
  l2Note: string;
}

/** Nearest ancestor of cwd containing .git, else cwd itself. */
export function nearestProjectRoot(cwd: string): string {
  let dir = resolve(cwd);
  for (;;) {
    try {
      if (statSync(join(dir, ".git")).isDirectory()) return dir;
    } catch {
      /* not a root — keep climbing */
    }
    const parent = dirname(dir);
    if (parent === dir) return resolve(cwd);
    dir = parent;
  }
}

function readIfFile(path: string): string | null {
  try {
    if (!statSync(path).isFile()) return null;
    return readFileSync(path, "utf8");
  } catch {
    return null;
  }
}

/**
 * Build the shared context for a session working in cwd.
 * Pure shared layers only (L0 + L1). Session-private L2 scratch is never
 * touched — two sessions in one project get deep-equal results.
 */
export function buildContext(
  cwd: string,
  opts: { dshHome?: string } = {},
): LayeredContext {
  const home = opts.dshHome ?? join(homedir(), ".dsh");

  const l0Path = join(home, "AGENTS.md");
  const l0Body = readIfFile(l0Path);
  const l0: MemoryDoc | null =
    l0Body === null ? null : { scope: "l0", path: l0Path, content: l0Body };

  const root = nearestProjectRoot(cwd);
  const brainDir = join(root, ".dsh", "brain");
  const l1: MemoryDoc[] = [];
  let names: string[] = [];
  try {
    names = readdirSync(brainDir).sort();
  } catch {
    names = [];
  }
  for (const name of names) {
    if (name.startsWith(".")) continue;
    if (!name.endsWith(".md")) continue;
    const p = join(brainDir, name);
    const body = readIfFile(p);
    if (body !== null) l1.push({ scope: "l1", path: p, content: body });
  }

  return {
    l0,
    l1,
    l2Note: "L2 session scratch is caller-owned and never loaded by buildContext.",
  };
}

/** Render L0 then L1, broad-to-specific. L2 is never rendered from here. */
export function renderContext(ctx: LayeredContext): string {
  const out: string[] = ["<layered-memory>"];
  if (ctx.l0) {
    out.push(`[L0 harness: ${ctx.l0.path}]`);
    out.push(ctx.l0.content);
  }
  for (const doc of ctx.l1) {
    out.push(`[L1 project: ${doc.path}]`);
    out.push(doc.content);
  }
  out.push("</layered-memory>");
  return out.join("\n");
}
