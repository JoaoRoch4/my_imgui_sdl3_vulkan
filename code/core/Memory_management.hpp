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