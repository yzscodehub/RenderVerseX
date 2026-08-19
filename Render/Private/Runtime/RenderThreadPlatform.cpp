#include "Runtime/RenderThreadPlatform.h"

#if defined(_WIN32)
    #include <Windows.h>
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#endif

namespace RVX
{
    RenderThreadPlatformBootstrap::RenderThreadPlatformBootstrap() noexcept
    {
#if defined(_WIN32)
        SetThreadDescription(GetCurrentThread(), L"RVX Render");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), "RVX Render");

        // Render work uses the normal, non-realtime scheduler. Failure to
        // replace an inherited policy is non-fatal and preserves OS policy.
        sched_param parameters{};
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &parameters);
#endif
    }

    RenderThreadPlatformBootstrap::~RenderThreadPlatformBootstrap() noexcept =
        default;

    RenderThreadPlatformIterationScope::
        RenderThreadPlatformIterationScope() noexcept = default;

    RenderThreadPlatformIterationScope::
        ~RenderThreadPlatformIterationScope() noexcept = default;
} // namespace RVX
