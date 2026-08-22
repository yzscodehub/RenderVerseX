#pragma once

/**
 * @file RenderFeatureGraphBuilder.h
 * @brief Top-level facade used by features to compose RenderGraph passes.
 */

#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Passes/IRenderPass.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace RVX
{
    /**
     * @brief Top-level graph composition builder with no pass-local resource API.
     *
     * RenderGraphBuilder remains the short-lived resource declaration object
     * supplied inside one pass setup callback. This separate facade prevents
     * nested pass registration and preserves the RenderGraph lifecycle.
     */
    class RenderFeatureGraphBuilder final
    {
    public:
        explicit RenderFeatureGraphBuilder(RenderGraph& graph) noexcept
            : m_graph(&graph)
            , m_provenance{
                  graph.GetGraphIdentity(), graph.GetRecordingGeneration()}
        {
        }

        RenderFeatureGraphBuilder(
            RenderGraph& graph,
            RenderGraphBlackboardProvenance provenance) noexcept
            : m_graph(&graph)
            , m_provenance(provenance)
        {
        }

        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_graph != nullptr && m_graph->GetGraphIdentity() != 0 &&
                   m_graph->GetRecordingGeneration() != 0;
        }

        [[nodiscard]] uint64 GetGraphIdentity() const noexcept
        {
            return m_graph != nullptr ? m_graph->GetGraphIdentity() : 0;
        }

        [[nodiscard]] uint64 GetRecordingGeneration() const noexcept
        {
            return m_graph != nullptr ? m_graph->GetRecordingGeneration() : 0;
        }

        [[nodiscard]] const RenderGraphBlackboardProvenance& GetProvenance()
            const noexcept
        {
            return m_provenance;
        }

        template<typename Data>
        [[nodiscard]] bool AddPass(
            const char* name,
            RenderGraphPassType type,
            std::function<void(RenderGraphBuilder&, Data&)> setup,
            std::function<void(const Data&, RHICommandContext&)> execute)
        {
            if (!IsValid())
            {
                return false;
            }
            m_graph->AddPass<Data>(
                name, type, std::move(setup), std::move(execute));
            m_registeredPassNames.emplace_back(name != nullptr ? name
                                                                : "RenderPass");
            return true;
        }

        template<typename Data>
        [[nodiscard]] bool AddPass(
            const char* name,
            RenderGraphPassType type,
            std::function<void(RenderGraphBuilder&, Data&)> setup,
            std::function<void(const Data&, RenderGraphPassContext&)> execute)
        {
            if (!IsValid())
            {
                return false;
            }
            m_graph->AddPass<Data>(
                name, type, std::move(setup), std::move(execute));
            m_registeredPassNames.emplace_back(name != nullptr ? name
                                                                : "RenderPass");
            return true;
        }

        /** @brief Compatibility bridge for one value-capturing legacy pass. */
        [[nodiscard]] bool AddLegacyPass(
            IRenderPass& pass,
            const RenderPassRecordContext& context)
        {
            if (!IsValid() || !context.MatchesTargetGraph(*m_graph))
            {
                return false;
            }
            pass.AddToGraph(*m_graph, context);
            m_registeredPassNames.emplace_back(
                pass.GetName() != nullptr ? pass.GetName() : "RenderPass");
            return true;
        }

        /**
         * @brief Diagnostic-only registration names.
         *
         * The feature registry derives authoritative per-feature pass evidence
         * from RenderGraph diagnostics, because one legacy pass may expand to
         * multiple graph nodes.
         */
        [[nodiscard]] const std::vector<std::string>& GetRegisteredPassNames()
            const noexcept
        {
            return m_registeredPassNames;
        }

        [[nodiscard]] const RenderGraph& GetGraph() const noexcept
        {
            return *m_graph;
        }

    private:
        RenderGraph* m_graph = nullptr;
        RenderGraphBlackboardProvenance m_provenance{};
        std::vector<std::string> m_registeredPassNames{};
    };
} // namespace RVX
