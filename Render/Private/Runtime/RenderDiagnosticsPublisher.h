#pragma once

/**
 * @file RenderDiagnosticsPublisher.h
 * @brief Atomic publication of immutable render diagnostics snapshots.
 */

#include "Render/RenderDiagnostics.h"

#include <atomic>
#include <memory>
#include <string>

namespace RVX
{
    class RenderDiagnosticsPublisher final : public NonMovable
    {
    public:
        RenderDiagnosticsPublisher();

        void Publish(RenderDiagnosticsSnapshot snapshot);
        [[nodiscard]] std::shared_ptr<const RenderDiagnosticsSnapshot>
            AcquireShared() const noexcept;
        [[nodiscard]] RenderDiagnosticsSnapshot GetSnapshot() const;
        /** @brief Best-effort owned JSON artifact for fatal-process handoff. */
        [[nodiscard]] static bool SaveArtifact(
            const RenderDiagnosticsSnapshot& snapshot,
            const std::string& path) noexcept;

    private:
        std::atomic<std::shared_ptr<const RenderDiagnosticsSnapshot>> m_snapshot;
    };
} // namespace RVX
