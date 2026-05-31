#pragma once

#if defined(NDEBUG)
    #define APP_DEBUG_LOG(...) ((void)0)
#else// NOLINT
    #include <print> 
    #define APP_DEBUG_LOG(...) std::println(__VA_ARGS__)
#endif
