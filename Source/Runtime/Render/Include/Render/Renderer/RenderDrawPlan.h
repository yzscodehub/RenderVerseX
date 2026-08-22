#pragma once

/**
 * @file RenderDrawPlan.h
 * @brief Strategy-neutral scene classification and per-view draw planning.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Visibility/RenderVisibility.h"

#include <array>
#include <span>
#include <vector>

namespace RVX
{
    class RenderScene;
    struct RenderFrameExecutionPlan;

    /** @brief Freezes view-independent resource bindings into a draw packet. */
    class IRenderDrawClassificationBindingResolver
    {
    public:
        virtual ~IRenderDrawClassificationBindingResolver() = default;
        virtual void Resolve(RenderDrawPacket& packet) const = 0;
    };

    /** @brief Optional immutable sources frozen into scene classification. */
    struct RenderSceneDrawClassificationSources
    {
        const IRenderDrawClassificationBindingResolver* bindingResolver =
            nullptr;
    };

    /** @brief One view-independent material/surface classification result. */
    struct RenderSceneDrawClassificationEntry
    {
        uint32 classificationIndex = RVX_INVALID_INDEX;
        RenderDrawItem draw{};
        uint32 layerMask = ~0U;
        AABB worldBounds{};
        Vec3 worldPosition{0.0f};
        bool sceneVisible = true;
        bool drawable = false;
        bool castsShadow = false;
        bool skinned = false;

    };

    /** @brief Honest counts and stable identity for one retained-scene classification. */
    struct RenderSceneDrawClassificationStats
    {
        uint32 sceneObjectCount = 0;
        uint32 classifiedPacketCount = 0;
        uint32 classifiedPrimitiveCount = 0;
        uint32 materialBindingCount = 0;
        uint32 uniqueMaterialCount = 0;
        uint64 semanticHash = 0;

        bool operator==(
            const RenderSceneDrawClassificationStats&) const = default;
    };

    /**
     * @brief Frozen surface classification shared by every view and execution lane.
     *
     * Entries deliberately retain visibility/layer facts without applying a
     * camera. Direct and GPU-driven execution must project this same value.
     */
    struct RenderSceneDrawClassification
    {
        std::vector<RenderSceneDrawClassificationEntry> entries{};
        RenderSceneDrawClassificationStats stats{};
        bool structurallyValid = false;

        void Clear();
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Build a stable classification without consulting a camera or lane. */
    [[nodiscard]] bool BuildRenderSceneDrawClassification(
        const RenderScene& scene,
        RenderSceneDrawClassificationSources sources,
        RenderSceneDrawClassification& outClassification);

    /** @brief Build a resource-neutral classification for focused value tests. */
    [[nodiscard]] bool BuildRenderSceneDrawClassification(
        const RenderScene& scene,
        RenderSceneDrawClassification& outClassification);

    /**
     * @brief Project a frozen classification into camera-visible material lists.
     *
     * The visibility mask is the only frustum/distance input. Layer selection
     * is applied here because it is a view contract, not a scene classification.
     */
    [[nodiscard]] bool ProjectRenderSceneDrawClassification(
        const RenderSceneDrawClassification& classification,
        std::span<const uint8> visibleObjectMask,
        uint32 viewCullingMask,
        const Vec3& cameraPosition,
        std::vector<RenderDrawItem>& outOpaqueDrawItems,
        std::vector<RenderDrawItem>& outMaskedDrawItems,
        std::vector<RenderDrawItem>& outTransparentDrawItems,
        TransparentDrawListDiagnostics* outTransparentDiagnostics = nullptr);

    /** @brief One source packet in a pass plan, before lane selection. */
    struct RenderPassDrawPlanEntry
    {
        uint32 sourcePacketIndex = RVX_INVALID_INDEX;
        uint32 sourceOrdinal = 0;
        uint32 classificationIndex = RVX_INVALID_INDEX;
        RenderDrawPacket packet{};
        RenderDrawGroupKey semanticGroup{};
        float32 viewDepth = 0.0f;
        bool passRelevant = false;
        bool cpuVisible = false;

        bool operator==(const RenderPassDrawPlanEntry&) const = default;
    };

    /** @brief Strategy-neutral semantic counts for one view/pass. */
    struct RenderPassDrawPlanStats
    {
        uint32 inputPacketCount = 0;
        uint32 semanticPacketCount = 0;
        uint32 viewCandidatePacketCount = 0;
        uint32 visiblePacketCount = 0;
        uint32 candidatePrimitiveCount = 0;
        uint32 visiblePrimitiveCount = 0;
        uint32 materialBindingCount = 0;
        uint32 uniqueMaterialCount = 0;
        uint32 semanticDrawGroupCount = 0;
        uint64 candidateIdentityHash = 0;
        uint64 visibleIdentityHash = 0;
        uint64 materialIdentityHash = 0;
        uint64 semanticGroupHash = 0;
        /** @brief Linear integrity fingerprint over entries and every derived field. */
        uint64 integrityHash = 0;

        bool operator==(const RenderPassDrawPlanStats&) const = default;
    };

    /** @brief Immutable draw intent for one pass; contains no execution strategy. */
    struct RenderPassDrawPlan
    {
        RenderPassKind pass = RenderPassKind::None;
        std::vector<RenderPassDrawPlanEntry> entries{};
        RenderPassDrawPlanStats stats{};
        bool structurallyValid = false;

        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Immutable per-frame/view draw intent consumed before policy selection. */
    struct RenderViewDrawPlan
    {
        uint64 frameSequence = 0;
        uint32 viewOrdinal = 0;
        uint64 sceneClassificationHash = 0;
        std::array<RenderPassDrawPlan, 4> passes{};
        uint64 semanticHash = 0;
        bool structurallyValid = false;

        [[nodiscard]] const RenderPassDrawPlan* FindPass(
            RenderPassKind pass) const noexcept;
        [[nodiscard]] bool IsValid() const noexcept;
    };

    enum class RenderDrawPlanBuildCode : uint8
    {
        Built = 0,
        InvalidIdentity,
        InvalidClassification,
        InvalidPreparation,
        MissingVisibility,
        ClassificationMismatch,
        ClassificationCoverageMismatch,
        CountOverflow,
    };

    struct RenderViewDrawPlanBuildResult
    {
        RenderDrawPlanBuildCode code =
            RenderDrawPlanBuildCode::InvalidPreparation;
        RenderViewDrawPlan plan{};

        [[nodiscard]] bool IsSuccess() const noexcept
        {
            return code == RenderDrawPlanBuildCode::Built && plan.IsValid();
        }
    };

    /** @brief Freeze semantic pass intent before Direct/GPU policy selection. */
    [[nodiscard]] RenderViewDrawPlanBuildResult BuildRenderViewDrawPlan(
        uint64 frameSequence,
        uint32 viewOrdinal,
        const RenderSceneDrawClassification& classification,
        const SceneMeshPassPreparation& preparation,
        const RenderVisibilityResult& visibility);

    /** @brief Prove a compiled lane plan preserved the strategy-neutral DrawPlan. */
    [[nodiscard]] bool ValidateRenderFrameExecutionPlanDrawParity(
        const RenderFrameExecutionPlan& executionPlan,
        const RenderViewDrawPlan& drawPlan) noexcept;
} // namespace RVX
