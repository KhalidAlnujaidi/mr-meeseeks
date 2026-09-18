// sister_shm.cpp — H6: cross-process SisterLeafBus over POSIX shm (Linux).
//
// Segment layout: SisterLeafShmHeader (with process-shared ROBUST mutex +
// publish semaphore) followed by SISTER_SHM_MAX_ENTRIES slots.
// Publish is last-writer-wins per (parentSessionId, namespace, key) within
// one partition; cross-partition reads fail closed. Magic/version mismatch
// refuses attach instead of reinterpreting garbage.
//
// Robust-mutex discipline: lock with pthread_mutex_lock; on
// EOWNERDEAD call pthread_mutex_consistent() (previous owner died holding
// the lock — its partial slot write is discarded by re-scanning) and
// continue. ENOTRECOVERABLE => unrecoverable; refuse ops until re-created.

#include "harness.hpp"

#if HARNESS_OS_LINUX

#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace harness {
namespace {

std::uint64_t nowMsShm() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void copyField(char* dst, std::size_t cap, const std::string& s) {
  std::memset(dst, 0, cap);
  std::memcpy(dst, s.data(), std::min(cap - 1, s.size()));
}

std::string fromField(const char* src, std::size_t cap) {
  std::size_t n = 0;
  while (n < cap && src[n] != '\0') ++n;
  return std::string(src, n);
}

void slotToEntry(const SisterLeafMemoryBuffer& s, SisterLeafEntry& e) {
  e.taskId = fromField(s.taskId, SISTER_TASK_ID_MAX);
  e.parentSessionId = fromField(s.parentSessionId, SISTER_SESSION_ID_MAX);
  e.namespace_ = fromField(s.namespace_, SISTER_NAMESPACE_MAX);
  e.key = fromField(s.key, SISTER_KEY_MAX);
  e.updatedBy = fromField(s.updatedBy, SISTER_WORKER_ID_MAX);
  e.visibility = (s.visibility == 1) ? Visibility::PromotableToL1
                                     : Visibility::SisterOnly;
  e.timestampMs = s.timestampMs;
  e.value.assign(s.value, s.valueLen);
}

bool slotMatch(const SisterLeafMemoryBuffer& s, const std::string& p,
               const std::string& n, const std::string& k) {
  // Compare fixed fields without allocating: length-prefixed semantics over
  // NUL-padded storage.
  auto fieldEq = [](const char* f, std::size_t cap, const std::string& v) {
    if (v.size() >= cap) return false;
    if (std::memcmp(f, v.data(), v.size()) != 0) return false;
    for (std::size_t i = v.size(); i < cap; ++i) {
      if (f[i] != '\0') return false;
    }
    return true;
  };
  return fieldEq(s.parentSessionId, SISTER_SESSION_ID_MAX, p) &&
         fieldEq(s.namespace_, SISTER_NAMESPACE_MAX, n) &&
         fieldEq(s.key, SISTER_KEY_MAX, k);
}

}  // namespace

class ShmSisterLeafBus : public SisterLeafBus {
 public:
  // attach==false => create+init (first opener); attach==true => open only
  // and verify magic/version. Factory tries open-first, create-on-ENOENT.
  ShmSisterLeafBus(const std::string& name, bool creator, int fd, void* base)
      : name_(name), creator_(creator), fd_(fd), base_(static_cast<char*>(base)) {
    header_ = reinterpret_cast<SisterLeafShmHeader*>(base_);
    slots_ = reinterpret_cast<SisterLeafMemoryBuffer*>(
        base_ + sizeof(SisterLeafShmHeader));
  }

  ~ShmSisterLeafBus() override {
    ::munmap(base_, sisterLeafShmSize());
    ::close(fd_);
  }

