#include "CpuScheduling.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <tuple>

#if defined(__linux__)
#include <sched.h>
#include <sys/mman.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neothemis::detail {
namespace {

bool earlier_cpu(const CpuSlot& left, const CpuSlot& right) {
    return std::tie(left.group, left.index) < std::tie(right.group, right.index);
}

#if defined(__linux__)
std::vector<unsigned> linux_allowed_cpus() {
    // A fixed cpu_set_t silently loses CPUs above CPU_SETSIZE. The kernel asks
    // for a larger buffer with EINVAL when its configured CPU count is larger.
    for (std::size_t words = 16; words <= 16384; words *= 2) {
        std::vector<unsigned long> mask(words, 0);
        if (sched_getaffinity(0, mask.size() * sizeof(mask[0]),
                              reinterpret_cast<cpu_set_t*>(mask.data())) != 0) {
            if (errno == EINVAL) {
                continue;
            }
            return {};
        }
        std::vector<unsigned> result;
        constexpr unsigned bits = sizeof(unsigned long) * 8;
        for (std::size_t word = 0; word < mask.size(); ++word) {
            for (unsigned bit = 0; bit < bits; ++bit) {
                if ((mask[word] & (1UL << bit)) != 0) {
                    result.push_back(static_cast<unsigned>(word * bits + bit));
                }
            }
        }
        return result;
    }
    return {};
}

std::string read_token(const std::string& path) {
    std::ifstream input(path);
    std::string value;
    input >> value;
    return value;
}

unsigned read_positive_number(const std::string& path) {
    std::ifstream input(path);
    std::uint64_t value = 0;
    if (!(input >> value) || value > std::numeric_limits<unsigned>::max()) {
        return 0;
    }
    return static_cast<unsigned>(value);
}

CpuTopology linux_topology() {
    std::vector<LogicalCpu> processors;
    std::vector<unsigned> capacities;
    std::vector<unsigned> frequencies;
    for (unsigned cpu : linux_allowed_cpus()) {
        const std::string root = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
        std::string siblings = read_token(root + "/topology/core_cpus_list");
        if (siblings.empty()) {
            siblings = read_token(root + "/topology/thread_siblings_list");
        }
        std::string key;
        if (!siblings.empty()) {
            key = "linux:siblings:" + siblings;
        } else {
            const std::string package = read_token(root + "/topology/physical_package_id");
            const std::string core = read_token(root + "/topology/core_id");
            const std::string die = read_token(root + "/topology/die_id");
            if (!package.empty() && package != "-1" && !core.empty() && core != "-1") {
                key = "linux:core:" + package + ":" + die + ":" + core;
            }
        }
        processors.push_back({{0, cpu, 0, key}, true});
        capacities.push_back(read_positive_number(root + "/cpu_capacity"));
        frequencies.push_back(read_positive_number(root + "/cpufreq/cpuinfo_max_freq"));
    }
    const auto complete = [](const std::vector<unsigned>& values) {
        return !values.empty() &&
               std::all_of(values.begin(), values.end(), [](unsigned value) { return value != 0; });
    };
    if (complete(capacities)) {
        for (std::size_t i = 0; i < processors.size(); ++i) {
            processors[i].slot.performance = capacities[i];
        }
    } else if (complete(frequencies)) {
        const std::uint64_t maximum = *std::max_element(frequencies.begin(), frequencies.end());
        for (std::size_t i = 0; i < processors.size(); ++i) {
            // Small boost-bin differences do not turn a homogeneous CPU into
            // a one-worker system. Frequency is only a fallback class heuristic.
            processors[i].slot.performance =
                std::uint64_t(frequencies[i]) * 10 >= maximum * 9 ? 2 : 1;
        }
    }
    return select_physical_cores(processors);
}
#elif defined(_WIN32)
using CpuAddress = std::pair<unsigned, unsigned>;

template <typename Function>
Function load_kernel_proc(const char* name) {
    const FARPROC raw = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(raw), "unexpected Windows function-pointer size");
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

std::set<CpuAddress> windows_cpu_set_restrictions(bool& available) {
    available = false;
    using GetInformation = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    using GetSelections = BOOL(WINAPI*)(HANDLE, PULONG, ULONG, PULONG);
    const auto get_information = load_kernel_proc<GetInformation>(
        "GetSystemCpuSetInformation");
    const auto get_process_sets = load_kernel_proc<GetSelections>(
        "GetProcessDefaultCpuSets");
    const auto get_thread_sets = load_kernel_proc<GetSelections>(
        "GetThreadSelectedCpuSets");
    if (!get_information || !get_process_sets || !get_thread_sets) {
        return {};
    }
    auto selections = [](GetSelections function, HANDLE handle) {
        ULONG count = 0;
        function(handle, nullptr, 0, &count);
        std::vector<ULONG> ids(count);
        if (count != 0 && !function(handle, ids.data(), count, &count)) {
            ids.clear();
        }
        return std::set<ULONG>(ids.begin(), ids.end());
    };
    auto selected = selections(get_thread_sets, GetCurrentThread());
    if (selected.empty()) {
        selected = selections(get_process_sets, GetCurrentProcess());
    }
    ULONG bytes = 0;
    get_information(nullptr, 0, &bytes, GetCurrentProcess(), 0);
    if (bytes == 0) {
        return {};
    }
    std::vector<unsigned char> buffer(bytes);
    if (!get_information(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                         bytes, &bytes, GetCurrentProcess(), 0)) {
        return {};
    }
    std::set<CpuAddress> allowed;
    for (std::size_t offset = 0; offset + sizeof(SYSTEM_CPU_SET_INFORMATION) <= bytes;) {
        const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (info->Size < sizeof(SYSTEM_CPU_SET_INFORMATION) || info->Size > bytes - offset) {
            return {};
        }
        if (info->Type == CpuSetInformation &&
            (!info->CpuSet.Allocated || info->CpuSet.AllocatedToTargetProcess) &&
            (selected.empty() || selected.count(info->CpuSet.Id) != 0)) {
            allowed.emplace(info->CpuSet.Group, info->CpuSet.LogicalProcessorIndex);
        }
        offset += info->Size;
    }
    available = true;
    return allowed;
}

CpuTopology windows_topology() {
    GROUP_AFFINITY current{};
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (!GetThreadGroupAffinity(GetCurrentThread(), &current) ||
        !GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        return {};
    }
    // All shifts below are group-relative, never flattened CPU IDs.
    std::set<USHORT> groups{current.Group};
    USHORT count = GetActiveProcessorGroupCount();
    std::vector<USHORT> process_groups(count);
    if (count != 0 && GetProcessGroupAffinity(GetCurrentProcess(), &count, process_groups.data())) {
        groups = std::set<USHORT>(process_groups.begin(), process_groups.begin() + count);
    }
    bool cpu_sets_available = false;
    const auto cpu_sets = windows_cpu_set_restrictions(cpu_sets_available);
    auto allowed = [&](unsigned group, unsigned bit) {
        if (groups.count(static_cast<USHORT>(group)) == 0) {
            return false;
        }
        if (group == current.Group &&
            ((process_mask & current.Mask) & (DWORD_PTR(1) << bit)) == 0) {
            return false;
        }
        return !cpu_sets_available || cpu_sets.count({group, bit}) != 0;
    };

    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    std::vector<unsigned char> buffer(bytes);
    std::vector<LogicalCpu> processors;
    if (bytes != 0 && GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bytes)) {
        for (std::size_t offset = 0; offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= bytes;) {
            const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
            if (info->Size < sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) || info->Size > bytes - offset) {
                break;
            }
            if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount == 1) {
                const auto& mask = info->Processor.GroupMask[0];
                std::string key;
                // Older MinGW headers expose EfficiencyClass as the first
                // reserved byte; this offset is the documented Windows ABI.
                const unsigned performance = reinterpret_cast<const unsigned char*>(&info->Processor)[1];
                for (unsigned bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                    if ((mask.Mask & (KAFFINITY(1) << bit)) == 0) {
                        continue;
                    }
                    if (key.empty()) {
                        key = "windows:core:" + std::to_string(mask.Group) + ":" + std::to_string(bit);
                    }
                    processors.push_back({{mask.Group, bit, performance, key}, allowed(mask.Group, bit)});
                }
            }
            offset += info->Size;
        }
    }
    if (processors.empty()) {
        for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8; ++bit) {
            if (allowed(current.Group, bit)) {
                processors.push_back({{current.Group, bit, 0, {}}, true});
            }
        }
    }
    return select_physical_cores(processors);
}
#endif

} // namespace

