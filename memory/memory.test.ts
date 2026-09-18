import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { buildContext, renderContext, nearestProjectRoot } from "./memory.ts";

function mkHomeWithGlobal(body: string): string {
  const home = mkdtempSync(join(tmpdir(), "mem-home-"));
  writeFileSync(join(home, "AGENTS.md"), body);
  return home;
}

function mkProject(brainFiles: Record<string, string>): string {
  const root = mkdtempSync(join(tmpdir(), "mem-proj-"));
  mkdirSync(join(root, ".git"));
  mkdirSync(join(root, ".dsh", "brain"), { recursive: true });
  for (const [name, body] of Object.entries(brainFiles)) {
    writeFileSync(join(root, ".dsh", "brain", name), body);
  }
  return root;
}

test("two sessions in one project share L1, L2 scratch never leaks", () => {
  const home = mkdtempSync(join(tmpdir(), "mem-home-"));
  const root = mkProject({ "BRAIN.md": "# brain\ndeploy via deploy.sh\n" });
  const scratchA = `SESSION-A-SECRET-${Math.random()}`;
  void scratchA;
  const a = buildContext(join(root, "app"), { dshHome: home });
  const b = buildContext(join(root, "other"), { dshHome: home });
  assert.match(JSON.stringify(a), /deploy via deploy\.sh/);
  assert.deepEqual(a, b);
  assert.doesNotMatch(JSON.stringify(a) + JSON.stringify(b), /SESSION-A-SECRET-/);
});

test("L0 global loads first, L1 project second, broad-to-specific order", () => {
  const home = mkHomeWithGlobal("# GLOBAL-HARNESS\nmacOS 26\n");
  const root = mkProject({ "BRAIN.md": "# PROJECT-BRAIN\nlayout map here\n" });
  const ctx = buildContext(root, { dshHome: home });
  assert.match(ctx.l0?.content ?? "", /GLOBAL-HARNESS/);
  assert.equal(ctx.l1.length, 1);
  assert.match(ctx.l1[0]?.content ?? "", /PROJECT-BRAIN/);
  const rendered = renderContext(ctx);
  assert.ok(rendered.indexOf("GLOBAL-HARNESS") < rendered.indexOf("PROJECT-BRAIN"));
});

test("nearestProjectRoot finds .git ancestor, else cwd", () => {
  const root = mkProject({});
  assert.equal(nearestProjectRoot(join(root, "a", "b")), root);
  const lone = mkdtempSync(join(tmpdir(), "mem-lone-"));
  assert.equal(nearestProjectRoot(lone), lone);
});
