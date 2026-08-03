#pragma once

/**
 * @file RenderVisibility.h
 * @brief Frame-owned camera visibility candidates and canonical CPU/GPU policy inputs.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Policy/RenderPolicyTypes.h"

#include <array>
#include <vector>

namespace RVX
{
    /** @brief Stable frame/view identity for one visibility candidate. */
    struct RenderVisibilityCandidate
    {
        uint32 candidateIndex = RVX_INVALID_INDEX;
        uint32 sourcePacketIndex = RVX_INVALID_INDEX;
        /// Compatibility/debug value only. It is never used as an identity lookup.
        uint32 sourceOrdinal = 0;
        uint32 objectIndex = RVX_INVALID_INDEX;
        RenderPassKind pass = RenderPassKind::None;
        bool objectVisible = true;
        bool drawable = false;
        AABB worldBounds{};
    };

    /** @brief Sanitized GPU payload derived from a visibility candidate. */
    struct RenderVisibilityGPUInput
    {
        Vec4 aabbMin{0.0f};
        Vec4 aabbMax{0.0f};
        uint32 forceVisible = 0;
    };

    /** @brief Value-only camera visibility request shared by CPU providers. */
    struct RenderVisibilityRequest
    {
        Mat4 viewProjection = Mat4Identity();
        Vec3 cameraPosition{0.0f};
        bool enableDistanceCulling = false;
        float maxDrawDistance = 0.0f;
    };

    /** @brief Canonical world-space AABB frustum. Clip depth is [0, 1]. */
    struct RenderVisibilityFrustum
    {
        std::array<Vec4, 6> planes{};

        [[nodiscard]] static RenderVisibilityFrustum FromViewProjection(
            const Mat4& viewProjection) noexcept;
    };

    [[nodiscard]] bool IsFiniteRenderVisibilityBounds(const AABB& bounds) noexcept;
    [[nodiscard]] RenderVisibilityGPUInput MakeRenderVisibilityGPUInput(
        const AABB& bounds) noexcept;
    /**
     * @brief Conservative canonical AABB/frustum test.
     * Invalid or non-finite bounds are visible and set @p outInvalidBounds.
     */
    [[nodiscard]] bool IsRenderVisibilityAABBVisible(
        const AABB& bounds,
        const RenderVisibilityFrustum& frustum,
        bool* outInvalidBounds = nullptr) noexcept;

    /** @brief Immutable-in-use, frame/view-owned candidate collection. */
    class RenderCandidateSet
    {
    public:
        void Clear();
        [[nodiscard]] uint32 Add(RenderVisibilityCandidate candidate);
        [[nodiscard]] const RenderVisibilityCandidate* Find(
            RenderPassKind pass,
            uint32 sourcePacketIndex) const noexcept;
        [[nodiscard]] const std::vector<RenderVisibilityCandidate>& GetCandidates() const
        {
            return m_candidates;
        }
        [[nodiscard]] bool IsValid() const noexcept { return m_valid; }

    private:
        static constexpr size_t kPassCount =
            static_cast<size_t>(RenderPassKind::Transparent) + 1;
        std::vector<RenderVisibilityCandidate> m_candidates;
        std::array<std::vector<uint32>, kPassCount> m_sourceToCandidate{};
        bool m_valid = true;
    };

    struct RenderVisibilityPassResult
    {
        RenderPassKind pass = RenderPassKind::None;
        std::vector<uint32> coarseCandidateIndices;
        /// Dense sourcePacketIndex visibility bitset. It is pass-aware.
        std::vector<uint8> cpuVisibleBySourcePacket;
        std::vector<uint8> sourcePacketKnown;

        [[nodiscard]] bool IsSourcePacketVisible(uint32 sourcePacketIndex) const noexcept;
        [[nodiscard]] bool HasSourcePacket(uint32 sourcePacketIndex) const noexcept;
    };

    struct RenderVisibilityDiagnostics
    {
        uint32 sceneObjectCandidateCount = 0;
        uint32 cpuVisibleObjectCount = 0;
        uint32 passCandidateCount = 0;
        uint32 invalidBoundsCount = 0;
        uint32 gpuDeferredCandidateCount = 0;
        bool gpuReadbackPerformed = false;
        bool occlusionRequested = false;
        bool occlusionAvailable = false;
    };

    /** @brief Frame-owned visibility result borrowed by current pass recording only. */
    struct RenderVisibilityResult
    {
        bool structurallyValid = true;
        std::vector<uint32> cpuVisibleObjectIndices;
        std::vector<uint32> coarseVisibleObjectIndices;
        /// Dense object-index masks used to project one object test onto pass packets.
        std::vector<uint8> cpuVisibleObjectMask;
        std::vector<uint8> coarseVisibleObjectMask;
        std::array<RenderVisibilityPassResult,
                   static_cast<size_t>(RenderPassKind::Transparent) + 1> passes{};
        RenderVisibilityDiagnostics diagnostics{};

        [[nodiscard]] bool IsDirectPacketVisible(
            RenderPassKind pass,
            uint32 sourcePacketIndex) const noexcept;
        [[nodiscard]] bool HasDirectPacketSource(
            RenderPassKind pass,
            uint32 sourcePacketIndex) const noexcept;
    };

    class IRenderVisibilityProvider
    {
    public:
        virtual ~IRenderVisibilityProvider() = default;
        virtual void Evaluate(const RenderCandidateSet& candidates,
                              const RenderVisibilityRequest& request,
                              RenderVisibilityResult& outResult) const = 0;
    };

    /** @brief Canonical CPU final-visibility provider used by Direct/legacy consumers. */
    class CPUVisibilityProvider final : public IRenderVisibilityProvider
    {
    public:
        void Evaluate(const RenderCandidateSet& candidates,
                      const RenderVisibilityRequest& request,
                      RenderVisibilityResult& outResult) const override;
    };

    /**
     * @brief GPU candidate provider contract.
     *
     * GPU visibility remains deferred/no-readback at this stage. CPU final
     * visibility is still published for Direct and legacy consumers.
     */
    class GPUVisibilityProvider final : public IRenderVisibilityProvider
    {
    public:
        void Evaluate(const RenderCandidateSet& candidates,
                      const RenderVisibilityRequest& request,
                      RenderVisibilityResult& outResult) const override;

        /**
         * @brief Append pass results by reusing canonical object visibility.
         *
         * Pass candidates are projections of scene-object candidates. Reusing
         * the object masks avoids a second per-frame frustum/distance test.
         */
        void AppendPassCandidates(const RenderCandidateSet& candidates,
                                  uint32 firstCandidateIndex,
                                  RenderVisibilityResult& inOutResult) const;
    };
} // namespace RVX
