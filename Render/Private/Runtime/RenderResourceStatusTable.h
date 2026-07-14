#pragma once

/**
 * @file RenderResourceStatusTable.h
 * @brief Fixed-capacity atomic render-resource status publication.
 */

#include "RenderContracts/IRenderResourceGateway.h"

#include <atomic>
#include <memory>

namespace RVX
{
    struct PackedRenderResourceStatus
    {
        uint32 generation = 0;
        RenderResourcePublicState state = RenderResourcePublicState::Released;
        RenderResourceFailureCode failure = RenderResourceFailureCode::None;
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
        std::unique_ptr<std::atomic<uint64>[]> m_words;
    };
} // namespace RVX
