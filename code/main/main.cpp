#include "pch.hpp"
#include "main.hpp"
#include "app.hpp"


int main(int argc, char* argv[])  {
   
    return start();
}

int start() {

 while (true) {
	App	  app;
	const int code = app.run();
	if (code != App::k_reopen_exit_code)
	    return code;
    }
}

