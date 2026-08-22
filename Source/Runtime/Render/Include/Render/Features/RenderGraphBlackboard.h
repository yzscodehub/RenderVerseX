#pragma once

/**
 * @file RenderGraphBlackboard.h
 * @brief Closed, typed exchange of graph-generation-scoped feature resources.
 */

#include "Render/Features/RenderFeatureTypes.h"
#include "Render/Graph/RenderGraph.h"

#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace RVX
{
    struct RenderGraphBlackboardProvenance
    {
        uint64 graphIdentity = 0;
        uint64 recordingGeneration = 0;
        uint64 frameSequence = 0;
        uint32 viewOrdinal = 0;
        uint64 recordEpoch = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return graphIdentity != 0 && recordingGeneration != 0 &&
                   frameSequence != 0 && recordEpoch != 0;
        }

        friend constexpr bool operator==(
            const RenderGraphBlackboardProvenance&,
            const RenderGraphBlackboardProvenance&) = default;
    };

    struct SceneTextures
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::SceneTextures;
        static constexpr uint32 SchemaVersion = 1;

        RenderGraphBlackboardProvenance provenance{};
        RGTextureHandle hdrSceneColor{};
        RGTextureHandle deviceDepth{};
        RGTextureHandle linearDepth{};
        RGTextureHandle worldNormal{};
        RGTextureHandle roughnessMetallic{};
        RGTextureHandle velocity{};
        RGTextureHandle ambientOcclusion{};
        RGTextureHandle reactiveMask{};
        RGTextureHandle distortionVectors{};
        RGBufferHandle exposureState{};
        RGTextureHandle opaqueSceneSnapshot{};
    };

    struct VisibilityOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::VisibilityOutputs;
        static constexpr uint32 SchemaVersion = 1;

        RenderGraphBlackboardProvenance provenance{};
        RGBufferHandle visibleInstances{};
        RGBufferHandle indirectDraws{};
        RGBufferHandle drawCount{};
    };

    struct ShadowOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::ShadowOutputs;
        static constexpr uint32 SchemaVersion = 1;

        RenderGraphBlackboardProvenance provenance{};
        RGTextureHandle directionalDepth{};
        RGTextureHandle rayTracedMask{};
    };

    struct LightingOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::LightingOutputs;
        static constexpr uint32 SchemaVersion = 1;

        RenderGraphBlackboardProvenance provenance{};
        RGTextureHandle hdrSceneColor{};
        RGBufferHandle clusteredLights{};
    };

    struct ReflectionOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::ReflectionOutputs;
        static constexpr uint32 SchemaVersion = 1;

        RenderGraphBlackboardProvenance provenance{};
        RGTextureHandle rawReflection{};
        RGTextureHandle denoisedReflection{};
        RGTextureHandle confidence{};
    };

    struct TranslucencyOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::TranslucencyOutputs;
        static constexpr uint32 SchemaVersion = 1;

        RenderGraphBlackboardProvenance provenance{};
        RGTextureHandle hdrSceneColor{};
        RGTextureHandle reactiveMask{};
        RGTextureHandle distortionVectors{};
    };

    struct PostProcessOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::PostProcessOutputs;
        static constexpr uint32 SchemaVersion = 1;

        RenderGraphBlackboardProvenance provenance{};
        RGTextureHandle presentableColor{};
    };

    enum class RenderGraphBlackboardCode : uint8
    {
        Success = 0,
        InvalidProvenance,
        UndeclaredProducer,
        DuplicateProducer,
        MissingResource,
        UndeclaredConsumer,
        OptionalFallbackNotDeclared,
        SchemaMismatch,
        StaleGraphResource,
    };

    [[nodiscard]] constexpr const char* GetRenderGraphBlackboardCodeName(
        RenderGraphBlackboardCode code) noexcept
    {
        switch (code)
        {
            case RenderGraphBlackboardCode::Success: return "Success";
            case RenderGraphBlackboardCode::InvalidProvenance:
                return "InvalidProvenance";
            case RenderGraphBlackboardCode::UndeclaredProducer:
                return "UndeclaredProducer";
            case RenderGraphBlackboardCode::DuplicateProducer:
                return "DuplicateProducer";
            case RenderGraphBlackboardCode::MissingResource:
                return "MissingResource";
            case RenderGraphBlackboardCode::UndeclaredConsumer:
                return "UndeclaredConsumer";
            case RenderGraphBlackboardCode::OptionalFallbackNotDeclared:
                return "OptionalFallbackNotDeclared";
            case RenderGraphBlackboardCode::SchemaMismatch:
                return "SchemaMismatch";
            case RenderGraphBlackboardCode::StaleGraphResource:
                return "StaleGraphResource";
            default: return "Unknown";
        }
    }

    /**
     * @brief Per-graph typed blackboard with one producer per semantic slot.
     *
     * Storage is a closed tuple of concrete entry types. It deliberately uses
     * neither void pointers, string lookup, std::any, nor cross-frame handles.
     */
    class RenderGraphBlackboard final
    {
    public:
        explicit RenderGraphBlackboard(
            RenderGraphBlackboardProvenance provenance) noexcept
            : m_provenance(provenance)
        {
        }

        [[nodiscard]] const RenderGraphBlackboardProvenance& GetProvenance()
            const noexcept
        {
            return m_provenance;
        }

        template<typename Entry>
        [[nodiscard]] RenderGraphBlackboardCode Publish(
            RenderFeatureId producer,
            const RenderFeatureDescriptor& descriptor,
            Entry entry) noexcept
        {
            static_assert(IsSupportedEntry<Entry>());
            if (!m_provenance.IsValid() || entry.provenance != m_provenance)
            {
                return RecordFailure(
                    producer, RenderGraphBlackboardCode::InvalidProvenance);
            }
            const RenderFeatureResourceContract contract{
                Entry::ResourceId, Entry::SchemaVersion};
            if (descriptor.featureId != producer ||
                !ContainsContract(descriptor.producedResources, contract))
            {
                return RecordFailure(
                    producer, RenderGraphBlackboardCode::UndeclaredProducer);
            }
            if (!EntryMatchesGraph(entry, m_provenance))
            {
                return RecordFailure(
                    producer, RenderGraphBlackboardCode::StaleGraphResource);
            }

            auto& storage = std::get<std::optional<Entry>>(m_entries);
            if (storage.has_value())
            {
                return RecordFailure(
                    producer, RenderGraphBlackboardCode::DuplicateProducer);
            }
            storage = std::move(entry);
            m_producers[ToIndex(Entry::ResourceId)] = producer;
            return RenderGraphBlackboardCode::Success;
        }

        template<typename Entry>
        [[nodiscard]] const Entry* Require(
            RenderFeatureId consumer,
            const RenderFeatureDescriptor& descriptor,
            RenderGraphBlackboardCode* outCode = nullptr) const
        {
            static_assert(IsSupportedEntry<Entry>());
            const RenderFeatureResourceContract contract{
                Entry::ResourceId, Entry::SchemaVersion};
            if (descriptor.featureId != consumer ||
                !ContainsContract(descriptor.consumedResources, contract))
            {
                SetCode(outCode,
                        RenderGraphBlackboardCode::UndeclaredConsumer);
                RecordFailure(
                    consumer, RenderGraphBlackboardCode::UndeclaredConsumer);
                return nullptr;
            }
            const auto& storage = std::get<std::optional<Entry>>(m_entries);
            if (!storage.has_value())
            {
                SetCode(outCode, RenderGraphBlackboardCode::MissingResource);
                RecordFailure(
                    consumer, RenderGraphBlackboardCode::MissingResource);
                return nullptr;
            }
            RecordConsumption(consumer, Entry::ResourceId);
            SetCode(outCode, RenderGraphBlackboardCode::Success);
            return &*storage;
        }

        template<typename Entry>
        [[nodiscard]] const Entry* TryGetOptional(
            RenderFeatureId consumer,
            const RenderFeatureDescriptor& descriptor,
            RenderGraphBlackboardCode* outCode = nullptr) const
        {
            static_assert(IsSupportedEntry<Entry>());
            if (descriptor.fallbackPolicy == RenderFeatureFallbackPolicy::None)
            {
                SetCode(
                    outCode,
                    RenderGraphBlackboardCode::OptionalFallbackNotDeclared);
                RecordFailure(
                    consumer,
                    RenderGraphBlackboardCode::OptionalFallbackNotDeclared);
                return nullptr;
            }
            return Require<Entry>(consumer, descriptor, outCode);
        }

        [[nodiscard]] bool WasPublishedBy(
            RenderFeatureResourceContract contract,
            RenderFeatureId producer) const noexcept
        {
            const size_t index = ToIndex(contract.resource);
            return producer.IsValid() && index < m_producers.size() &&
                   contract.schemaVersion ==
                       GetResourceSchemaVersion(contract.resource) &&
                   m_producers[index] == producer;
        }

        [[nodiscard]] bool WasConsumedBy(
            RenderFeatureResourceContract contract,
            RenderFeatureId consumer) const noexcept
        {
            const size_t index = ToIndex(contract.resource);
            if (!consumer.IsValid() || index >= m_consumers.size() ||
                contract.schemaVersion !=
                    GetResourceSchemaVersion(contract.resource))
            {
                return false;
            }
            const std::vector<RenderFeatureId>& consumers = m_consumers[index];
            return std::find(consumers.begin(), consumers.end(), consumer) !=
                   consumers.end();
        }

        [[nodiscard]] RenderGraphBlackboardCode GetFailureCode()
            const noexcept
        {
            return m_failureCode;
        }

        [[nodiscard]] RenderFeatureId GetFailureFeature() const noexcept
        {
            return m_failureFeature;
        }

        [[nodiscard]] RenderFeatureId GetProducer(
            RenderBlackboardResourceId resource) const noexcept
        {
            const size_t index = ToIndex(resource);
            return index < m_producers.size() ? m_producers[index]
                                              : RenderFeatureId{};
        }

    private:
        using Entries = std::tuple<
            std::optional<SceneTextures>,
            std::optional<VisibilityOutputs>,
            std::optional<ShadowOutputs>,
            std::optional<LightingOutputs>,
            std::optional<ReflectionOutputs>,
            std::optional<TranslucencyOutputs>,
            std::optional<PostProcessOutputs>>;

        template<typename Entry>
        [[nodiscard]] static consteval bool IsSupportedEntry()
        {
            return std::is_same_v<Entry, SceneTextures> ||
                   std::is_same_v<Entry, VisibilityOutputs> ||
                   std::is_same_v<Entry, ShadowOutputs> ||
                   std::is_same_v<Entry, LightingOutputs> ||
                   std::is_same_v<Entry, ReflectionOutputs> ||
                   std::is_same_v<Entry, TranslucencyOutputs> ||
                   std::is_same_v<Entry, PostProcessOutputs>;
        }

        [[nodiscard]] static constexpr size_t ToIndex(
            RenderBlackboardResourceId resource) noexcept
        {
            return static_cast<size_t>(resource);
        }

        [[nodiscard]] static constexpr uint32 GetResourceSchemaVersion(
            RenderBlackboardResourceId resource) noexcept
        {
            switch (resource)
            {
                case RenderBlackboardResourceId::SceneTextures:
                    return SceneTextures::SchemaVersion;
                case RenderBlackboardResourceId::VisibilityOutputs:
                    return VisibilityOutputs::SchemaVersion;
                case RenderBlackboardResourceId::ShadowOutputs:
                    return ShadowOutputs::SchemaVersion;
                case RenderBlackboardResourceId::LightingOutputs:
                    return LightingOutputs::SchemaVersion;
                case RenderBlackboardResourceId::ReflectionOutputs:
                    return ReflectionOutputs::SchemaVersion;
                case RenderBlackboardResourceId::TranslucencyOutputs:
                    return TranslucencyOutputs::SchemaVersion;
                case RenderBlackboardResourceId::PostProcessOutputs:
                    return PostProcessOutputs::SchemaVersion;
                default: return 0;
            }
        }

        [[nodiscard]] static bool ContainsContract(
            const std::vector<RenderFeatureResourceContract>& contracts,
            const RenderFeatureResourceContract& target) noexcept
        {
            return std::find(contracts.begin(), contracts.end(), target) !=
                   contracts.end();
        }

        [[nodiscard]] static bool HandleMatchesGraph(
            const RGTextureHandle& handle,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            return !handle.IsValid() ||
                   (handle.graphIdentity == provenance.graphIdentity &&
                    handle.recordingGeneration ==
                        provenance.recordingGeneration);
        }

        [[nodiscard]] static bool HandleMatchesGraph(
            const RGBufferHandle& handle,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            return !handle.IsValid() ||
                   (handle.graphIdentity == provenance.graphIdentity &&
                    handle.recordingGeneration ==
                        provenance.recordingGeneration);
        }

        template<typename Entry>
        [[nodiscard]] static bool EntryMatchesGraph(
            const Entry& entry,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            if constexpr (std::is_same_v<Entry, SceneTextures>)
            {
                return HandleMatchesGraph(entry.hdrSceneColor, provenance) &&
                       HandleMatchesGraph(entry.deviceDepth, provenance) &&
                       HandleMatchesGraph(entry.linearDepth, provenance) &&
                       HandleMatchesGraph(entry.worldNormal, provenance) &&
                       HandleMatchesGraph(
                           entry.roughnessMetallic, provenance) &&
                       HandleMatchesGraph(entry.velocity, provenance) &&
                       HandleMatchesGraph(entry.ambientOcclusion, provenance) &&
                       HandleMatchesGraph(entry.reactiveMask, provenance) &&
                       HandleMatchesGraph(entry.distortionVectors, provenance) &&
                       HandleMatchesGraph(entry.exposureState, provenance) &&
                       HandleMatchesGraph(
                           entry.opaqueSceneSnapshot, provenance);
            }
            else if constexpr (std::is_same_v<Entry, VisibilityOutputs>)
            {
                return HandleMatchesGraph(entry.visibleInstances, provenance) &&
                       HandleMatchesGraph(entry.indirectDraws, provenance) &&
                       HandleMatchesGraph(entry.drawCount, provenance);
            }
            else if constexpr (std::is_same_v<Entry, ShadowOutputs>)
            {
                return HandleMatchesGraph(entry.directionalDepth, provenance) &&
                       HandleMatchesGraph(entry.rayTracedMask, provenance);
            }
            else if constexpr (std::is_same_v<Entry, LightingOutputs>)
            {
                return HandleMatchesGraph(entry.hdrSceneColor, provenance) &&
                       HandleMatchesGraph(entry.clusteredLights, provenance);
            }
            else if constexpr (std::is_same_v<Entry, ReflectionOutputs>)
            {
                return HandleMatchesGraph(entry.rawReflection, provenance) &&
                       HandleMatchesGraph(entry.denoisedReflection, provenance) &&
                       HandleMatchesGraph(entry.confidence, provenance);
            }
            else if constexpr (std::is_same_v<Entry, TranslucencyOutputs>)
            {
                return HandleMatchesGraph(entry.hdrSceneColor, provenance) &&
                       HandleMatchesGraph(entry.reactiveMask, provenance) &&
                       HandleMatchesGraph(entry.distortionVectors, provenance);
            }
            else
            {
                return HandleMatchesGraph(entry.presentableColor, provenance);
            }
        }

        static void SetCode(RenderGraphBlackboardCode* output,
                            RenderGraphBlackboardCode code) noexcept
        {
            if (output != nullptr)
            {
                *output = code;
            }
        }

        RenderGraphBlackboardCode RecordFailure(
            RenderFeatureId feature,
            RenderGraphBlackboardCode code) const noexcept
        {
            if (m_failureCode == RenderGraphBlackboardCode::Success)
            {
                m_failureCode = code;
                m_failureFeature = feature;
            }
            return code;
        }

        void RecordConsumption(RenderFeatureId consumer,
                               RenderBlackboardResourceId resource) const
        {
            std::vector<RenderFeatureId>& consumers =
                m_consumers[ToIndex(resource)];
            if (std::find(consumers.begin(), consumers.end(), consumer) ==
                consumers.end())
            {
                consumers.push_back(consumer);
            }
        }

        RenderGraphBlackboardProvenance m_provenance{};
        Entries m_entries{};
        std::array<RenderFeatureId,
                   static_cast<size_t>(RenderBlackboardResourceId::Count)>
            m_producers{};
        mutable std::array<
            std::vector<RenderFeatureId>,
            static_cast<size_t>(RenderBlackboardResourceId::Count)>
            m_consumers{};
        mutable RenderGraphBlackboardCode m_failureCode =
            RenderGraphBlackboardCode::Success;
        mutable RenderFeatureId m_failureFeature{};
    };
} // namespace RVX
