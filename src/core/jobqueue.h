// A queue of fixed-size jobs run, in order, by one worker thread: the emulation thread fills a slot
// (beginJob / endJob) and goes on, the worker runs the jobs behind it, and waitIdle waits for the last
// one. The PPU uses it to mix each line's layers on another core (see Ppu::setThreaded).
//
// The queue's logic is written once, here, over a small platform class (threads, a mutex and two
// conditions); the host build uses std::thread (jobqueue_std.h), the Switch build libnx's threads
// (platform/switch/jobqueue_nx.cpp), so the logic can be tested on a PC.
//
// A worker that does not finish in a second marks the queue broken (beginJob returns null, waitIdle
// false) and the caller goes on without it: a stuck worker must not hang the game.
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

class JobQueue {
public:
  using Fn = void (*)(void* job, void* user);
  virtual ~JobQueue() = default;
  // A free slot to fill (jobSize bytes); blocks while the queue is full. Null if the worker is stuck.
  virtual void* beginJob() = 0;
  // The slot is filled: the worker may take it.
  virtual void endJob() = 0;
  // Wakes the worker if it is asleep with jobs waiting (it is woken by itself only when a batch has built up,
  // so that a wake - a system call on each side - is not paid for every line).
  virtual void kick() = 0;
  // Returns once every job given so far has been run. False if the worker did not finish in time.
  virtual bool waitIdle() = 0;

  double stallMs = 0;  // time the emulation thread has spent waiting on the worker (for the log)
  bool broken = false;
};

// Makes a queue that runs fn(job, user) for every job, on a thread of its own. Null if no thread could
// be started. Set by the platform (null: no worker threads, the work is done in place).
using JobQueueFactory = JobQueue* (*)(size_t jobSize, int capacity, JobQueue::Fn fn, void* user);

// P: the platform. It must provide
//   bool spawn(void (*entry)(void*), void* arg);  // starts the worker thread
//   void join();                                    // waits for it to end
//   void lock(); void unlock();                     // one mutex
//   bool waitWork(int64_t ns);  // (mutex held) waits for notifyWork; ns < 0: no limit; false on timeout
//   void notifyWork();          // (mutex held)
//   bool waitDone(int64_t ns);  // the same for the "a job finished" condition
//   void notifyDone();
template <class P>
class ThreadedJobQueue final : public JobQueue {
public:
  ~ThreadedJobQueue() override {
    if (started_) {
      if (!broken) waitIdle();
      quit_.store(true, std::memory_order_seq_cst);
      p_.lock();
      p_.notifyWork();
      p_.unlock();
      if (!broken) p_.join();  // (a stuck worker is left alone: joining it would hang too)
    }
    if (!broken) std::free(slots_);
  }

  bool start(size_t jobSize, int capacity, Fn fn, void* user) {
    jobSize_ = (jobSize + 63) & ~(size_t)63;
    cap_ = (uint32_t)capacity;
    slots_ = static_cast<uint8_t*>(std::aligned_alloc(64, jobSize_ * cap_));
    if (!slots_) return false;
    fn_ = fn;
    user_ = user;
    if (!p_.spawn(&ThreadedJobQueue::entry, this)) {
      std::free(slots_);
      slots_ = nullptr;
      return false;
    }
    started_ = true;
    return true;
  }

  void* beginJob() override {
    if (broken) return nullptr;
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    if (t - head_.load(std::memory_order_seq_cst) >= cap_) {  // full
      if (!waitUntil([&] { return t - head_.load(std::memory_order_seq_cst) < cap_; })) {
        broken = true;
        return nullptr;
      }
    }
    return slots_ + (size_t)(t % cap_) * jobSize_;
  }

  void endJob() override {
    const uint32_t t = tail_.load(std::memory_order_relaxed) + 1;
    tail_.store(t, std::memory_order_seq_cst);
    if (t - head_.load(std::memory_order_seq_cst) >= kWakeBatch) wake();  // (a batch has built up)
  }

  void kick() override {
    if (tail_.load(std::memory_order_relaxed) != head_.load(std::memory_order_seq_cst)) wake();
  }

  bool waitIdle() override {
    if (broken) return false;
    kick();
    if (!waitUntil([&] { return head_.load(std::memory_order_seq_cst) == tail_.load(std::memory_order_relaxed); })) {
      broken = true;
      return false;
    }
    return true;
  }

private:
  static constexpr int64_t kTimeoutNs = 1000000000;  // a second
  static constexpr uint32_t kWakeBatch = 12;         // jobs waiting before a sleeping worker is woken

  void wake() {
    if (sleeping_.load(std::memory_order_seq_cst)) {  // (the worker is asleep: wake it)
      p_.lock();
      p_.notifyWork();
      p_.unlock();
    }
  }

  static void entry(void* self) { static_cast<ThreadedJobQueue*>(self)->run(); }

  void run() {
    for (;;) {
      const uint32_t h = head_.load(std::memory_order_relaxed);
      if (h != tail_.load(std::memory_order_acquire)) {
        fn_(slots_ + (size_t)(h % cap_) * jobSize_, user_);
        head_.store(h + 1, std::memory_order_seq_cst);
        if (doneWaiting_.load(std::memory_order_seq_cst)) {  // (the emulation thread is waiting for this)
          p_.lock();
          p_.notifyDone();
          p_.unlock();
        }
        continue;
      }
      if (quit_.load(std::memory_order_seq_cst)) return;
      p_.lock();
      sleeping_.store(true, std::memory_order_seq_cst);
      // (the job may have come between the check above and here: look again before sleeping)
      if (tail_.load(std::memory_order_seq_cst) == h && !quit_.load(std::memory_order_seq_cst)) p_.waitWork(-1);
      sleeping_.store(false, std::memory_order_seq_cst);
      p_.unlock();
    }
  }

  template <class Pred>
  bool waitUntil(Pred ok) {
    if (ok()) return true;
    wake();  // (whatever is being waited for, the worker has to be running to bring it about)
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsedNs = [&] {
      return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    };
    bool good = true;
    p_.lock();
    doneWaiting_.store(true, std::memory_order_seq_cst);
    while (!ok()) {
      const int64_t left = kTimeoutNs - elapsedNs();
      if (left <= 0) {
        good = ok();
        break;
      }
      p_.waitDone(left);
    }
    doneWaiting_.store(false, std::memory_order_seq_cst);
    p_.unlock();
    stallMs += (double)elapsedNs() / 1e6;
    return good;
  }

  P p_;
  uint8_t* slots_ = nullptr;
  size_t jobSize_ = 0;
  uint32_t cap_ = 0;
  Fn fn_ = nullptr;
  void* user_ = nullptr;
  bool started_ = false;
  alignas(64) std::atomic<uint32_t> head_{0};  // jobs run (the worker's)
  alignas(64) std::atomic<uint32_t> tail_{0};  // jobs given (the emulation thread's)
  std::atomic<bool> sleeping_{false}, doneWaiting_{false}, quit_{false};
};
