#include "Runtime/RenderThreadPlatform.h"

#import <Foundation/Foundation.h>

#include <pthread.h>
#include <pthread/qos.h>

namespace RVX
{
    RenderThreadPlatformBootstrap::RenderThreadPlatformBootstrap() noexcept
    {
        m_state = reinterpret_cast<void*>([[NSAutoreleasePool alloc] init]);
        pthread_setname_np("RVX Render");
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    }

    RenderThreadPlatformBootstrap::~RenderThreadPlatformBootstrap() noexcept
    {
        [reinterpret_cast<NSAutoreleasePool*>(m_state) drain];
        m_state = nullptr;
    }

    RenderThreadPlatformIterationScope::
        RenderThreadPlatformIterationScope() noexcept
    {
        m_state = reinterpret_cast<void*>([[NSAutoreleasePool alloc] init]);
    }

    RenderThreadPlatformIterationScope::
        ~RenderThreadPlatformIterationScope() noexcept
    {
        [reinterpret_cast<NSAutoreleasePool*>(m_state) drain];
        m_state = nullptr;
    }
} // namespace RVX
