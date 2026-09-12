#pragma once

#include "CpuScheduling.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace neothemis::detail {

// Reservations are shared by judge invocations in this process. Only child
// processes are pinned: the GUI, monitors and scheduler remain unrestricted.
class CpuLease {
public:
    CpuLease(const CpuTopology& topology, unsigned int eligible_count,
             const std::function<bool()>& should_cancel = {}, bool exclusive = false) {
        (void)eligible_count;
        auto& state = registry();
        std::unique_lock<std::mutex> lock(state.mutex);
        if (exclusive) {
            ++state.exclusive_waiters;
        }
        try {
            while (true) {
                // User callbacks must not execute under the global lease lock.
                lock.unlock();
                const bool cancelled = should_cancel && should_cancel();
                lock.lock();
                if (cancelled) {
                    throw std::runtime_error("judging cancelled");
                }
                if (!state.exclusive &&
                    (exclusive ? state.busy.empty() : state.exclusive_waiters == 0)) {
                    // Worker count limits how many leases are requested, not
                    // which physical cores are eligible. Searching the whole
                    // topology lets a second low-concurrency judge use a free
                    // core instead of waiting behind core 0.
                    for (std::size_t i = 0; i < std::max<std::size_t>(1, topology.cores.size()); ++i) {
                        const CpuSlot* candidate = topology.cores.empty()
                            ? nullptr : &topology.cores[i];
                        const std::string key = candidate
                            ? (candidate->core_key.empty()
                                ? std::to_string(candidate->group) + ":" +
                                      std::to_string(candidate->index)
                                : candidate->core_key)
                            : "unknown-cpu";
                        if (state.busy.count(key) != 0) {
                            continue;
                        }
                        key_ = key;
                        if (candidate) {
                            slot_ = *candidate;
                            has_cpu_ = true;
                        }
                        state.busy.insert(key_);
                        state.exclusive = exclusive;
                        exclusive_ = exclusive;
                        if (exclusive) {
                            --state.exclusive_waiters;
                        }
                        return;
                    }
                }
                state.changed.wait_for(lock, std::chrono::milliseconds(50));
            }
        } catch (...) {
            if (!lock.owns_lock()) {
                lock.lock();
            }
            if (exclusive) {
                --state.exclusive_waiters;
                state.changed.notify_all();
            }
            throw;
        }
    }

    ~CpuLease() {
        auto& state = registry();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.busy.erase(key_);
            if (exclusive_) {
                state.exclusive = false;
            }
        }
        state.changed.notify_all();
    }

    CpuLease(const CpuLease&) = delete;
    CpuLease& operator=(const CpuLease&) = delete;

    const CpuSlot* cpu() const noexcept { return has_cpu_ ? &slot_ : nullptr; }

private:
    struct Registry {
        std::mutex mutex;
        std::condition_variable changed;
        std::set<std::string> busy;
        unsigned int exclusive_waiters = 0;
        bool exclusive = false;
    };

    static Registry& registry() {
        static Registry state;
        return state;
    }

    CpuSlot slot_{};
    std::string key_;
    bool has_cpu_ = false;
    bool exclusive_ = false;
};

} // namespace neothemis::detail
