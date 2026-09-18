import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { proposePromotion } from "./promotion.ts";

function mkRoot(): string {
  const root = mkdtempSync(join(tmpdir(), "mem-prom-"));
  mkdirSync(join(root, ".git"));
  mkdirSync(join(root, ".dsh", "brain"), { recursive: true });
  return root;
}

test("promotion blocks secrets and never writes by itself", () => {
  const root = mkRoot();
  const r = proposePromotion(
    "deploy via deploy.sh\nOPENROUTER_API_KEY=sk-or-v1-abc123\n",
    { projectRoot: root, author: "session-A" },
  );
  assert.equal(r.ok, false);
  assert.ok(r.blocked.length > 0, "expected at least one blocked hit");
  // nothing written: brain dir still empty of new files
  assert.equal(r.wrote, null);
});

test("promotion of clean project fact passes gate, writes only on approve", () => {
  const root = mkRoot();
  const prop = proposePromotion("deploy via ./deploy.sh from repo root\n", {
    projectRoot: root,
    author: "session-A",
  });
  assert.equal(prop.ok, true);
  assert.match(prop.candidate, /deploy\.sh/);
  assert.equal(prop.wrote, null); // propose never writes
  const done = prop.approve("BRAIN.md");
  assert.match(done.wrote ?? "", /BRAIN\.md/);
});