CpuTopology select_physical_cores(const std::vector<LogicalCpu>& processors) {
    std::map<std::string, CpuSlot> physical;
    const CpuSlot* fallback = nullptr;
    for (const auto& processor : processors) {
        if (!processor.allowed) {
            continue;
        }
        const auto& slot = processor.slot;
        if (!fallback || earlier_cpu(slot, *fallback)) {
            fallback = &slot;
        }
        if (slot.core_key.empty()) {
            continue;
        }
        auto found = physical.find(slot.core_key);
        if (found == physical.end()) {
            physical.emplace(slot.core_key, slot);
        } else {
            const unsigned performance = std::max(slot.performance, found->second.performance);
            if (earlier_cpu(slot, found->second)) {
                found->second = slot;
            }
            found->second.performance = performance;
        }
    }
    CpuTopology topology;
    for (const auto& entry : physical) {
        topology.cores.push_back(entry.second);
    }
    if (topology.cores.empty() && fallback) {
        topology.cores.push_back(*fallback);
        // All unknown topology shares one reservation identity, including
        // callers that inherited different allowed logical-CPU masks.
        topology.cores.back().core_key = "unknown-topology";
    }
    std::sort(topology.cores.begin(), topology.cores.end(), [](const CpuSlot& left, const CpuSlot& right) {
        return left.performance != right.performance
            ? left.performance > right.performance : earlier_cpu(left, right);
    });
    if (!topology.cores.empty()) {
        const unsigned best = topology.cores.front().performance;
        topology.preferred_count = static_cast<unsigned>(std::count_if(
            topology.cores.begin(), topology.cores.end(),
            [best](const CpuSlot& slot) { return slot.performance == best; }));
    }
    return topology;
}

