// harnessd-proxy.ts — TS sidecar client for the harnessd Unix-socket daemon.
//
// Opens the socket, sends the token auth envelope (HARNESSD_TOKEN), then
// one JSON line per op. Used by harness/loop.ts to drive the native host
// (token check, sandboxed exec, sister-bus vector post) without node-ffi.
import * as net from "node:net";

export interface DaemonCallOpts {
  sockPath?: string;
  token?: string;
  timeoutMs?: number;
}

function sockOf(o: DaemonCallOpts): string {
  return (
    o.sockPath || process.env.HARNESSD_SOCK || "/tmp/harnessd.sock"
  );
}

function tokenOf(o: DaemonCallOpts): string {
  const t = o.token || process.env.HARNESSD_TOKEN || "";
  if (!t) throw new Error("harnessd-proxy: HARNESSD_TOKEN required");
  return t;
}

export function daemonCall(
  op: Record<string, unknown>,
  opts: DaemonCallOpts = {},
): Promise<Record<string, unknown>> {
  const sockPath = sockOf(opts);
  const token = tokenOf(opts);
  const timeoutMs = opts.timeoutMs ?? 320_000;
  return new Promise((resolve, reject) => {
    const conn = net.createConnection(sockPath);
    let buf = "";
    let authed = false;
    let settled = false;
    const done = (
      err: Error | null,
      val?: Record<string, unknown>,
    ) => {
      if (settled) return;
      settled = true;
      conn.destroy();
      if (err) reject(err);
      else resolve(val as Record<string, unknown>);
    };
    const timer = setTimeout(
      () => done(new Error("harnessd-proxy: timeout")),
      timeoutMs,
    );
    conn.on("connect", () => {
      conn.write(JSON.stringify({ token }) + "\n");
    });
    conn.on("data", (chunk: Buffer) => {
      buf += chunk.toString("utf8");
      let idx: number;
      while ((idx = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, idx).trim();
        buf = buf.slice(idx + 1);
        if (!line) continue;
        let msg: Record<string, unknown>;
        try {
          msg = JSON.parse(line) as Record<string, unknown>;
        } catch {
          clearTimeout(timer);
          done(new Error("harnessd-proxy: bad JSON from daemon"));
          return;
        }
        if (!authed) {
          authed = true;
          if (msg["ok"] !== true) {
            clearTimeout(timer);
            done(new Error("harnessd-proxy: auth refused"));
            return;
          }
          conn.write(JSON.stringify(op) + "\n");
          continue;
        }
        clearTimeout(timer);
        if (msg["ok"] === true) done(null, msg);
        else done(new Error(`harnessd-proxy: ${msg["error"] || "op failed"}`));
        return;
      }
    });
    conn.on("error", (e) => {
      clearTimeout(timer);
      done(e);
    });
  });
}

export const daemonOps = {
  ping: (o?: DaemonCallOpts) => daemonCall({ op: "ping" }, o),
  backendLabel: (o?: DaemonCallOpts) =>
    daemonCall({ op: "backend_label" }, o),
  checkSplit: (
    p: Record<string, unknown>,
    o?: DaemonCallOpts,
  ) => daemonCall({ op: "check_split", ...p }, o),
  busPost: (p: Record<string, unknown>, o?: DaemonCallOpts) =>
    daemonCall({ op: "bus_post", ...p }, o),
  sandboxExec: (p: Record<string, unknown>, o?: DaemonCallOpts) =>
    daemonCall({ op: "sandbox_exec", ...p }, o),
  worktreeAcquire: (p: Record<string, unknown>, o?: DaemonCallOpts) =>
    daemonCall({ op: "worktree_acquire", ...p }, o),
  worktreeRelease: (p: Record<string, unknown>, o?: DaemonCallOpts) =>
    daemonCall({ op: "worktree_release", ...p }, o),
};
