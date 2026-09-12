#include "CpuScheduling.hpp"

#include <iostream>
#include <set>
#include <stdexcept>

#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace neothemis::detail;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_topology_selection() {
    const auto topology = select_physical_cores({
        {{0, 0, 9, "package0:core0"}, false},
        {{0, 8, 9, "package0:core0"}, true},
        {{0, 1, 9, "package0:core1"}, true},
        {{0, 9, 9, "package0:core1"}, true},
        {{0, 2, 2, "package0:core2"}, true},
        {{0, 3, 2, "package0:core3"}, false},
    });
    require(topology.cores.size() == 3, "select exactly one allowed sibling per physical core");
    require(topology.cores[0].index == 1 && topology.cores[1].index == 8,
            "prefer fast allowed cores, then stable logical order");
    require(topology.cores[2].index == 2, "keep efficient cores for explicit worker counts");
    require(topology.preferred_count == 2, "auto selects highest performance class only");
    require(resolve_worker_count(0, topology) == 2, "auto worker count uses performance cores");
    require(resolve_worker_count(1, topology) == 1, "serial worker count is preserved");
    require(resolve_worker_count(16, topology) == 3, "worker count cannot oversubscribe physical cores");
}

void test_groups_and_unknown_topology() {
    const auto grouped = select_physical_cores({
        {{0, 5, 0, "group0:5"}, true},
        {{1, 5, 0, "group1:5"}, true},
        {{1, 37, 0, "group1:5"}, true},
    });
    require(grouped.cores.size() == 2 && grouped.cores[1].group == 1,
            "processor-group-relative indices must not collide");
    require(grouped.preferred_count == 2 && resolve_worker_count(0, grouped) == 2,
            "homogeneous CPUs use all physical cores");
    const auto unknown = select_physical_cores({
        {{0, 17, 0, {}}, true}, {{0, 4, 0, {}}, true}, {{0, 2, 0, {}}, false},
    });
    require(unknown.cores.size() == 1 && unknown.cores[0].index == 4,
            "unknown SMT topology must conservatively use one allowed CPU");
    require(unknown.cores[0].core_key == "unknown-topology", "unknown topologies share a lease identity");
    require(resolve_worker_count(8, unknown) == 1, "unknown topology cannot be oversubscribed");
    const auto unavailable = select_physical_cores({{{0, 1, 1, "core"}, false}});
    require(unavailable.cores.empty(), "no allowed processor must not fabricate CPU zero");
    require(resolve_worker_count(0, unavailable) == 1, "unavailable discovery keeps a serial fallback");
    CpuTopology malformed{{{0, 1, 0, "core"}}, 100};
    require(resolve_worker_count(0, malformed) == 1, "auto count is clamped to the actual pool");
}

void test_live_topology() {
    const auto topology = detect_cpu_topology();
    std::set<std::string> cores;
    for (const auto& slot : topology.cores) {
        require(!slot.core_key.empty() && cores.insert(slot.core_key).second,
                "live topology must never reserve the same physical core twice");
    }
#ifdef __linux__
    require(!topology.cores.empty(), "Linux must discover an allowed CPU");
    // Pin only a disposable child; the test runner's affinity must stay intact.
    const pid_t child = fork();
    require(child >= 0, "could not fork affinity test");
    if (child == 0) {
        _exit(apply_current_thread_cpu(topology.cores.front()) ? 0 : 1);
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "the selected allowed CPU must accept affinity");
#endif
}
} // namespace

int main() {
    try {
        test_topology_selection();
        test_groups_and_unknown_topology();
        test_live_topology();
        std::cout << "CPU scheduling tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