CpuTopology detect_cpu_topology() {
#if defined(__linux__)
    return linux_topology();
#elif defined(_WIN32)
    return windows_topology();
#else
    // Platforms without a supported affinity API retain the historical
    // half-logical-thread default. There is no physical identity to enforce,
    // so leases use stable synthetic keys and do not pin child processes.
    unsigned logical = std::thread::hardware_concurrency();
    if (logical > 1) {
        logical = (logical + 1) / 2;
    }
    logical = std::max(1U, logical);
    CpuTopology topology;
    topology.preferred_count = logical;
    topology.cores.reserve(logical);
    for (unsigned index = 0; index < logical; ++index) {
        topology.cores.push_back({0, index, 1, "generic:" + std::to_string(index)});
    }
    return topology;
#endif
}

unsigned resolve_worker_count(unsigned requested, const CpuTopology& topology) {
    const unsigned maximum = static_cast<unsigned>(topology.cores.size());
    if (maximum == 0) {
        return 1;
    }
    if (requested == 0) {
        requested = topology.preferred_count != 0 ? topology.preferred_count : maximum;
    }
    return std::max(1U, std::min(requested, maximum));
}

bool apply_current_thread_cpu(const CpuSlot& slot) {
#if defined(__linux__)
    if (slot.group != 0 || slot.index >= 1048576) {
        return false;
    }
    constexpr unsigned bits = sizeof(unsigned long) * 8;
    const std::size_t bytes = (slot.index / bits + 1) * sizeof(unsigned long);
    // mmap, sched_setaffinity and munmap avoid inherited userspace allocator
    // locks in a child forked while another judging worker was allocating.
    void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        return false;
    }
    auto* mask = static_cast<unsigned long*>(memory);
    mask[slot.index / bits] = 1UL << (slot.index % bits);
    const bool applied = sched_setaffinity(0, bytes, static_cast<cpu_set_t*>(memory)) == 0;
    munmap(memory, bytes);
    return applied;
#else
    (void)slot;
    return false;
#endif
}

bool apply_child_cpu(void* job, void* process, void* suspended_thread,
                     const CpuSlot& slot) {
#if defined(_WIN32)
    if (!job || !process || !suspended_thread || slot.group > std::numeric_limits<USHORT>::max() ||
        slot.index >= sizeof(KAFFINITY) * 8) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    USHORT group = static_cast<USHORT>(slot.group);
    GROUP_AFFINITY affinity{};
    affinity.Group = group;
    affinity.Mask = KAFFINITY(1) << slot.index;
    // The group-aware job limit follows future descendants and works when the
    // selected CPU is outside the newly created process's primary group. Some
    // older Windows versions do not support the job-group information classes;
    // keep the per-thread affinity in that case rather than turning an optional
    // performance optimization into a judging failure.
    (void)SetInformationJobObject(job, JobObjectGroupInformation, &group, sizeof(group));
    (void)SetInformationJobObject(job, JobObjectGroupInformationEx, &affinity, sizeof(affinity));
    return SetThreadGroupAffinity(suspended_thread, &affinity, nullptr) != FALSE;
#else
    (void)job;
    (void)process;
    (void)suspended_thread;
    (void)slot;
    return false;
#endif
}

} // namespace neothemis::detail
