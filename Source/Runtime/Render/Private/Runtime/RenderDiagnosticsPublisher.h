#pragma once

/**
 * @file RenderDiagnosticsPublisher.h
 * @brief Thread-safe publication of immutable render diagnostics snapshots.
 */

#include "Render/RenderDiagnostics.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

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
        /**
         * @brief Render-runtime-local storage for an immutable diagnostics snapshot.
         *
         * Apple libc++ does not yet provide C++20 atomic shared_ptr support. Keep
         * the publication contract identical and use a mutex only on standard
         * libraries that do not advertise the specialization.
         */
        class SnapshotStorage final : public NonMovable
        {
        public:
            using SnapshotRef =
                std::shared_ptr<const RenderDiagnosticsSnapshot>;

            explicit SnapshotStorage(SnapshotRef initialSnapshot)
                : m_snapshot(std::move(initialSnapshot))
            {
            }

            void Store(SnapshotRef snapshot) noexcept
            {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
                m_snapshot.store(std::move(snapshot), std::memory_order_release);
#else
                std::lock_guard lock(m_mutex);
                m_snapshot = std::move(snapshot);
#endif
            }

            [[nodiscard]] SnapshotRef Load() const noexcept
            {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
                return m_snapshot.load(std::memory_order_acquire);
#else
                std::lock_guard lock(m_mutex);
                return m_snapshot;
#endif
            }

        private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            std::atomic<SnapshotRef> m_snapshot;
#else
            mutable std::mutex m_mutex;
            SnapshotRef m_snapshot;
#endif
        };

        SnapshotStorage m_snapshot;
    };
} // namespace RVX
