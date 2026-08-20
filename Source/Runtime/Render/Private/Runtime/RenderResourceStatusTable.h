#pragma once

/**
 * @file RenderResourceStatusTable.h
 * @brief Fixed-capacity atomic render-resource status publication.
 */

#include "RenderContracts/IRenderResourceGateway.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace RVX
{
    /** Preserve the currently published revision during a status-only change. */
    inline constexpr uint64 RVX_RENDER_RESOURCE_CONTENT_REVISION_PRESERVE =
        ~uint64{0};

    struct PackedRenderResourceStatus
    {
        uint32 generation = 0;
        RenderResourcePublicState state = RenderResourcePublicState::Released;
        RenderResourceFailureCode failure = RenderResourceFailureCode::None;
        /**
         * Set an exact value only when content becomes committed. Status-only
         * transitions preserve the current value by default.
         */
        uint64 committedContentRevision =
            RVX_RENDER_RESOURCE_CONTENT_REVISION_PRESERVE;
    };

    enum class RenderStatusWriter : uint8
    {
        Update = 0,
        Render = 1
    };

    class RenderResourceReservationDirectory;
    struct RenderResourceStatusTableTestAccess;

    class RenderResourceStatusTable final
    {
    public:
        explicit RenderResourceStatusTable(uint32 capacity);
        RenderResourceStatus Query(RenderResourceHandle handle) const noexcept;
        bool CompareExchange(RenderResourceHandle handle,
                             PackedRenderResourceStatus expected,
                             PackedRenderResourceStatus desired,
                             RenderStatusWriter writer) noexcept;
        [[nodiscard]] uint32 GetCapacity() const noexcept;

    private:
        friend class RenderResourceReservationDirectory;
        friend struct RenderResourceStatusTableTestAccess;

        bool LoadSlot(uint32 slot,
                      PackedRenderResourceStatus& status) const noexcept;

        uint32 m_capacity = 0;
        // Serializes each two-word status/value publication. The status word
        // remains atomic for existing fixed-table storage and test seams, while
        // this lock makes Query observe the corresponding revision linearly.
        mutable std::mutex m_mutex;
        std::unique_ptr<std::atomic<uint64>[]> m_words;
        std::unique_ptr<std::atomic<uint64>[]> m_contentRevisions;
    };
} // namespace RVX
