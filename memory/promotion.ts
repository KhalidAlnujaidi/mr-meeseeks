/**
 * memory/promotion.ts — L2 -> L1 promotion gate (tracer bullet).
 *
 * Rule: session scratch NEVER becomes project memory by itself.
 * proposePromotion() only checks + stages a candidate. It never writes.
 * Only the returned approve() writes, and only into <root>/.dsh/brain/<file>.
 * Secrets (keys, tokens, passwords, private material) are refused outright.
 */

import { appendFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";

export interface PromotionProposal {
  ok: boolean;
  candidate: string;
  blocked: string[];
  wrote: string | null;
  approve: (file?: string) => { wrote: string | null };
}

const SECRET_PATTERNS: RegExp[] = [
  /sk-or-v1-[A-Za-z0-9_-]{5,}/i, // OpenRouter keys
  /\bsk-[A-Za-z0-9_-]{8,}/, // generic sk- keys
  /\b(api[_-]?key|api[_-]?secret|token|bearer|password|passwd|secret)\b\s*[:=]\s*\S+/i,
  /\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b.*\bpassword\b/i,
];

export function findSecrets(text: string): string[] {
  const hits: string[] = [];
  for (const re of SECRET_PATTERNS) {
    const m = text.match(re);
    if (m) hits.push(m[0].slice(0, 48));
  }
  return hits;
}

export function proposePromotion(
  text: string,
  opts: { projectRoot: string; author: string },
): PromotionProposal {
  const candidate = text.trim();
  const blocked = candidate ? findSecrets(candidate) : ["empty candidate"];
  const ok = blocked.length === 0;
  return {
    ok,
    candidate,
    blocked,
    wrote: null, // propose never writes
    approve: (file = "BRAIN.md") => {
      if (!ok) return { wrote: null };
      const safe = file.replace(/[^A-Za-z0-9_.-]/g, "_");
      const dir = join(opts.projectRoot, ".dsh", "brain");
      mkdirSync(dir, { recursive: true });
      const target = join(dir, safe.endsWith(".md") ? safe : `${safe}.md`);
      appendFileSync(
        target,
        `\n<!-- promoted from ${opts.author} -->\n${candidate}\n`,
      );
      return { wrote: target };
    },
  };
}
