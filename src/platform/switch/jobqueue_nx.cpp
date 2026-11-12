// The job queue on libnx's threads: see jobqueue_nx.h and core/jobqueue.h.
#include "platform/switch/jobqueue_nx.h"
#include <switch.h>
#include <cstdlib>

void logf(const char* fmt, ...);  // (the app's log file, smas_port.log: see app/app.cpp)

namespace {

struct NxPlatform {
  Thread th{};
  Mutex mu;
  CondVar work, done;
  void* stack = nullptr;
  bool started = false;

  NxPlatform() {
    mutexInit(&mu);
    condvarInit(&work);
    condvarInit(&done);
  }
  ~NxPlatform() { join(); }

  bool spawn(void (*entry)(void*), void* arg) {
    constexpr size_t kStack = 0x20000;  // (the work is small: a line's mix)
    stack = std::aligned_alloc(0x1000, kStack);
    if (!stack) return false;
    // (the emulation runs on core 0; the system keeps core 3)
    static const int kCores[] = {1, 2, -2};
    for (int core : kCores) {
      if (R_FAILED(threadCreate(&th, entry, arg, stack, kStack, 0x2C, core))) continue;
      if (R_SUCCEEDED(threadStart(&th))) {
        started = true;
        logf("video thread: started on core %d (the emulation is on core %u)", core, (unsigned)svcGetCurrentProcessorNumber());
        return true;
      }
      threadClose(&th);
    }
    logf("video thread: no thread could be started");
    std::free(stack);
    stack = nullptr;
    return false;
  }
  void join() {
    if (!started) return;
    threadWaitForExit(&th);
    threadClose(&th);
    std::free(stack);
    stack = nullptr;
    started = false;
  }
  void lock() { mutexLock(&mu); }
  void unlock() { mutexUnlock(&mu); }
  bool waitWork(int64_t ns) {
    if (ns < 0) {
      condvarWait(&work, &mu);
      return true;
    }
    return R_SUCCEEDED(condvarWaitTimeout(&work, &mu, (u64)ns));
  }
  void notifyWork() { condvarWakeAll(&work); }
  bool waitDone(int64_t ns) { return R_SUCCEEDED(condvarWaitTimeout(&done, &mu, (u64)ns)); }
  void notifyDone() { condvarWakeAll(&done); }
};

}  // namespace

JobQueue* makeNxJobQueue(size_t jobSize, int capacity, JobQueue::Fn fn, void* user) {
  auto* q = new ThreadedJobQueue<NxPlatform>();
  if (!q->start(jobSize, capacity, fn, user)) {
    delete q;
    return nullptr;
  }
  return q;
}
