// git_native.cpp — native libgit2 worktree add + diff backend.
//
// Replaces the shell-`git worktree add` path when HARNESS_USE_LIBGIT2 is set:
// worktree add via git_worktree_add(), base-SHA resolution via revparse, and
// worktree diff via git_diff_tree_to_workdir. Shell remains the fallback when
// libgit2 is absent (HARNESS_USE_LIBGIT2 unset) — identical GateCode surface.

#include "harness.hpp"

#ifdef HARNESS_USE_LIBGIT2
#include <git2.h>

#include <cstring>

namespace harness {
namespace git_native {

namespace {
struct LibInit {
  LibInit() { git_libgit2_init(); }
  ~LibInit() { git_libgit2_shutdown(); }
};
}  // namespace

std::string resolveBaseShaNative(const std::string& repoRoot,
                                 const std::string& rev) {
  LibInit init;
  git_repository* repo = nullptr;
  if (git_repository_open(&repo, repoRoot.c_str()) != 0) return {};
  git_object* obj = nullptr;
  std::string sha;
  if (git_revparse_single(&obj, repo, rev.c_str()) == 0) {
    const git_oid* oid = git_object_id(obj);
    char buf[GIT_OID_HEXSZ + 1];
    git_oid_tostr(buf, sizeof(buf), oid);
    sha = buf;
    git_object_free(obj);
  }
  git_repository_free(repo);
  return sha;
}

int addWorktreeNative(const std::string& repoRoot, const std::string& branch,
                      const std::string& path, const std::string& baseSha,
                      std::string& errOut) {
  LibInit init;
  git_repository* repo = nullptr;
  if (git_repository_open(&repo, repoRoot.c_str()) != 0) {
    errOut = "open repo failed";
    return -1;
  }
  git_oid baseOid;
  if (git_oid_fromstr(&baseOid, baseSha.c_str()) != 0) {
    git_repository_free(repo);
    errOut = "bad base sha";
    return -1;
  }
  git_commit* baseCommit = nullptr;
  if (git_commit_lookup(&baseCommit, repo, &baseOid) != 0) {
    git_repository_free(repo);
    errOut = "base commit lookup failed";
    return -1;
  }
  // CLI `git worktree add <path> -b <branch>` sanitizes the admin dir to
  // .git/worktrees/<basename-ish>; libgit2 instead uses `name` VERBATIM as
  // .git/worktrees/<name> WITHOUT recursive mkdir, so a slashed branch name
  // like "task/team/id-a1" fails with "failed to make directory". Fix: pass
  // a flattened worktree NAME (slashes -> dashes) while opts.ref still
  // points at the real (slashed) branch ref.
  std::string wtName = branch;
  for (char& ch : wtName)
    if (ch == '/') ch = '-';
  git_reference* newBranch = nullptr;
  if (git_branch_create(&newBranch, repo, branch.c_str(), baseCommit, 0) !=
      0) {
    const git_error* e = git_error_last();
    errOut = e ? e->message : "branch create failed";
    git_commit_free(baseCommit);
    git_repository_free(repo);
    return -1;
  }
  git_worktree_add_options opts;
  git_worktree_add_options_init(&opts, GIT_WORKTREE_ADD_OPTIONS_VERSION);
  opts.lock = 0;
  // NOTE: libgit2 >= 1.8 has opts.checkout_existing; 1.7 (apt) does not.
  // opts.ref = fresh newBranch already pins the checkout, so no extra flag
  // is needed on either version.
  opts.ref = newBranch;
  opts.checkout_options.checkout_strategy = GIT_CHECKOUT_SAFE;
  // NOTE: out param must be non-NULL (NULL => "invalid argument: 'out'").
  git_worktree* wt = nullptr;
  int rc =
      git_worktree_add(&wt, repo, wtName.c_str(), path.c_str(), &opts);
  if (rc == 0) {
    git_worktree_free(wt);
  } else {
    const git_error* e = git_error_last();
    errOut = e ? e->message : "worktree add failed";
  }
  git_reference_free(newBranch);
  git_commit_free(baseCommit);
  git_repository_free(repo);
  return rc;
}

// Diff worktree path vs base SHA: returns "stat\n---\nporcelain-ish names".
// Implemented via git_diff_tree_to_workdir so no shell is spawned.
int diffWorktreeNative(const std::string& repoRoot, const std::string& wtPath,
                       const std::string& baseSha, std::string& statOut,
                       std::string& namesOut, std::string& errOut) {
  LibInit init;
  git_repository* repo = nullptr;
  if (git_repository_open(&repo, repoRoot.c_str()) != 0) {
    errOut = "open repo failed";
    return -1;
  }
  git_oid oid;
  if (git_oid_fromstr(&oid, baseSha.c_str()) != 0) {
    git_repository_free(repo);
    errOut = "bad base sha";
    return -1;
  }
  git_commit* commit = nullptr;
  if (git_commit_lookup(&commit, repo, &oid) != 0) {
    git_repository_free(repo);
    errOut = "base lookup failed";
    return -1;
  }
  git_tree* tree = nullptr;
  if (git_commit_tree(&tree, commit) != 0) {
    git_commit_free(commit);
    git_repository_free(repo);
    errOut = "tree lookup failed";
    return -1;
  }
  git_diff* diff = nullptr;
  git_diff_options opts;
  std::memset(&opts, 0, sizeof(opts));
  opts.version = GIT_DIFF_OPTIONS_VERSION;
  opts.context_lines = 3;
  // Diff the BASE TREE against the worktree dir on disk.
  int rc = git_diff_tree_to_workdir(&diff, repo, tree, &opts);
  if (rc == 0) {
    git_diff_stats* stats = nullptr;
    if (git_diff_get_stats(&stats, diff) == 0) {
      git_diff_stats_format_t fmt =
          static_cast<git_diff_stats_format_t>(GIT_DIFF_STATS_FULL |
                                               GIT_DIFF_STATS_INCLUDE_SUMMARY);
      git_buf buf = {nullptr, 0, 0};
      if (git_diff_stats_to_buf(&buf, stats, fmt, 0) == 0 && buf.ptr) {
        statOut.assign(buf.ptr, buf.size);
      }
      git_buf_dispose(&buf);
      git_diff_stats_free(stats);
    }
    std::size_t n = git_diff_num_deltas(diff);
    for (std::size_t i = 0; i < n; ++i) {
      const git_diff_delta* d = git_diff_get_delta(diff, i);
      if (d && d->new_file.path) {
        namesOut += d->new_file.path;
        namesOut += '\n';
      }
    }
    // Include untracked worktree files (wtPath scan via workdir iterator).
    git_status_options sopts;
    std::memset(&sopts, 0, sizeof(sopts));
    sopts.version = GIT_STATUS_OPTIONS_VERSION;
    sopts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                  GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    git_status_list* sl = nullptr;
    if (git_status_list_new(&sl, repo, &sopts) == 0) {
      std::size_t sn = git_status_list_entrycount(sl);
      for (std::size_t i = 0; i < sn; ++i) {
        const git_status_entry* e = git_status_byindex(sl, i);
        if (e && e->status == GIT_STATUS_WT_NEW &&
            e->head_to_index == nullptr && e->index_to_workdir) {
          const char* p = e->index_to_workdir->new_file.path;
          if (p && namesOut.find(p) == std::string::npos) {
            namesOut += p;
            namesOut += '\n';
          }
        }
      }
      git_status_list_free(sl);
    }
    git_diff_free(diff);
  } else {
    const git_error* e = git_error_last();
    errOut = e ? e->message : "diff failed";
  }
  (void)wtPath;
  git_tree_free(tree);
  git_commit_free(commit);
  git_repository_free(repo);
  return rc;
}

}  // namespace git_native
}  // namespace harness

#endif  // HARNESS_USE_LIBGIT2
