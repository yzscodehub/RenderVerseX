#include "Runtime/RenderDiagnosticsPublisher.h"

#include <utility>

namespace RVX
{
    RenderDiagnosticsPublisher::RenderDiagnosticsPublisher()
        : m_snapshot(std::make_shared<const RenderDiagnosticsSnapshot>())
    {
    }

    void RenderDiagnosticsPublisher::Publish(RenderDiagnosticsSnapshot snapshot)
    {
        auto publication =
            std::make_shared<const RenderDiagnosticsSnapshot>(std::move(snapshot));
        m_snapshot.store(std::move(publication), std::memory_order_release);
    }

    std::shared_ptr<const RenderDiagnosticsSnapshot>
        RenderDiagnosticsPublisher::AcquireShared() const noexcept
    {
        return m_snapshot.load(std::memory_order_acquire);
    }

    RenderDiagnosticsSnapshot RenderDiagnosticsPublisher::GetSnapshot() const
    {
        const auto snapshot = AcquireShared();
        return snapshot != nullptr ? *snapshot : RenderDiagnosticsSnapshot{};
    }
} // namespace RVX
