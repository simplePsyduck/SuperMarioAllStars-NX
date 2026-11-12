// The job queue on std::thread: for the host build and the tests (the Switch build has its own, on libnx's
// threads: platform/switch/jobqueue_nx.cpp).
#pragma once
#include "core/jobqueue.h"
#include <condition_variable>
#include <mutex>
#include <thread>

struct StdJobQueuePlatform {
  std::thread th;
  std::mutex mu;
  std::condition_variable_any work, done;

  bool spawn(void (*entry)(void*), void* arg) {
    try {
      th = std::thread(entry, arg);
    } catch (...) {
      return false;
    }
    return true;
  }
  void join() {
    if (th.joinable()) th.join();
  }
  void lock() { mu.lock(); }
  void unlock() { mu.unlock(); }
  bool waitWork(int64_t ns) {
    if (ns < 0) {
      work.wait(mu);
      return true;
    }
    return work.wait_for(mu, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout;
  }
  void notifyWork() { work.notify_all(); }
  bool waitDone(int64_t ns) { return done.wait_for(mu, std::chrono::nanoseconds(ns)) == std::cv_status::no_timeout; }
  void notifyDone() { done.notify_all(); }
};

// The factory for Ppu::queueFactory on the host.
inline JobQueue* makeStdJobQueue(size_t jobSize, int capacity, JobQueue::Fn fn, void* user) {
  auto* q = new ThreadedJobQueue<StdJobQueuePlatform>();
  if (!q->start(jobSize, capacity, fn, user)) {
    delete q;
    return nullptr;
  }
  return q;
}
