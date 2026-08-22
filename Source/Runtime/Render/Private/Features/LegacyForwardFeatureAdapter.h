#pragma once

/**
 * @file LegacyForwardFeatureAdapter.h
 * @brief Transitional feature wrapper for the established forward pass chain.
 */

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
     * @brief Bridges the existing sorted forward-pass registry into GFX02 features.
     *
     * This is intentionally a narrow compatibility adapter. It owns neither the
     * pass registry nor the opaque pass, and it snapshots the full record context
     * before graph composition so callbacks never consult SceneRenderer state.
     */
    class LegacyForwardFeatureAdapter final : public IRenderFeature
    {
    public:
        LegacyForwardFeatureAdapter(RenderPassRegistry* passRegistry,
                                    OpaquePass* opaquePass) noexcept;

        /** @brief Replace the value-owned context used for the next graph build. */
        void SetRecordContext(RenderPassRecordContext context);

        /** @brief Clear or replace the non-owning opaque-pass bridge safely. */
        void SetOpaquePass(OpaquePass* opaquePass) noexcept
        {
            m_opaquePass = opaquePass;
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
        RenderPassRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        LegacyForwardFeaturePassChainStats m_lastPassChainStats{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
    };
} // namespace RVX
