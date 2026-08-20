/** @file RHIResources.cpp @brief Process-unique RHI resource identity */

#include "RHI/RHIResources.h"

#include "Core/Assert.h"

#include <atomic>

namespace RVX
{
    namespace
    {
        std::atomic<uint64> g_nextResourceInstanceId{1};

        RHIResourceInstanceId AllocateResourceInstanceId()
        {
            const uint64 value = g_nextResourceInstanceId.fetch_add(
                1, std::memory_order_relaxed);
            RVX_ASSERT_MSG(value != 0,
                           "RHI resource instance identity space exhausted");
            return RHIResourceInstanceId{value};
        }
    } // namespace

    RHIResource::RHIResource()
        : m_resourceInstanceId(AllocateResourceInstanceId())
    {
    }
} // namespace RVX
