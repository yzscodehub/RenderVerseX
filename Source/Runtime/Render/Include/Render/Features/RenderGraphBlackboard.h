#pragma once

/**
 * @file RenderGraphBlackboard.h
 * @brief Closed, typed exchange of graph-generation-scoped feature resources.
 */

#include "Render/Features/RenderFeatureTypes.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/PostProcess/PostProcessTypes.h"

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
        static constexpr uint32 SchemaVersion = 4;
        static constexpr uint32 ExposureStateSchemaVersion = 1;
        static constexpr uint32 ExposureStateStride = 48;

        /**
         * @brief Graph-independent description for one scene texture.
         *
         * This is deliberately a compact value contract rather than an RHI
         * resource reference. Features can validate their negotiated scene
         * surface layout while graph provenance still protects every handle.
         */
        struct Metadata
        {
            RenderOutputExtent extent{};
            RHIFormat format = RHIFormat::Unknown;
            RHISampleCount sampleCount = RHISampleCount::Count1;

            friend constexpr bool operator==(const Metadata&,
                                             const Metadata&) = default;
        };

        struct FormatContract
        {
            RHIFormat format = RHIFormat::Unknown;
            RHISampleCount sampleCount = RHISampleCount::Count1;

            friend constexpr bool operator==(const FormatContract&,
                                             const FormatContract&) = default;
        };

        /** Graph-independent ABI and configuration identity for exposure state. */
        struct ExposureStateMetadata
        {
            uint32 schemaVersion = 0;
            uint32 stride = 0;
            RenderExposureMode mode = RenderExposureMode::ManualEV100;
            uint64 configurationHash = 0;

            [[nodiscard]] constexpr bool IsValid() const noexcept
            {
                return schemaVersion == ExposureStateSchemaVersion &&
                       stride == ExposureStateStride &&
                       IsValidRenderExposureMode(mode) &&
                       configurationHash != 0;
            }

            friend constexpr bool operator==(
                const ExposureStateMetadata&,
                const ExposureStateMetadata&) = default;
        };

        // The renderer's fixed deferred layout. RHI names the requested
        // RGBA8_SRGB storage format as RGBA8_UNORM_SRGB.
        inline static constexpr FormatContract HDRSceneColorContract{
            RHIFormat::RGBA16_FLOAT, RHISampleCount::Count1};
        inline static constexpr FormatContract GBuffer0Contract{
            RHIFormat::RGBA8_UNORM_SRGB, RHISampleCount::Count1};
        inline static constexpr FormatContract GBuffer1Contract{
            RHIFormat::RGBA8_UNORM, RHISampleCount::Count1};
        inline static constexpr FormatContract GBuffer2Contract{
            RHIFormat::RGBA16_FLOAT, RHISampleCount::Count1};
        inline static constexpr FormatContract GBuffer3Contract{
            RHIFormat::R32_UINT, RHISampleCount::Count1};
        inline static constexpr FormatContract VelocityContract{
            RHIFormat::RG16_FLOAT, RHISampleCount::Count1};
        inline static constexpr FormatContract ReactiveMaskContract{
            RHIFormat::R8_UNORM, RHISampleCount::Count1};
        inline static constexpr FormatContract DeviceDepthContract{
            RHIFormat::D32_FLOAT, RHISampleCount::Count1};

        RenderGraphBlackboardProvenance provenance{};
        RGTextureHandle hdrSceneColor{};
        Metadata hdrSceneColorMetadata{};
        RGTextureHandle deviceDepth{};
        Metadata deviceDepthMetadata{};
        RGTextureHandle linearDepth{};
        Metadata linearDepthMetadata{};
        RGTextureHandle worldNormal{};
        Metadata worldNormalMetadata{};
        RGTextureHandle roughnessMetallic{};
        Metadata roughnessMetallicMetadata{};
        // Optional until a velocity-producing feature is selected.  When
        // present it remains subject to the exact RG16F scene contract below;
        // Deferred geometry must not publish an uninitialized stand-in merely
        // to satisfy temporal consumers that are not part of this recipe.
        RGTextureHandle velocity{};
        Metadata velocityMetadata{};
        RGTextureHandle reactiveMask{};
        Metadata reactiveMaskMetadata{};
        RGTextureHandle distortionVectors{};
        Metadata distortionVectorsMetadata{};
        RGBufferHandle exposureState{};
        ExposureStateMetadata exposureStateMetadata{};
        RGTextureHandle opaqueSceneSnapshot{};
        Metadata opaqueSceneSnapshotMetadata{};

        // Deferred opaque geometry. GBuffer1 carries octahedral normal plus
        // roughness/metallic, while GBuffer3 indexes the material table.
        RGTextureHandle gBuffer0{};
        Metadata gBuffer0Metadata{};
        RGTextureHandle gBuffer1{};
        Metadata gBuffer1Metadata{};
        RGTextureHandle gBuffer2{};
        Metadata gBuffer2Metadata{};
        RGTextureHandle gBuffer3{};
        Metadata gBuffer3Metadata{};
        RGBufferHandle materialTable{};
    };

    enum class DeferredSceneTexturesValidationCode : uint8
    {
        Success = 0,
        MissingRequiredHandle,
        MissingMaterialTable,
        InvalidExposureState,
        InvalidExtent,
        FormatMismatch,
        ExtentMismatch,
        SampleCountMismatch,
    };

    /**
     * @brief Fail-closed value validation of Deferred opaque scene outputs.
     *
     * The function is intentionally graph-independent and side-effect-free:
     * publication validates graph generation/provenance separately. Deferred
     * composition must call this after creating the resources and before
     * declaring the GBuffer/lighting dependency edge.
     */
    [[nodiscard]] inline DeferredSceneTexturesValidationCode
    ValidateDeferredSceneTextures(const SceneTextures& textures) noexcept
    {
        if (!textures.hdrSceneColor.IsValid() ||
            !textures.deviceDepth.IsValid() ||
            !textures.exposureState.IsValid() ||
            !textures.gBuffer0.IsValid() || !textures.gBuffer1.IsValid() ||
            !textures.gBuffer2.IsValid() || !textures.gBuffer3.IsValid())
        {
            return DeferredSceneTexturesValidationCode::MissingRequiredHandle;
        }
        if (!textures.materialTable.IsValid())
        {
            return DeferredSceneTexturesValidationCode::MissingMaterialTable;
        }
        if (!textures.exposureStateMetadata.IsValid())
        {
            return DeferredSceneTexturesValidationCode::InvalidExposureState;
        }

        const RenderOutputExtent extent = textures.hdrSceneColorMetadata.extent;
        if (!extent.IsValid())
        {
            return DeferredSceneTexturesValidationCode::InvalidExtent;
        }

        const auto validate = [extent](const SceneTextures::Metadata& metadata,
                                       SceneTextures::FormatContract contract)
            -> DeferredSceneTexturesValidationCode
        {
            if (!metadata.extent.IsValid())
            {
                return DeferredSceneTexturesValidationCode::InvalidExtent;
            }
            if (metadata.extent != extent)
            {
                return DeferredSceneTexturesValidationCode::ExtentMismatch;
            }
            if (metadata.sampleCount != contract.sampleCount)
            {
                return DeferredSceneTexturesValidationCode::SampleCountMismatch;
            }
            if (metadata.format != contract.format)
            {
                return DeferredSceneTexturesValidationCode::FormatMismatch;
            }
            return DeferredSceneTexturesValidationCode::Success;
        };

        for (const auto [metadata, contract] : std::array{
                 std::pair{textures.hdrSceneColorMetadata,
                           SceneTextures::HDRSceneColorContract},
                 std::pair{textures.deviceDepthMetadata,
                           SceneTextures::DeviceDepthContract},
                 std::pair{textures.gBuffer0Metadata,
                           SceneTextures::GBuffer0Contract},
                 std::pair{textures.gBuffer1Metadata,
                           SceneTextures::GBuffer1Contract},
                 std::pair{textures.gBuffer2Metadata,
                           SceneTextures::GBuffer2Contract},
                 std::pair{textures.gBuffer3Metadata,
                           SceneTextures::GBuffer3Contract}})
        {
            const DeferredSceneTexturesValidationCode result =
                validate(metadata, contract);
            if (result != DeferredSceneTexturesValidationCode::Success)
            {
                return result;
            }
        }
        if (textures.velocity.IsValid())
        {
            const DeferredSceneTexturesValidationCode result = validate(
                textures.velocityMetadata, SceneTextures::VelocityContract);
            if (result != DeferredSceneTexturesValidationCode::Success)
            {
                return result;
            }
        }
        return DeferredSceneTexturesValidationCode::Success;
    }

    /**
     * @brief Canonical GTAO visibility field produced before opaque lighting.
     *
     * The field is intentionally independent from SceneTextures so a consumer
     * must declare the AO dependency explicitly rather than silently treating
     * it as a mutable scene-color side channel.
     */
    struct AmbientOcclusionOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::AmbientOcclusionOutputs;
        static constexpr uint32 SchemaVersion = 1;
        inline static constexpr SceneTextures::FormatContract VisibilityContract{
            RHIFormat::R8_UNORM, RHISampleCount::Count1};

        RenderGraphBlackboardProvenance provenance{};
        /** Exact frame/view and temporal key that generated this field. */
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RenderViewHistoryIdentity historyIdentity{};
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        /** Hash of the immutable RenderAmbientOcclusionSettings value. */
        uint64 settingsHash = 0;
        /** Closed indirect-only lighting consumer contract. */
        RenderAmbientOcclusionConsumerMask consumerMask = 0;
        /** Immutable content identity of the published R8 visibility field. */
        uint64 ambientOcclusionFieldHash = 0;
        RGTextureHandle ambientOcclusion{};
        SceneTextures::Metadata ambientOcclusionMetadata{};
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
        static constexpr uint32 SchemaVersion = 3;

        RenderGraphBlackboardProvenance provenance{};
        /** Immutable LightingFramePlan identity shared by every lighting consumer. */
        uint64 lightingPlanIdentity = 0;
        RGTextureHandle hdrSceneColor{};
        RGBufferHandle clusteredLights{};

        [[nodiscard]] bool IsCompatibleWithLightingPlan(
            uint64 expectedLightingPlanIdentity) const noexcept
        {
            return expectedLightingPlanIdentity != 0 &&
                   lightingPlanIdentity == expectedLightingPlanIdentity;
        }
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
        static constexpr uint32 SchemaVersion = 3;

        RenderGraphBlackboardProvenance provenance{};
        /** Exact frozen frame/view that produced these graph resources. */
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        /** The canonical scene-linear HDR surface after special/transparent work. */
        RGTextureHandle hdrSceneColor{};
        SceneTextures::Metadata hdrSceneColorMetadata{};
        /** Optional scene inputs retained for typed post-process planning. */
        RGTextureHandle deviceDepth{};
        SceneTextures::Metadata deviceDepthMetadata{};
        RGTextureHandle worldNormal{};
        SceneTextures::Metadata worldNormalMetadata{};
        RGTextureHandle velocity{};
        SceneTextures::Metadata velocityMetadata{};
        RGTextureHandle reactiveMask{};
        SceneTextures::Metadata reactiveMaskMetadata{};
        RGTextureHandle distortionVectors{};
        SceneTextures::Metadata distortionVectorsMetadata{};
        /** Committed prior-presented state used by all HDR producers. */
        RGBufferHandle exposureState{};
        SceneTextures::ExposureStateMetadata exposureStateMetadata{};
        PostProcessColorDomain colorDomain =
            PostProcessColorDomain::SceneLinearHDR;
        PostProcessTransfer transfer = PostProcessTransfer::None;
    };

    struct PostProcessOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::PostProcessOutputs;
        static constexpr uint32 SchemaVersion = 2;

        RenderGraphBlackboardProvenance provenance{};
        /** Exact frozen frame/view that owns the presentation surface. */
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RGTextureHandle presentableColor{};
        SceneTextures::Metadata presentableColorMetadata{};
        /**
         * Presentation publishers must state the final stored domain and the
         * transfer that produced it. Defaults intentionally make an omitted
         * transition fail closed instead of fabricating an encode boundary.
         */
        PostProcessColorDomain colorDomain = PostProcessColorDomain::Unknown;
        PostProcessTransfer transfer = PostProcessTransfer::None;
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
        InvalidSemanticContract,
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
            case RenderGraphBlackboardCode::InvalidSemanticContract:
                return "InvalidSemanticContract";
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
            if (!EntryHasValidSemanticContract(entry, m_provenance))
            {
                return RecordFailure(
                    producer,
                    RenderGraphBlackboardCode::InvalidSemanticContract);
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
            std::optional<PostProcessOutputs>,
            std::optional<AmbientOcclusionOutputs>>;

        template<typename Entry>
        [[nodiscard]] static consteval bool IsSupportedEntry()
        {
            return std::is_same_v<Entry, SceneTextures> ||
                   std::is_same_v<Entry, VisibilityOutputs> ||
                   std::is_same_v<Entry, ShadowOutputs> ||
                   std::is_same_v<Entry, LightingOutputs> ||
                   std::is_same_v<Entry, ReflectionOutputs> ||
                   std::is_same_v<Entry, TranslucencyOutputs> ||
                   std::is_same_v<Entry, PostProcessOutputs> ||
                   std::is_same_v<Entry, AmbientOcclusionOutputs>;
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
                case RenderBlackboardResourceId::AmbientOcclusionOutputs:
                    return AmbientOcclusionOutputs::SchemaVersion;
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
                       HandleMatchesGraph(entry.reactiveMask, provenance) &&
                       HandleMatchesGraph(entry.distortionVectors, provenance) &&
                       HandleMatchesGraph(entry.exposureState, provenance) &&
                       HandleMatchesGraph(
                           entry.opaqueSceneSnapshot, provenance) &&
                       HandleMatchesGraph(entry.gBuffer0, provenance) &&
                       HandleMatchesGraph(entry.gBuffer1, provenance) &&
                       HandleMatchesGraph(entry.gBuffer2, provenance) &&
                       HandleMatchesGraph(entry.gBuffer3, provenance) &&
                       HandleMatchesGraph(entry.materialTable, provenance);
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
                return entry.lightingPlanIdentity != 0 &&
                       HandleMatchesGraph(entry.hdrSceneColor, provenance) &&
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
                       HandleMatchesGraph(entry.deviceDepth, provenance) &&
                       HandleMatchesGraph(entry.worldNormal, provenance) &&
                       HandleMatchesGraph(entry.velocity, provenance) &&
                       HandleMatchesGraph(entry.reactiveMask, provenance) &&
                       HandleMatchesGraph(entry.distortionVectors, provenance) &&
                       HandleMatchesGraph(entry.exposureState, provenance);
            }
            else if constexpr (std::is_same_v<Entry, AmbientOcclusionOutputs>)
            {
                return HandleMatchesGraph(entry.ambientOcclusion, provenance);
            }
            else
            {
                return HandleMatchesGraph(entry.presentableColor, provenance);
            }
        }

        [[nodiscard]] static bool MetadataMatches(
            const SceneTextures::Metadata& metadata,
            SceneTextures::FormatContract contract) noexcept
        {
            return metadata.extent.IsValid() &&
                   metadata.format == contract.format &&
                   metadata.sampleCount == contract.sampleCount;
        }

        [[nodiscard]] static bool OptionalMetadataMatches(
            const RGTextureHandle& handle,
            const SceneTextures::Metadata& metadata,
            SceneTextures::FormatContract contract) noexcept
        {
            return !handle.IsValid() || MetadataMatches(metadata, contract);
        }

        [[nodiscard]] static bool HasMatchingFrameViewProvenance(
            const RenderFrameIdentity& frameIdentity,
            const RenderViewIdentity& viewIdentity,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            return frameIdentity.IsValid() && viewIdentity.IsValid() &&
                   frameIdentity.frameSequence == provenance.frameSequence;
        }

        [[nodiscard]] static bool HasValidTranslucencyOutputs(
            const TranslucencyOutputs& outputs,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            return HasMatchingFrameViewProvenance(
                       outputs.frameIdentity, outputs.viewIdentity,
                       provenance) &&
                   outputs.hdrSceneColor.IsValid() &&
                   MetadataMatches(outputs.hdrSceneColorMetadata,
                                   SceneTextures::HDRSceneColorContract) &&
                   outputs.colorDomain ==
                       PostProcessColorDomain::SceneLinearHDR &&
                   outputs.transfer == PostProcessTransfer::None &&
                   outputs.exposureState.IsValid() &&
                   outputs.exposureStateMetadata.IsValid() &&
                   OptionalMetadataMatches(
                       outputs.deviceDepth, outputs.deviceDepthMetadata,
                       SceneTextures::DeviceDepthContract) &&
                   OptionalMetadataMatches(
                       outputs.worldNormal, outputs.worldNormalMetadata,
                       SceneTextures::FormatContract{
                           RHIFormat::RGBA8_UNORM,
                           RHISampleCount::Count1}) &&
                   OptionalMetadataMatches(
                       outputs.velocity, outputs.velocityMetadata,
                       SceneTextures::VelocityContract) &&
                   OptionalMetadataMatches(
                       outputs.reactiveMask, outputs.reactiveMaskMetadata,
                       SceneTextures::ReactiveMaskContract) &&
                   OptionalMetadataMatches(
                       outputs.distortionVectors,
                       outputs.distortionVectorsMetadata,
                       SceneTextures::FormatContract{
                           RHIFormat::RG16_FLOAT,
                           RHISampleCount::Count1});
        }

        [[nodiscard]] static bool HasValidPostProcessOutputs(
            const PostProcessOutputs& outputs,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            return HasMatchingFrameViewProvenance(
                       outputs.frameIdentity, outputs.viewIdentity,
                       provenance) &&
                   outputs.presentableColor.IsValid() &&
                   outputs.presentableColorMetadata.extent.IsValid() &&
                   outputs.presentableColorMetadata.format !=
                       RHIFormat::Unknown &&
                   outputs.presentableColorMetadata.sampleCount ==
                       RHISampleCount::Count1 &&
                   outputs.colorDomain ==
                       PostProcessColorDomain::DisplayEncodedLDR &&
                   (outputs.transfer ==
                        PostProcessTransfer::DisplayLinearToEncoded ||
                    outputs.transfer ==
                        PostProcessTransfer::SceneLinearToDisplayEncoded);
        }

        [[nodiscard]] static bool HasValidAmbientOcclusionOutputs(
            const AmbientOcclusionOutputs& outputs,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            return HasMatchingFrameViewProvenance(
                       outputs.frameIdentity, outputs.viewIdentity,
                       provenance) &&
                   outputs.historyIdentity.IsValid() &&
                   outputs.historyIdentity.previousFrame ==
                       outputs.frameIdentity &&
                   outputs.historyIdentity.view == outputs.viewIdentity &&
                   outputs.historyIdentity.extent ==
                       outputs.ambientOcclusionMetadata.extent &&
                   outputs.settingsHash != 0 &&
                   IsValidRenderAmbientOcclusionConsumerMask(
                       outputs.consumerMask) &&
                   outputs.ambientOcclusionFieldHash != 0 &&
                   outputs.ambientOcclusion.IsValid() &&
                   MetadataMatches(outputs.ambientOcclusionMetadata,
                                   AmbientOcclusionOutputs::VisibilityContract);
        }

        template<typename Entry>
        [[nodiscard]] static bool EntryHasValidSemanticContract(
            const Entry& entry,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            if constexpr (std::is_same_v<Entry, TranslucencyOutputs>)
            {
                return HasValidTranslucencyOutputs(entry, provenance);
            }
            else if constexpr (std::is_same_v<Entry, PostProcessOutputs>)
            {
                return HasValidPostProcessOutputs(entry, provenance);
            }
            else if constexpr (std::is_same_v<Entry, AmbientOcclusionOutputs>)
            {
                return HasValidAmbientOcclusionOutputs(entry, provenance);
            }
            else
            {
                return true;
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
