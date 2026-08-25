#pragma once

/**
 * @file LegacyForwardFeatureAdapter.h
 * @brief Recipe-specific halves of the established forward pass chain.
 */

#include "Features/VelocityPassBindings.h"
#include "Render/Features/IRenderFeature.h"
#include "Render/Passes/RenderPassRecordContext.h"

#include <cstddef>
#include <string>
#include <vector>

namespace RVX
{
    class OpaquePass;
    class RenderPassRegistry;

    /** @brief Per-frame observable accounting for the legacy forward chain. */
    struct LegacyForwardFeaturePassChainStats
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

    /**
     * @brief Produces ForwardStandard depth/surface inputs before lighting.
     *
     * GFX13 keeps stable feature id 1 but narrows its responsibility to passes
     * before Opaque.  It publishes SceneTextures so GTAO can be an ordinary
     * typed dependency instead of a hidden callback in the legacy chain.
     */
    class LegacyForwardFeatureAdapter final : public IRenderFeature
    {
    public:
        LegacyForwardFeatureAdapter(RenderPassRegistry* passRegistry,
                                    OpaquePass* opaquePass,
                                    VelocityPassBindings velocityPasses = {}) noexcept;

        /** @brief Replace the value-owned context used for the next graph build. */
        void SetRecordContext(RenderPassRecordContext context);

        /** @brief Clear or replace the non-owning opaque-pass bridge safely. */
        void SetOpaquePass(OpaquePass* opaquePass) noexcept
        {
            m_opaquePass = opaquePass;
        }

        void SetVelocityPasses(VelocityPassBindings velocityPasses) noexcept
        {
            m_velocityPasses = velocityPasses;
        }

        [[nodiscard]] RenderFeatureDescriptor Describe() const override;
        void Prepare(const RenderFeatureContext& context) override;
        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override;
        void AddPasses(RenderFeatureGraphBuilder& builder,
                       RenderGraphBlackboard& blackboard) override;
        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt()
            const override;

        [[nodiscard]] const LegacyForwardFeaturePassChainStats&
        GetLastPassChainStats() const noexcept
        {
            return m_lastPassChainStats;
        }

        /** @brief Return an ownership-free diagnostics snapshot for callers. */
        [[nodiscard]] LegacyForwardFeaturePassChainStats
        GetPassChainStatsSnapshot() const
        {
            return m_lastPassChainStats;
        }

        [[nodiscard]] const RenderFeatureReadiness& GetLastReadiness()
            const noexcept
        {
            return m_readiness;
        }

        [[nodiscard]] const RenderFeatureExecutionReceipt& GetLastReceipt()
            const noexcept
        {
            return m_lastReceipt;
        }

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        void SetCompositionFailure(std::string diagnostic);

        RenderPassRegistry* m_passRegistry = nullptr;
        OpaquePass* m_opaquePass = nullptr;
        VelocityPassBindings m_velocityPasses{};
        RenderPassRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        LegacyForwardFeaturePassChainStats m_lastPassChainStats{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
    };

    /**
     * @brief Consumes the exact AO field, then records Forward opaque and later passes.
     *
     * This adapter is the sole ForwardStandard consumer of screen-space AO.
     * Transparent, sky, particles, water, and other later passes receive no AO
     * input even though they remain in the same compatibility pass registry.
     */
    class ForwardLightingFeatureAdapter final : public IRenderFeature
    {
    public:
        ForwardLightingFeatureAdapter(RenderPassRegistry* passRegistry,
                                      OpaquePass* opaquePass,
                                      VelocityPassBindings velocityPasses = {}) noexcept;

        void SetRecordContext(RenderPassRecordContext context);
        void SetOpaquePass(OpaquePass* opaquePass) noexcept
        {
            m_opaquePass = opaquePass;
        }
        void SetVelocityPasses(VelocityPassBindings velocityPasses) noexcept
        {
            m_velocityPasses = velocityPasses;
        }

        [[nodiscard]] RenderFeatureDescriptor Describe() const override;
        void Prepare(const RenderFeatureContext& context) override;
        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override;
        void AddPasses(RenderFeatureGraphBuilder& builder,
                       RenderGraphBlackboard& blackboard) override;
        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt()
            const override;

        [[nodiscard]] LegacyForwardFeaturePassChainStats
        GetPassChainStatsSnapshot() const
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
        void SetCompositionFailure(std::string diagnostic);

        RenderPassRegistry* m_passRegistry = nullptr;
        OpaquePass* m_opaquePass = nullptr;
        VelocityPassBindings m_velocityPasses{};
        RenderPassRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        LegacyForwardFeaturePassChainStats m_lastPassChainStats{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
    };
} // namespace RVX
