/**
 Concurrent Order Processor library

 Author: Sergey Mikhailik

 Copyright (C) 2009-2026 Sergey Mikhailik

 Distributed under the GNU General Public License (GPL).

 See http://orderprocessor.sourceforge.net updates, documentation, and revision history.
*/

#pragma once

#include <sys/mman.h>
#include <numaif.h>
#include <unistd.h>
#include <dirent.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace COP {

/**
 * NUMA-aware memory allocator for multi-socket systems.
 *
 * Allocates memory and binds it to a specific NUMA node using the mbind()
 * syscall. This ensures that memory is physically allocated on the same
 * socket as the threads accessing it, avoiding cross-socket memory traffic
 * which can add 40-100ns per access.
 *
 * Falls back to regular mmap if NUMA binding fails.
 *
 * Prerequisites:
 * - Multi-socket system with NUMA topology
 * - /sys/devices/system/node/ available for topology detection
 */
class NumaAllocator {
public:
    /**
     * Detect the number of NUMA nodes on the system.
     * Reads from /sys/devices/system/node/
     * @return Number of NUMA nodes (1 for UMA systems)
     */
    static int nodeCount() noexcept {
        int count = 0;
        DIR* dir = opendir("/sys/devices/system/node");
        if (!dir) return 1;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            // Look for directories named "node0", "node1", ...
            if (entry->d_type == DT_DIR && std::strncmp(entry->d_name, "node", 4) == 0) {
                ++count;
            }
        }
        closedir(dir);
        return count > 0 ? count : 1;
    }

    /**
     * Check if the system has multiple NUMA nodes.
     */
    static bool isNumaAvailable() noexcept {
        return nodeCount() > 1;
    }

    /**
     * Allocate memory bound to a specific NUMA node.
     *
     * @param size Size in bytes to allocate (page-aligned internally)
     * @param node NUMA node to bind to (0-indexed)
     * @return Pointer to allocated memory, or nullptr on failure
     */
    static void* allocateOnNode(size_t size, int node) noexcept {
        size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        size_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);

        void* ptr = mmap(nullptr, alignedSize,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);

        if (ptr == MAP_FAILED) {
            return nullptr;
        }

        // Build a nodemask with only the target node bit set
        unsigned long nodemask = 1UL << node;
        unsigned long maxnode = static_cast<unsigned long>(node) + 2;

        // Bind the memory to the specified NUMA node
        // MPOL_BIND: strict allocation on specified nodes
        int ret = mbind(ptr, alignedSize, MPOL_BIND,
                        &nodemask, maxnode, MPOL_MF_MOVE);

        if (ret != 0) {
            // mbind failed — memory is still usable but not NUMA-bound.
            // Touch the pages to fault them in on the current node instead.
            volatile char* p = static_cast<volatile char*>(ptr);
            for (size_t offset = 0; offset < alignedSize; offset += pageSize) {
                p[offset] = 0;
            }
        }

        return ptr;
    }

    /**
     * Allocate memory local to the current thread's NUMA node.
     *
     * @param size Size in bytes to allocate
     * @return Pointer to allocated memory, or nullptr on failure
     */
    static void* allocateLocal(size_t size) noexcept {
        size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        size_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);

        void* ptr = mmap(nullptr, alignedSize,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);

        if (ptr == MAP_FAILED) {
            return nullptr;
        }

        // MPOL_PREFERRED with empty nodemask = allocate on current node
        int ret = mbind(ptr, alignedSize, MPOL_PREFERRED,
                        nullptr, 0, 0);

        if (ret != 0) {
            // First-touch policy: fault pages on current node
            volatile char* p = static_cast<volatile char*>(ptr);
            for (size_t offset = 0; offset < alignedSize; offset += pageSize) {
                p[offset] = 0;
            }
        }

        return ptr;
    }

    /**
     * Deallocate memory previously allocated with allocateOnNode/allocateLocal.
     *
     * @param ptr Pointer to memory to deallocate
     * @param size Original size requested (will be page-aligned internally)
     */
    static void deallocate(void* ptr, size_t size) noexcept {
        if (ptr != nullptr) {
            size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
            size_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);
            munmap(ptr, alignedSize);
        }
    }
};

/**
 * STL-compatible allocator that binds memory to a specific NUMA node.
 */
template<typename T>
class NumaNodeAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    explicit NumaNodeAllocator(int node = 0) noexcept : node_(node) {}

    template<typename U>
    NumaNodeAllocator(const NumaNodeAllocator<U>& other) noexcept : node_(other.node()) {}

    int node() const noexcept { return node_; }

    T* allocate(std::size_t n) {
        void* ptr = NumaAllocator::allocateOnNode(n * sizeof(T), node_);
        if (!ptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* ptr, std::size_t n) noexcept {
        NumaAllocator::deallocate(ptr, n * sizeof(T));
    }

    template<typename U>
    bool operator==(const NumaNodeAllocator<U>& other) const noexcept {
        return node_ == other.node();
    }

    template<typename U>
    bool operator!=(const NumaNodeAllocator<U>& other) const noexcept {
        return node_ != other.node();
    }

private:
    int node_;
};

} // namespace COP
