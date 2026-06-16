#pragma once
#include "pch.hpp"

template <typename T> [[nodiscard]] constexpr std::string_view TypeNameOf() {
	constexpr std::string_view signature {__PRETTY_FUNCTION__};
	constexpr std::string_view key {"T = "};
	constexpr std::size_t      start {signature.find(key) + key.size()};
	constexpr std::size_t      end {signature.find_first_of(";]", start)};
	return signature.substr(start, end - start);
}

class MemoryManagement {

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
		void* findErased(std::type_index type) const {
			for (ObjectEntry const& entry : m_entries) {
				if (entry.type == type) {
					std::println("[MemoryManagement] Lookup hit for type '{}' at {}", entry.typeName,
						static_cast<void const*>(entry.instance.get()));
					return entry.instance.get();
				}
			}
			std::println("[MemoryManagement] Lookup miss: no object of the requested type");
			return nullptr;
		}

		void* registerErased(std::shared_ptr<void> instance, std::type_index type, std::size_t sizeBytes,
			std::string_view debugName, std::string_view typeName) {
			void* address {instance.get()};
			m_entries.push_back(ObjectEntry {
				.instance  = std::move(instance),
				.type      = type,
				.sizeBytes = sizeBytes,
				.debugName = std::string {debugName},
				.typeName  = std::string {typeName},
			});
			std::println("[ObjectRegistry] Created subobject '{}' (type '{}', {} bytes) at {}", debugName, typeName,
				sizeBytes, address);
			return address;
		}


	public:

		static MemoryManagement& Get() {
			static MemoryManagement instance;
			return instance;
		}

		static MemoryManagement* GetPtr() { return &Get(); }


		MemoryManagement(MemoryManagement const&)            = delete;
		MemoryManagement& operator=(MemoryManagement const&) = delete;
		MemoryManagement(MemoryManagement&&)                 = delete;
		MemoryManagement& operator=(MemoryManagement&&)      = delete;

		template <typename T, typename... Args>
		[[nodiscard]] T* PushGet(std::string_view debugName, Args&&... args) {
			std::shared_ptr<void> instance {std::make_unique<T>(std::forward<Args>(args)...)};
			void* address {registerErased(std::move(instance), std::type_index {typeid(T)}, sizeof(T), debugName,
				TypeNameOf<T>())};
			return static_cast<T*>(address);
		}

		template <typename T, typename... Args> [[nodiscard]] T* PushGet(std::string_view debugName) {
			std::shared_ptr<void> instance {std::make_unique<T>()};
			void* address {registerErased(std::move(instance), std::type_index {typeid(T)}, sizeof(T), debugName,
				TypeNameOf<T>())};
			return static_cast<T*>(address);
		}

		template <typename T> [[nodiscard]] T* GetSubobject() const {
			void* address {findErased(std::type_index {typeid(T)})};
			return static_cast<T*>(address);
		}

		/**
		 * @brief Static, type-keyed accessor for an already-stored object.
		 *
		 * The static sibling of GetSubobject: any class anywhere can reach a
		 * shared object by type without first obtaining the singleton, e.g.
		 * MemoryManagement::GetInstance<sdl3_context>(). The lookup itself is
		 * not duplicated here — it routes through the global instance and the
		 * existing const GetSubobject so the search logic has a single home.
		 * A missing object is treated as an unrecoverable invariant violation
		 * (the type was never PushGet'd): the failure is logged and the
		 * process aborts, trapping immediately under the debugger rather than
		 * propagating a null that some distant call site would dereference.
		 */
		template <typename T> [[nodiscard]] static T* GetInstance() {
			T* instance {Get().GetSubobject<T>()};
			if (!instance) {
				std::println("[MemoryManagement] Fatal: no stored object of type '{}' — was it PushGet'd first?",
					TypeNameOf<T>());
				std::abort();
			}
			return instance;
		}

		/**
		 * @brief Destroys every stored object of type T right now.
		 *
		 * The deterministic counterpart to PushGet: erasing the entry drops the
		 * owning shared_ptr<void>, so the object's destructor runs at THIS call
		 * site instead of being deferred to static-exit teardown. Returns the
		 * registry to a state where a later PushGet<T> creates a genuinely fresh
		 * instance — without this, a second PushGet<T> would append a duplicate
		 * and the type-keyed lookup would keep handing back the stale first one.
		 * Call only after any explicit cleanup method (cleanup()/shutdown()) has
		 * run, and never on a type whose instance is executing this code.
		 */
		template <typename T> void Release() {
			std::type_index const type {typeid(T)};
			std::size_t const     removed {std::erase_if(m_entries,
                [type](ObjectEntry const& entry) { return entry.type == type; })};
			if (removed > 0)
				std::println("[MemoryManagement] Released '{}' ({} entr{})", TypeNameOf<T>(), removed,
					removed == 1 ? "y" : "ies");
		}

		/**
		 * @brief Destroys all stored objects in reverse registration order.
		 *
		 * Pops entries back-to-front (LIFO) so dependents fall before the
		 * dependencies they were built on, mirroring how stack-allocated members
		 * unwind. Intended for a clean global shutdown. Must NOT be invoked from
		 * inside a method of an object that is itself registered (e.g. App), as
		 * it would free the very object running the call.
		 */
		void ReleaseAll() {
			while (!m_entries.empty())
				m_entries.pop_back();
		}

		[[nodiscard]] std::size_t GetObjectCount() const;
		[[nodiscard]] std::size_t GetTotalMemoryBytes() const;
		void                      DumpReflectionInfo() const;

	private:

		MemoryManagement()
			: m_entries {} {
			std::println("[MemoryManagement] Global registry constructed at {}", static_cast<void const*>(this));
		}

		struct ObjectEntry {
				std::shared_ptr<void> instance;
				std::type_index       type;
				std::size_t           sizeBytes;
				std::string           debugName;
				std::string           typeName;
		};




		std::vector<ObjectEntry> m_entries;
};

