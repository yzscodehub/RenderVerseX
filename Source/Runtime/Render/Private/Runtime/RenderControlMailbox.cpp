#include "Runtime/RenderControlMailbox.h"

namespace RVX
{
    RenderControlSignal::RenderControlSignal(WakeFunction wakeFunction,
                                             void* wakeContext) noexcept
        : m_wakeFunction(wakeFunction), m_wakeContext(wakeContext)
    {
    }

    void RenderControlSignal::RequestStop() noexcept
    {
        m_stopRequested.store(true, std::memory_order_release);
        Wake();
    }

    bool RenderControlSignal::IsStopRequested() const noexcept
    {
        return m_stopRequested.load(std::memory_order_acquire);
    }

    void RenderControlSignal::Wake() const noexcept
    {
        if (m_wakeFunction != nullptr)
        {
            m_wakeFunction(m_wakeContext);
        }
    }
} // namespace RVX
