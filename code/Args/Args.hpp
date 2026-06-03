#pragma once

#include "pch.hpp"

class Args {

	public:

		Args();
		~Args();
		void parseArgs(int argc, char* const argv[]);
           [[nodiscard]] bool hasArg(std::string_view target) const;
    void               printArgs() const;

	private:

		int                           m_argc;
std::vector<std::string_view> m_argv;
};