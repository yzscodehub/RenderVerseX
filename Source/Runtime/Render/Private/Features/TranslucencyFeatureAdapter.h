#pragma once

/**
 * @file TranslucencyFeatureAdapter.h
 * @brief Recipe-neutral translucency composition after Sky and Water.
 */

#include "Render/Features/IRenderFeature.h"
#include "Render/Passes/RenderPassRecordContext.h"

#include <string>
#include <memory>
#include <vector>

namespace RVX
{
    class RenderPassRegistry;
    class PipelineCache;
    struct ParticleBillboardDrawPlan;
    class TranslucencyFramePlan;

    struct TranslucencyFeaturePassChainStats
    {
        uint64 frameSequence = 0;
        uint64 planIdentity = 0;
        uint32 weightedMeshCount = 0;
        uint32 weightedParticleBatchCount = 0;
        uint32 weightedParticleItemCount = 0;
        uint32 sortedEntryCount = 0;
        uint32 additiveEntryCount = 0;
        bool weightedBlendedRequested = false;
        bool weightedBlendedRequired = false;
        bool weightedBlendedEnabled = false;
        bool weightedBlendedFallbackUsed = false;
        bool accumulationClearRegistered = false;
        bool compositeRegistered = false;
        size_t registeredPassCount = 0;
        size_t graphPassCount = 0;
        size_t skippedDisabledPassCount = 0;
        size_t skippedUnsupportedPassCount = 0;
        std::vector<RenderPassStatus> passStatuses{};
        std::vector<std::string> graphPassNames{};
        std::vector<std::string> unsupportedPassNames{};
    };

    /**
     * @brief Owns all mesh/particle translucency ordering for every raster
     *        recipe.
     *
     * GFX22 initially preserves the established pass implementations behind
     * a real typed feature boundary. Weighted accumulation, its single
     * composite, sorted refraction, and direct additive work are registered
     * only from this owner.
     */
    class TranslucencyFeatureAdapter final : public IRenderFeature
    {
    public:
        explicit TranslucencyFeatureAdapter(
            RenderPassRegistry* passRegistry,
            PipelineCache* pipelineCache = nullptr) noexcept;

        void SetRecordContext(RenderPassRecordContext context);

        [[nodiscard]] RenderFeatureDescriptor Describe() const override;
        void Prepare(const RenderFeatureContext& context) override;
        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override;
        void AddPasses(RenderFeatureGraphBuilder& builder,
                       RenderGraphBlackboard& blackboard) override;
        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt()
            const override;

        [[nodiscard]] const TranslucencyFeaturePassChainStats&
        GetPassChainStats() const noexcept
        {
            return m_lastPassChainStats;
        }

        [[nodiscard]] const RenderFeatureReadiness& GetLastReadiness()
            const noexcept
        {
            return m_readiness;
        }

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasValidLightingPlan(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool BuildFramePlan(
            const RenderFeatureContext& context,
            std::string& outDiagnostic);
        void SetCompositionFailure(std::string diagnostic);

        RenderPassRegistry* m_passRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        RenderPassRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        std::shared_ptr<const ParticleBillboardDrawPlan> m_particleDrawPlan{};
        std::shared_ptr<const TranslucencyFramePlan> m_framePlan{};
        TranslucencyFeaturePassChainStats m_lastPassChainStats{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
    };
} // namespace RVX
