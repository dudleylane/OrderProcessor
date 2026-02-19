/**
 Concurrent Order Processor library

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>      // For FILE, fopen, fgets, fscanf, fclose, sscanf
#include <cstddef>
#include <cstdint>
#include <new>

namespace COP {

/**
 * Utility class for allocating memory using huge pages.
 *
 * Huge pages (2MB on x86-64) reduce TLB misses for large memory allocations,
 * improving performance in latency-sensitive applications.
 *
 * Prerequisites:
 * - System must have huge pages configured:
 *   echo 512 > /proc/sys/vm/nr_hugepages
 *   or add 'vm.nr_hugepages=512' to /etc/sysctl.conf
 *
 * - Application may need CAP_IPC_LOCK capability or memlock limits set
 */
class HugePages {
public:
    /// Standard huge page size on x86-64 (2MB)
    static constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;

    /**
     * Allocate memory using huge pages.
     * Falls back to regular pages if huge pages are unavailable.
     *
     * @param size Size in bytes to allocate (will be rounded up to huge page boundary)
     * @param fallbackToRegular If true, falls back to regular mmap if huge pages fail
     * @return Pointer to allocated memory, or nullptr on failure
     */
    static void* allocate(size_t size, bool fallbackToRegular = true) noexcept {
        // Round up to huge page size
        size_t alignedSize = roundUpToHugePage(size);

        // Try huge page allocation first
        void* ptr = mmap(nullptr, alignedSize,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                         -1, 0);

        if (ptr != MAP_FAILED) {
            return ptr;
        }

        // Fall back to regular pages if allowed
        if (fallbackToRegular) {
            ptr = mmap(nullptr, alignedSize,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);

            if (ptr != MAP_FAILED) {
                // Hint to kernel that this memory will be accessed randomly
                madvise(ptr, alignedSize, MADV_RANDOM);
                return ptr;
            }
        }

        return nullptr;
    }

    /**
     * Deallocate memory previously allocated with allocate().
     *
     * @param ptr Pointer to memory to deallocate
     * @param size Original size requested (will be rounded up internally)
     */
    static void deallocate(void* ptr, size_t size) noexcept {
        if (ptr != nullptr) {
            size_t alignedSize = roundUpToHugePage(size);
            munmap(ptr, alignedSize);
        }
    }

    /**
     * Round size up to the nearest huge page boundary.
     */
    static constexpr size_t roundUpToHugePage(size_t size) noexcept {
        return (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
    }

    /**
     * Check if huge pages are available on the system.
     * @return true if huge pages are configured and available
     */
    static bool isAvailable() noexcept {
        void* ptr = mmap(nullptr, HUGE_PAGE_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                         -1, 0);

        if (ptr != MAP_FAILED) {
            munmap(ptr, HUGE_PAGE_SIZE);
            return true;
        }
        return false;
    }

    /**
     * Get the number of configured huge pages on the system.
     * Reads from /proc/sys/vm/nr_hugepages
     * @return Number of huge pages, or -1 on error
     */
    static int getConfiguredCount() noexcept {
        FILE* f = fopen("/proc/sys/vm/nr_hugepages", "r");
        if (!f) return -1;

        int count = -1;
        if (fscanf(f, "%d", &count) != 1) {
            count = -1;
        }
        fclose(f);
        return count;
    }

    /**
     * Get the number of free huge pages available.
     * Reads from /proc/meminfo
     * @return Number of free huge pages, or -1 on error
     */
    static int getFreeCount() noexcept {
        FILE* f = fopen("/proc/meminfo", "r");
        if (!f) return -1;

        char line[256];
        int freePages = -1;

        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "HugePages_Free: %d", &freePages) == 1) {
                break;
            }
        }
        fclose(f);
        return freePages;
    }
};

/**
 * Custom allocator that uses huge pages.
 * Compatible with STL containers.
 */
template<typename T>
class HugePageAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    HugePageAllocator() = default;

    template<typename U>
    HugePageAllocator(const HugePageAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        void* ptr = HugePages::allocate(n * sizeof(T));
        if (!ptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* ptr, std::size_t n) noexcept {
        HugePages::deallocate(ptr, n * sizeof(T));
    }

    template<typename U>
    bool operator==(const HugePageAllocator<U>&) const noexcept { return true; }

    template<typename U>
    bool operator!=(const HugePageAllocator<U>&) const noexcept { return false; }
};

} // namespace COP
