#include "Memory_management.hpp"

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
MemoryManagement& MemoryManagement::Get() {
    static MemoryManagement instance;
    return instance;
}

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
MemoryManagement::MemoryManagement() : m_entries{} {
    std::println("[MemoryManagement] Global registry constructed at {}",
                 static_cast<const void*>(this));
}

/**
 * @brief Tears the registry down, releasing every owned subobject.
 *
 * Defaulted here (out of line) because the Meyers singleton's static
 * instance is destroyed at program exit, which requires a definition.
 * Each shared_ptr<void> in m_entries was captured with the concrete
 * type's deleter, so the correct destructor still runs even though the
 * static type was erased.
 */
MemoryManagement::~MemoryManagement() = default;

/**
 * @brief Takes ownership of a built object and records its metadata.
 *
 * The concrete object arrives already constructed and hidden behind a
 * shared_ptr<void>, which preserves the correct destructor while
 * erasing the static type. Alongside it come the runtime type index,
 * the compile-time byte size, a human debug name and the compile-time
 * type name. A new entry is appended, the event is logged, and the raw
 * observer address is returned so the caller can static_cast it back.
 */
void* MemoryManagement::registerErased(std::shared_ptr<void> instance,
                                     std::type_index type,
                                     std::size_t sizeBytes,
                                     std::string_view debugName,
                                     std::string_view typeName) {
    void* address{instance.get()};
    m_entries.push_back(ObjectEntry{
        .instance = std::move(instance),
        .type = type,
        .sizeBytes = sizeBytes,
        .debugName = std::string{debugName},
        .typeName = std::string{typeName},
    });
    std::println("[MemoryManagement] Created subobject '{}' (type '{}', {} bytes) at {}",
                 debugName, typeName, sizeBytes, address);
    return address;
}

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
void* MemoryManagement::findErased(std::type_index type) const {
    for (const ObjectEntry& entry : m_entries) {
        if (entry.type == type) {
            std::println("[MemoryManagement] Lookup hit for type '{}' at {}",
                         entry.typeName, static_cast<const void*>(entry.instance.get()));
            return entry.instance.get();
        }
    }
    std::println("[MemoryManagement] Lookup miss: no object of the requested type");
    return nullptr;
}

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
