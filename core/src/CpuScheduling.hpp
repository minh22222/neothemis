#pragma once

#include <string>
#include <vector>

namespace neothemis::detail {

struct CpuSlot {
    unsigned group = 0;
    unsigned index = 0;
    unsigned performance = 0;
    // Physical identity is independent of which allowed SMT sibling is chosen.
    std::string core_key;
};

struct CpuTopology {
    // One allowed logical processor per physical core, fastest class first.
    std::vector<CpuSlot> cores;
    unsigned preferred_count = 0;
};

struct LogicalCpu {
    CpuSlot slot;
    bool allowed = true;
};

// Kept separate from OS discovery so sparse masks, SMT and heterogeneous CPUs
// can be tested without depending on the machine running the tests.
CpuTopology select_physical_cores(const std::vector<LogicalCpu>& processors);
CpuTopology detect_cpu_topology();
unsigned resolve_worker_count(unsigned requested, const CpuTopology& topology);

// Linux: call in the forked child before installing the sandbox/exec. Does not
// allocate through the C++ heap, so it is safe after a multithreaded fork.
bool apply_current_thread_cpu(const CpuSlot& slot);

// Windows: call after assigning the suspended process to its job, before resume.
// Job affinity also constrains the submission's future threads/child processes.
bool apply_child_cpu(void* job, void* process, void* suspended_thread,
                     const CpuSlot& slot);

} // namespace neothemis::detail
