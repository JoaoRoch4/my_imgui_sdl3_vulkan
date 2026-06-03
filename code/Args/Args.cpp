#include "pch.hpp"

#include "Args.hpp"

Args::Args() : m_argc(0), m_argv() {}
Args::~Args() {}






void Args::parseArgs(int argc, char* const argv[]) {

    if (argc < 1 || !argv) return;

    std::span<char* const> raw{argv, static_cast<std::size_t>(argc)};

    m_argc = argc;

    m_argv.reserve(raw.size());

 for (std::string_view const arg : raw)         m_argv.emplace_back(arg);
}
        
/**
 * @brief Checks whether a specific argument was passed on the command line.
 * @details Performs a linear search over the stored argument views with
 *          std::ranges::find. The comparison is exact and case-sensitive,
 *          returning on the first match. Safe to call before parsing: an
 *          empty container simply yields false. The query result is logged
 *          for traceability during command-line handling.
 * @param target The argument text to look for.
 * @return true if the argument is present, false otherwise.
 */
bool Args::hasArg(std::string_view target) const {
    bool const found{std::ranges::find(m_argv, target) != m_argv.end()};
    std::println("[Args] hasArg(\"{}\") -> {}", target, found);
    return found;
}

/**
 * @brief Prints every command-line argument that was captured.
 * @details Logs the total count as a header, then iterates the stored views
 *          using std::views::enumerate to pair each value with its index.
 *          Indices follow the conventional argv layout, where position 0 is
 *          the program name. The container being empty is handled gracefully,
 *          reporting a count of zero and printing no entries afterwards.
 */
void Args::printArgs() const {
    std::println("[Args] {} argument(s) captured:", m_argc);
    for (auto const [i, arg] : std::views::enumerate(m_argv)) {
        std::println("  argv[{}] = \"{}\"", i, arg);
    }
}
