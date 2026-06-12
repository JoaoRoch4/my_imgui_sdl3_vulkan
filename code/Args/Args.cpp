#include "pch.hpp"

#include "Args.hpp"

Args::Args() : m_argc(0), m_argv() {}
Args::~Args() {}






void Args::parseArgs(int argc, char* const argv[]) {

    if (argc < 1 || !argv) return;

    std::span<char* const> raw{argv, static_cast<std::size_t>(argc)};

    // Ignore argv[0] (the program path) entirely: it is never a flag and must
    // not be echoed by printArgs() or matched by hasArg(). Everything after it
    // is a real argument.
    std::span<char* const> const flags = raw.subspan(1);

    m_argc = static_cast<int>(flags.size());

    m_argv.reserve(flags.size());

    for (std::string_view const arg : flags)
        m_argv.emplace_back(arg);
}
        
/**
 * @brief Checks whether a specific argument was passed on the command line.
 * @details Performs a linear search over the stored argument views with
 *          std::ranges::find. The comparison is exact and case-sensitive,
 *          returning on the first match. Safe to call before parsing: an
 *          empty container simply yields false. Silent by design so that a
 *          run with no arguments produces no output.
 * @param target The argument text to look for.
 * @return true if the argument is present, false otherwise.
 */
bool Args::hasArg(std::string_view target) const {
    return std::ranges::find(m_argv, target) != m_argv.end();
}

/**
 * @brief Prints every command-line argument that was captured.
 * @details Logs the total count as a header, then iterates the stored views
 *          using std::views::enumerate to pair each value with its index.
 *          argv[0] (the program path) is excluded at parse time, so the count
 *          and entries cover real arguments only; indices are printed 1-based
 *          to match their original position on the command line.
 */
void Args::printArgs() const {
    std::println("[Args] {} argument(s) captured:", m_argc);
    for (auto const [i, arg] : std::views::enumerate(m_argv)) {
        std::println("  argv[{}] = \"{}\"", i + 1, arg);
    }
}
