#pragma once

/**
 * @file RenderGraphBlackboard.h
 * @brief Closed, typed exchange of graph-generation-scoped feature resources.
 */

#include "Render/Features/RenderFeatureTypes.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/RenderGraphExecutionReceipt.h"
#include "Render/Lighting/VolumetricLightingFramePlan.h"
#include "Render/PostProcess/PostProcessTypes.h"

#include <algorithm>
#include <array>
#include <optional>
#include <memory>
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
        static constexpr uint32 SchemaVersion = 5;
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
        /**
         * Exact pre-exposed indirect-specular baseline.  Reflection routing
         * replaces this contribution instead of adding a second environment
         * term to already-lit HDR scene color.
         */
        inline static constexpr FormatContract BaseIndirectSpecularContract{
            RHIFormat::RGBA16_FLOAT, RHISampleCount::Count1};
        /**
         * Dimensionless material/visibility response for a routed indirect
         * specular source.  It is kept in RGBA16F so Forward and Deferred
         * expose one lossless, backend-neutral replacement ABI.
         */
        inline static constexpr FormatContract SpecularResponseContract{
            RHIFormat::RGBA16_FLOAT, RHISampleCount::Count1};

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
        PostProcessMaterialTableMetadata materialTableMetadata{};
    };

    /**
     * @brief Full-resolution sampled D32 surface produced once by the selected
     *        special-surface composition and borrowed by Water/Particle.
     *
     * The blackboard captures semantic provenance and the copy receipt. It
     * does not replace the concrete RenderGraph Read edge each consumer must
     * register when it samples @ref depthSnapshot.
     */
    struct SceneDepthSnapshot
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::SceneDepthSnapshot;
        static constexpr uint32 SchemaVersion = 1;
        inline static constexpr SceneTextures::FormatContract FormatContract{
            RHIFormat::D32_FLOAT, RHISampleCount::Count1};

        RenderGraphBlackboardProvenance provenance{};
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        /** Output extent negotiated by the selected composition. */
        RenderOutputExtent outputExtent{};
        /** At least one special surface requested sampled scene depth. */
        bool requested = false;
        /** A selected consumer cannot use its declared fallback without depth. */
        bool required = false;
        /** A current D32 CopySrc source was copied and is safe to sample. */
        bool available = false;
        RGTextureHandle depthSnapshot{};
        SceneTextures::Metadata depthSnapshotMetadata{};
        std::shared_ptr<SceneDepthSnapshotCopyReceipt> receipt{};
    };

    enum class DeferredSceneTexturesValidationCode : uint8
    {
        Success = 0,
        MissingRequiredHandle,
        MissingMaterialTable,
        InvalidMaterialTable,
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
        if (!textures.materialTableMetadata.IsValid() ||
            textures.materialTableMetadata.sourceExtent !=
                textures.gBuffer3Metadata.extent ||
            textures.materialTableMetadata.sourceSampleCount !=
                textures.gBuffer3Metadata.sampleCount)
        {
            return DeferredSceneTexturesValidationCode::InvalidMaterialTable;
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

    /** Exact point/spot shadow-atlas registration for one frame/view. */
    struct LocalShadowOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::LocalShadowOutputs;
        static constexpr uint32 SchemaVersion = 1;

        RenderGraphBlackboardProvenance provenance{};
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint64 lightingPlanIdentity = 0;
        uint64 localShadowPlanIdentity = 0;
        uint64 atlasPlanIdentity = 0;
        uint64 atlasGeneration = 0;
        RGTextureHandle depthAtlas{};
        uint32 pageCount = 0;
        uint32 pageResolution = 0;
        bool requested = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            const bool validIdentity = frameIdentity.IsValid() &&
                viewIdentity.IsValid() && lightingPlanIdentity != 0 &&
                localShadowPlanIdentity != 0 && atlasPlanIdentity != 0 &&
                atlasGeneration != 0;
            return validIdentity &&
                ((!requested && pageCount == 0 && !depthAtlas.IsValid()) ||
                 (requested && pageCount != 0 && pageResolution != 0 &&
                  depthAtlas.IsValid()));
        }
    };

    struct LightingOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::LightingOutputs;
        static constexpr uint32 SchemaVersion = 5;

        RenderGraphBlackboardProvenance provenance{};
        /** Exact frozen frame/view that produced the lit HDR surface. */
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        /** Immutable LightingFramePlan identity shared by every lighting consumer. */
        uint64 lightingPlanIdentity = 0;
        RGTextureHandle hdrSceneColor{};
        SceneTextures::Metadata hdrSceneColorMetadata{};
        RGBufferHandle clusteredLights{};
        /**
         * Exact pre-exposed baseline already contained in hdrSceneColor.
         * Reflection resolve must subtract this value before adding a routed
         * screen-space/probe source materialized with specularResponse.
         */
        RGTextureHandle baseIndirectSpecular{};
        SceneTextures::Metadata baseIndirectSpecularMetadata{};
        /**
         * Dimensionless Fresnel/BRDF/material-AO/specular-AO response.  This
         * is intentionally separate from baseIndirectSpecular so a new
         * radiance source cannot double-apply the material response.
         */
        RGTextureHandle specularResponse{};
        SceneTextures::Metadata specularResponseMetadata{};
        /** Exact committed exposure identity shared by all three surfaces. */
        RGBufferHandle exposureState{};
        SceneTextures::ExposureStateMetadata exposureStateMetadata{};
        uint64 exposureStateContentHash = 0;
        /** Optional surface guides preserved for HDR stylized post processing. */
        RGTextureHandle worldNormal{};
        SceneTextures::Metadata worldNormalMetadata{};
        RGTextureHandle materialIdentity{};
        SceneTextures::Metadata materialIdentityMetadata{};
        RGBufferHandle materialParameterTable{};
        PostProcessMaterialTableMetadata materialParameterTableMetadata{};

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
        static constexpr uint32 SchemaVersion = 3;

        RenderGraphBlackboardProvenance provenance{};
        /** Exact frozen frame/view and lighting plan resolved by this feature. */
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint64 lightingPlanIdentity = 0;
        /** Canonical scene-linear HDR surface after reflection resolution. */
        RGTextureHandle resolvedSceneColor{};
        SceneTextures::Metadata resolvedSceneColorMetadata{};
        /** Optional surface guides forwarded without mutation. */
        RGTextureHandle worldNormal{};
        SceneTextures::Metadata worldNormalMetadata{};
        RGTextureHandle materialIdentity{};
        SceneTextures::Metadata materialIdentityMetadata{};
        RGBufferHandle materialParameterTable{};
        PostProcessMaterialTableMetadata materialParameterTableMetadata{};
        /** Future reflection implementations may publish these typed fields. */
        RGTextureHandle rawReflection{};
        RGTextureHandle denoisedReflection{};
        RGTextureHandle confidence{};
        /** Capability-honest execution state for the current identity resolve. */
        bool screenSpaceReflectionsApplied = false;
        bool reflectionProbesApplied = false;
        /**
         * @brief True only when the frozen hybrid policy selected the RT
         * reflection route for this graph. Exact execution is reported by the
         * frame receipt after submission rather than mutating this immutable
         * blackboard payload.
         */
        bool rayTracedReflectionsScheduled = false;
        bool rayTracedReflectionsApplied = false;
        /** Nonzero only when this boundary consumed an immutable hybrid plan. */
        uint64 hybridRayTracingPlanIdentity = 0;
        bool imageBasedLightingPreserved = false;

        [[nodiscard]] bool IsCompatibleWithLightingPlan(
            uint64 expectedLightingPlanIdentity) const noexcept
        {
            return expectedLightingPlanIdentity != 0 &&
                   lightingPlanIdentity == expectedLightingPlanIdentity;
        }
    };

    /**
     * @brief Exact GPU layout for the integrated froxel scattering field.
     *
     * The grid itself is renderer policy. This metadata is deliberately kept
     * separate so a graph handle cannot pretend that a 2D surface or a
     * differently-sized 3D allocation is the sealed froxel result.
     */
    struct FroxelVolumeMetadata
    {
        RHITextureDimension dimension = RHITextureDimension::Texture1D;
        RHIFormat format = RHIFormat::Unknown;
        RHISampleCount sampleCount = RHISampleCount::Count1;
        uint32 width = 0;
        uint32 height = 0;
        uint32 depth = 0;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return dimension == RHITextureDimension::Texture1D &&
                   format == RHIFormat::Unknown &&
                   sampleCount == RHISampleCount::Count1 && width == 0 &&
                   height == 0 && depth == 0;
        }

        [[nodiscard]] bool IsValidFor(
            const FroxelGridMetadata& grid) const noexcept
        {
            return grid.IsValid() &&
                   dimension == RHITextureDimension::Texture3D &&
                   format == RHIFormat::RGBA16_FLOAT &&
                   sampleCount == RHISampleCount::Count1 &&
                   width == grid.froxelWidth && height == grid.froxelHeight &&
                   depth == grid.depthSliceCount;
        }

        friend constexpr bool operator==(const FroxelVolumeMetadata&,
                                         const FroxelVolumeMetadata&) = default;
    };

    /**
     * @brief Immutable froxel lighting result shared by special/translucent work.
     *
     * The identity route forwards the exact input HDR surface and has no GPU
     * froxel resource. A non-identity result only exposes the integrated
     * scattering/transmittance volume; consumers apply it at their own depth
     * and must not re-run an unbounded per-material ray march.
     */
    struct VolumetricLightingOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::VolumetricLightingOutputs;
        static constexpr uint32 SchemaVersion = 1;

        RenderGraphBlackboardProvenance provenance{};
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RenderViewHistoryIdentity historyIdentity{};
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        uint64 lightingPlanIdentity = 0;
        uint64 volumetricPlanIdentity = 0;
        uint64 settingsHash = 0;
        FroxelGridMetadata froxelGrid{};
        /** The exact HDR surface passed to this feature. */
        RGTextureHandle sourceSceneColor{};
        SceneTextures::Metadata sourceSceneColorMetadata{};
        /** The HDR surface observed by all downstream volumetric consumers. */
        RGTextureHandle resolvedSceneColor{};
        SceneTextures::Metadata resolvedSceneColorMetadata{};
        RGTextureHandle integratedScatteringTransmittance{};
        FroxelVolumeMetadata integratedScatteringTransmittanceMetadata{};
        RGTextureHandle worldNormal{};
        SceneTextures::Metadata worldNormalMetadata{};
        RGTextureHandle materialIdentity{};
        SceneTextures::Metadata materialIdentityMetadata{};
        RGBufferHandle materialParameterTable{};
        PostProcessMaterialTableMetadata materialParameterTableMetadata{};
        /** True only for the exact no-density/disabled identity publication. */
        bool identityRoute = true;
        /** True only when this graph actually consumed compatible history. */
        bool temporalHistoryUsed = false;
    };

    /**
     * @brief Scene-linear surface after Sky/Water but before mesh or particle
     *        translucency consumes it.
     *
     * This boundary deliberately excludes OIT accumulation resources. Those
     * resources are private to the recipe-neutral Translucency feature and
     * cannot escape into post processing or a later frame.
     */
    struct PreTranslucencyOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::PreTranslucencyOutputs;
        static constexpr uint32 SchemaVersion = 2;

        RenderGraphBlackboardProvenance provenance{};
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint64 lightingPlanIdentity = 0;
        RGTextureHandle hdrSceneColor{};
        SceneTextures::Metadata hdrSceneColorMetadata{};
        RGTextureHandle deviceDepth{};
        SceneTextures::Metadata deviceDepthMetadata{};
        RGTextureHandle worldNormal{};
        SceneTextures::Metadata worldNormalMetadata{};
        RGTextureHandle materialIdentity{};
        SceneTextures::Metadata materialIdentityMetadata{};
        RGBufferHandle materialParameterTable{};
        PostProcessMaterialTableMetadata materialParameterTableMetadata{};
        RGTextureHandle velocity{};
        SceneTextures::Metadata velocityMetadata{};
        RGTextureHandle reactiveMask{};
        SceneTextures::Metadata reactiveMaskMetadata{};
        RGTextureHandle opaqueSceneSnapshot{};
        SceneTextures::Metadata opaqueSceneSnapshotMetadata{};
        std::shared_ptr<OpaqueSceneSnapshotCopyReceipt>
            opaqueSceneSnapshotReceipt{};
        RGBufferHandle exposureState{};
        SceneTextures::ExposureStateMetadata exposureStateMetadata{};
        /** Exact froxel result forwarded by the selected special-surface route. */
        uint64 volumetricPlanIdentity = 0;
        FroxelGridMetadata froxelGrid{};
        RenderViewHistoryIdentity volumetricHistoryIdentity{};
        RenderHistoryResetReason volumetricHistoryResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        RGTextureHandle integratedScatteringTransmittance{};
        FroxelVolumeMetadata integratedScatteringTransmittanceMetadata{};
        bool volumetricIdentityRoute = true;
        bool volumetricTemporalHistoryUsed = false;
        PostProcessColorDomain colorDomain =
            PostProcessColorDomain::SceneLinearHDR;
        PostProcessTransfer transfer = PostProcessTransfer::None;
    };

    struct TranslucencyOutputs
    {
        static constexpr RenderBlackboardResourceId ResourceId =
            RenderBlackboardResourceId::TranslucencyOutputs;
        static constexpr uint32 SchemaVersion = 5;

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
        /**
         * Full-resolution R32_UINT parameter-table slot identity.  It and
         * worldNormal are intentionally retained after translucency so HDR
         * stylized post passes can select material semantics without guessing
         * them from scene color.
         */
        RGTextureHandle materialIdentity{};
        SceneTextures::Metadata materialIdentityMetadata{};
        /**
         * Exact structured MaterialGPUConstants table addressed by
         * materialIdentity.  The metadata binds its ABI and immutable content
         * identity to this frame/view scene surface.
         */
        RGBufferHandle materialParameterTable{};
        PostProcessMaterialTableMetadata materialParameterTableMetadata{};
        RGTextureHandle velocity{};
        SceneTextures::Metadata velocityMetadata{};
        RGTextureHandle reactiveMask{};
        SceneTextures::Metadata reactiveMaskMetadata{};
        /** Immutable HDR copy after Sky and before every translucent writer. */
        RGTextureHandle opaqueSceneSnapshot{};
        SceneTextures::Metadata opaqueSceneSnapshotMetadata{};
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
            std::optional<LocalShadowOutputs>,
            std::optional<LightingOutputs>,
            std::optional<ReflectionOutputs>,
            std::optional<VolumetricLightingOutputs>,
            std::optional<PreTranslucencyOutputs>,
            std::optional<TranslucencyOutputs>,
            std::optional<PostProcessOutputs>,
            std::optional<AmbientOcclusionOutputs>,
            std::optional<SceneDepthSnapshot>>;

        template<typename Entry>
        [[nodiscard]] static consteval bool IsSupportedEntry()
        {
            return std::is_same_v<Entry, SceneTextures> ||
                   std::is_same_v<Entry, SceneDepthSnapshot> ||
                   std::is_same_v<Entry, VisibilityOutputs> ||
                   std::is_same_v<Entry, ShadowOutputs> ||
                   std::is_same_v<Entry, LocalShadowOutputs> ||
                   std::is_same_v<Entry, LightingOutputs> ||
                   std::is_same_v<Entry, ReflectionOutputs> ||
                   std::is_same_v<Entry, VolumetricLightingOutputs> ||
                   std::is_same_v<Entry, PreTranslucencyOutputs> ||
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
                case RenderBlackboardResourceId::SceneDepthSnapshot:
                    return SceneDepthSnapshot::SchemaVersion;
                case RenderBlackboardResourceId::VisibilityOutputs:
                    return VisibilityOutputs::SchemaVersion;
                case RenderBlackboardResourceId::ShadowOutputs:
                    return ShadowOutputs::SchemaVersion;
                case RenderBlackboardResourceId::LocalShadowOutputs:
                    return LocalShadowOutputs::SchemaVersion;
                case RenderBlackboardResourceId::LightingOutputs:
                    return LightingOutputs::SchemaVersion;
                case RenderBlackboardResourceId::ReflectionOutputs:
                    return ReflectionOutputs::SchemaVersion;
                case RenderBlackboardResourceId::VolumetricLightingOutputs:
                    return VolumetricLightingOutputs::SchemaVersion;
                case RenderBlackboardResourceId::PreTranslucencyOutputs:
                    return PreTranslucencyOutputs::SchemaVersion;
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
                       HandleMatchesGraph(entry.opaqueSceneSnapshot, provenance) &&
                       HandleMatchesGraph(entry.distortionVectors, provenance) &&
                       HandleMatchesGraph(entry.exposureState, provenance) &&
                       HandleMatchesGraph(entry.gBuffer0, provenance) &&
                       HandleMatchesGraph(entry.gBuffer1, provenance) &&
                       HandleMatchesGraph(entry.gBuffer2, provenance) &&
                       HandleMatchesGraph(entry.gBuffer3, provenance) &&
                       HandleMatchesGraph(entry.materialTable, provenance);
            }
            else if constexpr (std::is_same_v<Entry, SceneDepthSnapshot>)
            {
                return HandleMatchesGraph(entry.depthSnapshot, provenance);
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
            else if constexpr (std::is_same_v<Entry, LocalShadowOutputs>)
            {
                return entry.IsValid() &&
                       HandleMatchesGraph(entry.depthAtlas, provenance);
            }
            else if constexpr (std::is_same_v<Entry, LightingOutputs>)
            {
                return HandleMatchesGraph(entry.hdrSceneColor, provenance) &&
                       HandleMatchesGraph(entry.clusteredLights, provenance) &&
                       HandleMatchesGraph(
                           entry.baseIndirectSpecular, provenance) &&
                       HandleMatchesGraph(entry.specularResponse, provenance) &&
                       HandleMatchesGraph(entry.exposureState, provenance) &&
                       HandleMatchesGraph(entry.worldNormal, provenance) &&
                       HandleMatchesGraph(entry.materialIdentity, provenance) &&
                       HandleMatchesGraph(
                           entry.materialParameterTable, provenance);
            }
            else if constexpr (std::is_same_v<Entry, ReflectionOutputs>)
            {
                return HandleMatchesGraph(
                           entry.resolvedSceneColor, provenance) &&
                       HandleMatchesGraph(entry.worldNormal, provenance) &&
                       HandleMatchesGraph(entry.materialIdentity, provenance) &&
                       HandleMatchesGraph(
                           entry.materialParameterTable, provenance) &&
                       HandleMatchesGraph(entry.rawReflection, provenance) &&
                       HandleMatchesGraph(entry.denoisedReflection, provenance) &&
                       HandleMatchesGraph(entry.confidence, provenance);
            }
            else if constexpr (std::is_same_v<Entry,
                                              VolumetricLightingOutputs>)
            {
                return HandleMatchesGraph(entry.sourceSceneColor, provenance) &&
                       HandleMatchesGraph(entry.resolvedSceneColor, provenance) &&
                       HandleMatchesGraph(
                           entry.integratedScatteringTransmittance,
                           provenance) &&
                       HandleMatchesGraph(entry.worldNormal, provenance) &&
                       HandleMatchesGraph(entry.materialIdentity, provenance) &&
                       HandleMatchesGraph(
                           entry.materialParameterTable, provenance);
            }
            else if constexpr (std::is_same_v<Entry, PreTranslucencyOutputs>)
            {
                return HandleMatchesGraph(entry.hdrSceneColor, provenance) &&
                       HandleMatchesGraph(entry.deviceDepth, provenance) &&
                       HandleMatchesGraph(entry.worldNormal, provenance) &&
                       HandleMatchesGraph(entry.materialIdentity, provenance) &&
                       HandleMatchesGraph(
                           entry.materialParameterTable, provenance) &&
                       HandleMatchesGraph(entry.velocity, provenance) &&
                       HandleMatchesGraph(entry.reactiveMask, provenance) &&
                       HandleMatchesGraph(entry.opaqueSceneSnapshot, provenance) &&
                       HandleMatchesGraph(entry.exposureState, provenance) &&
                       HandleMatchesGraph(
                           entry.integratedScatteringTransmittance,
                           provenance);
            }
            else if constexpr (std::is_same_v<Entry, TranslucencyOutputs>)
            {
                return HandleMatchesGraph(entry.hdrSceneColor, provenance) &&
                       HandleMatchesGraph(entry.deviceDepth, provenance) &&
                       HandleMatchesGraph(entry.worldNormal, provenance) &&
                       HandleMatchesGraph(entry.materialIdentity, provenance) &&
                       HandleMatchesGraph(
                           entry.materialParameterTable, provenance) &&
                       HandleMatchesGraph(entry.velocity, provenance) &&
                       HandleMatchesGraph(entry.reactiveMask, provenance) &&
                       HandleMatchesGraph(entry.opaqueSceneSnapshot, provenance) &&
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

        [[nodiscard]] static bool TextureHandlesExactlyMatch(
            const RGTextureHandle& left,
            const RGTextureHandle& right) noexcept
        {
            return left.index == right.index &&
                   left.hasSubresourceRange == right.hasSubresourceRange &&
                   left.subresourceRange.baseMipLevel ==
                       right.subresourceRange.baseMipLevel &&
                   left.subresourceRange.mipLevelCount ==
                       right.subresourceRange.mipLevelCount &&
                   left.subresourceRange.baseArrayLayer ==
                       right.subresourceRange.baseArrayLayer &&
                   left.subresourceRange.arrayLayerCount ==
                       right.subresourceRange.arrayLayerCount &&
                   left.subresourceRange.aspect ==
                       right.subresourceRange.aspect &&
                   left.graphIdentity == right.graphIdentity &&
                   left.recordingGeneration == right.recordingGeneration;
        }

        [[nodiscard]] static bool HasValidVolumetricHistory(
            const RenderFrameIdentity& frameIdentity,
            const RenderViewIdentity& viewIdentity,
            const RenderViewHistoryIdentity& historyIdentity,
            RenderHistoryResetReason historyResetReasons,
            bool temporalHistoryUsed,
            RenderOutputExtent sceneExtent) noexcept
        {
            if (!historyIdentity.IsValid() ||
                historyIdentity.previousFrame != frameIdentity ||
                historyIdentity.view != viewIdentity ||
                historyIdentity.extent != sceneExtent)
            {
                return false;
            }

            // History can only be sampled when the sealed temporal key did
            // not request a reset. An identity route is allowed to carry a
            // compatible key without binding a history texture, but it must
            // report temporalHistoryUsed=false separately.
            return !temporalHistoryUsed ||
                   historyResetReasons == RenderHistoryResetReason::None;
        }

        [[nodiscard]] static bool HasValidVolumetricLightingOutputs(
            const VolumetricLightingOutputs& outputs,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            const RenderOutputExtent sceneExtent =
                outputs.resolvedSceneColorMetadata.extent;
            const bool validSceneSurface =
                outputs.sourceSceneColor.IsValid() &&
                outputs.resolvedSceneColor.IsValid() &&
                MetadataMatches(outputs.sourceSceneColorMetadata,
                                SceneTextures::HDRSceneColorContract) &&
                MetadataMatches(outputs.resolvedSceneColorMetadata,
                                SceneTextures::HDRSceneColorContract) &&
                outputs.sourceSceneColorMetadata ==
                    outputs.resolvedSceneColorMetadata;
            const bool validCommon =
                HasMatchingFrameViewProvenance(
                    outputs.frameIdentity, outputs.viewIdentity, provenance) &&
                outputs.lightingPlanIdentity != 0 &&
                outputs.volumetricPlanIdentity != 0 &&
                outputs.settingsHash != 0 && outputs.froxelGrid.IsValid() &&
                validSceneSurface &&
                outputs.froxelGrid.outputExtent == sceneExtent &&
                HasValidOptionalSurfaceGuides(outputs, sceneExtent) &&
                HasValidVolumetricHistory(
                    outputs.frameIdentity, outputs.viewIdentity,
                    outputs.historyIdentity, outputs.historyResetReasons,
                    outputs.temporalHistoryUsed, sceneExtent);
            if (!validCommon)
            {
                return false;
            }

            if (outputs.identityRoute)
            {
                return !outputs.temporalHistoryUsed &&
                       !outputs.integratedScatteringTransmittance.IsValid() &&
                       outputs.integratedScatteringTransmittanceMetadata
                           .IsEmpty() &&
                       TextureHandlesExactlyMatch(outputs.sourceSceneColor,
                                                  outputs.resolvedSceneColor);
            }

            return outputs.integratedScatteringTransmittance.IsValid() &&
                   outputs.integratedScatteringTransmittanceMetadata.IsValidFor(
                       outputs.froxelGrid);
        }

        [[nodiscard]] static bool HasValidPreTranslucencyVolumetricOutputs(
            const PreTranslucencyOutputs& outputs) noexcept
        {
            const RenderOutputExtent sceneExtent =
                outputs.hdrSceneColorMetadata.extent;
            if (outputs.volumetricPlanIdentity == 0 ||
                !outputs.froxelGrid.IsValid() ||
                outputs.froxelGrid.outputExtent != sceneExtent ||
                !HasValidVolumetricHistory(
                    outputs.frameIdentity, outputs.viewIdentity,
                    outputs.volumetricHistoryIdentity,
                    outputs.volumetricHistoryResetReasons,
                    outputs.volumetricTemporalHistoryUsed, sceneExtent))
            {
                return false;
            }

            if (outputs.volumetricIdentityRoute)
            {
                return !outputs.volumetricTemporalHistoryUsed &&
                       !outputs.integratedScatteringTransmittance.IsValid() &&
                       outputs.integratedScatteringTransmittanceMetadata
                           .IsEmpty();
            }
            return outputs.integratedScatteringTransmittance.IsValid() &&
                   outputs.integratedScatteringTransmittanceMetadata.IsValidFor(
                       outputs.froxelGrid);
        }

        [[nodiscard]] static bool HasValidPreTranslucencyOutputs(
            const PreTranslucencyOutputs& outputs,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            return HasMatchingFrameViewProvenance(
                       outputs.frameIdentity, outputs.viewIdentity,
                       provenance) &&
                   outputs.lightingPlanIdentity != 0 &&
                   outputs.hdrSceneColor.IsValid() &&
                   MetadataMatches(outputs.hdrSceneColorMetadata,
                                   SceneTextures::HDRSceneColorContract) &&
                   outputs.opaqueSceneSnapshot.IsValid() &&
                   MetadataMatches(outputs.opaqueSceneSnapshotMetadata,
                                   SceneTextures::HDRSceneColorContract) &&
                   outputs.opaqueSceneSnapshotMetadata.extent ==
                       outputs.hdrSceneColorMetadata.extent &&
                   outputs.opaqueSceneSnapshot.index !=
                       outputs.hdrSceneColor.index &&
                   outputs.opaqueSceneSnapshotReceipt != nullptr &&
                   outputs.colorDomain ==
                       PostProcessColorDomain::SceneLinearHDR &&
                   outputs.transfer == PostProcessTransfer::None &&
                   outputs.exposureState.IsValid() &&
                   outputs.exposureStateMetadata.IsValid() &&
                   HasValidPreTranslucencyVolumetricOutputs(outputs) &&
                   OptionalMetadataMatches(
                       outputs.deviceDepth, outputs.deviceDepthMetadata,
                       SceneTextures::DeviceDepthContract) &&
                   OptionalMetadataMatches(
                       outputs.worldNormal, outputs.worldNormalMetadata,
                       SceneTextures::FormatContract{
                           RHIFormat::RGBA8_UNORM,
                           RHISampleCount::Count1}) &&
                   HasValidTranslucencyOutlineInputs(outputs) &&
                   OptionalMetadataMatches(
                       outputs.velocity, outputs.velocityMetadata,
                       SceneTextures::VelocityContract) &&
                   OptionalMetadataMatches(
                       outputs.reactiveMask, outputs.reactiveMaskMetadata,
                       SceneTextures::ReactiveMaskContract);
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
                   outputs.opaqueSceneSnapshot.IsValid() &&
                   MetadataMatches(outputs.opaqueSceneSnapshotMetadata,
                                   SceneTextures::HDRSceneColorContract) &&
                   outputs.opaqueSceneSnapshotMetadata.extent ==
                       outputs.hdrSceneColorMetadata.extent &&
                   outputs.opaqueSceneSnapshot.index !=
                       outputs.hdrSceneColor.index &&
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
                   HasValidTranslucencyOutlineInputs(outputs) &&
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

        [[nodiscard]] static bool HasValidLocalShadowOutputs(
            const LocalShadowOutputs& outputs,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            return outputs.IsValid() &&
                   HasMatchingFrameViewProvenance(
                       outputs.frameIdentity, outputs.viewIdentity,
                       provenance);
        }

        template<typename Outputs>
        [[nodiscard]] static bool HasValidOptionalSurfaceGuides(
            const Outputs& outputs,
            RenderOutputExtent hdrExtent) noexcept
        {
            const bool hasNormal = outputs.worldNormal.IsValid();
            const bool hasMaterialIdentity = outputs.materialIdentity.IsValid();
            const bool hasMaterialTable =
                outputs.materialParameterTable.IsValid();
            if (!hasNormal && !hasMaterialIdentity && !hasMaterialTable)
            {
                return !outputs.worldNormalMetadata.extent.IsValid() &&
                       !outputs.materialIdentityMetadata.extent.IsValid() &&
                       !outputs.materialParameterTableMetadata.IsValid();
            }
            return hasNormal && hasMaterialIdentity && hasMaterialTable &&
                   MetadataMatches(
                       outputs.worldNormalMetadata,
                       SceneTextures::GBuffer1Contract) &&
                   MetadataMatches(
                       outputs.materialIdentityMetadata,
                       SceneTextures::GBuffer3Contract) &&
                   outputs.materialParameterTableMetadata.IsValid() &&
                   outputs.worldNormalMetadata.extent == hdrExtent &&
                   outputs.materialIdentityMetadata.extent == hdrExtent &&
                   outputs.materialParameterTableMetadata.sourceExtent ==
                       hdrExtent &&
                   outputs.materialParameterTableMetadata.sourceSampleCount ==
                       RHISampleCount::Count1;
        }

        [[nodiscard]] static bool HasValidLightingOutputs(
            const LightingOutputs& outputs,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            const bool hasReplacementBundle =
                outputs.baseIndirectSpecular.IsValid() ||
                outputs.specularResponse.IsValid() ||
                outputs.exposureState.IsValid() ||
                outputs.baseIndirectSpecularMetadata.extent.IsValid() ||
                outputs.specularResponseMetadata.extent.IsValid() ||
                outputs.exposureStateMetadata.IsValid() ||
                outputs.exposureStateContentHash != 0;
            const bool hasCompleteReplacementBundle =
                outputs.baseIndirectSpecular.IsValid() &&
                outputs.specularResponse.IsValid() &&
                outputs.exposureState.IsValid() &&
                outputs.exposureStateMetadata.IsValid() &&
                outputs.exposureStateContentHash != 0 &&
                MetadataMatches(
                    outputs.baseIndirectSpecularMetadata,
                    SceneTextures::BaseIndirectSpecularContract) &&
                MetadataMatches(outputs.specularResponseMetadata,
                                SceneTextures::SpecularResponseContract) &&
                outputs.baseIndirectSpecularMetadata.extent ==
                    outputs.hdrSceneColorMetadata.extent &&
                outputs.specularResponseMetadata.extent ==
                    outputs.hdrSceneColorMetadata.extent;
            const bool hasNoReplacementBundle =
                !outputs.baseIndirectSpecular.IsValid() &&
                !outputs.specularResponse.IsValid() &&
                !outputs.exposureState.IsValid() &&
                !outputs.baseIndirectSpecularMetadata.extent.IsValid() &&
                !outputs.specularResponseMetadata.extent.IsValid() &&
                !outputs.exposureStateMetadata.IsValid() &&
                outputs.exposureStateContentHash == 0;
            return HasMatchingFrameViewProvenance(
                       outputs.frameIdentity, outputs.viewIdentity,
                       provenance) &&
                   outputs.lightingPlanIdentity != 0 &&
                   outputs.hdrSceneColor.IsValid() &&
                   outputs.clusteredLights.IsValid() &&
                   MetadataMatches(outputs.hdrSceneColorMetadata,
                                   SceneTextures::HDRSceneColorContract) &&
                   (!hasReplacementBundle || hasCompleteReplacementBundle) &&
                   (hasCompleteReplacementBundle || hasNoReplacementBundle) &&
                   HasValidOptionalSurfaceGuides(
                       outputs, outputs.hdrSceneColorMetadata.extent);
        }

        [[nodiscard]] static bool HasValidReflectionOutputs(
            const ReflectionOutputs& outputs,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            return HasMatchingFrameViewProvenance(
                       outputs.frameIdentity, outputs.viewIdentity,
                       provenance) &&
                   outputs.lightingPlanIdentity != 0 &&
                   outputs.resolvedSceneColor.IsValid() &&
                   MetadataMatches(outputs.resolvedSceneColorMetadata,
                                   SceneTextures::HDRSceneColorContract) &&
                   outputs.imageBasedLightingPreserved &&
                   (!outputs.rayTracedReflectionsApplied ||
                    outputs.rayTracedReflectionsScheduled) &&
                   (!outputs.rayTracedReflectionsScheduled ||
                    outputs.hybridRayTracingPlanIdentity != 0) &&
                   HasValidOptionalSurfaceGuides(
                       outputs, outputs.resolvedSceneColorMetadata.extent);
        }

        template<typename Outputs>
        [[nodiscard]] static bool HasValidTranslucencyOutlineInputs(
            const Outputs& outputs) noexcept
        {
            const bool hasMaterialIdentity =
                outputs.materialIdentity.IsValid();
            const bool hasMaterialTable =
                outputs.materialParameterTable.IsValid();
            if (!hasMaterialIdentity && !hasMaterialTable)
            {
                // Pre-existing consumers can publish a normal guide without
                // opting into a material-identity surface. Once either half
                // of the outline pair is present, the full typed contract is
                // mandatory.
                return !outputs.materialIdentityMetadata.extent.IsValid() &&
                       !outputs.materialParameterTableMetadata.IsValid();
            }

            if (!outputs.worldNormal.IsValid() || !hasMaterialIdentity ||
                !hasMaterialTable ||
                !outputs.materialParameterTableMetadata.IsValid())
            {
                return false;
            }

            const RenderOutputExtent hdrExtent =
                outputs.hdrSceneColorMetadata.extent;
            return MetadataMatches(
                       outputs.worldNormalMetadata,
                       SceneTextures::FormatContract{
                           RHIFormat::RGBA8_UNORM,
                           RHISampleCount::Count1}) &&
                   MetadataMatches(outputs.materialIdentityMetadata,
                                   SceneTextures::GBuffer3Contract) &&
                   outputs.worldNormalMetadata.extent == hdrExtent &&
                   outputs.materialIdentityMetadata.extent == hdrExtent &&
                   outputs.materialParameterTableMetadata.sourceExtent ==
                       hdrExtent &&
                   outputs.materialParameterTableMetadata.sourceSampleCount ==
                       RHISampleCount::Count1;
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

        [[nodiscard]] static bool HasValidSceneDepthSnapshot(
            const SceneDepthSnapshot& snapshot,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            const bool hasAnyResourceState =
                snapshot.depthSnapshot.IsValid() ||
                snapshot.depthSnapshotMetadata.extent.IsValid() ||
                snapshot.depthSnapshotMetadata.format != RHIFormat::Unknown ||
                snapshot.depthSnapshotMetadata.sampleCount !=
                    RHISampleCount::Count1 ||
                snapshot.receipt != nullptr;
            if (!HasMatchingFrameViewProvenance(
                    snapshot.frameIdentity, snapshot.viewIdentity,
                    provenance) ||
                !snapshot.outputExtent.IsValid() ||
                (snapshot.required && !snapshot.requested))
            {
                return false;
            }

            if (snapshot.available)
            {
                return snapshot.requested && snapshot.depthSnapshot.IsValid() &&
                       MetadataMatches(snapshot.depthSnapshotMetadata,
                                       SceneDepthSnapshot::FormatContract) &&
                       snapshot.depthSnapshotMetadata.extent ==
                           snapshot.outputExtent &&
                       snapshot.receipt != nullptr;
            }

            // No request is a normal no-work contract. An unavailable optional
            // request is also valid, but it must remain explicitly empty so no
            // consumer can accidentally sample a stale or partial surface.
            return !hasAnyResourceState && (!snapshot.requested ||
                                            !snapshot.required);
        }

        template<typename Entry>
        [[nodiscard]] static bool EntryHasValidSemanticContract(
            const Entry& entry,
            const RenderGraphBlackboardProvenance& provenance) noexcept
        {
            if constexpr (std::is_same_v<Entry, SceneDepthSnapshot>)
            {
                return HasValidSceneDepthSnapshot(entry, provenance);
            }
            else if constexpr (std::is_same_v<Entry, PreTranslucencyOutputs>)
            {
                return HasValidPreTranslucencyOutputs(entry, provenance);
            }
            else if constexpr (std::is_same_v<Entry, TranslucencyOutputs>)
            {
                return HasValidTranslucencyOutputs(entry, provenance);
            }
            else if constexpr (std::is_same_v<Entry, LightingOutputs>)
            {
                return HasValidLightingOutputs(entry, provenance);
            }
            else if constexpr (std::is_same_v<Entry, LocalShadowOutputs>)
            {
                return HasValidLocalShadowOutputs(entry, provenance);
            }
            else if constexpr (std::is_same_v<Entry, ReflectionOutputs>)
            {
                return HasValidReflectionOutputs(entry, provenance);
            }
            else if constexpr (std::is_same_v<Entry,
                                              VolumetricLightingOutputs>)
            {
                return HasValidVolumetricLightingOutputs(entry, provenance);
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
