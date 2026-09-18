// harnessd.cpp — Unix-socket daemon serving the C ABI to the TS proxy.
//
// Protocol (one JSON object per line, UTF-8):
//   client -> server line 1: {"token": "..."}            (auth envelope)
//   client -> server line N: {"op": "<op>", ...params}    (op envelope)
//   server -> client:        {"ok": true, ...} | {"ok": false, "error": "..."}
//
// Ops: check_split | report | bus_post | sandbox_exec |
//      worktree_acquire | worktree_release | backend_label | ping |
//      jev_status | jev_route
// Jev note: the daemon holds NO TypeSafe key. jev_status reports routing
// availability/policy; jev_route enforces the hard B^D budget check BEFORE
// any Jev/LLM dispatch (deny => caller must not dispatch remotely).
// Token source: HARNESSD_TOKEN env (required). Socket path: argv[1] or
// $HARNESSD_SOCK or /tmp/harnessd.sock. Config: argv[2] JSON or
// $HARNESSD_CONFIG_JSON. Single-threaded accept loop, per-conn line budget
// 1 MiB, daemon binds 0600-equivalent Unix socket (no TCP, loopback-only by
// construction — no ingress surface).

#include "harness_c.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
volatile bool g_stop = false;
void onSig(int) { g_stop = true; }

std::string jfield(const std::string& j, const std::string& key) {
  std::string q = "\"" + key + "\"";
  auto p = j.find(q);
  if (p == std::string::npos) return {};
  p = j.find(':', p + q.size());
  if (p == std::string::npos) return {};
  auto s = j.find('"', p);
  if (s == std::string::npos) return {};
  std::string out;
  for (std::size_t i = s + 1; i < j.size(); ++i) {
    char c = j[i];
    if (c == '\\' && i + 1 < j.size()) {
      char n = j[++i];
      if (n == 'n') out += '\n';
      else out += n;
      continue;
    }
    if (c == '"') break;
    out += c;
  }
  return out;
}

long jnum(const std::string& j, const std::string& key, long dflt = 0) {
  std::string q = "\"" + key + "\"";
  auto p = j.find(q);
  if (p == std::string::npos) return dflt;
  p = j.find(':', p + q.size());
  if (p == std::string::npos) return dflt;
  return std::atol(j.c_str() + p + 1);
}

std::string jesc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o;
}

bool sendAll(int fd, const std::string& s) {
  std::size_t n = 0;
  while (n < s.size()) {
    ssize_t w = ::write(fd, s.data() + n, s.size() - n);
    if (w <= 0) return false;
    n += static_cast<std::size_t>(w);
  }
  return true;
}

std::string readLine(int fd, std::size_t cap = 1 << 20) {
  std::string out;
  char c;
  while (out.size() < cap) {
    ssize_t r = ::read(fd, &c, 1);
    if (r <= 0) break;
    if (c == '\n') break;
    out += c;
  }
  return out;
}

// Constant-time token compare (auth envelope must not leak length early).
bool tokenEq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned>(a[i] ^ b[i]);
  }
  return diff == 0;
}

