#pragma once

#include "pch.hpp"

template <typename T>
[[nodiscard]] constexpr std::string_view TypeNameOf();

class MemoryManagement {
	public:

		static MemoryManagement& Get();

		MemoryManagement(MemoryManagement const&)            = delete;
		MemoryManagement& operator=(MemoryManagement const&) = delete;
		MemoryManagement(MemoryManagement&&)                 = delete;
		MemoryManagement& operator=(MemoryManagement&&)      = delete;


		template <typename T, typename... Args>
		[[nodiscard]] T* CreateDefaultSubobject(std::string_view debugName, Args&&... args) ;

		template <typename T> [[nodiscard]] T* GetSubobject() const;

		[[nodiscard]] std::size_t GetObjectCount() const;
		[[nodiscard]] std::size_t GetTotalMemoryBytes() const;
		void                      DumpReflectionInfo() const;

	private:

		MemoryManagement();
		~MemoryManagement();
		struct ObjectEntry {
				std::shared_ptr<void> instance;
				std::type_index       type;
				std::size_t           sizeBytes;
				std::string           debugName;
				std::string           typeName;
		};

		void* registerErased(std::shared_ptr<void> instance, std::type_index type, std::size_t sizeBytes,
			std::string_view debugName, std::string_view typeName);

		[[nodiscard]] void* findErased(std::type_index type) const;

		std::vector<ObjectEntry> m_entries;
};

// ── template / constexpr implementations ──────────────────────────────────────
// These must live in the header: callers in other translation units instantiate
// them, so the definitions have to be visible at every point of use. The
// non-template members live in Memory_management.cpp (compiled exactly once).

template <typename T> [[nodiscard]] constexpr std::string_view TypeNameOf() {
	constexpr std::string_view signature {__PRETTY_FUNCTION__};
	constexpr std::string_view key {"T = "};
	constexpr std::size_t      start {signature.find(key) + key.size()};
	constexpr std::size_t      end {signature.find_first_of(";]", start)};
	return signature.substr(start, end - start);
}

template <typename T, typename... Args>
[[nodiscard]] T* MemoryManagement::CreateDefaultSubobject(std::string_view debugName, Args&&... args) {
	std::shared_ptr<void> instance {std::make_unique<T>(std::forward<Args>(args)...)};
	void* address {registerErased(std::move(instance), std::type_index {typeid(T)}, sizeof(T), debugName,
		TypeNameOf<T>())};
	return static_cast<T*>(address);
}

template <typename T> [[nodiscard]] T* MemoryManagement::GetSubobject() const {
	void* address {findErased(std::type_index {typeid(T)})};
	return static_cast<T*>(address);
}
