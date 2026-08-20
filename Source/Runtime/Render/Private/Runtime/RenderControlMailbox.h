#pragma once

/**
 * @file RenderControlMailbox.h
 * @brief Generation-coalesced owned control values and stop preemption.
 */

#include "Core/Types.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace RVX
{
    class RenderControlSignal final
    {
    public:
        using WakeFunction = void (*)(void*) noexcept;

        explicit RenderControlSignal(WakeFunction wakeFunction = nullptr,
                                     void* wakeContext = nullptr) noexcept;
        RenderControlSignal(const RenderControlSignal&) = delete;
        RenderControlSignal& operator=(const RenderControlSignal&) = delete;

        void RequestStop() noexcept;
        [[nodiscard]] bool IsStopRequested() const noexcept;
        void Wake() const noexcept;

    private:
        std::atomic<bool> m_stopRequested = false;
        WakeFunction m_wakeFunction = nullptr;
        void* m_wakeContext = nullptr;
    };

    template <typename Value>
    struct GenerationOwnedControlValue
    {
        uint64 generation = 0;
        Value value;
    };

    template <typename SurfaceValue, typename ResizeValue>
    struct BasicRenderControlBatch
    {
        bool stopRequested = false;
        std::optional<GenerationOwnedControlValue<SurfaceValue>> surface;
        std::optional<GenerationOwnedControlValue<ResizeValue>> resize;
    };

    template <typename SurfaceValue, typename ResizeValue>
    class BasicRenderControlMailbox final
    {
    public:
        static_assert(std::is_nothrow_move_constructible_v<SurfaceValue>);
        static_assert(std::is_nothrow_move_assignable_v<SurfaceValue>);
        static_assert(std::is_nothrow_move_constructible_v<ResizeValue>);
        static_assert(std::is_nothrow_move_assignable_v<ResizeValue>);

        using WakeFunction = RenderControlSignal::WakeFunction;

        explicit BasicRenderControlMailbox(
            WakeFunction wakeFunction = nullptr,
            void* wakeContext = nullptr) noexcept
            : m_signal(wakeFunction, wakeContext)
        {
        }

        BasicRenderControlMailbox(const BasicRenderControlMailbox&) = delete;
        BasicRenderControlMailbox& operator=(
            const BasicRenderControlMailbox&) = delete;

        bool TryPublishSurface(uint64 generation, SurfaceValue value)
        {
            if (generation == 0U)
            {
                return false;
            }

            std::optional<GenerationOwnedControlValue<SurfaceValue>> replaced;
            {
                std::lock_guard lock(m_mutex);
                if (generation <= m_surfaceGeneration)
                {
                    return false;
                }
                replaced = std::move(m_surface);
                m_surface.emplace(generation, std::move(value));
                m_surfaceGeneration = generation;
            }
            m_signal.Wake();
            return true;
        }

        bool TryPublishResize(uint64 generation, ResizeValue value)
        {
            if (generation == 0U)
            {
                return false;
            }

            std::optional<GenerationOwnedControlValue<ResizeValue>> replaced;
            {
                std::lock_guard lock(m_mutex);
                if (generation <= m_resizeGeneration)
                {
                    return false;
                }
                replaced = std::move(m_resize);
                m_resize.emplace(generation, std::move(value));
                m_resizeGeneration = generation;
            }
            m_signal.Wake();
            return true;
        }

        void RequestStop() noexcept
        {
            m_signal.RequestStop();
        }

        [[nodiscard]] bool IsStopRequested() const noexcept
        {
            return m_signal.IsStopRequested();
        }

        [[nodiscard]] BasicRenderControlBatch<SurfaceValue, ResizeValue>
            AcquireLatest()
        {
            BasicRenderControlBatch<SurfaceValue, ResizeValue> result;
            std::lock_guard lock(m_mutex);
            if (m_signal.IsStopRequested())
            {
                result.stopRequested = true;
                return result;
            }

            result.surface = std::move(m_surface);
            result.resize = std::move(m_resize);
            m_surface.reset();
            m_resize.reset();
            return result;
        }

    private:
        RenderControlSignal m_signal;
        mutable std::mutex m_mutex;
        std::optional<GenerationOwnedControlValue<SurfaceValue>> m_surface;
        std::optional<GenerationOwnedControlValue<ResizeValue>> m_resize;
        uint64 m_surfaceGeneration = 0;
        uint64 m_resizeGeneration = 0;
    };
} // namespace RVX