std::string dispatch(harness_host_t* host, const std::string& line) {
  std::string op = jfield(line, "op");
  char buf[1 << 20];
  int trunc = 0;
  int rc = HARNESS_C_ERR;
  std::string payload;
  if (op == "ping") {
    return "{\"ok\":true,\"pong\":true}";
  } else if (op == "backend_label") {
    rc = harness_backend_label(buf, sizeof(buf), &trunc);
    if (rc == 0) {
      return std::string("{\"ok\":true,\"label\":\"") + jesc(buf) + "\"}";
    }
  } else if (op == "check_split") {
    std::string models = "{\"models\":[" + jfield(line, "models_raw") + "]}";
    rc = harness_check_split(
        host, jfield(line, "task_id").c_str(), (int)jnum(line, "parent_depth"),
        (int)jnum(line, "breadth", 1), (int)jnum(line, "resplit_count"),
        (int)jnum(line, "parent_atomic"), (int)jnum(line, "parent_split"),
        models.c_str(), jnum(line, "tree_used"), jnum(line, "day_used"), buf,
        sizeof(buf), &trunc);
    if (rc == 0) {
      payload.assign(buf);
      return "{\"ok\":true,\"result\":" + payload + "}";
    }
  } else if (op == "jev_status") {
    rc = harness_jev_status(buf, sizeof(buf), &trunc);
    if (rc == 0) {
      payload.assign(buf);
      return "{\"ok\":true,\"result\":" + payload + "}";
    }
  } else if (op == "jev_route") {
    rc = harness_jev_route(host, (int)jnum(line, "parent_depth"),
                           (int)jnum(line, "breadth", 1), jnum(line, "tree_used"),
                           jnum(line, "day_used"), buf, sizeof(buf), &trunc);
    if (rc == 0) {
      payload.assign(buf);
      return "{\"ok\":true,\"result\":" + payload + "}";
    }
  } else if (op == "report") {
    rc = harness_report(host, line.c_str(), buf, sizeof(buf), &trunc);
    if (rc == 0) {
      payload.assign(buf);
      return "{\"ok\":true,\"result\":" + payload + "}";
    }
  } else if (op == "bus_post") {
    rc = harness_bus_post(host, line.c_str(), buf, sizeof(buf), &trunc);
    if (rc == 0) {
      payload.assign(buf);
      return "{\"ok\":true,\"result\":" + payload + "}";
    }
  } else if (op == "sandbox_exec") {
    rc = harness_sandbox_exec(host, line.c_str(), buf, sizeof(buf), &trunc);
    if (rc == 0) {
      payload.assign(buf);
      return "{\"ok\":true,\"result\":" + payload + "}";
    }
  } else if (op == "worktree_acquire") {
    rc = harness_worktree_acquire(
        host, jfield(line, "team").c_str(), jfield(line, "task_id").c_str(),
        (int)jnum(line, "attempt", 1), jfield(line, "base_branch").c_str(),
        (int)jnum(line, "flat"), buf, sizeof(buf), &trunc);
    if (rc == 0) {
      payload.assign(buf);
      return "{\"ok\":true,\"result\":" + payload + "}";
    }
  } else if (op == "worktree_release") {
    rc = harness_worktree_release(host, jfield(line, "team").c_str(),
                                  jfield(line, "task_id").c_str(),
                                  (int)jnum(line, "merged"), buf, sizeof(buf),
                                  &trunc);
    if (rc == 0) {
      payload.assign(buf);
      return "{\"ok\":true,\"result\":" + payload + "}";
    }
  } else {
    return "{\"ok\":false,\"error\":\"unknown op\"}";
  }
  (void)trunc;
  return "{\"ok\":false,\"error\":\"handler failed\"}";
}
}  // namespace

int main(int argc, char** argv) {
  const char* token = ::getenv("HARNESSD_TOKEN");
  if (!token || !*token) {
    std::fprintf(stderr, "harnessd: HARNESSD_TOKEN required\n");
    return 2;
  }
  // Socket path precedence: argv[1] (BARE path, no --socket flag),
  // then $HARNESSD_SOCK, then /tmp/harnessd.sock.
  // Exact invocations:
  //   HARNESSD_TOKEN=... harnessd /tmp/hd.sock
  //   HARNESSD_TOKEN=... HARNESSD_SOCK=/tmp/hd.sock harnessd
  std::string sockPath = (argc > 1) ? argv[1] : "";
  const char* sockSrc = "argv[1]";
  if (sockPath.empty()) {
    const char* e = ::getenv("HARNESSD_SOCK");
    sockPath = (e && *e) ? e : "/tmp/harnessd.sock";
    sockSrc = (e && *e) ? "HARNESSD_SOCK" : "default";
  }
  std::string config = (argc > 2) ? argv[2] : "";
  if (config.empty()) {
    const char* e = ::getenv("HARNESSD_CONFIG_JSON");
    if (e) config = e;
  }
  ::signal(SIGINT, onSig);
  ::signal(SIGTERM, onSig);
  ::signal(SIGPIPE, SIG_IGN);

  harness_host_t* host = harness_host_create(config.c_str());
  if (!host) {
    std::fprintf(stderr, "harnessd: host create failed\n");
    return 1;
  }

  ::unlink(sockPath.c_str());
  int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (srv < 0) return 1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::perror("bind");
    return 1;
  }
  ::chmod(sockPath.c_str(), 0600);
  if (::listen(srv, 8) != 0) return 1;
  std::printf("harnessd sock=%s (src=%s) ready\n", sockPath.c_str(), sockSrc);
  std::fflush(stdout);

  while (!g_stop) {
    int c = ::accept(srv, nullptr, nullptr);
    if (c < 0) {
      if (g_stop) break;
      continue;
    }
    std::string auth = readLine(c);
    if (!tokenEq(jfield(auth, "token"), token)) {
      sendAll(c, "{\"ok\":false,\"error\":\"bad token\"}\n");
      ::close(c);
      continue;
    }
    sendAll(c, "{\"ok\":true,\"auth\":\"ok\"}\n");
    while (!g_stop) {
      std::string line = readLine(c);
      if (line.empty()) break;
      std::string resp = dispatch(host, line);
      if (!sendAll(c, resp + "\n")) break;
    }
    ::close(c);
  }
  ::close(srv);
  ::unlink(sockPath.c_str());
  harness_host_destroy(host);
  return 0;
}
