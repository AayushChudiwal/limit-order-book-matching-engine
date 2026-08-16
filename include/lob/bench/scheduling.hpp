#pragma once

#include <cstddef>

namespace lob::bench {

// Apple Silicon exposes no thread-affinity API -- there is no equivalent
// of Linux's isolcpus/sched_setaffinity to pin this process to a P-core.
// QOS class is the only lever available: it's a scheduling HINT, not a
// guarantee, so the thread can still migrate under load. Returns false if
// the call itself failed (continue anyway -- this is best-effort).
[[nodiscard]] bool SetInteractiveQos();

// The OS-reported CPU core index this thread is currently running on, via
// pthread_cpu_number_np -- a public, documented pthread extension
// (confirmed linkable and returning a plausible index on this machine
// before use, the same bar every other OS-specific call in this project
// is held to). Read before and after a timed region: if the two values
// differ, the thread migrated cores mid-measurement and that sample's
// cycle count reflects two different cores, not one -- see
// docs/benchmark_methodology.md for how this is used to flag
// contaminated samples instead of silently discarding them.
[[nodiscard]] std::size_t CurrentCpuNumber();

}  // namespace lob::bench
