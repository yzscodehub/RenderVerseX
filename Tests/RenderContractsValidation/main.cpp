#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderContracts/RenderAmbientOcclusion.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderFrameValidation.h"
#include "RenderContracts/RenderExposure.h"
#include "RenderContracts/RenderFeatureChannelRegistry.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderMaterial.h"
#include "RenderContracts/RenderPipelineProfile.h"
#include "RenderContracts/RenderSceneUpdate.h"
#include "RenderContracts/ResourceUploadRequest.h"
#include "RenderContracts/TemporalJitter.h"
#include "ResourceUploadRequestInternal.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace RVX
{
namespace
{
    static_assert(!std::is_default_constructible_v<ResourceUploadRequest>);
    static_assert(!std::is_copy_constructible_v<ResourceUploadRequest>);
    static_assert(!std::is_move_constructible_v<ResourceUploadRequest>);
    static_assert(std::is_same_v<
                  decltype(EffectFrameTime{}.deterministicSeed),
                  uint32>);
    ResourceUploadRequestCreateInfo MakeValidMeshRequestInfo()
    {
        MeshUploadPayload payload;
        payload.createInfo.vertexCount = 3;
        payload.createInfo.boundsMin = Vec3(-1.0f);
        payload.createInfo.boundsMax = Vec3(1.0f);
        payload.bytes.resize(36, 7U);
        payload.positionRange = UploadByteRange{0, 36, 12};

        ResourceUploadRequestCreateInfo info;
        info.sequence = 1;
        info.assetId = AssetId{1};
        info.handle = RenderResourceHandle{1, 1};
        info.kind = RenderResourceKind::Mesh;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = 36;
        return info;
    }

    ResourceUploadRequestCreateInfo MakeValidTextureRequestInfo()
    {
        TextureUploadPayload payload;
        payload.createInfo.width = 1;
        payload.createInfo.height = 1;
        payload.createInfo.depth = 1;
        payload.createInfo.mipLevels = 1;
        payload.createInfo.arrayLayers = 1;
        payload.createInfo.format = TextureUploadFormat::RGBA8;
        payload.bytes = {11U, 22U, 33U, 44U};
        payload.subresources.push_back(
            TextureUploadSubresource{UploadByteRange{0, 4, 0}, 0, 0, 4, 4});

        ResourceUploadRequestCreateInfo info;
        info.sequence = 1;
        info.assetId = AssetId{1};
        info.handle = RenderResourceHandle{1, 1};
        info.kind = RenderResourceKind::Texture;
        info.payload = std::move(payload);
        info.declaredPayloadBytes =
            4 + static_cast<uint64>(sizeof(TextureUploadSubresource));
        return info;
    }

    ResourceUploadRequestCreateInfo MakeValidMaterialRequestInfo()
    {
        MaterialUploadPayload payload;

        ResourceUploadRequestCreateInfo info;
        info.sequence = 1;
        info.assetId = AssetId{1};
        info.handle = RenderResourceHandle{1, 1};
        info.kind = RenderResourceKind::Material;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = 0;
        return info;
    }

    TEST(RenderContractsValidation, UV1RangesAreValidatedAndRemainOptional)
    {
        auto validInfo = MakeValidMeshRequestInfo();
        auto& validPayload = std::get<MeshUploadPayload>(validInfo.payload);
        validPayload.bytes.resize(60U, 7U);
        validPayload.uv1Range = UploadByteRange{36U, 24U, 8U};
        validInfo.declaredPayloadBytes = 60U;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(validInfo)).code,
                  ResourceUploadRequestCreateCode::Created);

        auto invalidInfo = MakeValidMeshRequestInfo();
        auto& invalidPayload = std::get<MeshUploadPayload>(invalidInfo.payload);
        invalidPayload.bytes.resize(60U, 7U);
        invalidPayload.uv1Range = UploadByteRange{36U, 12U, 8U};
        invalidInfo.declaredPayloadBytes = 60U;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(invalidInfo)).code,
                  ResourceUploadRequestCreateCode::InvalidPayload);

        auto wrongStrideInfo = MakeValidMeshRequestInfo();
        auto& wrongStridePayload =
            std::get<MeshUploadPayload>(wrongStrideInfo.payload);
        wrongStridePayload.bytes.resize(60U, 7U);
        wrongStridePayload.uv1Range = UploadByteRange{36U, 24U, 4U};
        wrongStrideInfo.declaredPayloadBytes = 60U;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(wrongStrideInfo)).code,
                  ResourceUploadRequestCreateCode::InvalidPayload);

        auto shortRangeInfo = MakeValidMeshRequestInfo();
        auto& shortRangePayload =
            std::get<MeshUploadPayload>(shortRangeInfo.payload);
        shortRangePayload.bytes.resize(60U, 7U);
        shortRangePayload.uv1Range = UploadByteRange{36U, 23U, 8U};
        shortRangeInfo.declaredPayloadBytes = 60U;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(shortRangeInfo)).code,
                  ResourceUploadRequestCreateCode::InvalidPayload);

        auto longRangeInfo = MakeValidMeshRequestInfo();
        auto& longRangePayload =
            std::get<MeshUploadPayload>(longRangeInfo.payload);
        longRangePayload.bytes.resize(61U, 7U);
        longRangePayload.uv1Range = UploadByteRange{36U, 25U, 8U};
        longRangeInfo.declaredPayloadBytes = 61U;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(longRangeInfo)).code,
                  ResourceUploadRequestCreateCode::InvalidPayload);

        auto wrongUV0StrideInfo = MakeValidMeshRequestInfo();
        auto& wrongUV0StridePayload =
            std::get<MeshUploadPayload>(wrongUV0StrideInfo.payload);
        wrongUV0StridePayload.bytes.resize(60U, 7U);
        wrongUV0StridePayload.uvRange = UploadByteRange{36U, 24U, 4U};
        wrongUV0StrideInfo.declaredPayloadBytes = 60U;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(wrongUV0StrideInfo)).code,
                  ResourceUploadRequestCreateCode::InvalidPayload);

        auto overflowInfo = MakeValidMeshRequestInfo();
        auto& overflowPayload = std::get<MeshUploadPayload>(overflowInfo.payload);
        overflowPayload.createInfo.vertexCount = 1U;
        overflowPayload.bytes.resize(12U, 7U);
        overflowPayload.positionRange = UploadByteRange{0U, 12U, 12U};
        overflowPayload.uv1Range = UploadByteRange{
            std::numeric_limits<uint64>::max() - 7U, 8U, 8U};
        overflowInfo.declaredPayloadBytes = 12U;
        EXPECT_EQ(ResourceUploadRequest::Create(std::move(overflowInfo)).code,
                  ResourceUploadRequestCreateCode::PayloadRangeOverflow);
    }

    RenderViewSnapshot MakeValidView()
    {
        RenderViewSnapshot view;
        view.identity = {
            .sceneRuntimeId = 71,
            .cameraEntityPacked = 0x0000000700000004ULL,
            .subViewIndex = 0,
            .role = ViewRole::Main,
        };
        view.cameraCutRevision = 1;
        view.viewportWidth = 1;
        view.viewportHeight = 1;
        return view;
    }

    RenderFrameHeaderV5 MakeValidFrameHeader(uint64 sequence = 1,
                                             uint64 sceneRevision = 1)
    {
        RenderFrameHeaderV5 header;
        header.sequence = sequence;
        header.requiredSceneRevision = sceneRevision;
        header.worldRevision = 1;
        header.temporalEpoch = 1;
        header.viewFamilyId = 1;
        header.surfaceGeneration = 1;
        header.configurationRevision = 1;
        header.formatRevision = 1;
        header.simulationTick = sequence - 1;
        header.effectFixedDeltaSeconds = 1.0f / 60.0f;
        header.effectSimulationSeconds =
            static_cast<float64>(sequence - 1) / 60.0;
        header.deterministicSeed =
            0x9e3779b9U ^ static_cast<uint32>(sequence);
        header.outputWidth = 1;
        header.outputHeight = 1;
        return header;
    }

    RenderExtractionDiagnostics MakeCompleteDiagnostics()
    {
        RenderExtractionDiagnostics diagnostics;
        diagnostics.complete = true;
        return diagnostics;
    }

    void ExpectCreationCode(const ResourceUploadRequestCreateInfo& info,
                            ResourceUploadRequestCreateCode expected)
    {
        const ResourceUploadRequestCreateResult result =
            ResourceUploadRequest::Create(info);
        EXPECT_EQ(result.code, expected);
        EXPECT_EQ(result.request != nullptr,
                  expected == ResourceUploadRequestCreateCode::Created);
    }

    TEST(RenderContractsValidation, ZeroIdentityValuesAreInvalid)
    {
        EXPECT_FALSE(AssetId{}.IsValid());
        EXPECT_FALSE(RenderResourceHandle{}.IsValid());
        EXPECT_FALSE((RenderResourceHandle{1, 0}.IsValid()));
        EXPECT_TRUE((RenderResourceHandle{1, 1}.IsValid()));
    }

    TEST(RenderContractsValidation, IdentityHashersAreDeterministic)
    {
        const AssetId asset{42};
        const RenderResourceHandle handle{0x10203040U, 0x50607080U};
        const uint64 packed =
            (static_cast<uint64>(handle.slot) << 32U) |
            static_cast<uint64>(handle.generation);

        EXPECT_EQ(AssetIdHash{}(asset), AssetIdHash{}(asset));
        EXPECT_EQ(RenderResourceHandleHash{}(handle),
                  RenderResourceHandleHash{}(handle));
        EXPECT_EQ(RenderResourceHandleHash{}(handle),
                  std::hash<uint64>{}(packed));
    }

    TEST(RenderContractsValidation,
         LightCanonicalizationUsesOnlyExplicitPhotometricUnits)
    {
        constexpr float32 pi = 3.14159265358979323846f;

        RenderLightSnapshot directional;
        directional.type = RenderLightType::Directional;
        directional.intensity = 120000.0f;
        directional.intensityUnit = RenderLightIntensityUnit::Lux;
        directional.colorTemperatureKelvin = 6500.0f;
        const RenderLightCanonicalizationResult luxDirectional =
            CanonicalizeRenderLightSnapshot(directional);
        ASSERT_TRUE(luxDirectional.Succeeded());
        EXPECT_EQ(luxDirectional.canonicalUnit,
                  RenderLightIntensityUnit::Lux);
        EXPECT_FLOAT_EQ(luxDirectional.canonicalIntensity, 120000.0f);
        ASSERT_TRUE(luxDirectional.colorTemperatureKelvin.has_value());
        EXPECT_FLOAT_EQ(*luxDirectional.colorTemperatureKelvin, 6500.0f);

        RenderLightSnapshot point;
        point.type = RenderLightType::Point;
        point.intensityUnit = RenderLightIntensityUnit::Lumen;
        point.intensity = 4.0f * pi;
        const RenderLightCanonicalizationResult lumenPoint =
            CanonicalizeRenderLightSnapshot(point);
        ASSERT_TRUE(lumenPoint.Succeeded());
        EXPECT_EQ(lumenPoint.canonicalUnit,
                  RenderLightIntensityUnit::Candela);
        EXPECT_NEAR(lumenPoint.canonicalIntensity, 1.0f, 1.0e-5f);

        RenderLightSnapshot spot;
        spot.type = RenderLightType::Spot;
        spot.innerConeRadians = pi / 6.0f;
        spot.outerConeRadians = pi / 3.0f;
        spot.intensityUnit = RenderLightIntensityUnit::Lumen;
        // For a pi/3 half-angle, omega is pi steradians.
        spot.intensity = pi;
        const RenderLightCanonicalizationResult lumenSpot =
            CanonicalizeRenderLightSnapshot(spot);
        ASSERT_TRUE(lumenSpot.Succeeded());
        EXPECT_EQ(lumenSpot.canonicalUnit,
                  RenderLightIntensityUnit::Candela);
        EXPECT_NEAR(lumenSpot.canonicalIntensity, 1.0f, 1.0e-5f);
    }

    TEST(RenderContractsValidation,
         LightCanonicalizationRejectsInvalidUnitsConesAndTemperatures)
    {
        RenderLightSnapshot unspecified;
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(unspecified).code,
                  RenderLightCanonicalizationCode::UnspecifiedIntensityUnit);

        RenderLightSnapshot directional;
        directional.type = RenderLightType::Directional;
        directional.intensityUnit = RenderLightIntensityUnit::Lumen;
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(directional).code,
                  RenderLightCanonicalizationCode::IncompatibleIntensityUnit);

        RenderLightSnapshot point;
        point.type = RenderLightType::Point;
        point.intensityUnit = RenderLightIntensityUnit::Lux;
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(point).code,
                  RenderLightCanonicalizationCode::IncompatibleIntensityUnit);

        RenderLightSnapshot spot;
        spot.type = RenderLightType::Spot;
        spot.intensityUnit = RenderLightIntensityUnit::Candela;
        spot.outerConeRadians = 0.0f;
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(spot).code,
                  RenderLightCanonicalizationCode::InvalidSpotCone);
        spot.outerConeRadians = 0.5f;
        spot.innerConeRadians = 0.75f;
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(spot).code,
                  RenderLightCanonicalizationCode::InvalidSpotCone);

        RenderLightSnapshot negativeIntensity;
        negativeIntensity.intensityUnit = RenderLightIntensityUnit::Lux;
        negativeIntensity.intensity = -0.1f;
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(negativeIntensity).code,
                  RenderLightCanonicalizationCode::InvalidIntensity);
        negativeIntensity.intensity =
            std::numeric_limits<float32>::infinity();
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(negativeIntensity).code,
                  RenderLightCanonicalizationCode::InvalidIntensity);

        RenderLightSnapshot invalidTemperature;
        invalidTemperature.intensityUnit = RenderLightIntensityUnit::Lux;
        invalidTemperature.colorTemperatureKelvin = 999.0f;
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(invalidTemperature).code,
                  RenderLightCanonicalizationCode::InvalidColorTemperature);
        invalidTemperature.colorTemperatureKelvin =
            std::numeric_limits<float32>::quiet_NaN();
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(invalidTemperature).code,
                  RenderLightCanonicalizationCode::InvalidColorTemperature);

        RenderLightSnapshot invalidUnit;
        invalidUnit.intensityUnit =
            static_cast<RenderLightIntensityUnit>(255U);
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(invalidUnit).code,
                  RenderLightCanonicalizationCode::InvalidIntensityUnit);
        RenderLightSnapshot invalidType;
        invalidType.intensityUnit = RenderLightIntensityUnit::Lux;
        invalidType.type = static_cast<RenderLightType>(255U);
        EXPECT_EQ(CanonicalizeRenderLightSnapshot(invalidType).code,
                  RenderLightCanonicalizationCode::InvalidLightType);
    }

    TEST(RenderContractsValidation,
         LightSnapshotEqualityIncludesPhotometricAuthoringContract)
    {
        RenderLightSnapshot baseline;
        baseline.intensityUnit = RenderLightIntensityUnit::Lux;
        RenderLightSnapshot equal = baseline;
        EXPECT_TRUE(AreRenderLightSnapshotsEqual(baseline, equal));

        equal.intensityUnit = RenderLightIntensityUnit::Candela;
        EXPECT_FALSE(AreRenderLightSnapshotsEqual(baseline, equal));
        equal = baseline;
        equal.colorTemperatureKelvin = 5600.0f;
        EXPECT_FALSE(AreRenderLightSnapshotsEqual(baseline, equal));
    }

    TEST(RenderContractsValidation,
         LightMutationAccumulatorRejectsUnspecifiedAndIncompatibleUnits)
    {
        RenderLightSnapshot light;
        light.lightId = 71;
        light.source = {
            .sceneRuntimeId = 1,
            .entitySlot = 71,
            .entityGeneration = 0,
        };
        light.type = RenderLightType::Directional;

        RenderSceneMutationAccumulator accumulator;
        accumulator.Begin(0, true);
        EXPECT_FALSE(accumulator.UpsertLight(light, true));

        light.intensityUnit = RenderLightIntensityUnit::Lumen;
        EXPECT_FALSE(accumulator.UpsertLight(light, true));

        light.intensityUnit = RenderLightIntensityUnit::Lux;
        EXPECT_TRUE(accumulator.UpsertLight(light, true));
        EXPECT_TRUE(accumulator.Build(1).IsStructurallyValid());
    }

    TEST(RenderContractsValidation,
         PipelineProfileUsesStableMachineValuesAndNonZeroHashes)
    {
        static_assert(static_cast<uint8>(RenderPipelineRecipe::Minimal) == 0U);
        static_assert(static_cast<uint8>(RenderPipelineRecipe::ForwardStandard) ==
                      1U);
        static_assert(static_cast<uint8>(RenderPipelineRecipe::DeferredQuality) ==
                      2U);
        static_assert(static_cast<uint8>(RenderPipelineRecipe::HybridRayTracing) ==
                      3U);
        static_assert(static_cast<uint8>(RenderPipelineRecipe::Headless) == 4U);
        static_assert(static_cast<uint8>(RenderQualityTier::Low) == 0U);
        static_assert(static_cast<uint8>(RenderQualityTier::Medium) == 1U);
        static_assert(static_cast<uint8>(RenderQualityTier::High) == 2U);
        static_assert(static_cast<uint8>(RenderQualityTier::Ultra) == 3U);

        constexpr RenderPipelineRecipe recipes[]{
            RenderPipelineRecipe::Minimal,
            RenderPipelineRecipe::ForwardStandard,
            RenderPipelineRecipe::DeferredQuality,
            RenderPipelineRecipe::HybridRayTracing,
            RenderPipelineRecipe::Headless,
        };
        constexpr RenderQualityTier tiers[]{
            RenderQualityTier::Low,
            RenderQualityTier::Medium,
            RenderQualityTier::High,
            RenderQualityTier::Ultra,
        };

        for (RenderPipelineRecipe recipe : recipes)
        {
            EXPECT_TRUE(IsDeclaredRenderPipelineRecipe(recipe));
            RenderPipelineRecipe parsed = RenderPipelineRecipe::Minimal;
            EXPECT_TRUE(ParseRenderPipelineRecipe(
                GetRenderPipelineRecipeName(recipe), parsed));
            EXPECT_EQ(parsed, recipe);
            for (RenderQualityTier tier : tiers)
            {
                const uint64 hash = ComputeRenderPipelineProfileHash(recipe, tier);
                EXPECT_NE(hash, 0U);
                EXPECT_EQ(hash, ComputeRenderPipelineProfileHash(recipe, tier));
            }
        }
        for (RenderQualityTier tier : tiers)
        {
            EXPECT_TRUE(IsDeclaredRenderQualityTier(tier));
            RenderQualityTier parsed = RenderQualityTier::Low;
            EXPECT_TRUE(ParseRenderQualityTier(GetRenderQualityTierName(tier),
                                               parsed));
            EXPECT_EQ(parsed, tier);
        }
        RenderPipelineRecipe invalidRecipeParse =
            RenderPipelineRecipe::ForwardStandard;
        RenderQualityTier invalidTierParse = RenderQualityTier::Medium;
        EXPECT_FALSE(ParseRenderPipelineRecipe("forward", invalidRecipeParse));
        EXPECT_FALSE(ParseRenderQualityTier("default", invalidTierParse));
        EXPECT_EQ(ComputeRenderPipelineProfileHash(
                      static_cast<RenderPipelineRecipe>(99U),
                      RenderQualityTier::Low),
                  0U);
    }

    TEST(RenderContractsValidation,
         PipelineRequestsAndResolutionsFailClosedAndKeepForwardCompatibility)
    {
        const RenderPipelineRequest defaultRequest;
        EXPECT_TRUE(IsValidRenderPipelineRequest(defaultRequest));
        EXPECT_EQ(defaultRequest.recipe, RenderPipelineRecipe::ForwardStandard);
        EXPECT_EQ(defaultRequest.quality, RenderQualityTier::Medium);

        const RenderPipelineResolution resolved =
            ResolveRenderPipelineRequest(defaultRequest);
        EXPECT_TRUE(resolved.IsSuccessful());
        EXPECT_EQ(resolved.code, RenderPipelineResolutionCode::Resolved);
        EXPECT_TRUE(IsValidRenderPipelineResolution(resolved));
        EXPECT_EQ(resolved.resolvedRecipe, defaultRequest.recipe);
        EXPECT_EQ(resolved.resolvedQuality, defaultRequest.quality);
        EXPECT_NE(resolved.profileHash, 0U);

        RenderPipelineRequest invalidRecipe = defaultRequest;
        invalidRecipe.recipe = static_cast<RenderPipelineRecipe>(99U);
        EXPECT_FALSE(IsValidRenderPipelineRequest(invalidRecipe));
        const RenderPipelineResolution rejected =
            ResolveRenderPipelineRequest(invalidRecipe);
        EXPECT_FALSE(rejected.IsSuccessful());
        EXPECT_EQ(rejected.code, RenderPipelineResolutionCode::Rejected);
        EXPECT_EQ(rejected.profileHash, 0U);
        EXPECT_TRUE(IsValidRenderPipelineResolution(rejected));

        RenderPipelineRequest invalidSchema = defaultRequest;
        ++invalidSchema.schemaVersion;
        EXPECT_FALSE(IsValidRenderPipelineRequest(invalidSchema));

        RenderPipelineResolution fallback = resolved;
        fallback.code = RenderPipelineResolutionCode::Fallback;
        EXPECT_FALSE(IsValidRenderPipelineResolution(fallback));
        fallback.resolvedRecipe =
            RenderPipelineRecipe::DeferredQuality;
        fallback.profileHash = ComputeRenderPipelineProfileHash(
            fallback.resolvedRecipe, fallback.resolvedQuality);
        EXPECT_TRUE(IsValidRenderPipelineResolution(fallback));
    }

    TEST(RenderContractsValidation,
         FrameSettingsRejectInvalidPipelineRequest)
    {
        RenderFrameSettings settings;
        EXPECT_TRUE(IsValidRenderFrameSettings(settings));

        settings.pipelineRequest.quality =
            static_cast<RenderQualityTier>(99U);
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
    }

    TEST(RenderContractsValidation,
         PostProcessSettingsUseOneFailClosedAntiAliasingMode)
    {
        RenderFrameSettings settings;
        EXPECT_FALSE(settings.postProcess.enableVelocityOutput);
        EXPECT_EQ(settings.postProcess.antiAliasingMode,
                  RenderAntiAliasingMode::FXAA);
        EXPECT_TRUE(settings.postProcess.enableColorGrading);
        EXPECT_FALSE(settings.postProcess.enableToonOutline);
        EXPECT_FLOAT_EQ(settings.postProcess.toonOutlineDepthThreshold,
                        0.015f);
        EXPECT_FLOAT_EQ(settings.postProcess.toonOutlineNormalThreshold,
                        0.20f);
        EXPECT_FALSE(settings.postProcess.ambientOcclusion.enabled);
        EXPECT_TRUE(IsValidRenderAmbientOcclusionConsumerMask(
            settings.postProcess.ambientOcclusion.consumerMask));
        EXPECT_FALSE(settings.postProcess.enableSSR);
        EXPECT_TRUE(IsValidRenderFrameSettings(settings));

        settings.postProcess.enableVelocityOutput = true;
        EXPECT_TRUE(IsValidRenderFrameSettings(settings));

        settings.postProcess.antiAliasingMode = RenderAntiAliasingMode::TAA;
        EXPECT_TRUE(IsValidRenderFrameSettings(settings));

        settings.postProcess.enableVelocityOutput = false;
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
        settings.postProcess.enableVelocityOutput = true;

        settings.postProcess.antiAliasingMode =
            static_cast<RenderAntiAliasingMode>(255U);
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
        settings.postProcess.antiAliasingMode = RenderAntiAliasingMode::None;
        EXPECT_TRUE(IsValidRenderFrameSettings(settings));

        settings.postProcess.colorGradingContrast = -0.1f;
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
        settings.postProcess.colorGradingContrast = 1.0f;

        settings.postProcess.colorGradingSaturation = -0.1f;
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
        settings.postProcess.colorGradingSaturation = 1.0f;

        settings.postProcess.colorGradingBrightness =
            std::numeric_limits<float32>::quiet_NaN();
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
        settings.postProcess.colorGradingBrightness = 0.0f;

        settings.postProcess.colorGradingContrast =
            std::numeric_limits<float32>::infinity();
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
        settings.postProcess.colorGradingContrast = 1.0f;

        settings.postProcess.colorGradingSaturation =
            std::numeric_limits<float32>::quiet_NaN();
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
        settings.postProcess.colorGradingSaturation = 1.0f;

        settings.postProcess.toonOutlineDepthThreshold = 0.0f;
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
        settings.postProcess.toonOutlineDepthThreshold = 0.015f;

        settings.postProcess.toonOutlineNormalThreshold = 2.01f;
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
        settings.postProcess.toonOutlineNormalThreshold = 0.20f;

        EXPECT_TRUE(IsValidRenderFrameSettings(settings));
    }

    TEST(RenderContractsValidation,
         AmbientOcclusionSettingsAndReferenceMathFailClosed)
    {
        RenderAmbientOcclusionSettings settings;
        EXPECT_TRUE(IsValidRenderAmbientOcclusionSettings(settings));
        const uint64 settingsHash =
            CalculateRenderAmbientOcclusionSettingsHash(settings);
        EXPECT_NE(settingsHash, 0U);
        EXPECT_EQ(settingsHash,
                  CalculateRenderAmbientOcclusionSettingsHash(settings));

        const auto expectDistinctValidHash =
            [settingsHash](const RenderAmbientOcclusionSettings& changed)
        {
            ASSERT_TRUE(IsValidRenderAmbientOcclusionSettings(changed));
            const uint64 changedHash =
                CalculateRenderAmbientOcclusionSettingsHash(changed);
            EXPECT_NE(changedHash, 0U);
            EXPECT_NE(changedHash, settingsHash);
        };
        RenderAmbientOcclusionSettings changed = settings;
        changed.enabled = !settings.enabled;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.radiusMeters = 2.0f;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.falloffStartMeters = 0.2f;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.falloffEndMeters = 4.0f;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.thicknessMeters = 0.25f;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.directionCount = 16;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.stepCount = 8;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.spatialRadius = 4;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.bilateralDepthSigma = 2.0f;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.bilateralNormalThreshold = 0.7f;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.temporalBlend = 0.8f;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.temporalDepthThresholdMeters = 0.1f;
        expectDistinctValidHash(changed);
        changed = settings;
        changed.temporalNormalThreshold = 0.75f;
        expectDistinctValidHash(changed);

        RenderAmbientOcclusionSettings invalid = settings;
        invalid.radiusMeters = 0.0f;
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.falloffEndMeters = invalid.falloffStartMeters;
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.thicknessMeters =
            std::numeric_limits<float32>::quiet_NaN();
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.directionCount = 0;
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.stepCount = RVX_RENDER_AMBIENT_OCCLUSION_MAX_STEP_COUNT + 1;
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid.directionCount =
            RVX_RENDER_AMBIENT_OCCLUSION_MAX_DIRECTION_COUNT;
        invalid.stepCount = RVX_RENDER_AMBIENT_OCCLUSION_MAX_STEP_COUNT;
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid.directionCount = 32;
        invalid.stepCount = 8;
        EXPECT_TRUE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.spatialRadius = 0;
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.bilateralDepthSigma = 0.0f;
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.bilateralNormalThreshold = 1.0f;
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.temporalBlend = 1.0f;
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.temporalDepthThresholdMeters =
            std::numeric_limits<float32>::infinity();
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        invalid = settings;
        invalid.consumerMask = ToMask(RenderAmbientOcclusionConsumer::IBL);
        EXPECT_FALSE(IsValidRenderAmbientOcclusionSettings(invalid));
        EXPECT_EQ(CalculateRenderAmbientOcclusionSettingsHash(invalid), 0U);

        float32 linearDepth = 0.0f;
        EXPECT_TRUE(TryLinearizeRenderAmbientOcclusionDepth(
            0.0f, 0.1f, 100.0f, linearDepth));
        EXPECT_NEAR(linearDepth, 0.1f, 1.0e-5f);
        EXPECT_TRUE(TryLinearizeRenderAmbientOcclusionDepth(
            1.0f, 0.1f, 100.0f, linearDepth));
        EXPECT_NEAR(linearDepth, 100.0f, 1.0e-3f);
        EXPECT_FALSE(TryLinearizeRenderAmbientOcclusionDepth(
            -0.01f, 0.1f, 100.0f, linearDepth));
        EXPECT_FALSE(TryLinearizeRenderAmbientOcclusionDepth(
            0.5f, 1.0f, 1.0f, linearDepth));

        float32 openVisibility = 0.0f;
        float32 cornerVisibility = 0.0f;
        ASSERT_TRUE(TryCalculateRenderAmbientOcclusionHorizonVisibility(
            3.0f, 3.0f, 0.5f, settings, openVisibility));
        ASSERT_TRUE(TryCalculateRenderAmbientOcclusionHorizonVisibility(
            3.0f, 1.0f, 0.5f, settings, cornerVisibility));
        EXPECT_FLOAT_EQ(openVisibility, 1.0f);
        EXPECT_LT(cornerVisibility, openVisibility);

        float32 continuousWeight = 0.0f;
        float32 depthDiscontinuityWeight = 0.0f;
        float32 normalDiscontinuityWeight = 0.0f;
        ASSERT_TRUE(TryCalculateRenderAmbientOcclusionBilateralWeight(
            3.0f, 3.0f, 1.0f, 1.0f, settings, continuousWeight));
        ASSERT_TRUE(TryCalculateRenderAmbientOcclusionBilateralWeight(
            3.0f, 12.0f, 1.0f, 1.0f, settings,
            depthDiscontinuityWeight));
        ASSERT_TRUE(TryCalculateRenderAmbientOcclusionBilateralWeight(
            3.0f, 3.0f, 0.0f, 1.0f, settings,
            normalDiscontinuityWeight));
        EXPECT_GT(continuousWeight, depthDiscontinuityWeight);
        EXPECT_FLOAT_EQ(normalDiscontinuityWeight, 0.0f);

        EXPECT_TRUE(IsRenderAmbientOcclusionTemporalHistoryAccepted(
            Vec3{0.0f, 0.0f, 3.0f}, Vec3{0.01f, 0.0f, 3.02f},
            0.9f, settings));
        EXPECT_FALSE(IsRenderAmbientOcclusionTemporalHistoryAccepted(
            Vec3{0.0f, 0.0f, 3.0f}, Vec3{0.0f, 0.0f, 3.2f},
            0.9f, settings));
        EXPECT_FALSE(IsRenderAmbientOcclusionTemporalHistoryAccepted(
            Vec3{0.0f, 0.0f, 3.0f}, Vec3{0.01f, 0.0f, 3.02f},
            0.5f, settings));
        EXPECT_FALSE(IsRenderAmbientOcclusionTemporalHistoryAccepted(
            Vec3{0.0f, 0.0f, 3.0f}, Vec3{0.2f, 0.0f, 3.0f},
            0.9f, settings));
        EXPECT_FALSE(IsRenderAmbientOcclusionTemporalHistoryAccepted(
            Vec3{0.0f, 0.0f, 3.0f}, Vec3{0.0f, 0.0f, 3.0f},
            -0.9f, settings));
        float32 clampedVisibility = 0.0f;
        EXPECT_TRUE(TryClampRenderAmbientOcclusionTemporalHistory(
            0.1f, 0.4f, 0.8f, clampedVisibility));
        EXPECT_FLOAT_EQ(clampedVisibility, 0.4f);
        EXPECT_FALSE(TryClampRenderAmbientOcclusionTemporalHistory(
            0.1f, 0.8f, 0.4f, clampedVisibility));

        const RenderAmbientOcclusionIndirectLighting input{
            .directDiffuse = 3.0f,
            .directSpecular = 5.0f,
            .indirectDiffuse = 7.0f,
            .indirectSpecular = 11.0f,
            .iblDiffuse = 13.0f,
            .iblSpecular = 17.0f,
            .emissive = 19.0f,
            .sky = 23.0f};
        RenderAmbientOcclusionIndirectLighting composed;
        ASSERT_TRUE(TryComposeRenderAmbientOcclusionIndirectLighting(
            input, 0.5f,
            RVX_RENDER_AMBIENT_OCCLUSION_INDIRECT_CONSUMER_MASK,
            composed));
        EXPECT_FLOAT_EQ(composed.directDiffuse, input.directDiffuse);
        EXPECT_FLOAT_EQ(composed.directSpecular, input.directSpecular);
        EXPECT_FLOAT_EQ(composed.indirectDiffuse, 3.5f);
        EXPECT_FLOAT_EQ(composed.indirectSpecular, 5.5f);
        EXPECT_FLOAT_EQ(composed.iblDiffuse, 6.5f);
        EXPECT_FLOAT_EQ(composed.iblSpecular, 8.5f);
        EXPECT_FLOAT_EQ(composed.emissive, input.emissive);
        EXPECT_FLOAT_EQ(composed.sky, input.sky);
        EXPECT_FALSE(TryComposeRenderAmbientOcclusionIndirectLighting(
            input, 0.5f, ToMask(RenderAmbientOcclusionConsumer::IBL),
            composed));
    }

    TEST(RenderContractsValidation,
         TemporalJitterUsesTheFixedEightPhaseHaltonSequenceInPixelSpace)
    {
        const RenderOutputExtent extent{.width = 320U, .height = 180U};
        const std::array<Vec2, RVX_RENDER_TEMPORAL_JITTER_PHASE_COUNT>
            expectedPixels = {
                Vec2{0.0f, -1.0f / 6.0f},
                Vec2{-0.25f, 1.0f / 6.0f},
                Vec2{0.25f, -7.0f / 18.0f},
                Vec2{-0.375f, -1.0f / 18.0f},
                Vec2{0.125f, 5.0f / 18.0f},
                Vec2{-0.125f, -5.0f / 18.0f},
                Vec2{0.375f, 1.0f / 18.0f},
                Vec2{-0.4375f, 7.0f / 18.0f},
            };

        for (uint32 phase = 0U;
             phase < RVX_RENDER_TEMPORAL_JITTER_PHASE_COUNT;
             ++phase)
        {
            const RenderTemporalJitter jitter =
                MakeRenderTemporalJitterForPhase(phase, extent);
            EXPECT_TRUE(IsValidRenderTemporalJitter(jitter));
            EXPECT_EQ(jitter.phaseIndex, phase);
            EXPECT_NEAR(jitter.pixelOffset.x, expectedPixels[phase].x, 1.0e-6f);
            EXPECT_NEAR(jitter.pixelOffset.y, expectedPixels[phase].y, 1.0e-6f);
            EXPECT_NEAR(jitter.ndcOffset.x,
                        2.0f * expectedPixels[phase].x / 320.0f,
                        1.0e-7f);
            EXPECT_NEAR(jitter.ndcOffset.y,
                        2.0f * expectedPixels[phase].y / 180.0f,
                        1.0e-7f);
            EXPECT_EQ(MakeRenderTemporalJitter(static_cast<uint64>(phase) + 1U,
                                                extent),
                      jitter);
        }

        EXPECT_EQ(MakeRenderTemporalJitter(9U, extent),
                  MakeRenderTemporalJitter(1U, extent));
        EXPECT_FALSE(IsValidRenderTemporalJitter({}));
        EXPECT_FALSE(IsValidRenderTemporalJitter(
            MakeRenderTemporalJitterForPhase(
                RVX_RENDER_TEMPORAL_JITTER_PHASE_COUNT, extent)));
        EXPECT_FALSE(IsValidRenderTemporalJitter(
            MakeRenderTemporalJitter(1U, RenderOutputExtent{})));
    }

    TEST(RenderContractsValidation,
         TemporalJitterDoesNotMutateTheCanonicalProjectionMatrix)
    {
        const RenderOutputExtent extent{.width = 640U, .height = 360U};
        const RenderTemporalJitter jitter =
            MakeRenderTemporalJitterForPhase(1U, extent);
        Mat4 canonical = Mat4Identity();
        canonical[0][0] = 2.0f;
        canonical[1][1] = 3.0f;

        const Mat4 jittered = ApplyRenderTemporalJitter(canonical, jitter);
        EXPECT_EQ(canonical[2][0], 0.0f);
        EXPECT_EQ(canonical[2][1], 0.0f);
        EXPECT_FLOAT_EQ(jittered[2][0], jitter.ndcOffset.x);
        EXPECT_FLOAT_EQ(jittered[2][1], jitter.ndcOffset.y);
        EXPECT_EQ(jittered[0][0], canonical[0][0]);
        EXPECT_EQ(jittered[1][1], canonical[1][1]);

        RenderTemporalJitter malformed = jitter;
        malformed.ndcOffset.x += 0.001f;
        EXPECT_FALSE(IsValidRenderTemporalJitter(malformed));
        EXPECT_EQ(ApplyRenderTemporalJitter(canonical, malformed), canonical);
    }

    TEST(RenderContractsValidation,
         FeatureChannelCatalogHasStableTypedEntries)
    {
        static_assert(RVX_RENDER_FEATURE_CHANNEL_COUNT == 3);
        static_assert(std::is_same_v<
                      ParticleRenderSnapshotItem::ChannelDescriptor,
                      std::tuple_element_t<0, RenderFeatureChannelCatalog>>);
        static_assert(std::is_same_v<
                      WaterRenderSnapshotItem::ChannelDescriptor,
                      std::tuple_element_t<1, RenderFeatureChannelCatalog>>);
        static_assert(std::is_same_v<
                      TerrainRenderSnapshotItem::ChannelDescriptor,
                      std::tuple_element_t<2, RenderFeatureChannelCatalog>>);

        EXPECT_EQ(static_cast<uint32>(RenderFeatureChannelId::Particle), 1U);
        EXPECT_EQ(static_cast<uint32>(RenderFeatureChannelId::Water), 2U);
        EXPECT_EQ(static_cast<uint32>(RenderFeatureChannelId::Terrain), 3U);
        EXPECT_FALSE(IsKnownRenderFeatureChannel(RenderFeatureChannelId::Invalid));
        EXPECT_EQ(GetRenderFeatureChannelSchemaVersion(
                      static_cast<RenderFeatureChannelId>(999U)),
                  0U);
    }

    TEST(RenderContractsValidation,
         FeatureChannelRegistryRequiresCompleteUniqueRoutesBeforeSeal)
    {
        const auto registrationFor = [](RenderFeatureChannelId channelId)
        {
            return RenderFeatureChannelRegistration{
                .channelId = channelId,
                .schemaVersion =
                    GetRenderFeatureChannelSchemaVersion(channelId),
                .producerEndpoint =
                    RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore,
                .consumerEndpoint =
                    RenderFeatureConsumerEndpointId::RenderSceneDatabase,
            };
        };

        RenderFeatureChannelRegistry incomplete;
        EXPECT_EQ(incomplete.Register(
                      registrationFor(RenderFeatureChannelId::Particle)),
                  RenderFeatureChannelRegistryCode::Success);
        EXPECT_EQ(incomplete.Register(
                      registrationFor(RenderFeatureChannelId::Water)),
                  RenderFeatureChannelRegistryCode::Success);
        EXPECT_EQ(incomplete.Seal(),
                  RenderFeatureChannelRegistryCode::MissingChannel);
        EXPECT_FALSE(incomplete.IsSealed());

        RenderFeatureChannelRegistry duplicate;
        const RenderFeatureChannelRegistration particle =
            registrationFor(RenderFeatureChannelId::Particle);
        EXPECT_EQ(duplicate.Register(particle),
                  RenderFeatureChannelRegistryCode::Success);
        EXPECT_EQ(duplicate.Register(particle),
                  RenderFeatureChannelRegistryCode::DuplicateChannel);

        RenderFeatureChannelRegistry invalid;
        RenderFeatureChannelRegistration bad = particle;
        ++bad.schemaVersion;
        EXPECT_EQ(invalid.Register(bad),
                  RenderFeatureChannelRegistryCode::SchemaMismatch);
        bad = particle;
        bad.producerEndpoint = RenderFeatureProducerEndpointId::Invalid;
        EXPECT_EQ(invalid.Register(bad),
                  RenderFeatureChannelRegistryCode::InvalidProducerEndpoint);
        bad = particle;
        bad.consumerEndpoint = RenderFeatureConsumerEndpointId::Invalid;
        EXPECT_EQ(invalid.Register(bad),
                  RenderFeatureChannelRegistryCode::InvalidConsumerEndpoint);
        bad = particle;
        bad.channelId = static_cast<RenderFeatureChannelId>(999U);
        EXPECT_EQ(invalid.Register(bad),
                  RenderFeatureChannelRegistryCode::UnknownChannel);
        EXPECT_EQ(invalid.GetRegistrationCount(), 0U);
    }

    TEST(RenderContractsValidation,
         BuiltinFeatureChannelRegistrySealsExactProducerConsumerRoutes)
    {
        RenderFeatureChannelRegistry registry =
            CreateBuiltinRenderFeatureChannelRegistry();
        ASSERT_TRUE(registry.IsSealed());
        EXPECT_EQ(registry.GetRegistrationCount(),
                  RVX_RENDER_FEATURE_CHANNEL_COUNT);

        constexpr RenderFeatureChannelId channels[]{
            RenderFeatureChannelId::Particle,
            RenderFeatureChannelId::Water,
            RenderFeatureChannelId::Terrain,
        };
        for (RenderFeatureChannelId channelId : channels)
        {
            const RenderFeatureChannelRegistration* route =
                registry.Find(channelId);
            ASSERT_NE(route, nullptr);
            EXPECT_EQ(route->schemaVersion,
                      GetRenderFeatureChannelSchemaVersion(channelId));
            EXPECT_EQ(route->producerEndpoint,
                      RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore);
            EXPECT_EQ(route->consumerEndpoint,
                      RenderFeatureConsumerEndpointId::RenderSceneDatabase);
            EXPECT_EQ(registry.ValidateRoute(
                          channelId,
                          route->schemaVersion,
                          route->producerEndpoint,
                          route->consumerEndpoint),
                      RenderFeatureChannelRegistryCode::Success);
        }

        EXPECT_EQ(registry.Seal(),
                  RenderFeatureChannelRegistryCode::RegistrySealed);
        EXPECT_EQ(registry.Register(*registry.Find(
                      RenderFeatureChannelId::Particle)),
                  RenderFeatureChannelRegistryCode::RegistrySealed);
        EXPECT_EQ(registry.ValidateProducer(
                      RenderFeatureChannelId::Particle,
                      RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION + 1U,
                      RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore),
                  RenderFeatureChannelRegistryCode::SchemaMismatch);
        EXPECT_EQ(registry.ValidateRoute(
                      RenderFeatureChannelId::Particle,
                      RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION,
                      RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore,
                      RenderFeatureConsumerEndpointId::Invalid),
                  RenderFeatureChannelRegistryCode::InvalidConsumerEndpoint);
    }

    TEST(RenderContractsValidation,
         TypedFeatureChannelsRejectUnknownSchemaSequenceAndSourceFailures)
    {
        const RenderFeatureSourceIdentity source{
            .sceneRuntimeId = 71,
            .entitySlot = 4,
            .entityGeneration = 3,
        };
        const RenderFeatureSourceIdentity foreignSource{
            .sceneRuntimeId = 72,
            .entitySlot = 4,
            .entityGeneration = 3,
        };

        const auto verify = [&]<typename Item>()
        {
            Item item;
            item.header.captureSequence = 9;
            item.header.configurationRevision = 5;
            item.header.source = source;
            EXPECT_EQ(item.ValidateChannel(source, 8),
                      RenderFeatureChannelValidationCode::Valid);
            EXPECT_EQ(item.header.configurationRevision, 5U);

            Item invalid = item;
            invalid.header.channelId = static_cast<RenderFeatureChannelId>(999U);
            EXPECT_EQ(invalid.ValidateChannel(source),
                      RenderFeatureChannelValidationCode::UnknownChannel);

            invalid = item;
            invalid.header.channelId = RenderFeatureChannelId::Invalid;
            EXPECT_EQ(invalid.ValidateChannel(source),
                      RenderFeatureChannelValidationCode::UnknownChannel);

            invalid = item;
            invalid.header.schemaVersion = Item::SchemaVersion + 1U;
            EXPECT_EQ(invalid.ValidateChannel(source),
                      RenderFeatureChannelValidationCode::SchemaMismatch);

            invalid = item;
            invalid.header.captureSequence = 0;
            EXPECT_EQ(invalid.ValidateChannel(source),
                      RenderFeatureChannelValidationCode::ZeroCaptureSequence);

            invalid = item;
            EXPECT_EQ(invalid.ValidateChannel(source, 9),
                      RenderFeatureChannelValidationCode::StaleCaptureSequence);

            invalid = item;
            invalid.header.source = {};
            EXPECT_EQ(invalid.ValidateChannel(source),
                      RenderFeatureChannelValidationCode::InvalidSource);

            invalid = item;
            EXPECT_EQ(invalid.ValidateChannel(foreignSource),
                      RenderFeatureChannelValidationCode::SourceMismatch);
        };

        verify.template operator()<ParticleRenderSnapshotItem>();
        verify.template operator()<WaterRenderSnapshotItem>();
        verify.template operator()<TerrainRenderSnapshotItem>();
    }

    TEST(RenderContractsValidation, CreatesMesh)
    {
        const auto info = MakeValidMeshRequestInfo();
        const auto result = ResourceUploadRequest::Create(info);

        ASSERT_EQ(result.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_TRUE(result.request);
        EXPECT_EQ(result.request->GetSchemaId(), RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID);
        EXPECT_EQ(result.request->GetSchemaVersion(), RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION);
        EXPECT_EQ(result.request->GetSequence(), 1U);
        EXPECT_EQ(result.request->GetAssetId(), (AssetId{1}));
        EXPECT_EQ(result.request->GetHandle(), (RenderResourceHandle{1, 1}));
        EXPECT_EQ(result.request->GetKind(), RenderResourceKind::Mesh);
        EXPECT_TRUE(std::holds_alternative<MeshUploadPayload>(result.request->GetPayload()));
        EXPECT_EQ(result.request->GetDerivedPayloadBytes(), 36U);
        EXPECT_EQ(result.request->GetDeclaredPayloadBytes(), 36U);
        EXPECT_TRUE(result.request->GetDependencies().empty());
        EXPECT_EQ(result.request->GetDependencyReadiness(),
                  RenderDependencyReadiness::RequireAll);
        EXPECT_EQ(result.request->GetPriority(), RenderUploadPriority::Normal);
        EXPECT_EQ(result.request->GetProvenance().sourceRevision, 0U);
    }

    TEST(RenderContractsValidation, CreatesTexture)
    {
        const auto info = MakeValidTextureRequestInfo();
        const auto result = ResourceUploadRequest::Create(info);

        ASSERT_EQ(result.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_TRUE(result.request);
        EXPECT_EQ(result.request->GetKind(), RenderResourceKind::Texture);
        EXPECT_TRUE(std::holds_alternative<TextureUploadPayload>(result.request->GetPayload()));
        EXPECT_EQ(result.request->GetDerivedPayloadBytes(), info.declaredPayloadBytes);
        EXPECT_EQ(result.request->GetDeclaredPayloadBytes(), info.declaredPayloadBytes);
    }

    TEST(RenderContractsValidation, CreatesMaterial)
    {
        const auto info = MakeValidMaterialRequestInfo();
        const auto result = ResourceUploadRequest::Create(info);

        ASSERT_EQ(result.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_TRUE(result.request);
        EXPECT_EQ(result.request->GetKind(), RenderResourceKind::Material);
        EXPECT_TRUE(std::holds_alternative<MaterialUploadPayload>(result.request->GetPayload()));
        EXPECT_EQ(result.request->GetDerivedPayloadBytes(), 0U);
        EXPECT_EQ(result.request->GetDeclaredPayloadBytes(), 0U);
    }

    TEST(RenderContractsValidation,
         MaterialTextureDependenciesExactlyMatchBoundTextures)
    {
        auto valid = MakeValidMaterialRequestInfo();
        auto& payload = std::get<MaterialUploadPayload>(valid.payload);
        MaterialUploadTextureBinding binding;
        binding.texture = RenderResourceHandle{2, 1};
        payload.textureBindings.push_back(binding);
        payload.sourceData.textureFlags = 1U;
        payload.staticDescriptor.featureMask = 1U;
        valid.dependencies.push_back(binding.texture);
        valid.declaredPayloadBytes =
            sizeof(MaterialUploadTextureBinding) +
            sizeof(RenderResourceHandle);
        ExpectCreationCode(valid, ResourceUploadRequestCreateCode::Created);

        auto omitted = valid;
        omitted.dependencies.clear();
        omitted.declaredPayloadBytes = sizeof(MaterialUploadTextureBinding);
        ExpectCreationCode(
            omitted, ResourceUploadRequestCreateCode::InvalidDependency);

        auto extraneous = valid;
        extraneous.dependencies.push_back(RenderResourceHandle{3, 1});
        extraneous.declaredPayloadBytes += sizeof(RenderResourceHandle);
        ExpectCreationCode(
            extraneous, ResourceUploadRequestCreateCode::InvalidDependency);
    }

    TEST(RenderContractsValidation,
         MaterialUploadV4AcceptsEveryDeclaredLogicalTextureSlot)
    {
        auto info = MakeValidMaterialRequestInfo();
        auto& payload = std::get<MaterialUploadPayload>(info.payload);
        for (uint32 slot =
                 static_cast<uint32>(MaterialUploadTextureSlot::BaseColor);
             slot <= static_cast<uint32>(MaterialUploadTextureSlot::Thickness);
             ++slot)
        {
            MaterialUploadTextureBinding binding;
            binding.slot = static_cast<MaterialUploadTextureSlot>(slot);
            binding.texture = RenderResourceHandle{slot + 2u, 1u};
            binding.uvSet = static_cast<int32>(slot % 2u);
            payload.textureBindings.push_back(binding);
            payload.sourceData.textureFlags |= 1u << slot;
            payload.staticDescriptor.featureMask |= 1ull << slot;
            info.dependencies.push_back(binding.texture);
        }
        info.declaredPayloadBytes =
            static_cast<uint64>(payload.textureBindings.size()) *
                sizeof(MaterialUploadTextureBinding) +
            static_cast<uint64>(info.dependencies.size()) *
                sizeof(RenderResourceHandle);

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::Created);
    }

    TEST(RenderContractsValidation,
         CanonicalMaterialAllowsMirroredAndZeroTextureScale)
    {
        MaterialDefinition definition;
        MaterialTextureSlotDefinition slot;
        slot.slot = MaterialTextureSlot::Normal;
        slot.scale = Vec2{-1.0f, 1.0f};
        definition.textureSlots.push_back(slot);
        definition.descriptor.featureMask =
            1ull << static_cast<uint8>(slot.slot);
        definition.parameters.textureFlags =
            1u << static_cast<uint8>(slot.slot);

        std::string error;
        EXPECT_TRUE(ValidateMaterialDefinition(definition, error)) << error;
        definition.textureSlots.front().scale.x = 0.0f;
        EXPECT_TRUE(ValidateMaterialDefinition(definition, error)) << error;

        auto uploadInfo = MakeValidMaterialRequestInfo();
        auto& payload = std::get<MaterialUploadPayload>(uploadInfo.payload);
        MaterialUploadTextureBinding binding;
        binding.texture = RenderResourceHandle{2, 1};
        binding.scale = Vec2{0.0f, 1.0f};
        payload.textureBindings.push_back(binding);
        payload.sourceData.textureFlags = 1U;
        payload.staticDescriptor.featureMask = 1U;
        uploadInfo.dependencies.push_back(binding.texture);
        uploadInfo.declaredPayloadBytes =
            sizeof(MaterialUploadTextureBinding) +
            sizeof(RenderResourceHandle);
        ExpectCreationCode(
            uploadInfo, ResourceUploadRequestCreateCode::Created);
    }

    TEST(RenderContractsValidation, RejectsMaterialDescriptorLayoutMismatch)
    {
        auto info = MakeValidMaterialRequestInfo();
        auto& descriptor =
            std::get<MaterialUploadPayload>(info.payload).staticDescriptor;
        descriptor.parameterLayoutHash ^= 0x1ull;

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsMaterialDescriptorProgramMismatch)
    {
        auto info = MakeValidMaterialRequestInfo();
        std::get<MaterialUploadPayload>(info.payload)
            .staticDescriptor.programFamily = "wrong-family";

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsMaterialDescriptorAlphaMismatch)
    {
        auto info = MakeValidMaterialRequestInfo();
        std::get<MaterialUploadPayload>(info.payload)
            .staticDescriptor.blendMode = MaterialBlendMode::Masked;

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsMaterialDescriptorWorkflowMismatch)
    {
        auto info = MakeValidMaterialRequestInfo();
        auto& descriptor =
            std::get<MaterialUploadPayload>(info.payload).staticDescriptor;
        descriptor.shadingModel = MaterialShadingModel::Unlit;
        descriptor.programFamily =
            std::string(GetMaterialProgramFamily(descriptor.shadingModel));

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsMaterialDescriptorCullMismatch)
    {
        auto info = MakeValidMaterialRequestInfo();
        std::get<MaterialUploadPayload>(info.payload)
            .staticDescriptor.cullMode = MaterialCullMode::NoCull;

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation,
         RejectsMaterialTextureBindingOutsideUVSetContract)
    {
        auto info = MakeValidMaterialRequestInfo();
        auto& payload = std::get<MaterialUploadPayload>(info.payload);
        MaterialUploadTextureBinding binding;
        binding.texture = RenderResourceHandle{2, 1};
        binding.uvSet = 2;
        payload.textureBindings.push_back(binding);
        payload.sourceData.textureFlags = 1U;
        payload.staticDescriptor.featureMask = 1U;
        info.declaredPayloadBytes = sizeof(MaterialUploadTextureBinding);

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation,
         RejectsMaterialTextureBindingFeatureMaskMismatch)
    {
        auto info = MakeValidMaterialRequestInfo();
        auto& payload = std::get<MaterialUploadPayload>(info.payload);
        MaterialUploadTextureBinding binding;
        binding.texture = RenderResourceHandle{2, 1};
        payload.textureBindings.push_back(binding);
        payload.sourceData.textureFlags = 1U;
        info.declaredPayloadBytes = sizeof(MaterialUploadTextureBinding);

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);

        payload.staticDescriptor.featureMask = 1U;
        payload.sourceData.textureFlags = 0U;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);

        payload.textureBindings.clear();
        payload.sourceData.textureFlags = 0U;
        payload.staticDescriptor.featureMask =
            1ull << static_cast<uint32>(MaterialTextureSlot::ToonRamp);
        info.declaredPayloadBytes = 0U;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsSchema)
    {
        auto info = MakeValidTextureRequestInfo();
        info.schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION + 1U;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidSchema);
    }

    TEST(RenderContractsValidation, RejectsPreUV1AndMaterialDescriptorSchema)
    {
        auto info = MakeValidMeshRequestInfo();
        // Version 2 has neither the UV1 mesh-stream range nor the immutable
        // material descriptor contract. It must not cross the v3 boundary.
        info.schemaVersion = 2U;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidSchema);
    }

    TEST(RenderContractsValidation, RejectsZeroSequence)
    {
        auto info = MakeValidTextureRequestInfo();
        info.sequence = 0;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidSequence);
    }

    TEST(RenderContractsValidation, RejectsZeroAsset)
    {
        auto info = MakeValidTextureRequestInfo();
        info.assetId = {};
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidAsset);
    }

    TEST(RenderContractsValidation, RejectsZeroHandlePart)
    {
        auto info = MakeValidTextureRequestInfo();
        info.handle = RenderResourceHandle{1, 0};
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidHandle);
    }

    TEST(RenderContractsValidation, RejectsInvalidKind)
    {
        auto info = MakeValidTextureRequestInfo();
        info.kind = RenderResourceKind::Invalid;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidKind);
    }

    TEST(RenderContractsValidation, RejectsVariantKindMismatch)
    {
        auto info = MakeValidMeshRequestInfo();
        info.kind = RenderResourceKind::Texture;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::PayloadKindMismatch);
    }

    TEST(RenderContractsValidation, RejectsMeshShape)
    {
        auto info = MakeValidMeshRequestInfo();
        std::get<MeshUploadPayload>(info.payload).bytes.clear();
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsTextureShape)
    {
        auto info = MakeValidTextureRequestInfo();
        auto& payload = std::get<TextureUploadPayload>(info.payload);
        payload.createInfo.arrayLayers = 2;
        payload.createInfo.isArray = true;
        payload.subresources.push_back(payload.subresources.front());
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsMaterialShape)
    {
        auto info = MakeValidMaterialRequestInfo();
        MaterialUploadTextureBinding binding;
        binding.texture = RenderResourceHandle{2, 1};
        auto& bindings = std::get<MaterialUploadPayload>(info.payload).textureBindings;
        bindings = {binding, binding};
        info.declaredPayloadBytes =
            2 * static_cast<uint64>(sizeof(MaterialUploadTextureBinding));
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation, RejectsRangeOverflow)
    {
        auto info = MakeValidTextureRequestInfo();
        std::get<TextureUploadPayload>(info.payload).subresources.front().bytes =
            UploadByteRange{std::numeric_limits<uint64>::max(), 2, 0};
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::PayloadRangeOverflow);
    }

    TEST(RenderContractsValidation, RejectsDeclaredBytes)
    {
        auto info = MakeValidTextureRequestInfo();
        ++info.declaredPayloadBytes;
        ExpectCreationCode(info,
                           ResourceUploadRequestCreateCode::DiagnosticByteCountMismatch);
    }

    TEST(RenderContractsValidation, RejectsInvalidDependency)
    {
        auto info = MakeValidTextureRequestInfo();
        info.dependencies.push_back(RenderResourceHandle{});
        info.declaredPayloadBytes += sizeof(RenderResourceHandle);
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidDependency);
    }

    TEST(RenderContractsValidation, RejectsSelfDependency)
    {
        auto info = MakeValidTextureRequestInfo();
        info.dependencies.push_back(info.handle);
        info.declaredPayloadBytes += sizeof(RenderResourceHandle);
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::SelfDependency);
    }

    TEST(RenderContractsValidation, RejectsDuplicateDependency)
    {
        auto info = MakeValidTextureRequestInfo();
        info.dependencies = {{2, 1}, {2, 1}};
        info.declaredPayloadBytes += 2 * sizeof(RenderResourceHandle);
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::DuplicateDependency);
    }

    TEST(RenderContractsValidation, UploadRequestOwnsCopiedPayloadBytes)
    {
        ResourceUploadRequestCreateInfo info = MakeValidTextureRequestInfo();
        const uint8 expected =
            std::get<TextureUploadPayload>(info.payload).bytes.front();
        const auto created = ResourceUploadRequest::Create(info);
        std::get<TextureUploadPayload>(info.payload).bytes.clear();
        ASSERT_EQ(created.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_TRUE(created.request);
        const auto* payload =
            std::get_if<TextureUploadPayload>(&created.request->GetPayload());
        ASSERT_NE(payload, nullptr);
        EXPECT_EQ(payload->bytes.front(), expected);
    }

    TEST(RenderContractsValidation, OwnsCopiedSourceValues)
    {
        auto meshInfo = MakeValidMeshRequestInfo();
        auto textureInfo = MakeValidTextureRequestInfo();
        auto materialInfo = MakeValidMaterialRequestInfo();
        MaterialUploadTextureBinding binding;
        binding.texture = RenderResourceHandle{2, 1};
        binding.offset = Vec2(0.25f, 0.5f);
        std::get<MaterialUploadPayload>(materialInfo.payload)
            .textureBindings.push_back(binding);
        std::get<MaterialUploadPayload>(materialInfo.payload)
            .sourceData.textureFlags = 1U;
        std::get<MaterialUploadPayload>(materialInfo.payload)
            .staticDescriptor.featureMask = 1U;
        materialInfo.dependencies.push_back(binding.texture);
        materialInfo.declaredPayloadBytes =
            sizeof(MaterialUploadTextureBinding) +
            sizeof(RenderResourceHandle);

        const auto mesh = ResourceUploadRequest::Create(meshInfo);
        const auto texture = ResourceUploadRequest::Create(textureInfo);
        const auto material = ResourceUploadRequest::Create(materialInfo);

        std::get<MeshUploadPayload>(meshInfo.payload).bytes.clear();
        std::get<TextureUploadPayload>(textureInfo.payload).subresources.clear();
        std::get<MaterialUploadPayload>(materialInfo.payload).textureBindings.clear();
        meshInfo = {};
        textureInfo = {};
        materialInfo = {};

        ASSERT_TRUE(mesh.request);
        ASSERT_TRUE(texture.request);
        ASSERT_TRUE(material.request);
        EXPECT_EQ(std::get<MeshUploadPayload>(mesh.request->GetPayload()).bytes.size(), 36U);
        EXPECT_EQ(std::get<TextureUploadPayload>(texture.request->GetPayload())
                      .subresources.size(),
                  1U);
        const auto& retainedBinding =
            std::get<MaterialUploadPayload>(material.request->GetPayload())
                .textureBindings.front();
        EXPECT_EQ(retainedBinding.texture, (RenderResourceHandle{2, 1}));
        EXPECT_FLOAT_EQ(retainedBinding.offset.x, 0.25f);
    }

    TEST(RenderContractsValidation, FactoryUsesDocumentedFirstFailureOrder)
    {
        auto info = MakeValidTextureRequestInfo();
        info.schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION + 1U;
        info.sequence = 0;
        info.assetId = {};
        info.handle = {};
        info.kind = RenderResourceKind::Invalid;
        std::get<TextureUploadPayload>(info.payload).bytes.clear();
        info.declaredPayloadBytes = std::numeric_limits<uint64>::max();

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidSchema);
        info.schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidSequence);
        info.sequence = 1;
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidAsset);
        info.assetId = AssetId{1};
        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidHandle);
    }

    TEST(RenderContractsValidation, AccumulateOwnedBytesChecksMultiplyAndAdd)
    {
        uint64 total = 5;
        EXPECT_EQ(Detail::AccumulateOwnedBytes(3, 4, total),
                  ResourceUploadRequestCreateCode::Created);
        EXPECT_EQ(total, 17U);

        total = 9;
        EXPECT_EQ(Detail::AccumulateOwnedBytes(
                      std::numeric_limits<uint64>::max(), 2, total),
                  ResourceUploadRequestCreateCode::PayloadByteCountOverflow);
        EXPECT_EQ(total, 9U);

        total = std::numeric_limits<uint64>::max();
        EXPECT_EQ(Detail::AccumulateOwnedBytes(1, 1, total),
                  ResourceUploadRequestCreateCode::PayloadByteCountOverflow);
        EXPECT_EQ(total, std::numeric_limits<uint64>::max());
    }

    enum class MeshRuleCase : uint8
    {
        InvalidIndexType,
        InvalidTopology,
        ZeroVertexCount,
        NonFiniteBounds,
        UnorderedBounds,
        NonCanonicalEmptyPosition,
        ZeroPositionStride,
        PositionCountOverflow,
        PositionTooSmall,
        NonCanonicalOptionalRange,
        ZeroOptionalStride,
        OptionalRangeTooSmall,
        BoneRangePairMismatch,
        ZeroIndexWithRange,
        ZeroIndexWithSubmesh,
        IndexedWithEmptyRange,
        IndexedWithWrongStride,
        IndexedCountOverflow,
        IndexedRangeTooSmall,
        SubmeshInvalidTopology,
        SubmeshZeroCount,
        SubmeshOverflow,
        SubmeshOutOfBounds,
        RangeAdditionOverflow,
        RangeOutOfBounds
    };

    class RenderContractsValidationMeshPayloadRules : public testing::TestWithParam<MeshRuleCase>
    {
    };

    void ConfigureIndexedMesh(ResourceUploadRequestCreateInfo& info)
    {
        auto& payload = std::get<MeshUploadPayload>(info.payload);
        payload.bytes.resize(48, 7U);
        payload.createInfo.indexCount = 3;
        payload.createInfo.indexType = MeshUploadIndexType::UInt32;
        payload.indexRange = UploadByteRange{36, 12, 4};
        info.declaredPayloadBytes = 48;
    }

    TEST_P(RenderContractsValidationMeshPayloadRules, RejectsInvalidShapeRangeOrEnum)
    {
        auto info = MakeValidMeshRequestInfo();
        auto& payload = std::get<MeshUploadPayload>(info.payload);
        ResourceUploadRequestCreateCode expected =
            ResourceUploadRequestCreateCode::InvalidPayload;

        switch (GetParam())
        {
            case MeshRuleCase::InvalidIndexType:
                payload.createInfo.indexType = static_cast<MeshUploadIndexType>(255);
                break;
            case MeshRuleCase::InvalidTopology:
                payload.createInfo.topology =
                    static_cast<MeshUploadPrimitiveTopology>(255);
                break;
            case MeshRuleCase::ZeroVertexCount:
                payload.createInfo.vertexCount = 0;
                break;
            case MeshRuleCase::NonFiniteBounds:
                payload.createInfo.boundsMax.x =
                    std::numeric_limits<float32>::quiet_NaN();
                break;
            case MeshRuleCase::UnorderedBounds:
                payload.createInfo.boundsMin.x = 2.0f;
                payload.createInfo.boundsMax.x = 1.0f;
                break;
            case MeshRuleCase::NonCanonicalEmptyPosition:
                payload.positionRange = UploadByteRange{1, 0, 0};
                break;
            case MeshRuleCase::ZeroPositionStride:
                payload.positionRange.stride = 0;
                break;
            case MeshRuleCase::PositionCountOverflow:
                payload.createInfo.vertexCount =
                    std::numeric_limits<uint64>::max();
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case MeshRuleCase::PositionTooSmall:
                payload.positionRange.size = 24;
                break;
            case MeshRuleCase::NonCanonicalOptionalRange:
                payload.normalRange = UploadByteRange{0, 0, 4};
                break;
            case MeshRuleCase::ZeroOptionalStride:
                payload.normalRange = UploadByteRange{0, 12, 0};
                break;
            case MeshRuleCase::OptionalRangeTooSmall:
                payload.normalRange = UploadByteRange{0, 8, 4};
                break;
            case MeshRuleCase::BoneRangePairMismatch:
                payload.boneIndexRange = UploadByteRange{0, 12, 4};
                break;
            case MeshRuleCase::ZeroIndexWithRange:
                payload.indexRange = UploadByteRange{0, 3, 1};
                break;
            case MeshRuleCase::ZeroIndexWithSubmesh:
                payload.submeshes.push_back(MeshUploadSubmesh{0, 1, 0,
                    MeshUploadPrimitiveTopology::Triangles});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                break;
            case MeshRuleCase::IndexedWithEmptyRange:
                payload.createInfo.indexCount = 3;
                break;
            case MeshRuleCase::IndexedWithWrongStride:
                ConfigureIndexedMesh(info);
                payload.indexRange.stride = 2;
                break;
            case MeshRuleCase::IndexedCountOverflow:
                ConfigureIndexedMesh(info);
                payload.createInfo.indexCount =
                    std::numeric_limits<uint64>::max();
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case MeshRuleCase::IndexedRangeTooSmall:
                ConfigureIndexedMesh(info);
                payload.indexRange.size = 8;
                break;
            case MeshRuleCase::SubmeshInvalidTopology:
                ConfigureIndexedMesh(info);
                payload.submeshes.push_back(MeshUploadSubmesh{0, 3, 0,
                    static_cast<MeshUploadPrimitiveTopology>(255)});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                break;
            case MeshRuleCase::SubmeshZeroCount:
                ConfigureIndexedMesh(info);
                payload.submeshes.push_back(MeshUploadSubmesh{});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                break;
            case MeshRuleCase::SubmeshOverflow:
                ConfigureIndexedMesh(info);
                payload.submeshes.push_back(MeshUploadSubmesh{
                    std::numeric_limits<uint32>::max(), 2, 0,
                    MeshUploadPrimitiveTopology::Triangles});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case MeshRuleCase::SubmeshOutOfBounds:
                ConfigureIndexedMesh(info);
                payload.submeshes.push_back(MeshUploadSubmesh{2, 2, 0,
                    MeshUploadPrimitiveTopology::Triangles});
                info.declaredPayloadBytes += sizeof(MeshUploadSubmesh);
                break;
            case MeshRuleCase::RangeAdditionOverflow:
                payload.normalRange = UploadByteRange{
                    std::numeric_limits<uint64>::max(), 2, 4};
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case MeshRuleCase::RangeOutOfBounds:
                payload.normalRange = UploadByteRange{32, 8, 4};
                break;
        }

        ExpectCreationCode(info, expected);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllMeshPayloadRules,
        RenderContractsValidationMeshPayloadRules,
        testing::Values(
            MeshRuleCase::InvalidIndexType,
            MeshRuleCase::InvalidTopology,
            MeshRuleCase::ZeroVertexCount,
            MeshRuleCase::NonFiniteBounds,
            MeshRuleCase::UnorderedBounds,
            MeshRuleCase::NonCanonicalEmptyPosition,
            MeshRuleCase::ZeroPositionStride,
            MeshRuleCase::PositionCountOverflow,
            MeshRuleCase::PositionTooSmall,
            MeshRuleCase::NonCanonicalOptionalRange,
            MeshRuleCase::ZeroOptionalStride,
            MeshRuleCase::OptionalRangeTooSmall,
            MeshRuleCase::BoneRangePairMismatch,
            MeshRuleCase::ZeroIndexWithRange,
            MeshRuleCase::ZeroIndexWithSubmesh,
            MeshRuleCase::IndexedWithEmptyRange,
            MeshRuleCase::IndexedWithWrongStride,
            MeshRuleCase::IndexedCountOverflow,
            MeshRuleCase::IndexedRangeTooSmall,
            MeshRuleCase::SubmeshInvalidTopology,
            MeshRuleCase::SubmeshZeroCount,
            MeshRuleCase::SubmeshOverflow,
            MeshRuleCase::SubmeshOutOfBounds,
            MeshRuleCase::RangeAdditionOverflow,
            MeshRuleCase::RangeOutOfBounds));

    enum class TextureRuleCase : uint8
    {
        ZeroWidth,
        ZeroHeight,
        ZeroDepth,
        ZeroMips,
        ZeroLayers,
        UnknownFormat,
        UndeclaredFormat,
        EmptyBytes,
        EmptySubresources,
        CubemapDepth,
        CubemapTooFewLayers,
        CubemapLayerMultiple,
        ArraySingleLayer,
        NonArrayMultipleLayers,
        SubresourceCountMultiplyOverflow,
        SubresourceCountMismatch,
        DuplicatePair,
        PairMipOutOfBounds,
        PairLayerOutOfBounds,
        EmptyRange,
        NonCanonicalEmptyRange,
        NonzeroRangeStride,
        RangeAdditionOverflow,
        RangeOutOfBounds,
        ZeroRowPitch,
        SliceBelowRow,
        RangeBelowSlice
    };

    class RenderContractsValidationTexturePayloadRules :
        public testing::TestWithParam<TextureRuleCase>
    {
    };

    void ConfigureTwoLayerTexture(ResourceUploadRequestCreateInfo& info)
    {
        auto& payload = std::get<TextureUploadPayload>(info.payload);
        payload.createInfo.arrayLayers = 2;
        payload.createInfo.isArray = true;
        payload.bytes.resize(8, 5U);
        payload.subresources = {
            TextureUploadSubresource{UploadByteRange{0, 4, 0}, 0, 0, 4, 4},
            TextureUploadSubresource{UploadByteRange{4, 4, 0}, 0, 1, 4, 4}};
        info.declaredPayloadBytes =
            8 + 2 * static_cast<uint64>(sizeof(TextureUploadSubresource));
    }

    TEST_P(RenderContractsValidationTexturePayloadRules, RejectsInvalidShapeRangeOrEnum)
    {
        auto info = MakeValidTextureRequestInfo();
        auto& payload = std::get<TextureUploadPayload>(info.payload);
        auto& createInfo = payload.createInfo;
        ResourceUploadRequestCreateCode expected =
            ResourceUploadRequestCreateCode::InvalidPayload;

        switch (GetParam())
        {
            case TextureRuleCase::ZeroWidth: createInfo.width = 0; break;
            case TextureRuleCase::ZeroHeight: createInfo.height = 0; break;
            case TextureRuleCase::ZeroDepth: createInfo.depth = 0; break;
            case TextureRuleCase::ZeroMips: createInfo.mipLevels = 0; break;
            case TextureRuleCase::ZeroLayers: createInfo.arrayLayers = 0; break;
            case TextureRuleCase::UnknownFormat:
                createInfo.format = TextureUploadFormat::Unknown;
                break;
            case TextureRuleCase::UndeclaredFormat:
                createInfo.format = static_cast<TextureUploadFormat>(255);
                break;
            case TextureRuleCase::EmptyBytes: payload.bytes.clear(); break;
            case TextureRuleCase::EmptySubresources:
                payload.subresources.clear();
                break;
            case TextureRuleCase::CubemapDepth:
                createInfo.isCubemap = true;
                createInfo.depth = 2;
                createInfo.arrayLayers = 6;
                break;
            case TextureRuleCase::CubemapTooFewLayers:
                createInfo.isCubemap = true;
                createInfo.arrayLayers = 5;
                break;
            case TextureRuleCase::CubemapLayerMultiple:
                createInfo.isCubemap = true;
                createInfo.arrayLayers = 7;
                break;
            case TextureRuleCase::ArraySingleLayer:
                createInfo.isArray = true;
                break;
            case TextureRuleCase::NonArrayMultipleLayers:
                createInfo.arrayLayers = 2;
                break;
            case TextureRuleCase::SubresourceCountMultiplyOverflow:
                createInfo.isArray = true;
                createInfo.mipLevels = std::numeric_limits<uint32>::max();
                createInfo.arrayLayers = 2;
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case TextureRuleCase::SubresourceCountMismatch:
                ConfigureTwoLayerTexture(info);
                payload.subresources.pop_back();
                break;
            case TextureRuleCase::DuplicatePair:
                ConfigureTwoLayerTexture(info);
                payload.subresources[1].arrayLayer = 0;
                break;
            case TextureRuleCase::PairMipOutOfBounds:
                payload.subresources.front().mipLevel = 1;
                break;
            case TextureRuleCase::PairLayerOutOfBounds:
                payload.subresources.front().arrayLayer = 1;
                break;
            case TextureRuleCase::EmptyRange:
                payload.subresources.front().bytes = {};
                break;
            case TextureRuleCase::NonCanonicalEmptyRange:
                payload.subresources.front().bytes = UploadByteRange{1, 0, 0};
                break;
            case TextureRuleCase::NonzeroRangeStride:
                payload.subresources.front().bytes.stride = 1;
                break;
            case TextureRuleCase::RangeAdditionOverflow:
                payload.subresources.front().bytes = UploadByteRange{
                    std::numeric_limits<uint64>::max(), 2, 0};
                expected = ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                break;
            case TextureRuleCase::RangeOutOfBounds:
                payload.subresources.front().bytes = UploadByteRange{2, 4, 0};
                break;
            case TextureRuleCase::ZeroRowPitch:
                payload.subresources.front().rowPitch = 0;
                break;
            case TextureRuleCase::SliceBelowRow:
                payload.subresources.front().rowPitch = 5;
                break;
            case TextureRuleCase::RangeBelowSlice:
                payload.subresources.front().slicePitch = 5;
                break;
        }

        ExpectCreationCode(info, expected);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllTexturePayloadRules,
        RenderContractsValidationTexturePayloadRules,
        testing::Values(
            TextureRuleCase::ZeroWidth,
            TextureRuleCase::ZeroHeight,
            TextureRuleCase::ZeroDepth,
            TextureRuleCase::ZeroMips,
            TextureRuleCase::ZeroLayers,
            TextureRuleCase::UnknownFormat,
            TextureRuleCase::UndeclaredFormat,
            TextureRuleCase::EmptyBytes,
            TextureRuleCase::EmptySubresources,
            TextureRuleCase::CubemapDepth,
            TextureRuleCase::CubemapTooFewLayers,
            TextureRuleCase::CubemapLayerMultiple,
            TextureRuleCase::ArraySingleLayer,
            TextureRuleCase::NonArrayMultipleLayers,
            TextureRuleCase::SubresourceCountMultiplyOverflow,
            TextureRuleCase::SubresourceCountMismatch,
            TextureRuleCase::DuplicatePair,
            TextureRuleCase::PairMipOutOfBounds,
            TextureRuleCase::PairLayerOutOfBounds,
            TextureRuleCase::EmptyRange,
            TextureRuleCase::NonCanonicalEmptyRange,
            TextureRuleCase::NonzeroRangeStride,
            TextureRuleCase::RangeAdditionOverflow,
            TextureRuleCase::RangeOutOfBounds,
            TextureRuleCase::ZeroRowPitch,
            TextureRuleCase::SliceBelowRow,
            TextureRuleCase::RangeBelowSlice));

    TEST(RenderContractsValidation, CreatesCompleteCubemapAndArrayTextures)
    {
        auto cubemapInfo = MakeValidTextureRequestInfo();
        auto& cubemap = std::get<TextureUploadPayload>(cubemapInfo.payload);
        cubemap.createInfo.isCubemap = true;
        cubemap.createInfo.arrayLayers = 6;
        cubemap.bytes.resize(24, 1U);
        cubemap.subresources.clear();
        for (uint32 layer = 0; layer < 6; ++layer)
        {
            cubemap.subresources.push_back(TextureUploadSubresource{
                UploadByteRange{layer * 4U, 4, 0}, 0, layer, 4, 4});
        }
        cubemapInfo.declaredPayloadBytes =
            24 + 6 * static_cast<uint64>(sizeof(TextureUploadSubresource));
        ExpectCreationCode(cubemapInfo, ResourceUploadRequestCreateCode::Created);

        auto arrayInfo = MakeValidTextureRequestInfo();
        ConfigureTwoLayerTexture(arrayInfo);
        ExpectCreationCode(arrayInfo, ResourceUploadRequestCreateCode::Created);
    }

    enum class MaterialSourceValidationCase : uint8
    {
        FiniteHdrAndNegativeColors,
        NormalScaleUpperBoundary,
        NormalScaleAboveBoundary,
        EmissiveStrengthUpperBoundary,
        EmissiveStrengthAboveBoundary,
        NonFiniteBaseColor,
        MetallicBelowRange,
        RoughnessAboveRange,
        OcclusionBelowRange,
        NonFiniteEmissiveColor,
        AlphaCutoffAboveRange,
        InvalidAlphaMode,
        InvalidWorkflow,
        ToonBandsBelowRange,
        ToonSpecularThresholdAboveRange,
        ToonRimStrengthAboveRange,
        ToonOutlineWidthAboveRange,
        NonFiniteToonShadowTint,
        TransmissionAboveRange,
        IndexOfRefractionBelowRange,
        ThicknessAboveRange,
        NonFiniteAttenuationColor,
        AttenuationColorAboveRange,
        ZeroAttenuationDistance
    };

    struct MaterialSourceValidationParameter
    {
        MaterialSourceValidationCase testCase;
        bool expectedValid;
    };

    class RenderContractsValidationMaterialSourceConsistency :
        public testing::TestWithParam<MaterialSourceValidationParameter>
    {
    };

    TEST_P(RenderContractsValidationMaterialSourceConsistency,
           DefinitionAndUploadUseTheSameCanonicalParameterContract)
    {
        const MaterialSourceValidationParameter parameter = GetParam();
        MaterialSourceData source;
        const float32 nan = std::numeric_limits<float32>::quiet_NaN();
        switch (parameter.testCase)
        {
            case MaterialSourceValidationCase::FiniteHdrAndNegativeColors:
                source.baseColorFactor = Vec4{-2.0f, 4.0f, 0.5f, 1.0f};
                source.emissiveColor = Vec3{-1.0f, 8.0f, 0.25f};
                break;
            case MaterialSourceValidationCase::NormalScaleUpperBoundary:
                source.normalScale = 8.0f;
                break;
            case MaterialSourceValidationCase::NormalScaleAboveBoundary:
                source.normalScale = 8.01f;
                break;
            case MaterialSourceValidationCase::EmissiveStrengthUpperBoundary:
                source.emissiveStrength = 65504.0f;
                break;
            case MaterialSourceValidationCase::EmissiveStrengthAboveBoundary:
                source.emissiveStrength = 65505.0f;
                break;
            case MaterialSourceValidationCase::NonFiniteBaseColor:
                source.baseColorFactor.x = nan;
                break;
            case MaterialSourceValidationCase::MetallicBelowRange:
                source.metallicFactor = -0.01f;
                break;
            case MaterialSourceValidationCase::RoughnessAboveRange:
                source.roughnessFactor = 1.01f;
                break;
            case MaterialSourceValidationCase::OcclusionBelowRange:
                source.occlusionStrength = -0.01f;
                break;
            case MaterialSourceValidationCase::NonFiniteEmissiveColor:
                source.emissiveColor.y = nan;
                break;
            case MaterialSourceValidationCase::AlphaCutoffAboveRange:
                source.alphaCutoff = 1.01f;
                break;
            case MaterialSourceValidationCase::InvalidAlphaMode:
                source.alphaMode = static_cast<MaterialSourceAlphaMode>(255);
                break;
            case MaterialSourceValidationCase::InvalidWorkflow:
                source.workflow = static_cast<MaterialSourceWorkflow>(255);
                break;
            case MaterialSourceValidationCase::ToonBandsBelowRange:
                source.toonBands = 0.0f;
                break;
            case MaterialSourceValidationCase::ToonSpecularThresholdAboveRange:
                source.toonSpecularThreshold = 1.01f;
                break;
            case MaterialSourceValidationCase::ToonRimStrengthAboveRange:
                source.toonRimStrength = 64.01f;
                break;
            case MaterialSourceValidationCase::ToonOutlineWidthAboveRange:
                source.toonOutlineWidth = 64.01f;
                break;
            case MaterialSourceValidationCase::NonFiniteToonShadowTint:
                source.toonShadowTint.z = nan;
                break;
            case MaterialSourceValidationCase::TransmissionAboveRange:
                source.transmission = 1.01f;
                break;
            case MaterialSourceValidationCase::IndexOfRefractionBelowRange:
                source.indexOfRefraction = 0.99f;
                break;
            case MaterialSourceValidationCase::ThicknessAboveRange:
                source.thickness = 100001.0f;
                break;
            case MaterialSourceValidationCase::NonFiniteAttenuationColor:
                source.attenuationColor.x = nan;
                break;
            case MaterialSourceValidationCase::AttenuationColorAboveRange:
                source.attenuationColor.y = 1.01f;
                break;
            case MaterialSourceValidationCase::ZeroAttenuationDistance:
                source.attenuationDistance = 0.0f;
                break;
        }

        std::string sourceError;
        const bool sourceValid = ValidateMaterialSourceData(source, sourceError);

        MaterialDefinition definition;
        definition.parameters = source;
        std::string definitionError;
        const bool definitionValid =
            ValidateMaterialDefinition(definition, definitionError);

        auto uploadInfo = MakeValidMaterialRequestInfo();
        std::get<MaterialUploadPayload>(uploadInfo.payload).sourceData = source;
        const ResourceUploadRequestCreateResult uploadResult =
            ResourceUploadRequest::Create(std::move(uploadInfo));
        const bool uploadValid =
            uploadResult.code == ResourceUploadRequestCreateCode::Created;

        EXPECT_EQ(sourceValid, parameter.expectedValid) << sourceError;
        EXPECT_EQ(definitionValid, sourceValid) << definitionError;
        EXPECT_EQ(uploadValid, sourceValid);
    }

    INSTANTIATE_TEST_SUITE_P(
        CanonicalMaterialSourceParameters,
        RenderContractsValidationMaterialSourceConsistency,
        testing::Values(
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::FiniteHdrAndNegativeColors, true},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::NormalScaleUpperBoundary, true},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::NormalScaleAboveBoundary, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::EmissiveStrengthUpperBoundary, true},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::EmissiveStrengthAboveBoundary, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::NonFiniteBaseColor, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::MetallicBelowRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::RoughnessAboveRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::OcclusionBelowRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::NonFiniteEmissiveColor, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::AlphaCutoffAboveRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::InvalidAlphaMode, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::InvalidWorkflow, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::ToonBandsBelowRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::ToonSpecularThresholdAboveRange,
                false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::ToonRimStrengthAboveRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::ToonOutlineWidthAboveRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::NonFiniteToonShadowTint, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::TransmissionAboveRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::IndexOfRefractionBelowRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::ThicknessAboveRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::NonFiniteAttenuationColor, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::AttenuationColorAboveRange, false},
            MaterialSourceValidationParameter{
                MaterialSourceValidationCase::ZeroAttenuationDistance, true}));

    enum class MaterialRuleCase : uint8
    {
        NonFiniteBaseColor,
        NonFiniteMetallic,
        NonFiniteRoughness,
        NonFiniteNormalScale,
        NonFiniteOcclusion,
        NonFiniteEmissiveColor,
        NonFiniteEmissiveStrength,
        NonFiniteAlphaCutoff,
        MetallicAboveRange,
        RoughnessBelowRange,
        OcclusionAboveRange,
        AlphaCutoffAboveRange,
        NegativeNormalScale,
        NegativeEmissiveStrength,
        InvalidAlphaMode,
        InvalidWorkflow,
        InvalidBindingHandle,
        NegativeUvSet,
        NonFiniteOffset,
        NonFiniteScale,
        NonFiniteRotation,
        DuplicateSlot,
        InvalidSlot,
        InvalidWrapS,
        InvalidWrapT,
        InvalidMinFilter,
        InvalidMagFilter,
        UnsupportedMagFilter
    };

    class RenderContractsValidationMaterialPayloadRules :
        public testing::TestWithParam<MaterialRuleCase>
    {
    };

    TEST_P(RenderContractsValidationMaterialPayloadRules, RejectsInvalidNumericEnumOrBinding)
    {
        auto info = MakeValidMaterialRequestInfo();
        auto& payload = std::get<MaterialUploadPayload>(info.payload);
        auto& source = payload.sourceData;
        const float32 nan = std::numeric_limits<float32>::quiet_NaN();
        const MaterialRuleCase testCase = GetParam();

        if (testCase >= MaterialRuleCase::InvalidBindingHandle)
        {
            MaterialUploadTextureBinding binding;
            binding.texture = RenderResourceHandle{2, 1};
            payload.textureBindings.push_back(binding);
            info.declaredPayloadBytes = sizeof(MaterialUploadTextureBinding);
        }

        switch (testCase)
        {
            case MaterialRuleCase::NonFiniteBaseColor: source.baseColorFactor.x = nan; break;
            case MaterialRuleCase::NonFiniteMetallic: source.metallicFactor = nan; break;
            case MaterialRuleCase::NonFiniteRoughness: source.roughnessFactor = nan; break;
            case MaterialRuleCase::NonFiniteNormalScale: source.normalScale = nan; break;
            case MaterialRuleCase::NonFiniteOcclusion: source.occlusionStrength = nan; break;
            case MaterialRuleCase::NonFiniteEmissiveColor: source.emissiveColor.x = nan; break;
            case MaterialRuleCase::NonFiniteEmissiveStrength: source.emissiveStrength = nan; break;
            case MaterialRuleCase::NonFiniteAlphaCutoff: source.alphaCutoff = nan; break;
            case MaterialRuleCase::MetallicAboveRange: source.metallicFactor = 1.1f; break;
            case MaterialRuleCase::RoughnessBelowRange: source.roughnessFactor = -0.1f; break;
            case MaterialRuleCase::OcclusionAboveRange: source.occlusionStrength = 1.1f; break;
            case MaterialRuleCase::AlphaCutoffAboveRange: source.alphaCutoff = 1.1f; break;
            case MaterialRuleCase::NegativeNormalScale: source.normalScale = -0.1f; break;
            case MaterialRuleCase::NegativeEmissiveStrength: source.emissiveStrength = -0.1f; break;
            case MaterialRuleCase::InvalidAlphaMode:
                source.alphaMode = static_cast<MaterialSourceAlphaMode>(255);
                break;
            case MaterialRuleCase::InvalidWorkflow:
                source.workflow = static_cast<MaterialSourceWorkflow>(255);
                break;
            case MaterialRuleCase::InvalidBindingHandle:
                payload.textureBindings.front().texture = {};
                break;
            case MaterialRuleCase::NegativeUvSet:
                payload.textureBindings.front().uvSet = -1;
                break;
            case MaterialRuleCase::NonFiniteOffset:
                payload.textureBindings.front().offset.x = nan;
                break;
            case MaterialRuleCase::NonFiniteScale:
                payload.textureBindings.front().scale.x = nan;
                break;
            case MaterialRuleCase::NonFiniteRotation:
                payload.textureBindings.front().rotation = nan;
                break;
            case MaterialRuleCase::DuplicateSlot:
                payload.textureBindings.push_back(payload.textureBindings.front());
                info.declaredPayloadBytes += sizeof(MaterialUploadTextureBinding);
                break;
            case MaterialRuleCase::InvalidSlot:
                payload.textureBindings.front().slot =
                    static_cast<MaterialUploadTextureSlot>(255);
                break;
            case MaterialRuleCase::InvalidWrapS:
                payload.textureBindings.front().wrapS =
                    static_cast<MaterialUploadWrapMode>(255);
                break;
            case MaterialRuleCase::InvalidWrapT:
                payload.textureBindings.front().wrapT =
                    static_cast<MaterialUploadWrapMode>(255);
                break;
            case MaterialRuleCase::InvalidMinFilter:
                payload.textureBindings.front().minFilter =
                    static_cast<MaterialUploadFilterMode>(255);
                break;
            case MaterialRuleCase::InvalidMagFilter:
                payload.textureBindings.front().magFilter =
                    static_cast<MaterialUploadFilterMode>(255);
                break;
            case MaterialRuleCase::UnsupportedMagFilter:
                payload.textureBindings.front().magFilter =
                    MaterialUploadFilterMode::LinearMipmapLinear;
                break;
        }

        ExpectCreationCode(info, ResourceUploadRequestCreateCode::InvalidPayload);
    }

    INSTANTIATE_TEST_SUITE_P(
        AllMaterialPayloadRules,
        RenderContractsValidationMaterialPayloadRules,
        testing::Values(
            MaterialRuleCase::NonFiniteBaseColor,
            MaterialRuleCase::NonFiniteMetallic,
            MaterialRuleCase::NonFiniteRoughness,
            MaterialRuleCase::NonFiniteNormalScale,
            MaterialRuleCase::NonFiniteOcclusion,
            MaterialRuleCase::NonFiniteEmissiveColor,
            MaterialRuleCase::NonFiniteEmissiveStrength,
            MaterialRuleCase::NonFiniteAlphaCutoff,
            MaterialRuleCase::MetallicAboveRange,
            MaterialRuleCase::RoughnessBelowRange,
            MaterialRuleCase::OcclusionAboveRange,
            MaterialRuleCase::AlphaCutoffAboveRange,
            MaterialRuleCase::NegativeNormalScale,
            MaterialRuleCase::NegativeEmissiveStrength,
            MaterialRuleCase::InvalidAlphaMode,
            MaterialRuleCase::InvalidWorkflow,
            MaterialRuleCase::InvalidBindingHandle,
            MaterialRuleCase::NegativeUvSet,
            MaterialRuleCase::NonFiniteOffset,
            MaterialRuleCase::NonFiniteScale,
            MaterialRuleCase::NonFiniteRotation,
            MaterialRuleCase::DuplicateSlot,
            MaterialRuleCase::InvalidSlot,
            MaterialRuleCase::InvalidWrapS,
            MaterialRuleCase::InvalidWrapT,
            MaterialRuleCase::InvalidMinFilter,
            MaterialRuleCase::InvalidMagFilter,
            MaterialRuleCase::UnsupportedMagFilter));

    TEST(RenderContractsValidation, RejectsUndeclaredReadinessAndPriority)
    {
        auto readinessInfo = MakeValidMaterialRequestInfo();
        readinessInfo.dependencyReadiness =
            static_cast<RenderDependencyReadiness>(255);
        ExpectCreationCode(readinessInfo,
                           ResourceUploadRequestCreateCode::InvalidPayload);

        auto priorityInfo = MakeValidMaterialRequestInfo();
        priorityInfo.priority = static_cast<RenderUploadPriority>(255);
        ExpectCreationCode(priorityInfo,
                           ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST(RenderContractsValidation,
         ViewClearPolicyIsVersionedCopiedAndRejectsInvalidValues)
    {
        RenderFrameHeaderV5 header = MakeValidFrameHeader(41, 9);
        RenderViewSnapshot view = MakeValidView();
        view.clearPolicy = RenderViewClearPolicy::SolidColor;
        view.clearColor = {0.25f, 0.5f, 0.75f, 1.0f};

        const std::unique_ptr<const RenderFramePacketV5> packet =
            RenderFramePacketV5::Create(header,
                                        view,
                                        RenderFrameSettings{},
                                        RenderFrameCaptureRequest{},
                                        MakeCompleteDiagnostics());
        ASSERT_NE(packet, nullptr);
        EXPECT_EQ(packet->GetHeader().schemaVersion,
                  RVX_RENDER_FRAME_PACKET_V5_SCHEMA_VERSION);
        EXPECT_EQ(packet->GetView().clearPolicy,
                  RenderViewClearPolicy::SolidColor);
        EXPECT_FLOAT_EQ(packet->GetView().clearColor.z, 0.75f);

        view.clearColor.x = std::numeric_limits<float32>::quiet_NaN();
        EXPECT_EQ(RenderFramePacketV5::Create(header,
                                               view,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        view = MakeValidView();
        view.clearPolicy = static_cast<RenderViewClearPolicy>(255);
        EXPECT_FALSE(IsValidRenderViewClearValues(view));
        EXPECT_EQ(RenderFramePacketV5::Create(header,
                                               view,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);
    }

    TEST(RenderContractsValidation,
         FrameV5RejectsIncompleteInputsAndOwnsAcceptedValues)
    {
        RenderFrameHeaderV5 header = MakeValidFrameHeader(87, 31);
        header.worldRevision = 12;

        RenderViewSnapshot view = MakeValidView();
        view.exposure = 1.25f;

        RenderFrameSettings settings;
        settings.renderScale = 0.75f;

        RenderFrameCaptureRequest captureRequest;
        captureRequest.requestId = 9;
        captureRequest.kind = RenderFrameCaptureKind::Color;
        captureRequest.width = 64;
        captureRequest.height = 32;

        const std::unique_ptr<const RenderFramePacketV5> packet =
            RenderFramePacketV5::Create(header,
                                        view,
                                        settings,
                                        captureRequest,
                                        MakeCompleteDiagnostics());
        ASSERT_NE(packet, nullptr);

        header.sequence = 0;
        view.exposure = 4.0f;
        settings.renderScale = 0.5f;
        captureRequest.width = 1;

        EXPECT_EQ(packet->GetHeader().sequence, 87U);
        EXPECT_EQ(packet->GetHeader().requiredSceneRevision, 31U);
        EXPECT_FLOAT_EQ(packet->GetView().exposure, 1.25f);
        EXPECT_FLOAT_EQ(packet->GetSettings().renderScale, 0.75f);
        EXPECT_EQ(packet->GetCaptureRequest().width, 64U);

        RenderFrameHeaderV5 invalidHeader = MakeValidFrameHeader();
        invalidHeader.requiredSceneRevision = 0;
        EXPECT_EQ(RenderFramePacketV5::Create(invalidHeader,
                                               MakeValidView(),
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        RenderExtractionDiagnostics incompleteDiagnostics =
            MakeCompleteDiagnostics();
        incompleteDiagnostics.complete = false;
        EXPECT_EQ(RenderFramePacketV5::Create(packet->GetHeader(),
                                               MakeValidView(),
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               incompleteDiagnostics),
                  nullptr);
    }

    TEST(RenderContractsValidation,
         SingleViewFamilyFailsClosedOnIdentitySurfaceAndNumericValues)
    {
        RenderFrameHeaderV5 header = MakeValidFrameHeader(101, 37);
        RenderViewSnapshot view = MakeValidView();

        const auto packet = RenderFramePacketV5::Create(
            header,
            view,
            RenderFrameSettings{},
            RenderFrameCaptureRequest{},
            MakeCompleteDiagnostics());
        ASSERT_NE(packet, nullptr);
        ASSERT_EQ(packet->GetViewFamily().views.size(), 1U);
        EXPECT_EQ(packet->GetViewFamily().primaryViewIndex, 0U);
        EXPECT_EQ(packet->GetFrameStamp().sequence, header.sequence);
        EXPECT_EQ(packet->GetHistoryIdentity().view, view.identity);
        EXPECT_EQ(packet->GetHistoryIdentity().surfaceGeneration,
                  header.surfaceGeneration);
        EXPECT_EQ(packet->GetViewFamily().identity.frameSequence,
                  header.sequence);
        EXPECT_EQ(packet->GetViewFamily().effectTime.simulationTick,
                  header.simulationTick);
        EXPECT_FLOAT_EQ(packet->GetViewFamily().effectTime.fixedDeltaSeconds,
                        header.effectFixedDeltaSeconds);
        EXPECT_DOUBLE_EQ(packet->GetViewFamily().effectTime.simulationSeconds,
                         header.effectSimulationSeconds);

        RenderViewFamilySnapshot unsupportedMultiView = packet->GetViewFamily();
        unsupportedMultiView.views.push_back(view);
        EXPECT_FALSE(IsValidSingleRenderViewFamily(unsupportedMultiView));

        RenderViewSnapshot invalidView = view;
        invalidView.identity = {};
        EXPECT_EQ(RenderFramePacketV5::Create(header,
                                               invalidView,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        invalidView = view;
        invalidView.cameraCutRevision = 0;
        EXPECT_EQ(RenderFramePacketV5::Create(header,
                                               invalidView,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        invalidView = view;
        invalidView.identity.role = static_cast<ViewRole>(255);
        EXPECT_EQ(RenderFramePacketV5::Create(header,
                                               invalidView,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        RenderFrameHeaderV5 invalidHeader = header;
        invalidHeader.surfaceGeneration = 0;
        EXPECT_EQ(RenderFramePacketV5::Create(invalidHeader,
                                               view,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        invalidHeader = header;
        invalidHeader.temporalEpoch = 0;
        EXPECT_EQ(RenderFramePacketV5::Create(invalidHeader,
                                               view,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        invalidView = view;
        invalidView.absoluteTime =
            std::numeric_limits<float64>::quiet_NaN();
        EXPECT_EQ(RenderFramePacketV5::Create(header,
                                               invalidView,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);

        invalidView = view;
        invalidView.viewMatrix[0].x =
            std::numeric_limits<float32>::quiet_NaN();
        EXPECT_EQ(RenderFramePacketV5::Create(header,
                                               invalidView,
                                               RenderFrameSettings{},
                                               RenderFrameCaptureRequest{},
                                               MakeCompleteDiagnostics()),
                  nullptr);
    }

    TEST(RenderContractsValidation,
         EffectTimeIsIndependentFromPresentationClock)
    {
        RenderFrameHeaderV5 firstHeader = MakeValidFrameHeader(301, 61);
        firstHeader.simulationTick = 17;
        firstHeader.effectFixedDeltaSeconds = 1.0f / 120.0f;
        firstHeader.effectSimulationSeconds = 17.0 / 120.0;
        firstHeader.deterministicSeed = 0x1234567U;
        RenderViewSnapshot firstView = MakeValidView();
        firstView.absoluteTime = 10.0;
        firstView.deltaTime = 1.0f / 30.0f;

        RenderFrameHeaderV5 secondHeader = firstHeader;
        secondHeader.sequence = 302;
        RenderViewSnapshot secondView = firstView;
        secondView.absoluteTime = 9000.0;
        secondView.deltaTime = 1.0f / 240.0f;

        const auto first = RenderFramePacketV5::Create(
            firstHeader,
            firstView,
            RenderFrameSettings{},
            RenderFrameCaptureRequest{},
            MakeCompleteDiagnostics());
        const auto second = RenderFramePacketV5::Create(
            secondHeader,
            secondView,
            RenderFrameSettings{},
            RenderFrameCaptureRequest{},
            MakeCompleteDiagnostics());
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(first->GetViewFamily().effectTime,
                  second->GetViewFamily().effectTime);
        EXPECT_NE(first->GetFrameStamp().absoluteTimeSeconds,
                  second->GetFrameStamp().absoluteTimeSeconds);
        EXPECT_NE(first->GetFrameStamp().deltaTimeSeconds,
                  second->GetFrameStamp().deltaTimeSeconds);
    }

    TEST(RenderContractsValidation,
         HistoryAvailabilityExplainsEveryCompatibilityBreak)
    {
        const auto packet = RenderFramePacketV5::Create(
            MakeValidFrameHeader(201, 51),
            MakeValidView(),
            RenderFrameSettings{},
            RenderFrameCaptureRequest{},
            MakeCompleteDiagnostics());
        ASSERT_NE(packet, nullptr);
        const RenderViewHistoryIdentity identity = packet->GetHistoryIdentity();

        RenderHistoryAvailability decision =
            EvaluateRenderHistoryAvailability(nullptr, identity, false);
        EXPECT_FALSE(decision.available);
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons,
            RenderHistoryResetReason::FirstFrame));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons,
            RenderHistoryResetReason::HistoryUnavailable));

        decision = EvaluateRenderHistoryAvailability(&identity, identity, false);
        EXPECT_TRUE(decision.available);
        EXPECT_EQ(decision.resetReasons, RenderHistoryResetReason::None);
        EXPECT_TRUE(decision.IsConsistent());

        RenderViewHistoryIdentity changed = identity;
        ++changed.cameraCutRevision;
        ++changed.temporalEpoch;
        ++changed.worldRevision;
        ++changed.surfaceGeneration;
        changed.previousFrame.surfaceGeneration = changed.surfaceGeneration;
        ++changed.extent.width;
        ++changed.formatRevision;
        ++changed.previousFrame.configurationRevision;
        decision = EvaluateRenderHistoryAvailability(
            &identity, changed, true, false);
        EXPECT_FALSE(decision.available);
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons, RenderHistoryResetReason::CameraCut));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons,
            RenderHistoryResetReason::TemporalEpochChanged));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons,
            RenderHistoryResetReason::SceneDiscontinuity));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons, RenderHistoryResetReason::SurfaceChanged));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons, RenderHistoryResetReason::Resize));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons, RenderHistoryResetReason::FormatChanged));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons,
            RenderHistoryResetReason::ConfigurationChanged));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons,
            RenderHistoryResetReason::ExplicitDiscontinuity));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons,
            RenderHistoryResetReason::HistoryUnavailable));
    }

    MaterialStaticDescriptor MakeSurfacePathDescriptor(
        MaterialShadingModel shadingModel,
        MaterialBlendMode blendMode,
        MaterialTranslucencyPhase translucencyPhase)
    {
        MaterialStaticDescriptor descriptor;
        descriptor.shadingModel = shadingModel;
        descriptor.blendMode = blendMode;
        descriptor.translucencyPhase = translucencyPhase;
        descriptor.programFamily =
            std::string(GetMaterialProgramFamily(shadingModel));
        return descriptor;
    }

    TEST(RenderContractsValidation,
         SurfacePathEligibilityRoutesTheSupportedMaterialMatrix)
    {
        struct Case
        {
            MaterialShadingModel shadingModel;
            MaterialBlendMode blendMode;
            MaterialTranslucencyPhase translucencyPhase;
            SurfacePathEligibility expected;
        };

        const std::vector<Case> cases = {
            {MaterialShadingModel::StandardLit, MaterialBlendMode::Opaque,
             MaterialTranslucencyPhase::Opaque,
             SurfacePathEligibility::DeferredOpaque},
            {MaterialShadingModel::StandardLit, MaterialBlendMode::Masked,
             MaterialTranslucencyPhase::Opaque,
             SurfacePathEligibility::DeferredOpaque},
            {MaterialShadingModel::Unlit, MaterialBlendMode::Opaque,
             MaterialTranslucencyPhase::Opaque,
             SurfacePathEligibility::ForwardSpecialOpaque},
            {MaterialShadingModel::Unlit, MaterialBlendMode::Masked,
             MaterialTranslucencyPhase::Opaque,
             SurfacePathEligibility::ForwardSpecialOpaque},
            {MaterialShadingModel::StandardLit, MaterialBlendMode::AlphaBlend,
             MaterialTranslucencyPhase::Standard,
             SurfacePathEligibility::ForwardTranslucency},
            {MaterialShadingModel::StandardLit,
             MaterialBlendMode::Premultiplied,
             MaterialTranslucencyPhase::AfterToneMap,
             SurfacePathEligibility::ForwardTranslucency},
            {MaterialShadingModel::Unlit, MaterialBlendMode::Additive,
             MaterialTranslucencyPhase::Standard,
             SurfacePathEligibility::ForwardTranslucency},
            {MaterialShadingModel::ThinTransmission,
             MaterialBlendMode::AlphaBlend,
             MaterialTranslucencyPhase::Standard,
             SurfacePathEligibility::ForwardTranslucency},
            {MaterialShadingModel::ThinTransmission,
             MaterialBlendMode::Premultiplied,
             MaterialTranslucencyPhase::Standard,
             SurfacePathEligibility::ForwardTranslucency},
            {MaterialShadingModel::ThinTransmission, MaterialBlendMode::Additive,
             MaterialTranslucencyPhase::Standard,
             SurfacePathEligibility::ForwardTranslucency},
        };

        for (const Case& testCase : cases)
        {
            const MaterialStaticDescriptor descriptor =
                MakeSurfacePathDescriptor(testCase.shadingModel,
                                          testCase.blendMode,
                                          testCase.translucencyPhase);
            std::string error;
            EXPECT_TRUE(ValidateMaterialStaticDescriptor(descriptor, error))
                << error;
            EXPECT_EQ(ClassifySurfacePathEligibility(descriptor),
                      testCase.expected);
        }
    }

    TEST(RenderContractsValidation,
         SurfacePathEligibilityRoutesOpaqueToonAndRejectsToonTranslucency)
    {
        for (const auto [blendMode, translucencyPhase] :
             {std::pair{MaterialBlendMode::Opaque,
                        MaterialTranslucencyPhase::Opaque},
              std::pair{MaterialBlendMode::Masked,
                        MaterialTranslucencyPhase::Opaque},
              std::pair{MaterialBlendMode::AlphaBlend,
                        MaterialTranslucencyPhase::Standard},
              std::pair{MaterialBlendMode::Premultiplied,
                        MaterialTranslucencyPhase::Standard},
              std::pair{MaterialBlendMode::Additive,
                        MaterialTranslucencyPhase::Standard}})
        {
            const MaterialStaticDescriptor descriptor =
                MakeSurfacePathDescriptor(MaterialShadingModel::ToonLit,
                                          blendMode,
                                          translucencyPhase);
            std::string error;
            EXPECT_TRUE(ValidateMaterialStaticDescriptor(descriptor, error))
                << error;
            const SurfacePathEligibility expected =
                blendMode == MaterialBlendMode::Opaque ||
                        blendMode == MaterialBlendMode::Masked
                    ? SurfacePathEligibility::DeferredOpaque
                    : SurfacePathEligibility::Unsupported;
            EXPECT_EQ(ClassifySurfacePathEligibility(descriptor), expected);
        }
    }

    TEST(RenderContractsValidation,
         SurfacePathEligibilityFailsClosedForInvalidOrContradictoryDescriptors)
    {
        const MaterialStaticDescriptor valid = MakeSurfacePathDescriptor(
            MaterialShadingModel::StandardLit, MaterialBlendMode::Opaque,
            MaterialTranslucencyPhase::Opaque);

        std::vector<MaterialStaticDescriptor> invalidDescriptors;

        MaterialStaticDescriptor unknownDomain = valid;
        unknownDomain.domain = static_cast<MaterialDomain>(255);
        invalidDescriptors.push_back(std::move(unknownDomain));

        MaterialStaticDescriptor unknownBlend = valid;
        unknownBlend.blendMode = static_cast<MaterialBlendMode>(255);
        invalidDescriptors.push_back(std::move(unknownBlend));

        MaterialStaticDescriptor contradictoryPhase = valid;
        contradictoryPhase.translucencyPhase =
            MaterialTranslucencyPhase::Standard;
        invalidDescriptors.push_back(std::move(contradictoryPhase));

        MaterialStaticDescriptor invalidThinTransmission =
            MakeSurfacePathDescriptor(MaterialShadingModel::ThinTransmission,
                                      MaterialBlendMode::Opaque,
                                      MaterialTranslucencyPhase::Opaque);
        invalidDescriptors.push_back(std::move(invalidThinTransmission));

        MaterialStaticDescriptor wrongProgramFamily = valid;
        wrongProgramFamily.programFamily = "unknown-program";
        invalidDescriptors.push_back(std::move(wrongProgramFamily));

        MaterialStaticDescriptor wrongLayout = valid;
        wrongLayout.parameterLayoutHash ^= 0x1ull;
        invalidDescriptors.push_back(std::move(wrongLayout));

        MaterialStaticDescriptor unknownFeature = valid;
        unknownFeature.featureMask = 1ull << 63u;
        invalidDescriptors.push_back(std::move(unknownFeature));

        for (const MaterialStaticDescriptor& descriptor : invalidDescriptors)
        {
            std::string error;
            EXPECT_FALSE(ValidateMaterialStaticDescriptor(descriptor, error));
            EXPECT_FALSE(error.empty());
            EXPECT_EQ(ClassifySurfacePathEligibility(descriptor),
                      SurfacePathEligibility::Unsupported);
        }
    }

    TEST(RenderContractsValidation, SurfacePathEligibilityNamesAreStable)
    {
        EXPECT_EQ(GetSurfacePathEligibilityName(
                      SurfacePathEligibility::DeferredOpaque),
                  "DeferredOpaque");
        EXPECT_EQ(GetSurfacePathEligibilityName(
                      SurfacePathEligibility::ForwardSpecialOpaque),
                  "ForwardSpecialOpaque");
        EXPECT_EQ(GetSurfacePathEligibilityName(
                      SurfacePathEligibility::ForwardTranslucency),
                  "ForwardTranslucency");
        EXPECT_EQ(GetSurfacePathEligibilityName(
                      SurfacePathEligibility::Unsupported),
                  "Unsupported");
        EXPECT_EQ(GetSurfacePathEligibilityName(
                      static_cast<SurfacePathEligibility>(255)),
                  "Unknown");
    }

    TEST(RenderContractsValidation,
         ExposureSettingsRejectInvalidModesRangesAndFrozenDelta)
    {
        RenderExposureSettings settings;
        EXPECT_TRUE(IsValidRenderExposureSettings(settings));
        EXPECT_TRUE(IsValidRenderExposureDeltaSeconds(0.0f));
        EXPECT_TRUE(IsValidRenderExposureDeltaSeconds(1.0f));
        EXPECT_FALSE(IsValidRenderExposureDeltaSeconds(-0.001f));
        EXPECT_FALSE(IsValidRenderExposureDeltaSeconds(
            std::numeric_limits<float32>::infinity()));
        EXPECT_FALSE(IsValidRenderExposureDeltaSeconds(1.001f));

        const std::array<RenderExposureSettings, 10> invalidSettings = []
        {
            std::array<RenderExposureSettings, 10> values{};
            values[0].mode = static_cast<RenderExposureMode>(255);
            values[1].manualEV100 =
                std::numeric_limits<float32>::quiet_NaN();
            values[2].exposureCompensationEV = 10.01f;
            values[3].histogramLowPercentile = -0.001f;
            values[4].histogramHighPercentile = 0.50f;
            values[4].histogramLowPercentile = 0.50f;
            values[5].histogramKeyValue = 0.0f;
            values[6].minEV100 = 4.0f;
            values[6].maxEV100 = 4.0f;
            values[7].maxEV100 = 24.01f;
            values[8].brighteningAdaptationSpeed = -0.001f;
            values[9].darkeningAdaptationSpeed =
                std::numeric_limits<float32>::infinity();
            return values;
        }();

        for (const RenderExposureSettings& invalid : invalidSettings)
            EXPECT_FALSE(IsValidRenderExposureSettings(invalid));

        RenderFrameSettings frameSettings;
        frameSettings.postProcess.exposureSettings = invalidSettings[0];
        EXPECT_FALSE(IsValidRenderFrameSettings(frameSettings));
    }

    TEST(RenderContractsValidation,
         ExposurePhysicalScaleAndHistogramEVUseOneCalibration)
    {
        EXPECT_TRUE(IsValidRenderExposureScale(
            RVX_RENDER_EXPOSURE_MIN_SCALE));
        EXPECT_TRUE(IsValidRenderExposureScale(
            RVX_RENDER_EXPOSURE_MAX_SCALE));
        EXPECT_FALSE(IsValidRenderExposureScale(std::nextafter(
            RVX_RENDER_EXPOSURE_MIN_SCALE, 0.0f)));
        EXPECT_FALSE(IsValidRenderExposureScale(std::nextafter(
            RVX_RENDER_EXPOSURE_MAX_SCALE,
            std::numeric_limits<float32>::infinity())));
        EXPECT_FALSE(IsValidRenderExposureScale(
            std::numeric_limits<float32>::infinity()));
        EXPECT_TRUE(AreRenderExposureScalesEquivalent(
            RVX_RENDER_EXPOSURE_MIN_SCALE,
            RVX_RENDER_EXPOSURE_MIN_SCALE));
        EXPECT_TRUE(AreRenderExposureScalesEquivalent(
            RVX_RENDER_EXPOSURE_MAX_SCALE,
            RVX_RENDER_EXPOSURE_MAX_SCALE));
        EXPECT_TRUE(AreRenderExposureScalesEquivalent(
            std::nextafter(RVX_RENDER_EXPOSURE_MIN_SCALE,
                           RVX_RENDER_EXPOSURE_MAX_SCALE),
            RVX_RENDER_EXPOSURE_MIN_SCALE));
        EXPECT_FALSE(AreRenderExposureScalesEquivalent(
            RVX_RENDER_EXPOSURE_MIN_SCALE * 2.0f,
            RVX_RENDER_EXPOSURE_MIN_SCALE));
        EXPECT_FALSE(AreRenderExposureScalesEquivalent(
            std::numeric_limits<float32>::infinity(),
            RVX_RENDER_EXPOSURE_MIN_SCALE));

        float32 exposureScale = 0.0f;
        EXPECT_TRUE(TryCalculateRenderExposureScale(0.0f, 0.0f,
                                                     exposureScale));
        EXPECT_FLOAT_EQ(exposureScale,
                        1.0f / RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION);

        EXPECT_TRUE(TryCalculateRenderExposureScale(2.0f, 1.0f,
                                                     exposureScale));
        EXPECT_FLOAT_EQ(exposureScale,
                        0.5f / RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION);
        EXPECT_TRUE(TryCalculateRenderExposureScale(
            RVX_RENDER_EXPOSURE_MAX_EV100,
            RVX_RENDER_EXPOSURE_MIN_COMPENSATION_EV,
            exposureScale));
        EXPECT_FLOAT_EQ(
            exposureScale,
            std::exp2(RVX_RENDER_EXPOSURE_MIN_COMPENSATION_EV -
                      RVX_RENDER_EXPOSURE_MAX_EV100) /
                RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION);
        EXPECT_GE(exposureScale, RVX_RENDER_EXPOSURE_MIN_SCALE);
        EXPECT_LE(exposureScale, RVX_RENDER_EXPOSURE_MAX_SCALE);
        EXPECT_TRUE(TryCalculateRenderExposureScale(
            RVX_RENDER_EXPOSURE_MIN_EV100,
            RVX_RENDER_EXPOSURE_MAX_COMPENSATION_EV,
            exposureScale));
        EXPECT_GE(exposureScale, RVX_RENDER_EXPOSURE_MIN_SCALE);
        EXPECT_LE(exposureScale, RVX_RENDER_EXPOSURE_MAX_SCALE);
        EXPECT_FALSE(TryCalculateRenderExposureScale(
            std::numeric_limits<float32>::quiet_NaN(), 0.0f, exposureScale));

        const uint64 validConfigurationHash =
            CalculateRenderExposureSettingsHash(RenderExposureSettings{});
        EXPECT_FALSE(MakeRenderExposureState(
                         RenderExposureMode::ManualEV100,
                         0.0f,
                         0.0f,
                         std::numeric_limits<float32>::infinity(),
                         1.0f,
                         validConfigurationHash)
                         .IsValid());
        EXPECT_FALSE(MakeRenderExposureState(
                         RenderExposureMode::ManualEV100,
                         0.0f,
                         0.0f,
                         std::nextafter(
                             RVX_RENDER_EXPOSURE_MAX_SCALE,
                             std::numeric_limits<float32>::infinity()),
                         1.0f,
                         validConfigurationHash)
                         .IsValid());

        RenderExposureSettings manualSettings;
        manualSettings.mode = RenderExposureMode::ManualEV100;
        manualSettings.manualEV100 = -5.0f;
        manualSettings.minEV100 = 0.0f;
        manualSettings.maxEV100 = 10.0f;
        ASSERT_TRUE(IsValidRenderExposureSettings(manualSettings));
        const RenderExposureResolveResult manual = ResolveRenderExposureState(
            manualSettings, 0.0f, 1.0f / 60.0f);
        ASSERT_TRUE(manual.Succeeded());
        EXPECT_FLOAT_EQ(manual.state.GetTargetEV100(), -5.0f);
        EXPECT_FLOAT_EQ(manual.state.GetAdaptedEV100(), -5.0f);

        RenderExposureSettings settings;
        settings.mode = RenderExposureMode::HistogramAuto;
        float32 ev100 = 0.0f;
        EXPECT_TRUE(TryCalculateHistogramEV100(
            settings,
            settings.histogramKeyValue *
                RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION,
            ev100));
        EXPECT_FLOAT_EQ(ev100, 0.0f);

        EXPECT_TRUE(TryCalculateHistogramEV100(
            settings,
            settings.histogramKeyValue *
                RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION * 4.0f,
            ev100));
        EXPECT_FLOAT_EQ(ev100, 2.0f);

        float32 blackEV100 = 0.0f;
        float32 floorEV100 = 0.0f;
        EXPECT_TRUE(TryCalculateHistogramEV100(settings, 0.0f,
                                                blackEV100));
        EXPECT_TRUE(TryCalculateHistogramEV100(
            settings, RVX_RENDER_EXPOSURE_MIN_METERED_LUMINANCE,
            floorEV100));
        EXPECT_FLOAT_EQ(blackEV100, floorEV100);

        float32 belowFloorEV100 = 0.0f;
        EXPECT_TRUE(TryCalculateHistogramEV100(
            settings, RVX_RENDER_EXPOSURE_MIN_METERED_LUMINANCE * 0.5f,
            belowFloorEV100));
        EXPECT_FLOAT_EQ(belowFloorEV100, floorEV100);

        EXPECT_TRUE(TryCalculateHistogramEV100(
            settings, RVX_RENDER_EXPOSURE_MAX_METERED_LUMINANCE, ev100));
        EXPECT_FALSE(TryCalculateHistogramEV100(
            settings,
            std::nextafter(RVX_RENDER_EXPOSURE_MAX_METERED_LUMINANCE,
                           std::numeric_limits<float32>::infinity()),
            ev100));

        EXPECT_FALSE(TryCalculateHistogramEV100(
            settings, -0.001f, ev100));
        EXPECT_FALSE(TryCalculateHistogramEV100(
            settings, std::numeric_limits<float32>::infinity(), ev100));
    }

    TEST(RenderContractsValidation,
         ExposureAdaptationUsesIndependentBrighteningAndDarkeningRates)
    {
        RenderExposureSettings settings;
        settings.mode = RenderExposureMode::HistogramAuto;
        settings.brighteningAdaptationSpeed = 4.0f;
        settings.darkeningAdaptationSpeed = 0.0f;

        float32 brightenedEV100 = 0.0f;
        EXPECT_TRUE(TryAdaptRenderExposureEV100(settings, 4.0f, 2.0f, 0.25f,
                                                 brightenedEV100));
        EXPECT_LT(brightenedEV100, 4.0f);
        EXPECT_GT(brightenedEV100, 2.0f);

        float32 darkenedEV100 = 0.0f;
        EXPECT_TRUE(TryAdaptRenderExposureEV100(settings, 4.0f, 6.0f, 0.25f,
                                                 darkenedEV100));
        EXPECT_FLOAT_EQ(darkenedEV100, 4.0f);

        settings.darkeningAdaptationSpeed = 4.0f;
        EXPECT_TRUE(TryAdaptRenderExposureEV100(settings, 4.0f, 6.0f, 0.25f,
                                                 darkenedEV100));
        EXPECT_GT(darkenedEV100, 4.0f);
        EXPECT_LT(darkenedEV100, 6.0f);

        float32 repeatedEV100 = 0.0f;
        EXPECT_TRUE(TryAdaptRenderExposureEV100(settings, 4.0f, 6.0f, 0.25f,
                                                 repeatedEV100));
        EXPECT_FLOAT_EQ(darkenedEV100, repeatedEV100);
        EXPECT_FALSE(TryAdaptRenderExposureEV100(
            settings, 4.0f, 6.0f,
            std::numeric_limits<float32>::quiet_NaN(), repeatedEV100));
    }

    TEST(RenderContractsValidation,
         ExposureStateIsImmutableAndResetsIncompatibleHistory)
    {
        RenderExposureSettings settings;
        settings.mode = RenderExposureMode::HistogramAuto;
        settings.brighteningAdaptationSpeed = 0.0f;
        settings.darkeningAdaptationSpeed = 0.0f;
        const float32 neutralLuminance =
            settings.histogramKeyValue *
            RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION;

        const RenderExposureResolveResult first = ResolveRenderExposureState(
            settings, neutralLuminance, 1.0f / 60.0f);
        ASSERT_TRUE(first.Succeeded());
        EXPECT_FALSE(first.UsedHistory());
        EXPECT_TRUE(first.state.IsValid());
        EXPECT_EQ(first.state.GetMode(), RenderExposureMode::HistogramAuto);
        EXPECT_FLOAT_EQ(first.state.GetTargetEV100(), 0.0f);
        EXPECT_FLOAT_EQ(first.state.GetAdaptedEV100(), 0.0f);
        EXPECT_FLOAT_EQ(first.state.GetExposureScale(),
                        1.0f / RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION);
        EXPECT_FLOAT_EQ(first.state.GetPreExposure(), 1.0f);
        EXPECT_NE(first.state.GetConfigurationHash(), 0U);

        const RenderExposureResolveResult second = ResolveRenderExposureState(
            settings, neutralLuminance * 4.0f, 1.0f / 60.0f, &first.state);
        ASSERT_TRUE(second.Succeeded());
        EXPECT_TRUE(second.UsedHistory());
        EXPECT_FLOAT_EQ(second.state.GetTargetEV100(), 2.0f);
        EXPECT_FLOAT_EQ(second.state.GetAdaptedEV100(), 0.0f);
        EXPECT_FLOAT_EQ(second.state.GetPreExposure(),
                        first.state.GetExposureScale());

        RenderExposureSettings changedSettings = settings;
        changedSettings.exposureCompensationEV = 1.0f;
        const RenderExposureResolveResult changed = ResolveRenderExposureState(
            changedSettings, neutralLuminance, 1.0f / 60.0f, &first.state);
        ASSERT_TRUE(changed.Succeeded());
        EXPECT_FALSE(changed.UsedHistory());
        EXPECT_FLOAT_EQ(changed.state.GetPreExposure(), 1.0f);
        EXPECT_NE(changed.state.GetConfigurationHash(),
                  first.state.GetConfigurationHash());

        const RenderExposureState invalidPrevious;
        const RenderExposureResolveResult invalid = ResolveRenderExposureState(
            settings, neutralLuminance, 1.0f / 60.0f, &invalidPrevious);
        EXPECT_EQ(invalid.code, RenderExposureResolveCode::InvalidPreviousState);
        EXPECT_FALSE(invalid.Succeeded());
    }

} // namespace
} // namespace RVX
