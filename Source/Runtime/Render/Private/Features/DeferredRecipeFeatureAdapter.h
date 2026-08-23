#pragma once

/**
 * @file DeferredRecipeFeatureAdapter.h
 * @brief DeferredQuality feature adapters over the established pass inventory.
 *
 * The adapters deliberately own no renderer resources.  They freeze the same
 * value-owned record context used by the compatibility ForwardStandard chain,
 * split that chain around opaque geometry, and let RenderGraph resource edges
 * order GBuffer, compute lighting, and forward-special surfaces.
 */

#include "Render/Features/IRenderFeature.h"
#include "Render/Passes/RenderPassRecordContext.h"

#include <cstddef>
#include <string>
#include <vector>

namespace RVX
{
    class DeferredLightingPass;
    class OpaquePass;
    class PipelineCache;
    class RenderPassRegistry;

    /** @brief Observable split-chain accounting for the deferred recipe. */
    struct DeferredRecipeFeaturePassChainStats
    {
        uint64 frameSequence = 0;
        size_t registeredPassCount = 0;
        size_t graphPassCount = 0;
        size_t skippedDisabledPassCount = 0;
        size_t skippedUnsupportedPassCount = 0;
        std::vector<RenderPassStatus> passStatuses{};
        std::vector<std::string> graphPassNames{};
        std::vector<std::string> unsupportedPassNames{};
    };

    /** @brief Creates the fixed GBuffer and records the shared opaque submission. */
    class DeferredGeometryFeatureAdapter final : public IRenderFeature
    {
    public:
        DeferredGeometryFeatureAdapter(RenderPassRegistry* passRegistry,
                                       OpaquePass* opaquePass,
                                       PipelineCache* pipelineCache) noexcept;

        void SetRecordContext(RenderPassRecordContext context);
        void SetOpaquePass(OpaquePass* opaquePass) noexcept;

        [[nodiscard]] RenderFeatureDescriptor Describe() const override;
        void Prepare(const RenderFeatureContext& context) override;
        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override;
        void AddPasses(RenderFeatureGraphBuilder& builder,
                       RenderGraphBlackboard& blackboard) override;
        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt() const override;

        [[nodiscard]] DeferredRecipeFeaturePassChainStats
        GetPassChainStatsSnapshot() const
        {
            return m_lastPassChainStats;
        }

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasDeferredTargets() const noexcept;
        void SetCompositionFailure(std::string diagnostic);

        RenderPassRegistry* m_passRegistry = nullptr;
        OpaquePass* m_opaquePass = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        RenderPassRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        DeferredRecipeFeaturePassChainStats m_lastPassChainStats{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
    };

    /** @brief Runs the real deferred compute lighting pipeline over SceneTextures. */
    class DeferredLightingFeatureAdapter final : public IRenderFeature
    {
    public:
        DeferredLightingFeatureAdapter(DeferredLightingPass* lightingPass,
                                       PipelineCache* pipelineCache) noexcept;

        void SetRecordContext(RenderPassRecordContext context);
        void SetLightingPass(DeferredLightingPass* lightingPass) noexcept;

        [[nodiscard]] RenderFeatureDescriptor Describe() const override;
        void Prepare(const RenderFeatureContext& context) override;
        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override;
        void AddPasses(RenderFeatureGraphBuilder& builder,
                       RenderGraphBlackboard& blackboard) override;
        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt() const override;

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasValidLightingPlan(
            const RenderFeatureContext& context) const noexcept;
        void SetCompositionFailure(std::string diagnostic);

        DeferredLightingPass* m_lightingPass = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        RenderPassRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
    };

    /** @brief Records only forward-special work that follows deferred lighting. */
    class DeferredForwardSpecialFeatureAdapter final : public IRenderFeature
    {
    public:
        DeferredForwardSpecialFeatureAdapter(RenderPassRegistry* passRegistry,
                                             OpaquePass* opaquePass) noexcept;

        void SetRecordContext(RenderPassRecordContext context);
        void SetOpaquePass(OpaquePass* opaquePass) noexcept;

        [[nodiscard]] RenderFeatureDescriptor Describe() const override;
        void Prepare(const RenderFeatureContext& context) override;
        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override;
        void AddPasses(RenderFeatureGraphBuilder& builder,
                       RenderGraphBlackboard& blackboard) override;
        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt() const override;

        [[nodiscard]] DeferredRecipeFeaturePassChainStats
        GetPassChainStatsSnapshot() const
        {
            return m_lastPassChainStats;
        }

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasValidLightingPlan(
            const RenderFeatureContext& context) const noexcept;
        void SetCompositionFailure(std::string diagnostic);

        RenderPassRegistry* m_passRegistry = nullptr;
        OpaquePass* m_opaquePass = nullptr;
        RenderPassRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        DeferredRecipeFeaturePassChainStats m_lastPassChainStats{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
    };
} // namespace RVX
