#pragma once

/**
 * @file EcsRenderSceneRetirement.h
 * @brief ECS-native proof that frozen-scene render identities have been removed.
 */

#include "Core/Types.h"
#include "ECS/Entity.h"

#include <array>
#include <cstddef>
#include <compare>
#include <vector>

namespace RVX
{
    /**
     * @brief Opaque process-local identity for one pending ECS extraction candidate.
     *
     * The extractor instance component is allocated monotonically for the
     * process and the sequence component is allocated monotonically by that
     * instance. Callers must compare this value only for exact equality.
     */
    class EcsFrameExtractionCandidateIdentity
    {
    public:
        constexpr EcsFrameExtractionCandidateIdentity() noexcept = default;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return extractorInstanceId != 0 && candidateSequence != 0;
        }

        auto operator<=>(const EcsFrameExtractionCandidateIdentity&) const = default;

    private:
        uint64 extractorInstanceId = 0;
        uint64 candidateSequence = 0;

        constexpr EcsFrameExtractionCandidateIdentity(
            uint64 extractorInstanceId,
            uint64 candidateSequence) noexcept
            : extractorInstanceId(extractorInstanceId),
              candidateSequence(candidateSequence)
        {
        }

        friend class EcsFrameExtractor;
    };

    /** @brief Explicit transport outcome for one ECS extraction candidate. */
    enum class EcsFramePublicationDisposition : uint8
    {
        Accepted = 0,
        NotAccepted,
        /** @brief Reliable Scene update applied, but the paired frame was rejected. */
        SceneUpdateAcceptedWithoutFrame,
    };

    /** @brief Stable caller-owned correlation for one ECS retirement request. */
    struct EcsRenderSceneRetirementCorrelationId
    {
        uint64 value = 0;

        [[nodiscard]] bool IsValid() const noexcept { return value != 0; }
        auto operator<=>(const EcsRenderSceneRetirementCorrelationId&) const = default;
    };

    /** @brief ECS source categories with an exact retained RenderScene mapping. */
    enum class EcsRenderSceneRetainedMemberType : uint8
    {
        Invalid = 0,
        Mesh,
        Light,
        Skybox,
        Particle,
        Water,
        Terrain,
    };

    /** @brief Generation-safe ECS identity which may own a retained render identity. */
    struct EcsRenderSceneRetirementMember
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        EcsRenderSceneRetainedMemberType type =
            EcsRenderSceneRetainedMemberType::Invalid;

        auto operator<=>(const EcsRenderSceneRetirementMember&) const = default;
    };

    /**
     * @brief One requested ECS retirement proof scoped to exactly one Scene runtime.
     *
     * Only members that exist in the accepted retained baseline are kept by the
     * extractor.  A member absent from that baseline never needs a render-side
     * removal acknowledgement.
     */
    struct EcsRenderSceneRetirementBarrier
    {
        EcsRenderSceneRetirementCorrelationId correlation;
        ECS::SceneRuntimeId sceneRuntimeId;
        std::vector<EcsRenderSceneRetirementMember> members;

        [[nodiscard]] bool IsStructurallyValid() const noexcept;
    };

    /** @brief Proof released after both removal and an accepted presentation frame. */
    struct EcsRenderSceneRetirementBinding
    {
        EcsRenderSceneRetirementCorrelationId correlation;
        uint64 targetSceneRevision = 0;
        uint64 minimumFrameSequence = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return correlation.IsValid() && targetSceneRevision != 0 &&
                   minimumFrameSequence != 0;
        }
    };

    inline constexpr uint32 RVX_ECS_RENDER_SCENE_RETIREMENT_MAX_BINDINGS = 64;

    /** @brief Allocation-free acknowledgement output for pending ECS retirements. */
    struct EcsRenderSceneRetirementResolution
    {
        EcsFrameExtractionCandidateIdentity candidateIdentity;
        EcsFramePublicationDisposition disposition =
            EcsFramePublicationDisposition::NotAccepted;
        std::array<EcsRenderSceneRetirementBinding,
                   RVX_ECS_RENDER_SCENE_RETIREMENT_MAX_BINDINGS>
            bindings{};
        uint32 bindingCount = 0;
        bool resolvedCandidate = false;

        [[nodiscard]] bool Empty() const noexcept { return bindingCount == 0; }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return resolvedCandidate;
        }
    };

    enum class EcsRenderSceneRetirementBarrierSubmitCode : uint8
    {
        Accepted = 0,
        InvalidBarrier,
        PublicationPending,
        NeverPublishedIdentity,
        SceneRuntimeMismatch,
        DuplicateCorrelation,
        CapacityExceeded,
    };

    /** @brief Result of admission against the accepted frozen ECS retained baseline. */
    struct EcsRenderSceneRetirementBarrierSubmitResult
    {
        EcsRenderSceneRetirementBarrierSubmitCode code =
            EcsRenderSceneRetirementBarrierSubmitCode::InvalidBarrier;
        size_t publishedMemberCount = 0;
        size_t ignoredNeverPublishedMemberCount = 0;

        [[nodiscard]] bool IsAccepted() const noexcept
        {
            return code == EcsRenderSceneRetirementBarrierSubmitCode::Accepted;
        }
    };

    static_assert(
        static_cast<uint8>(EcsRenderSceneRetainedMemberType::Invalid) == 0);
    static_assert(static_cast<uint8>(
                      EcsRenderSceneRetirementBarrierSubmitCode::Accepted) == 0);
} // namespace RVX
