#pragma once

/**
 * @file RenderFeatureRegistry.h
 * @brief Sealed renderer feature composition, dependency, and fallback policy.
 */

#include "Render/Features/IRenderFeature.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace RVX
{
    enum class RenderFeatureRegistryCode : uint8
    {
        Success = 0,
        RegistrySealed,
        RegistryNotSealed,
        InvalidFeature,
        DuplicateFeatureId,
        DuplicateFeatureName,
        InvalidRecipeMask,
        InvalidQualityMask,
        InvalidResourceContract,
        InvalidSnapshotChannel,
        DuplicateResourceContract,
        DuplicateProducer,
        MissingProducer,
        ResourceSchemaMismatch,
        DependencyCycle,
        IllegalPhaseEdge,
        InvalidFallback,
        MissingFallback,
        RecipeNotImplemented,
        InvalidFrameContext,
        RequiredFeatureUnavailable,
        OptionalFallbackUnavailable,
        GraphCompositionFailed,
    };

    [[nodiscard]] const char* GetRenderFeatureRegistryCodeName(
        RenderFeatureRegistryCode code) noexcept;

    struct RenderFeatureFramePrepareResult
    {
        RenderFeatureRegistryCode code =
            RenderFeatureRegistryCode::RegistryNotSealed;
        RenderPipelineResolution pipeline{};
        std::string diagnostic{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == RenderFeatureRegistryCode::Success &&
                   pipeline.IsSuccessful();
        }
    };

    /**
     * @brief Construction-time feature registry sealed before frame execution.
     *
     * Resource edges are topologically validated. Phase only rejects illegal
     * backwards edges; it is never used as the execution sort key.
     */
    class RenderFeatureRegistry final
    {
    public:
        [[nodiscard]] RenderFeatureRegistryCode Register(
            std::unique_ptr<IRenderFeature> feature);

        [[nodiscard]] RenderFeatureRegistryCode DeclareExternalResource(
            RenderFeatureResourceContract resource) noexcept;

        [[nodiscard]] RenderFeatureRegistryCode DeclareImplementedRecipe(
            RenderPipelineRecipe recipe) noexcept;

        [[nodiscard]] RenderFeatureRegistryCode Seal();

        [[nodiscard]] bool IsSealed() const noexcept
        {
            return m_sealed;
        }

        [[nodiscard]] size_t GetFeatureCount() const noexcept
        {
            return m_features.size();
        }

        [[nodiscard]] const RenderFeatureDescriptor* FindDescriptor(
            RenderFeatureId featureId) const noexcept;

        [[nodiscard]] const std::vector<RenderFeatureId>& GetExecutionOrder()
            const noexcept
        {
            return m_executionOrder;
        }

        [[nodiscard]] RenderFeatureFramePrepareResult PrepareFrame(
            RenderFeatureContext context);

        [[nodiscard]] RenderFeatureRegistryCode AddPasses(
            RenderFeatureGraphBuilder& builder,
            RenderGraphBlackboard& blackboard);

        /**
         * @brief Capture immutable graph-owned execution evidence for the
         * currently composed feature recording.
         *
         * Feature implementations never authoritatively report graph pass
         * counts or execution. The registry derives those values from this
         * exact graph recording after execution has completed.
         */
        [[nodiscard]] RenderFeatureRegistryCode FinalizeGraphExecution(
            const RenderGraph& graph);

        [[nodiscard]] RenderExecutionReceipt CollectExecutionReceipt(
            const RenderExecutionBuildIdentity& buildIdentity = {}) const;

        void ResetFrame() noexcept;

    private:
        struct Entry
        {
            std::unique_ptr<IRenderFeature> feature{};
            RenderFeatureDescriptor descriptor{};
            size_t registrationOrdinal = 0;
        };

        struct ActiveFeature
        {
            RenderFeatureId featureId{};
            RenderFeatureRequirement requirement =
                RenderFeatureRequirement::Optional;
            bool fallbackUsed = false;
            RenderFeatureId selectedFallbackFeatureId{};
            RenderFeatureId fallbackForFeatureId{};
            bool addPasses = true;
            RenderFeatureReadiness readiness{};
        };

        struct FeatureGraphRegistration
        {
            RenderFeatureId featureId{};
            std::vector<uint32> passIndices{};
            std::vector<std::string> passNames{};
            uint32 observedGraphPassCount = 0;
            uint32 observedExecutedGraphPassCount = 0;
            std::vector<std::string> observedGraphPassNames{};
            std::string semanticExecutionDiagnostic{};
            bool executionEvidenceFinalized = false;
        };

        [[nodiscard]] Entry* FindEntry(RenderFeatureId featureId) noexcept;
        [[nodiscard]] const Entry* FindEntry(
            RenderFeatureId featureId) const noexcept;
        [[nodiscard]] const FeatureGraphRegistration*
        FindFeatureGraphRegistration(RenderFeatureId featureId) const noexcept;
        [[nodiscard]] bool IsRecipeImplemented(
            RenderPipelineRecipe recipe) const noexcept;
        [[nodiscard]] bool IsFallbackTargetForProfile(
            RenderFeatureId featureId,
            RenderPipelineRecipe recipe,
            RenderQualityTier quality) const noexcept;
        [[nodiscard]] bool ReachesFallback(
            RenderFeatureId source,
            RenderFeatureId target) const noexcept;
        [[nodiscard]] RenderFeatureRegistryCode ValidateAndBuildExecutionOrder();
        [[nodiscard]] RenderFeatureRegistryCode SelectFeature(
            Entry& entry,
            RenderFeatureRequirement requirement,
            const RenderFeatureContext& context,
            bool selectedAsFallback,
            RenderFeatureId fallbackFor,
            std::string& outDiagnostic);

        std::vector<Entry> m_features{};
        std::vector<RenderFeatureId> m_executionOrder{};
        std::array<std::optional<uint32>,
                   static_cast<size_t>(RenderBlackboardResourceId::Count)>
            m_externalResources{};
        RenderPipelineRecipeMask m_implementedRecipes = 0;
        std::vector<ActiveFeature> m_activeFeatures{};
        std::vector<FeatureGraphRegistration> m_featureGraphRegistrations{};
        RenderFeatureContext m_activeContext{};
        RenderPipelineResolution m_activeResolution{};
        std::string m_activeDiagnostic{};
        bool m_sealed = false;
        bool m_frameAttempted = false;
        bool m_framePrepared = false;
        bool m_graphCompositionSucceeded = false;
        uint64 m_recordedGraphIdentity = 0;
        uint64 m_recordedGraphGeneration = 0;
        bool m_executionEvidenceFinalized = false;
    };
} // namespace RVX
