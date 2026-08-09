#include "Runtime/RenderResourceStatusTable.h"

#include <limits>
#include <stdexcept>

namespace RVX
{
namespace
{
    constexpr uint32 RVX_MINIMUM_STATUS_CAPACITY = 1024;
    constexpr uint64 RVX_STATUS_RESERVED_BITS_MASK = uint64{0xFF} << 8U;

    [[nodiscard]] bool IsKnownState(RenderResourcePublicState state) noexcept
    {
        return static_cast<uint8>(state) <=
               static_cast<uint8>(RenderResourcePublicState::Replacing);
    }

    [[nodiscard]] bool IsKnownFailure(
        RenderResourceFailureCode failure) noexcept
    {
        return static_cast<uint16>(failure) <=
               static_cast<uint16>(RenderResourceFailureCode::RuntimeFailure);
    }

    [[nodiscard]] uint64 Pack(PackedRenderResourceStatus status) noexcept
    {
        return (static_cast<uint64>(status.generation) << 32U) |
               (static_cast<uint64>(status.failure) << 16U) |
               static_cast<uint64>(status.state);
    }

    [[nodiscard]] bool Decode(uint64 word,
                              PackedRenderResourceStatus& status) noexcept
    {
        if ((word & RVX_STATUS_RESERVED_BITS_MASK) != 0)
        {
            return false;
        }

        const auto state = static_cast<RenderResourcePublicState>(word & 0xFFU);
        const auto failure = static_cast<RenderResourceFailureCode>(
            (word >> 16U) & 0xFFFFU);
        if (!IsKnownState(state) || !IsKnownFailure(failure))
        {
            return false;
        }

        status.generation = static_cast<uint32>(word >> 32U);
        status.state = state;
        status.failure = failure;
        return true;
    }

    [[nodiscard]] bool IsLiveState(RenderResourcePublicState state) noexcept
    {
        return state == RenderResourcePublicState::Reserved ||
               state == RenderResourcePublicState::UploadQueued ||
               state == RenderResourcePublicState::Uploading ||
               state == RenderResourcePublicState::GPUReady ||
               state == RenderResourcePublicState::Failed ||
               state == RenderResourcePublicState::ReplacementQueued ||
               state == RenderResourcePublicState::Replacing;
    }

    [[nodiscard]] bool IsValidTransition(
        PackedRenderResourceStatus expected,
        PackedRenderResourceStatus desired,
        RenderStatusWriter writer) noexcept
    {
        if (!IsKnownState(expected.state) || !IsKnownState(desired.state) ||
            !IsKnownFailure(expected.failure) ||
            !IsKnownFailure(desired.failure))
        {
            return false;
        }

        if (writer == RenderStatusWriter::Update)
        {
            if (expected.state == RenderResourcePublicState::Released &&
                desired.state == RenderResourcePublicState::Reserved)
            {
                return expected.generation !=
                           std::numeric_limits<uint32>::max() &&
                       desired.generation == expected.generation + 1U &&
                       desired.generation != 0;
            }

            if (desired.generation != expected.generation)
            {
                return false;
            }

            return (expected.state == RenderResourcePublicState::Reserved &&
                    desired.state ==
                        RenderResourcePublicState::UploadQueued) ||
                   (expected.state == RenderResourcePublicState::GPUReady &&
                    desired.state ==
                        RenderResourcePublicState::ReplacementQueued) ||
                   (IsLiveState(expected.state) &&
                    desired.state == RenderResourcePublicState::Evicting);
        }

        if (writer == RenderStatusWriter::Render)
        {
            if (desired.generation != expected.generation)
            {
                return false;
            }

            return (expected.state ==
                        RenderResourcePublicState::UploadQueued &&
                    desired.state == RenderResourcePublicState::Uploading) ||
                   (expected.state ==
                        RenderResourcePublicState::ReplacementQueued &&
                    desired.state == RenderResourcePublicState::Replacing) ||
                   (expected.state == RenderResourcePublicState::Uploading &&
                    (desired.state == RenderResourcePublicState::GPUReady ||
                     desired.state == RenderResourcePublicState::Failed)) ||
                   (expected.state == RenderResourcePublicState::Replacing &&
                    (desired.state == RenderResourcePublicState::GPUReady ||
                     desired.state == RenderResourcePublicState::Failed)) ||
                   (expected.state == RenderResourcePublicState::Evicting &&
                    desired.state == RenderResourcePublicState::Released);
        }

        return false;
    }
} // namespace

    RenderResourceStatusTable::RenderResourceStatusTable(uint32 capacity)
        : m_capacity(capacity)
    {
        if (capacity < RVX_MINIMUM_STATUS_CAPACITY)
        {
            throw std::invalid_argument(
                "Render resource status capacity must be at least 1024");
        }

        m_words = std::make_unique<std::atomic<uint64>[]>(capacity);
        for (uint32 slot = 0; slot < capacity; ++slot)
        {
            m_words[slot].store(0, std::memory_order_relaxed);
        }
    }

    RenderResourceStatus RenderResourceStatusTable::Query(
        RenderResourceHandle handle) const noexcept
    {
        RenderResourceStatus result;
        if (!handle.IsValid() || handle.slot >= m_capacity)
        {
            return result;
        }

        PackedRenderResourceStatus status;
        if (!LoadSlot(handle.slot, status))
        {
            return result;
        }

        if (status.generation != handle.generation)
        {
            result.code = RenderResourceStatusCode::StaleGeneration;
            return result;
        }

        result.code = RenderResourceStatusCode::Current;
        result.state = status.state;
        result.failure = status.failure;
        return result;
    }

    bool RenderResourceStatusTable::CompareExchange(
        RenderResourceHandle handle,
        PackedRenderResourceStatus expected,
        PackedRenderResourceStatus desired,
        RenderStatusWriter writer) noexcept
    {
        if (handle.slot == 0 || handle.slot >= m_capacity)
        {
            return false;
        }

        PackedRenderResourceStatus observedStatus;
        uint64 observedWord =
            m_words[handle.slot].load(std::memory_order_acquire);
        if (!Decode(observedWord, observedStatus) ||
            observedStatus.generation != handle.generation)
        {
            return false;
        }

        if (expected.generation != handle.generation ||
            Pack(observedStatus) != Pack(expected) ||
            !IsValidTransition(expected, desired, writer))
        {
            return false;
        }

        const uint64 expectedWord = Pack(expected);
        const uint64 desiredWord = Pack(desired);
        observedWord = expectedWord;
        for (;;)
        {
            if (m_words[handle.slot].compare_exchange_weak(
                    observedWord,
                    desiredWord,
                    std::memory_order_release,
                    std::memory_order_relaxed))
            {
                return true;
            }

            if (!Decode(observedWord, observedStatus) ||
                observedStatus.generation != handle.generation ||
                Pack(observedStatus) != expectedWord ||
                !IsValidTransition(expected, desired, writer))
            {
                return false;
            }
        }
    }

    uint32 RenderResourceStatusTable::GetCapacity() const noexcept
    {
        return m_capacity;
    }

    bool RenderResourceStatusTable::LoadSlot(
        uint32 slot,
        PackedRenderResourceStatus& status) const noexcept
    {
        if (slot == 0 || slot >= m_capacity)
        {
            return false;
        }

        const uint64 word = m_words[slot].load(std::memory_order_acquire);
        return Decode(word, status);
    }
} // namespace RVX
