#pragma once

/**
 * @file MeshPassProcessor.h
 * @brief Value-only pass classification and deterministic draw grouping.
 */

#include "Core/Types.h"
#include "Render/Renderer/RenderDrawPacket.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace RVX
{
    enum class MeshPassDisposition : uint8
    {
        GPUCandidate = 0,
        Direct = 1,
        Skip = 2
    };

    enum class MeshPassEligibilityReason : uint8
    {
        None = 0,
        PassIrrelevant = 1,
        Transparent = 2,
        Skinned = 3,
        SpecialMaterial = 4,
        UnsupportedTopology = 5,
        UnsupportedIndexType = 6,
        PipelineUnavailable = 7,
        GeometryUnavailable = 8,
        ResourcePending = 9,
        ResourceUnavailable = 10,
        PassRequiresDirect = 11,
        Count = 12
    };

    enum class MeshPassResourceAvailability : uint8
    {
        Ready = 0,
        Pending = 1,
        Unavailable = 2
    };

    enum class PrimitiveDataBinding : uint8
    {
        PerDrawConstants = 0,
        InstanceBuffer = 1
    };

    enum class MeshPassBindingRequirements : uint32
    {
        None = 0,
        Frame = 1U << 0U,
        Object = 1U << 1U,
        Geometry = 1U << 2U,
        Material = 1U << 3U,
        DefaultMaterial = 1U << 4U,
        Skinning = 1U << 5U
    };

    enum class MeshPassVertexStreams : uint32
    {
        None = 0,
        Position = 1U << 0U,
        Normal = 1U << 1U,
        TexCoord = 1U << 2U,
        Tangent = 1U << 3U,
        BoneIndices = 1U << 4U,
        BoneWeights = 1U << 5U,
        InstanceIndex = 1U << 6U
    };

    constexpr MeshPassBindingRequirements operator|(
        MeshPassBindingRequirements lhs,
        MeshPassBindingRequirements rhs) noexcept
    {
        return static_cast<MeshPassBindingRequirements>(
            static_cast<uint32>(lhs) | static_cast<uint32>(rhs));
    }

    constexpr MeshPassBindingRequirements& operator|=(
        MeshPassBindingRequirements& lhs,
        MeshPassBindingRequirements rhs) noexcept
    {
        lhs = lhs | rhs;
        return lhs;
    }

    [[nodiscard]] constexpr bool HasMeshPassBindingRequirement(
        MeshPassBindingRequirements requirements,
        MeshPassBindingRequirements requirement) noexcept
    {
        return (static_cast<uint32>(requirements) &
                static_cast<uint32>(requirement)) != 0;
    }

    constexpr MeshPassVertexStreams operator|(
        MeshPassVertexStreams lhs,
        MeshPassVertexStreams rhs) noexcept
    {
        return static_cast<MeshPassVertexStreams>(
            static_cast<uint32>(lhs) | static_cast<uint32>(rhs));
    }

    constexpr MeshPassVertexStreams& operator|=(
        MeshPassVertexStreams& lhs,
        MeshPassVertexStreams rhs) noexcept
    {
        lhs = lhs | rhs;
        return lhs;
    }

    [[nodiscard]] constexpr const char* ToName(
        MeshPassDisposition disposition) noexcept
    {
        switch (disposition)
        {
            case MeshPassDisposition::GPUCandidate: return "GPUCandidate";
            case MeshPassDisposition::Direct: return "Direct";
            case MeshPassDisposition::Skip: return "Skip";
            default: return "Unknown";
        }
    }

    [[nodiscard]] constexpr const char* GetName(
        MeshPassDisposition disposition) noexcept
    {
        return ToName(disposition);
    }

    [[nodiscard]] constexpr const char* ToName(
        MeshPassEligibilityReason reason) noexcept
    {
        switch (reason)
        {
            case MeshPassEligibilityReason::None: return "None";
            case MeshPassEligibilityReason::PassIrrelevant:
                return "PassIrrelevant";
            case MeshPassEligibilityReason::Transparent: return "Transparent";
            case MeshPassEligibilityReason::Skinned: return "Skinned";
            case MeshPassEligibilityReason::SpecialMaterial:
                return "SpecialMaterial";
            case MeshPassEligibilityReason::UnsupportedTopology:
                return "UnsupportedTopology";
            case MeshPassEligibilityReason::UnsupportedIndexType:
                return "UnsupportedIndexType";
            case MeshPassEligibilityReason::PipelineUnavailable:
                return "PipelineUnavailable";
            case MeshPassEligibilityReason::GeometryUnavailable:
                return "GeometryUnavailable";
            case MeshPassEligibilityReason::ResourcePending:
                return "ResourcePending";
            case MeshPassEligibilityReason::ResourceUnavailable:
                return "ResourceUnavailable";
            case MeshPassEligibilityReason::PassRequiresDirect:
                return "PassRequiresDirect";
            case MeshPassEligibilityReason::Count: return "Count";
            default: return "Unknown";
        }
    }

    [[nodiscard]] constexpr const char* GetName(
        MeshPassEligibilityReason reason) noexcept
    {
        return ToName(reason);
    }

    struct RenderSubmissionLayout
    {
        MeshPassVertexStreams vertexStreams = MeshPassVertexStreams::None;
        MeshPassBindingRequirements bindings =
            MeshPassBindingRequirements::None;
        PrimitiveDataBinding primitiveDataBinding =
            PrimitiveDataBinding::PerDrawConstants;

        [[nodiscard]] bool operator==(
            const RenderSubmissionLayout& other) const noexcept = default;
    };

    struct RenderDrawGroupKey
    {
        RenderPassKind pass = RenderPassKind::None;
        PipelineKey pipeline;
        GeometryBindingKey geometry;
        MaterialBindingKey material;
        RenderSubmissionLayout layout;

        [[nodiscard]] bool operator==(
            const RenderDrawGroupKey& other) const noexcept = default;
    };

    struct RenderDrawGroupKeyHasher
    {
        [[nodiscard]] size_t operator()(
            const RenderDrawGroupKey& key) const noexcept;
    };

    struct RenderDrawGroupKeyLess
    {
        [[nodiscard]] bool operator()(const RenderDrawGroupKey& lhs,
                                      const RenderDrawGroupKey& rhs) const noexcept;
    };

    [[nodiscard]] uint64 GetStableHash(
        const RenderSubmissionLayout& layout) noexcept;
    [[nodiscard]] uint64 GetStableHash(const RenderDrawGroupKey& key) noexcept;

    struct MeshPassAvailabilityFacts
    {
        MeshPassResourceAvailability pipeline =
            MeshPassResourceAvailability::Ready;
        MeshPassResourceAvailability geometry =
            MeshPassResourceAvailability::Ready;
        MeshPassResourceAvailability material =
            MeshPassResourceAvailability::Ready;
        bool specialMaterial = false;
    };

    struct MeshPassProcessorInput
    {
        RenderDrawPacket packet;
        MeshPassAvailabilityFacts availability;
        uint32 sourceOrdinal = 0;
        float32 viewDepth = 0.0f;
    };

    struct MeshPassProcessorResult
    {
        MeshPassDisposition disposition = MeshPassDisposition::Skip;
        MeshPassEligibilityReason reason =
            MeshPassEligibilityReason::PassIrrelevant;
        RenderDrawPacket packet;
        RenderDrawGroupKey groupKey;
        uint32 sourceOrdinal = 0;
        float32 viewDepth = 0.0f;

        [[nodiscard]] bool IsRelevant() const noexcept
        {
            return disposition != MeshPassDisposition::Skip;
        }

        [[nodiscard]] bool IsGPUCandidate() const noexcept
        {
            return disposition == MeshPassDisposition::GPUCandidate;
        }
    };

    struct MeshPassProcessorStats
    {
        uint32 inputPacketCount = 0;
        uint32 relevantPacketCount = 0;
        uint32 gpuCandidatePacketCount = 0;
        uint32 directPacketCount = 0;
        uint32 skippedPacketCount = 0;
        std::array<uint32,
                   static_cast<size_t>(MeshPassEligibilityReason::Count)>
            reasonCounts{};

        void Record(const MeshPassProcessorResult& result) noexcept;
        [[nodiscard]] uint32 GetReasonCount(
            MeshPassEligibilityReason reason) const noexcept;
        [[nodiscard]] bool HasCompleteRelevantOutcome() const noexcept;
        [[nodiscard]] bool HasExactlyOneReasonPerRejectedPacket() const noexcept;
    };

    struct RenderDrawGroupRange
    {
        RenderDrawGroupKey key;
        uint32 first = 0;
        uint32 count = 0;
    };

    /** @brief Frame-owned value stream prepared for one mesh pass. */
    struct MeshPassPacketStream
    {
        std::vector<MeshPassProcessorResult> packets;
        std::vector<MeshPassProcessorResult> sortedGPUCandidates;
        std::vector<RenderDrawGroupRange> groups;
        MeshPassProcessorStats stats;

        void Clear();
        void Record(MeshPassProcessorResult result);
        void FinalizeGroups();
    };

    /** @brief Pass-specific streams prepared alongside compatibility draw lists. */
    struct SceneMeshPassPreparation
    {
        MeshPassPacketStream depth;
        MeshPassPacketStream opaque;
        MeshPassPacketStream transparent;
        MeshPassPacketStream shadow;

        void Clear();
    };

    /**
     * @brief Copy GPU candidates, sort by exact fields plus source ordinal, and
     * form contiguous groups. Direct and skipped results are deliberately omitted.
     */
    void BuildDeterministicRenderDrawGroups(
        std::span<const MeshPassProcessorResult> results,
        std::vector<MeshPassProcessorResult>& outSortedCandidates,
        std::vector<RenderDrawGroupRange>& outGroups);

    class MeshPassProcessor
    {
    public:
        virtual ~MeshPassProcessor() = default;

        [[nodiscard]] virtual RenderPassKind GetPassKind() const noexcept = 0;
        [[nodiscard]] virtual MeshPassProcessorResult Process(
            const MeshPassProcessorInput& input) const noexcept = 0;

    protected:
        [[nodiscard]] static MeshPassProcessorResult MakeSkipped(
            const MeshPassProcessorInput& input) noexcept;
        [[nodiscard]] static MeshPassProcessorResult MakeRelevant(
            const MeshPassProcessorInput& input,
            RenderPassKind pass,
            MeshPassBindingRequirements bindings,
            MeshPassVertexStreams vertexStreams,
            MeshPassEligibilityReason intrinsicDirectReason) noexcept;
    };

    class DepthMeshPassProcessor final : public MeshPassProcessor
    {
    public:
        [[nodiscard]] RenderPassKind GetPassKind() const noexcept override;
        [[nodiscard]] MeshPassProcessorResult Process(
            const MeshPassProcessorInput& input) const noexcept override;
    };

    class OpaqueMeshPassProcessor final : public MeshPassProcessor
    {
    public:
        [[nodiscard]] RenderPassKind GetPassKind() const noexcept override;
        [[nodiscard]] MeshPassProcessorResult Process(
            const MeshPassProcessorInput& input) const noexcept override;
    };

    class TransparentMeshPassProcessor final : public MeshPassProcessor
    {
    public:
        [[nodiscard]] RenderPassKind GetPassKind() const noexcept override;
        [[nodiscard]] MeshPassProcessorResult Process(
            const MeshPassProcessorInput& input) const noexcept override;
    };

    class ShadowMeshPassProcessor final : public MeshPassProcessor
    {
    public:
        [[nodiscard]] RenderPassKind GetPassKind() const noexcept override;
        [[nodiscard]] MeshPassProcessorResult Process(
            const MeshPassProcessorInput& input) const noexcept override;
    };

    static_assert(static_cast<uint8>(MeshPassDisposition::GPUCandidate) == 0);
    static_assert(static_cast<uint8>(MeshPassDisposition::Direct) == 1);
    static_assert(static_cast<uint8>(MeshPassDisposition::Skip) == 2);
    static_assert(static_cast<uint8>(MeshPassEligibilityReason::None) == 0);
    static_assert(static_cast<uint8>(MeshPassEligibilityReason::Count) == 12);
} // namespace RVX
