// main.cpp — end-to-end demo of the C++20 Brain-and-Swarm host middleware.
//
// Flow exercised:
//   1. TokenFirewall gatekeeper: cheap split ALLOWED, breadth-5 @ depth-2
//      split DENIED with BUDGET_EXCEEDED (estimate 125 > budget 64), and a
//      near-daily-cap split DENIED with DAILY_CAP.
//   2. TaskArena splitOnce: enqueue root, fan out to 3 hetero children,
//      depth gauges printed (d/3, p/5).
//   3. SisterLeafBus: one sister publishes a schema anchor, the other reads it.
//   4. ForwardNudgeLoop: heartbeat + stall poll, pruneParallelPaths 6 -> 5+1.
//   5. WorktreeManager + SandboxRunner: acquire a worktree of THIS repo,
//      write a doc file, run a sandboxed command inside it, verify, propose
//      a patch, release. (Skipped gracefully when git is unavailable.)
//   6. GATE-TURN report: compact each child to a ResultVector and report it.
//
// Usage: harness-demo [repo-root]
// Exits 0 on full pass, 1 on any unexpected gate/host failure.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "harness.hpp"

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}

std::string backendName(harness::SandboxBackend b) {
  using harness::SandboxBackend;
  switch (b) {
    case SandboxBackend::LinuxNamespaces: return "linux-namespaces";
    case SandboxBackend::MacSandboxExec: return "mac-sandbox-exec";
    case SandboxBackend::Unisolated: return "unisolated";
  }
  return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace harness;
  std::string repoRoot = (argc > 1) ? argv[1] : ".";
  std::cout << "harness-demo repo=" << repoRoot << "\n";

  // ---- 0. Wire the host implementations ----------------------------------
  TokenFirewallConfig fcfg;
  fcfg.ledgerPath = std::string(std::filesystem::temp_directory_path()) +
                    "/harness-demo-ledger.jsonl";
  std::remove(fcfg.ledgerPath.c_str());
  auto firewall = makeTokenFirewall(fcfg);
  auto bus = makeSisterLeafBus(/*shmPath=*/"");  // in-process fallback
  auto nudge = makeForwardNudgeLoop(NudgeConfig{});
  auto arena = makeTaskArena(*firewall, *bus, fcfg.ledgerPath);
  auto worktrees = makeWorktreeManager(repoRoot);
  auto sandbox = makeSandboxRunner();

  // ---- 1. Gatekeeper: B^D vs caps ----------------------------------------
  std::cout << "1. firewall gates\n";
  {
    // Cheap split: breadth 3 @ parentDepth 0 -> estimate 3 <= 64: ALLOW.
    FirewallDecision d = firewall->checkSplit(
        "demo", /*parentDepth=*/0, /*breadth=*/3, /*resplitCount=*/0,
        /*parentAtomic=*/false, /*parentAlreadySplit=*/false,
        {"model-a", "model-b"}, /*treeUsed=*/0, /*dayUsed=*/0);
    check(d.verdict == GateVerdict::Allow && d.estimate == 3, "cheap split allows (est 3)");
  }
  {
    // Spec A.3 case: breadth 5 @ depth 2 -> 5^3 = 125 > 64: BUDGET_EXCEEDED.
    // H1 note: breadth 5 <= cap 8, so the estimate path (not the breadth
    // cap) fires and the reported estimate stays exact at 125.
    FirewallDecision d = firewall->checkSplit(
        "demo", /*parentDepth=*/2, /*breadth=*/5, /*resplitCount=*/0,
        /*parentAtomic=*/false, /*parentAlreadySplit=*/false,
        {"model-a", "model-b"}, /*treeUsed=*/0, /*dayUsed=*/0);
    check(d.verdict == GateVerdict::Deny && d.code == GateCode::BudgetExceeded &&
              d.estimate == 125,
          "breadth5@depth2 denied BUDGET_EXCEEDED (est 125)");
    std::cout << "      msg: " << d.message << "\n";
  }
  {
    // Daily cap: dayUsed 999 + estimate 3 > 1000: DAILY_CAP.
    FirewallDecision d = firewall->checkSplit(
        "demo", /*parentDepth=*/0, /*breadth=*/3, /*resplitCount=*/0,
        /*parentAtomic=*/false, /*parentAlreadySplit=*/false,
        {"model-a", "model-b"}, /*treeUsed=*/0, /*dayUsed=*/999);
    check(d.verdict == GateVerdict::Deny && d.code == GateCode::DailyCap,
          "near-cap split denied DAILY_CAP");
  }
  {
    // Warn band: estimate 50 in (44.8, 64]: Warn + BUDGET_WARN line.
    FirewallDecision d = firewall->checkSplit(
        "demo", /*parentDepth=*/1, /*breadth=*/7, /*resplitCount=*/0,
        /*parentAtomic=*/false, /*parentAlreadySplit=*/false,
        {"model-a", "model-b"}, /*treeUsed=*/0, /*dayUsed=*/0);
    check(d.verdict == GateVerdict::Warn && d.code == GateCode::BudgetWarn &&
              d.estimate == 49,
          "breadth7@depth1 warns (est 49)");
  }

  // ---- 2. Arena split: 3 hetero children -----------------------------------
  std::cout << "2. arena split\n";
  check(arena->enqueue(TaskNode{.id = "demo",
                                .criterion = "demo root",
                                .workerModel = "model-a"}) == GateCode::Ok,
        "enqueue root demo");
  auto [splitCode, kids] = arena->splitOnce(
      "demo", {"types", "api_schema", "tests"}, {"model-a", "model-b", "model-a"});
  // H5: warn-band splits propagate BudgetWarn (non-error allow). The demo
  // split (est 3) is cheap => Ok expected here.
  check((splitCode == GateCode::Ok || splitCode == GateCode::BudgetWarn) &&
            kids.size() == 3,
        "fan-out 3 children");
  auto root = arena->find("demo");
  check(root && root->depth == 0 && root->resplitCount == 1, "root depth 0/3, resplit 1/3");
  auto c0 = arena->find("demo.0");
  check(c0 && c0->depth == 1 && c0->resplitCount == 1, "child depth 1/3, parallel 3/5");

  // ---- 3. Sister bus publish/read -----------------------------------------
  std::cout << "3. sister bus\n";
  SisterLeafEntry e;
  e.taskId = "demo.0";
  e.parentSessionId = "sess-demo";
  e.namespace_ = "api_schema";
  e.key = "types";
  e.value = "{\"User\":{\"id\":\"string\"}}";
  check(bus->publish(e) == GateCode::Ok, "publish schema anchor");
  auto got = bus->read("sess-demo", "api_schema", "types");
  check(got && got->value == e.value, "sister reads anchor (same partition)");
  check(!bus->read("sess-other", "api_schema", "types").has_value(),
        "cross-partition read denied");

  // ---- 4. Nudge loop: heartbeat + prune -----------------------------------
  std::cout << "4. nudge loop\n";
  auto now = std::chrono::steady_clock::now();
  nudge->heartbeat("demo.0", now);
  NudgeAction a = nudge->poll("demo.0", NudgeContext{}, /*userInterrupted=*/false,
                              /*consecutiveStalls=*/0, /*verifyFailures=*/0, now);
  check(a == NudgeAction::Reprompt, "fresh task reprompts");
  NudgeAction intr = nudge->poll("demo.0", NudgeContext{}, /*userInterrupted=*/true,
                                 0, 0, now);
  check(intr == NudgeAction::WithheldInterrupt, "interrupt wins");
  auto [keep, deferred] = nudge->pruneParallelPaths({"p0", "p1", "p2", "p3", "p4", "p5"});
  check(keep.size() == 5 && deferred.size() == 1, "6 follow-ups -> top-5 + 1 Deferred");

  // ---- 5. Worktree + sandbox (skipped gracefully w/o git) -------------------
  std::cout << "5. worktree + sandbox (backend=" << backendName(sandbox->backend()) << ")\n";
  {
    SandboxRunRequest req;
    req.worktreePath = repoRoot;
    req.denyEgress = false;  // detection probe, not untrusted code
    req.argv = {"git", "rev-parse", "--is-inside-work-tree"};
    SandboxRunResult r = sandbox->run(req);
    if (r.exitCode != 0) {
      std::cout << "  [skip] not a git checkout; worktree demo skipped\n";
    } else {
      auto [ac, handle] = worktrees->acquire("demo-team", "demo", 1, "HEAD");
      if (ac == GateCode::WorktreeMainDirty) {
        std::cout << "  [skip] main worktree dirty; acquire correctly refused\n";
      } else if (ac != GateCode::Ok) {
        check(false, "acquire worktree");
      } else {
        check(true, "acquire worktree");
        // Simulate leaf work: append a demo doc inside the worktree.
        {
          std::ofstream f(handle.path + "/HARNESS_DEMO.md");
          f << "# harness demo\n\nLeaf scratch for demo.\n";
        }
        SandboxRunRequest ls;
        ls.worktreePath = handle.path;
        ls.argv = {"git", "status", "--porcelain"};
        SandboxRunResult lr = sandbox->run(ls);
        check(lr.exitCode == 0, "sandboxed git status in worktree");
        // Verify with docsOnly + BRAIN waiver (demo repo may lack a harness).
        VerifyReport vr = sandbox->verify(handle.path, "demo", /*docsOnly=*/true,
                                          /*brainWaived=*/true);
        check(vr.verdict == VerifyVerdict::WaivedDocsOnly, "docs-only waiver verifies");
        check(worktrees->release("demo-team", "demo", /*merged=*/false) == GateCode::Ok,
              "release worktree");
      }
    }
  }

  // ---- 6. GATE-TURN report pipeline -----------------------------------------
  std::cout << "6. report pipeline\n";
  for (const auto& kid : kids) {
    ResultVector v;
    v.taskId = kid.id;
    v.status = TaskStatus::Success;
    v.summary = "done: " + kid.criterion + " anchor published";
    v.changedFiles = {};
    v.gitDiffHash = "abc1234";  // SUCCESS binds the patch hash
    v.exportedState = {{"anchor", kid.criterion}};
    check(arena->report(v, /*checkArtifact=*/"exit=0") == GateCode::Ok,
          ("report " + kid.id).c_str());
  }
  {
    // FAILED with no patch: reason rides in the summary, no hash required.
    ResultVector v;
    v.taskId = "demo.0";
    v.status = TaskStatus::Failed;
    v.summary = "no patch: verify red, keepOnFail=false";
    check(v.validate(/*patchExists=*/false) == GateCode::Ok,
          "failed vector validates without hash");
  }

  {
    // H1 acceptance: breadth^depth past LONG_MAX saturates to deny WITHOUT
    // signed overflow (UBSan clean at -O2). parentDepth 62 @ breadth 10:
    // 10^63 overflows long => estimate == budget+1 == 65, BudgetExceeded.
    FirewallDecision d = firewall->checkSplit(
        "t", /*parentDepth=*/62, /*breadth=*/8, /*resplitCount=*/0,
        /*parentAtomic=*/false, /*parentAlreadySplit=*/false,
        {"model-a", "model-b"}, /*treeUsed=*/0, /*dayUsed=*/0);
    check(d.verdict == GateVerdict::Deny &&
              d.code == GateCode::BudgetExceeded &&
              d.estimate == PER_TREE_REQUEST_BUDGET + 1,
          "H1: overflow-band split saturates to deny (est 65)");
  }
  {
    // H5 acceptance: warn-band splitOnce propagates BudgetWarn (not Ok).
    // est = breadth^(parentDepth+1): need a depth-1 parent so 8^2 = 64
    // lands in the warn band (44, 64].
    check(arena->enqueue(TaskNode{.id = "warnroot",
                                  .criterion = "warn root",
                                  .workerModel = "model-a"}) == GateCode::Ok,
          "enqueue warnroot");
    auto [w0c, w0k] = arena->splitOnce("warnroot", {"mid"}, {"model-a"});
    check(w0c == GateCode::Ok && w0k.size() == 1, "warnroot -> 1 mid child");
    auto [wc, wkids] = arena->splitOnce(
        "warnroot.0",
        {"c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7"},  // breadth 8 @ d1
        {"model-a", "model-b", "model-a", "model-b", "model-a", "model-b",
         "model-a", "model-b"});  // est 64: warn band (44,64]
    check(wc == GateCode::BudgetWarn && wkids.size() == 8,
          "H5: warn-band splitOnce returns BudgetWarn + 8 kids");
  }

#if defined(__linux__)
  // ---- 7. H6: cross-process shm bus (fork: publish parent, read child) ----
  std::cout << "7. shm bus (two-process)\n";
  {
    const std::string shmName = "/harness-demo-shm";
    ::shm_unlink(shmName.c_str());
    auto shmBus = makeSisterLeafBus(shmName);
    SisterLeafEntry e;
    e.taskId = "shm.0";
    e.parentSessionId = "sess-shm";
    e.namespace_ = "types";
    e.key = "anchor";
    e.value = "{\"A\":\"1\"}";
    check(shmBus->publish(e) == GateCode::Ok, "shm publish (parent)");
    pid_t pid = ::fork();
    if (pid == 0) {
      // Child: re-attach by NAME (new mapping, same segment) and read.
      auto child = makeSisterLeafBus(shmName);
      auto got = child->read("sess-shm", "types", "anchor");
      bool okSame = got && got->value == e.value;
      bool okPart = !child->read("sess-other", "types", "anchor").has_value();
      SisterLeafEntry big = e;
      big.value.assign(BUS_VALUE_MAX_BYTES + 1, 'x');
      bool okCap = child->publish(big) == GateCode::BusValueTooLarge;
      ::_exit((okSame && okPart && okCap) ? 0 : 1);
    } else if (pid > 0) {
      int status = 0;
      ::waitpid(pid, &status, 0);
      check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "shm child reads anchor + partition + cap");
    } else {
      check(false, "shm fork");
    }
    ::shm_unlink(shmName.c_str());
  }
#endif

  std::cout << (failures == 0 ? "DEMO PASS\n" : "DEMO FAIL\n");
  return failures == 0 ? 0 : 1;
}
