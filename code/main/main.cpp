#include "app.hpp"

int main(int, char**)
{
    while (true) {
        App app;
        const int code = app.run();
        if (code != App::k_reopen_exit_code)
            return code;
    }
}
