#include "Runtime/RenderReleaseQueue.h"

#include <exception>
#include <stdexcept>

namespace RVX
{
namespace
{
    [[nodiscard]] uint32 GetUsableCapacityOrThrow(
        const RenderResourceStatusTable& statusTable,
        uint32 statusSlotCapacity)
    {
        if (statusSlotCapacity < 1024U ||
            statusSlotCapacity != statusTable.GetCapacity())
        {
            throw std::invalid_argument(
                "Render release capacity must match the status table");
        }
        return statusSlotCapacity - 1U;
    }
} // namespace

    RenderReleaseQueue::RenderReleaseQueue(
        RenderResourceReservationDirectory& reservationDirectory,
        const RenderResourceStatusTable& statusTable,
        uint32 statusSlotCapacity,
        WakeFunction wakeFunction,
        void* wakeContext,
        RuntimeFatalFunction runtimeFatalFunction,
        void* runtimeFatalContext)
        : m_reservationDirectory(reservationDirectory),
          m_usableCapacity(
              GetUsableCapacityOrThrow(statusTable, statusSlotCapacity)),
          m_slots(m_usableCapacity),
          m_wakeFunction(wakeFunction),
          m_wakeContext(wakeContext),
          m_runtimeFatalFunction(runtimeFatalFunction),
          m_runtimeFatalContext(runtimeFatalContext)
    {
    }

    RenderReleaseResult RenderReleaseQueue::RequestRelease(
        RenderResourceHandle handle) noexcept
    {
        const RenderReleaseResult result =
            m_reservationDirectory.RequestRelease(handle);
        if (result.code != RenderReleaseCode::Accepted)
        {
            return result;
        }

        bool publicationFailed = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_count == m_usableCapacity)
            {
                publicationFailed = true;
            }
            else
            {
                const uint32 appendIndex =
                    (m_head + m_count) % m_usableCapacity;
                m_slots[appendIndex] = handle;
                ++m_count;
            }
        }

        if (publicationFailed)
        {
            RuntimeFatal(
                "Accepted render release could not be published to its ring");
            return result;
        }

        Wake();
        return result;
    }

    RenderResourceHandle RenderReleaseQueue::TryDequeue() noexcept
    {
        std::lock_guard lock(m_mutex);
        if (m_count == 0U)
        {
            return {};
        }

        const RenderResourceHandle handle = m_slots[m_head];
        m_slots[m_head] = {};
        m_head = (m_head + 1U) % m_usableCapacity;
        --m_count;
        return handle;
    }

    uint32 RenderReleaseQueue::GetUsableCapacity() const noexcept
    {
        return m_usableCapacity;
    }

    uint32 RenderReleaseQueue::GetPendingCount() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_count;
    }

    void RenderReleaseQueue::Wake() const noexcept
    {
        if (m_wakeFunction != nullptr)
        {
            m_wakeFunction(m_wakeContext);
        }
    }

    void RenderReleaseQueue::RuntimeFatal(const char* message) const noexcept
    {
        if (m_runtimeFatalFunction != nullptr)
        {
            m_runtimeFatalFunction(m_runtimeFatalContext, message);
            return;
        }
        std::terminate();
    }
} // namespace RVX
