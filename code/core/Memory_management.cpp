#include "Memory_management.hpp"
#include "pch.hpp"




/**
 * @brief Returns the one application-wide registry instance.
 *
 * Implements the Meyers singleton idiom: the function-local static is
 * constructed exactly once on the first call and the same reference is
 * handed back forever after. This is the single global access point
 * that lets any class anywhere in the program reach the very same
 * stored object addresses, with thread-safe one-time initialization
 * guaranteed by the language and zero manual lifetime management.
 */

/**
 * @brief Builds an empty registry and announces its creation.
 *
 * The only owned member is the backing entry vector, which is brought
 * up empty through the constructor initializer list as the project
 * style requires. A single startup log line records the address of
 * the global instance so its lifetime is easy to follow in traces.
 * No heap work happens here; storage grows lazily as subobjects are
 * created later through CreateDefaultSubobject.
 */




/**
 * @brief Finds the first stored object matching a requested type.
 *
 * Walks the entry vector and compares each stored std::type_index to
 * the one asked for, returning the raw address of the first match so
 * the templated GetSubobject wrapper can static_cast it to the proper
 * concrete pointer. A miss returns nullptr instead of throwing, which
 * keeps lookups cheap and branch-friendly. Every hit and miss is
 * logged so it is obvious which call site shares which address.
 */

/**
 * @brief Reflection: how many objects the registry currently owns.
 *
 * Simply reports the size of the backing vector, which is exactly the
 * count of live subobjects created so far. This is the lightweight
 * "how many classes are allocated" query and never touches the heap.
 */
std::size_t MemoryManagement::GetObjectCount() const {
    return m_entries.size();
}

/**
 * @brief Reflection: total static weight of every owned object.
 *
 * Folds the per-entry sizeBytes values (each captured as sizeof(T) at
 * creation time, so a compile-time constant) into a running total
 * using the C++23 ranges fold_left algorithm. The result is the sum
 * of the static footprints of all registered objects, answering the
 * "how much do they weigh" question without any manual loop bookkeeping.
 */
std::size_t MemoryManagement::GetTotalMemoryBytes() const {
    return std::ranges::fold_left(m_entries, std::size_t{0},
                                  [](std::size_t total, const ObjectEntry& entry) {
                                      return total + entry.sizeBytes;
                                  });
}

/**
 * @brief Prints a full reflection report for every owned object.
 *
 * Emits a header with the aggregate count and total weight, then one
 * aligned line per entry showing its debug name, demangled type name,
 * static size and live address. This is the human-facing view of the
 * registry's reflection data and is handy when verifying that two
 * separate call sites really did receive the same shared address.
 */
void MemoryManagement::DumpReflectionInfo() const {
    std::println("==== MemoryManagement reflection dump ====");
    std::println("Objects allocated : {}", GetObjectCount());
    std::println("Total weight      : {} bytes", GetTotalMemoryBytes());
    for (const ObjectEntry& entry : m_entries) {
        std::println("  - {:<14} type='{}' size={} bytes addr={}",
                     entry.debugName, entry.typeName, entry.sizeBytes,
                     static_cast<const void*>(entry.instance.get()));
    }
    std::println("========================================");
}