  static std::unique_ptr<ShmSisterLeafBus> open(const std::string& name) {
    const std::size_t size = sisterLeafShmSize();
    int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd >= 0) {
      // Existing segment: map and verify BEFORE trusting a single byte (H6).
      void* base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (base == MAP_FAILED) {
        ::close(fd);
        return nullptr;
      }
      auto* h = static_cast<SisterLeafShmHeader*>(base);
      if (h->magic != SISTER_LEAF_SHM_MAGIC ||
          h->version != SISTER_LEAF_SHM_VERSION) {
        ::munmap(base, size);
        ::close(fd);
        return nullptr;  // corrupt / older version: refuse, never reinterpret
      }
      return std::unique_ptr<ShmSisterLeafBus>(
          new ShmSisterLeafBus(name, false, fd, base));
    }
    // Create path (first publisher).
    fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return nullptr;
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
      ::close(fd);
      ::shm_unlink(name.c_str());
      return nullptr;
    }
    void* base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
      ::close(fd);
      ::shm_unlink(name.c_str());
      return nullptr;
    }
    std::memset(base, 0, size);
    auto* h = static_cast<SisterLeafShmHeader*>(base);
    h->magic = SISTER_LEAF_SHM_MAGIC;
    h->version = SISTER_LEAF_SHM_VERSION;
    h->entryCount = 0;
    h->seq = 0;
    // Process-shared ROBUST mutex: a publisher that dies mid-slot leaves
    // EOWNERDEAD for the next locker instead of a wedged bus.
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
#ifdef PTHREAD_MUTEX_ROBUST
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
#endif
    pthread_mutex_init(&h->lock, &ma);
    pthread_mutexattr_destroy(&ma);
    sem_init(&h->publishSem, /*pshared=*/1, /*value=*/0);
    // Publish the mapping before any other process can observe the header.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return std::unique_ptr<ShmSisterLeafBus>(
        new ShmSisterLeafBus(name, true, fd, base));
  }

  GateCode publish(const SisterLeafEntry& entry) override {
    if (entry.value.size() > BUS_VALUE_MAX_BYTES) {
      return GateCode::BusValueTooLarge;  // entry unchanged
    }
    if (!lockBus()) return GateCode::WorktreeLocked;
    GateCode rc = GateCode::Ok;
    // Find existing slot for the triple, else claim a free slot.
    SisterLeafMemoryBuffer* slot = nullptr;
    SisterLeafMemoryBuffer* free = nullptr;
    for (std::size_t i = 0; i < SISTER_SHM_MAX_ENTRIES; ++i) {
      SisterLeafMemoryBuffer& s = slots_[i];
      if (s.magic == 0) {
        if (!free) free = &s;
        continue;
      }
      if (slotMatch(s, entry.parentSessionId, entry.namespace_, entry.key)) {
        slot = &s;
        break;
      }
    }
    if (!slot) {
      if (!free) {
        // Bus full: evict slot 0 (oldest-by-position LRU approximation).
        // Bounded and documented; 64 slots >> 3-sister working sets.
        free = &slots_[0];
      }
      slot = free;
      // Claim with magic==0 while filling; commit sets magic LAST under the
      // robust mutex, so a publisher that dies mid-fill leaves an invisible
      // slot (EOWNERDEAD recovery just marks consistent and continues).
      std::memset(slot, 0, sizeof(*slot));
      copyField(slot->parentSessionId, SISTER_SESSION_ID_MAX, entry.parentSessionId);
      copyField(slot->namespace_, SISTER_NAMESPACE_MAX, entry.namespace_);
      copyField(slot->key, SISTER_KEY_MAX, entry.key);
      if (header_->entryCount < SISTER_SHM_MAX_ENTRIES) header_->entryCount++;
    }
    copyField(slot->taskId, SISTER_TASK_ID_MAX, entry.taskId);
    copyField(slot->updatedBy, SISTER_WORKER_ID_MAX,
              entry.updatedBy.empty() ? "host" : entry.updatedBy);
    slot->visibility =
        (entry.visibility == Visibility::PromotableToL1) ? 1 : 0;
    slot->timestampMs = nowMsShm();
    slot->valueLen = static_cast<std::uint32_t>(entry.value.size());
    std::memcpy(slot->value, entry.value.data(), entry.value.size());
    __atomic_thread_fence(__ATOMIC_RELEASE);
    slot->magic = SISTER_LEAF_SHM_MAGIC;  // commit point
    slot->version = SISTER_LEAF_SHM_VERSION;
    header_->seq++;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    sem_post(&header_->publishSem);
    unlockBus();
    return rc;
  }

  std::optional<SisterLeafEntry> read(const std::string& parentSessionId,
                                      const std::string& namespace_,
                                      const std::string& key) override {
    if (!lockBus()) return std::nullopt;
    std::optional<SisterLeafEntry> out;
    for (std::size_t i = 0; i < SISTER_SHM_MAX_ENTRIES; ++i) {
      const SisterLeafMemoryBuffer& s = slots_[i];
      if (s.magic == 0) continue;
      if (slotMatch(s, parentSessionId, namespace_, key)) {
        SisterLeafEntry e;
        slotToEntry(s, e);
        out = e;
        break;
      }
    }
    unlockBus();
    return out;  // fail-closed: cross-partition triples never match
  }

  std::map<std::string, SisterLeafEntry> listNamespace(
      const std::string& parentSessionId, const std::string& namespace_) override {
    std::map<std::string, SisterLeafEntry> out;
    if (!lockBus()) return out;
    for (std::size_t i = 0; i < SISTER_SHM_MAX_ENTRIES; ++i) {
      const SisterLeafMemoryBuffer& s = slots_[i];
      if (s.magic == 0) continue;
      SisterLeafEntry e;
      slotToEntry(s, e);
      if (e.parentSessionId == parentSessionId && e.namespace_ == namespace_) {
        out.emplace(e.key, e);
      }
    }
    unlockBus();
    return out;
  }

 private:
  bool lockBus() {
    int r = pthread_mutex_lock(&header_->lock);
    if (r == 0) return true;
    if (r == EOWNERDEAD) {
      // Previous owner died mid-publish: mark consistent. Uncommitted slot
      // claims have magic==0 (commit sets magic LAST), so no torn slot is
      // ever visible; just continue.
      pthread_mutex_consistent(&header_->lock);
      return true;
    }
    return false;
  }
  void unlockBus() { pthread_mutex_unlock(&header_->lock); }

  std::string name_;
  bool creator_;
  int fd_;
  char* base_;
  SisterLeafShmHeader* header_;
  SisterLeafMemoryBuffer* slots_;
};

}  // namespace harness

namespace harness {
// Factory entry used by makeSisterLeafBus (declared locally at the call
// site to avoid dragging POSIX-shm types into the public header).
std::unique_ptr<SisterLeafBus> openShmSisterLeafBus(const std::string& name) {
  return ShmSisterLeafBus::open(name);
}
}  // namespace harness

#endif  // HARNESS_OS_LINUX
