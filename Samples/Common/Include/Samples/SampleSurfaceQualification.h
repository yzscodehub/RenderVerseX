#pragma once

#include "Core/Types.h"

namespace RVX
{
    enum class SampleSurfaceObservation : uint8
    {
        Stable,
        AwaitingResize,
        ResizeAccepted,
        UnexpectedChange,
    };

    /** Tracks renderer-observed framebuffer extents, never inferred allocation growth. */
    class SampleSurfaceQualification final
    {
    public:
        void Begin(uint32 width, uint32 height, uint64 generation) noexcept
        {
            m_width = width;
            m_height = height;
            m_generation = generation;
            m_pending = false;
        }

        [[nodiscard]] bool Request(uint32 width, uint32 height) noexcept
        {
            if (m_pending || width == 0 || height == 0 ||
                (width == m_width && height == m_height))
                return false;
            m_requestedWidth = width;
            m_requestedHeight = height;
            m_pending = true;
            return true;
        }

        [[nodiscard]] SampleSurfaceObservation Observe(
            uint32 width, uint32 height, uint64 generation) noexcept
        {
            if (width == 0 || height == 0 || generation == 0 ||
                m_width == 0 || m_height == 0 || m_generation == 0)
                return SampleSurfaceObservation::UnexpectedChange;
            if (width == m_width && height == m_height && generation == m_generation)
                return m_pending ? SampleSurfaceObservation::AwaitingResize
                                 : SampleSurfaceObservation::Stable;
            if (!m_pending || width != m_requestedWidth || height != m_requestedHeight ||
                generation <= m_generation)
                return SampleSurfaceObservation::UnexpectedChange;
            Begin(width, height, generation);
            return SampleSurfaceObservation::ResizeAccepted;
        }

        [[nodiscard]] uint32 GetExpectedWidth() const noexcept
        { return m_pending ? m_requestedWidth : m_width; }
        [[nodiscard]] uint32 GetExpectedHeight() const noexcept
        { return m_pending ? m_requestedHeight : m_height; }

    private:
        uint32 m_width = 0;
        uint32 m_height = 0;
        uint64 m_generation = 0;
        uint32 m_requestedWidth = 0;
        uint32 m_requestedHeight = 0;
        bool m_pending = false;
    };
}
