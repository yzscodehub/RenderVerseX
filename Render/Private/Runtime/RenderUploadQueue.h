#pragma once

/**
 * @file RenderUploadQueue.h
 * @brief Bounded immutable upload request transport and exact byte pressure.
 */

#include "Render/RenderTransportTypes.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "Runtime/RenderResourceStatusTable.h"

#include <mutex>
#include <vector>

namespace RVX
{
    class RenderUploadQueue final
    {
    public:
        using WakeFunction = void (*)(void*) noexcept;

        RenderUploadQueue(const RenderTransportConfig& config,
                          RenderResourceStatusTable& statusTable,
                          WakeFunction wakeFunction = nullptr,
                          void* wakeContext = nullptr);
        RenderUploadQueue(const RenderUploadQueue&) = delete;
        RenderUploadQueue& operator=(const RenderUploadQueue&) = delete;

        RenderUploadEnqueueResult TryEnqueue(
            const ResourceUploadRequestRef& request) noexcept;
        [[nodiscard]] ResourceUploadRequestRef TryDequeue() noexcept;
        [[nodiscard]] uint32 GetRetainedCount() const noexcept;
        [[nodiscard]] uint64 GetRetainedBytes() const noexcept;

    private:
        void Wake() const noexcept;

        RenderResourceStatusTable& m_statusTable;
        uint32 m_requestCapacity = 0;
        uint64 m_byteCapacity = 0;
        mutable std::mutex m_mutex;
        std::vector<ResourceUploadRequestRef> m_slots;
        uint32 m_head = 0;
        uint32 m_count = 0;
        uint64 m_retainedBytes = 0;
        WakeFunction m_wakeFunction = nullptr;
        void* m_wakeContext = nullptr;
    };
} // namespace RVX
