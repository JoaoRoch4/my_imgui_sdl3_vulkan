#pragma once

#include "pch.hpp"


class App {
public:
    static constexpr int k_reopen_exit_code = 42;

    App();
    int run();
};
