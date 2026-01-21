/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <pthread.h>
#include <sched.h>
#include <unistd.h>    // For sysconf, nice
#include <cstring>
#include <stdexcept>
#include <vector>
#include <string>

namespace COP {

/**
 * Utility class for setting CPU affinity to reduce context switches
 * and cache pollution in latency-sensitive code paths.
 */
class CpuAffinity {
public:
    /**
     * Pin the current thread to a specific CPU core.
     * @param coreId The CPU core to pin to (0-indexed)
     * @return true on success, false on failure
     */
    static bool pinThreadToCore(int coreId) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(coreId, &cpuset);

        int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        return result == 0;
    }

    /**
     * Pin the specified thread to a specific CPU core.
     * @param thread The pthread to pin
     * @param coreId The CPU core to pin to (0-indexed)
     * @return true on success, false on failure
     */
    static bool pinThreadToCore(pthread_t thread, int coreId) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(coreId, &cpuset);

        int result = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
        return result == 0;
    }

    /**
     * Pin the current thread to a set of CPU cores.
     * @param coreIds Vector of CPU core IDs to pin to
     * @return true on success, false on failure
     */
    static bool pinThreadToCores(const std::vector<int>& coreIds) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        for (int coreId : coreIds) {
            CPU_SET(coreId, &cpuset);
        }

        int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        return result == 0;
    }

    /**
     * Get the number of available CPU cores.
     * @return Number of online processors
     */
    static int getAvailableCores() noexcept {
        return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    }

    /**
     * Get the current thread's CPU affinity mask.
     * @param coreIds Output vector to receive the core IDs
     * @return true on success, false on failure
     */
    static bool getThreadAffinity(std::vector<int>& coreIds) noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
            return false;
        }

        coreIds.clear();
        int numCpus = getAvailableCores();
        for (int i = 0; i < numCpus; ++i) {
            if (CPU_ISSET(i, &cpuset)) {
                coreIds.push_back(i);
            }
        }
        return true;
    }

    /**
     * Reset thread affinity to allow running on all cores.
     * @return true on success, false on failure
     */
    static bool resetAffinity() noexcept {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        int numCpus = getAvailableCores();
        for (int i = 0; i < numCpus; ++i) {
            CPU_SET(i, &cpuset);
        }

        int result = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        return result == 0;
    }

    /**
     * Set thread scheduling priority to real-time FIFO.
     * Requires CAP_SYS_NICE or root privileges.
     * @param priority Priority level (1-99, higher = more priority)
     * @return true on success, false on failure
     */
    static bool setRealtimePriority(int priority = 50) noexcept {
        struct sched_param param;
        param.sched_priority = priority;
        return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
    }

    /**
     * Set thread nice value for higher priority.
     * @param niceValue Nice value (-20 to 19, lower = higher priority)
     * @return true on success, false on failure
     */
    static bool setNiceValue(int niceValue) noexcept {
        return nice(niceValue) != -1;
    }
};

/**
 * RAII guard for temporarily pinning a thread to a core.
 * Restores original affinity on destruction.
 */
class ScopedCpuAffinity {
public:
    explicit ScopedCpuAffinity(int coreId) : originalAffinity_(), restored_(false) {
        CpuAffinity::getThreadAffinity(originalAffinity_);
        CpuAffinity::pinThreadToCore(coreId);
    }

    ~ScopedCpuAffinity() {
        restore();
    }

    void restore() {
        if (!restored_ && !originalAffinity_.empty()) {
            CpuAffinity::pinThreadToCores(originalAffinity_);
            restored_ = true;
        }
    }

private:
    std::vector<int> originalAffinity_;
    bool restored_;

    // Non-copyable
    ScopedCpuAffinity(const ScopedCpuAffinity&) = delete;
    ScopedCpuAffinity& operator=(const ScopedCpuAffinity&) = delete;
};

} // namespace COP
