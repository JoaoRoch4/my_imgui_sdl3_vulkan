#pragma once

#include "pch.hpp"

class AppContext;
class sdl3_context;

class App {
public:
    static constexpr int k_reopen_exit_code = 42;

    App();
    bool run();

    protected:

    void KickStart();

    private:

    AppContext* m_AppContext;
    sdl3_context* m_Sdl;

};
