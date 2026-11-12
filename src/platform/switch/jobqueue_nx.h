// The job queue (core/jobqueue.h) on libnx's threads, for Ppu::queueFactory. Like nx_sys.h, no libnx
// header: jobqueue_nx.cpp is the one that includes it.
#pragma once
#include "core/jobqueue.h"

// Starts a worker thread on a core of its own (core 1, else 2, else wherever the system puts it: the
// emulation is on core 0) and returns a queue that feeds it. Null if no thread could be started.
JobQueue* makeNxJobQueue(size_t jobSize, int capacity, JobQueue::Fn fn, void* user);
