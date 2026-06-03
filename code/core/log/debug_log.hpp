#pragma once

#if defined(__APP_FORCE_LOGS) || (defined(_DEBUG))
     #define APP_DEBUG_LOG(...) std::println(__VA_ARGS__)

#else
    #define APP_DEBUG_LOG(...) (static_cast<void>(0))
#endif
