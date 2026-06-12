#include "pch.hpp"
#include "main.hpp"
#include "app.hpp"

#include "Args.hpp"
#include "startup_options.hpp"


int main(int argc, char* argv[])  {

    return start(argc, argv);
}

int start(int argc, char* argv[]) {

    // Parse the command line once, up front. argv strings live for the whole
    // process, so the resolved options are valid for every App we construct in
    // the reopen loop below — CLI flags survive a reopen.
    Args args;
    args.parseArgs(argc, argv);
    if (!args.empty()) // stay silent when no arguments were passed
        args.printArgs();
    StartupOptions const opts = resolve_startup_options(args);

 while (true) {
	App	  app{opts};
	const int code = app.run();
	if (code != App::k_reopen_exit_code)
	    return code;
    }
}
