#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/WaterFeaturePass.h"
#include "Render/Renderer/RenderClipConvention.h"
#include "Render/Water/GerstnerReference.h"
#include "RenderContracts/RenderExposure.h"

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

namespace
{
    RVX::WaterGerstnerWaveSet MakeWaveSet()
    {
        RVX::WaterGerstnerWaveSet waves;
        waves.waveCount = 2;
        waves.waves[0] = {
            .direction = {1.0f, 0.0f},
            .amplitude = 0.75f,
            .wavelength = 8.0f,
            .steepness = 0.45f,
            .speed = 1.0f,
            .phaseOffsetRadians = 0.0f};
        waves.waves[1] = {
            .direction = {0.0f, 1.0f},
            .amplitude = 0.35f,
            .wavelength = 3.0f,
            .steepness = 0.25f,
            .speed = 0.75f,
            .phaseOffsetRadians = 0.4f};
        EXPECT_TRUE(RVX::CanonicalizeWaterGerstnerWaveSet(waves));
        return waves;
    }

    [[nodiscard]] RVX::WaterRenderSnapshot MakeReadySnapshot(bool complete = true)
    {
        RVX::WaterRenderSnapshot snapshot;
        snapshot.BeginBuild(23u);
        RVX::WaterRenderSnapshotItem item;
        item.header = {
            .channelId = RVX::RenderFeatureChannelId::Water,
            .schemaVersion = RVX::WaterRenderFeatureChannelDescriptor::SchemaVersion,
            .captureSequence = 23u,
            .configurationRevision = 7u,
            .source = {
                .sceneRuntimeId = 0x18u,
                .entitySlot = 9u,
                .entityGeneration = 4u,
            },
        };
        item.payloadStatus = RVX::WaterRenderSnapshotPayloadStatus::Ready;
        item.componentId = 0x1809u;
        item.surfaceAssetId = {.value = 1001u};
        item.materialAssetId = {.value = 1002u};
        item.worldPosition = {0.0f, 0.0f, 0.0f};
        // The packet contract requires conservative bounds for the full
        // vertical Gerstner envelope (0.75 + 0.35) and the 4 m water depth.
        item.worldBounds = RVX::AABB({-5.8f, -5.2f, -5.8f},
                                     {5.8f, 1.2f, 5.8f});
        item.size = {10.0f, 10.0f};
        item.depth = 4.0f;
        item.surfaceType = RVX::WaterRenderSnapshotSurfaceType::Ocean;
        item.simulationType = RVX::WaterRenderSnapshotSimulationType::Gerstner;
        item.gerstnerWaves = MakeWaveSet();
        item.shallowColor = {0.02f, 0.24f, 0.30f};
        item.deepColor = {0.005f, 0.04f, 0.10f};
        item.foamColor = {0.92f, 0.96f, 1.0f};
        item.absorptionCoefficient = {0.15f, 0.06f, 0.02f};
        item.transparency = 0.72f;
        item.reflectionStrength = 0.65f;
        item.refractionStrength = 0.08f;
        item.roughness = 0.08f;
        item.normalScale = 1.0f;
        item.foamIntensity = 1.0f;
        item.foamCrestThreshold = 0.62f;
        item.reflectionEnabled = true;
        item.refractionEnabled = true;
        item.foamEnabled = true;
        item.contentHash = item.ComputeContentHash();
        snapshot.items.push_back(std::move(item));
        if (complete)
        {
            snapshot.MarkComplete();
        }
        return snapshot;
    }

    [[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
    }
}

TEST(WaterGerstnerReferenceValidation, UsesFrozenTimeAndAnalyticNormal)
{
    const RVX::WaterGerstnerWaveSet waves = MakeWaveSet();
    RVX::WaterGerstnerReferenceSample first;
    RVX::WaterGerstnerReferenceSample repeat;
    RVX::WaterGerstnerReferenceSample advanced;
    ASSERT_TRUE(RVX::EvaluateWaterGerstnerReference(
        waves, {2.0f, -1.0f}, 4.0, first));
    ASSERT_TRUE(RVX::EvaluateWaterGerstnerReference(
        waves, {2.0f, -1.0f}, 4.0, repeat));
    ASSERT_TRUE(RVX::EvaluateWaterGerstnerReference(
        waves, {2.0f, -1.0f}, 4.5, advanced));
    EXPECT_EQ(first.displacement, repeat.displacement);
    EXPECT_EQ(first.normal, repeat.normal);
    EXPECT_NE(first.displacement, advanced.displacement);
    EXPECT_NEAR(RVX::length(first.normal), 1.0f, 1.0e-5f);
    EXPECT_GT(first.normal.y, 0.0f);
}

TEST(WaterGerstnerReferenceValidation, RejectsNonCanonicalWaves)
{
    RVX::WaterGerstnerWaveSet waves = MakeWaveSet();
    waves.contentHash ^= 1u;
    RVX::WaterGerstnerReferenceSample sample;
    EXPECT_FALSE(RVX::EvaluateWaterGerstnerReference(
        waves, {0.0f, 0.0f}, 0.0, sample));
}

TEST(WaterGerstnerReferenceValidation, ReferenceAndPackedGpuMirrorShareCanonicalTerms)
{
    const RVX::WaterGerstnerWaveSet waves = MakeWaveSet();
    // One ordinary and one deliberately large surface-local coordinate prove
    // that Render's reference path uses the RenderContracts formula rather
    // than an independently derived k/omega/time implementation.
    for (const RVX::Vec2 localPosition : {RVX::Vec2{2.0f, -1.0f},
                                          RVX::Vec2{8191.25f, -8190.75f}})
    {
        RVX::WaterGerstnerReferenceSample reference;
        RVX::WaterGerstnerSurfaceMathSample canonical;
        RVX::WaterGerstnerSurfaceMathSample gpuMirror;
        ASSERT_TRUE(RVX::EvaluateWaterGerstnerReference(
            waves, localPosition, 21600.125, reference));
        ASSERT_TRUE(RVX::EvaluateWaterGerstnerSurfaceMath(
            waves, localPosition, 21600.125, canonical));
        ASSERT_TRUE(RVX::EvaluateWaterGerstnerGpuFloatMirror(
            waves, localPosition, 21600.125, gpuMirror));
        EXPECT_EQ(reference.displacement, canonical.displacement);
        // The reference normal divides by a cached length while `normalize`
        // may use a separately rounded reciprocal. The canonical direction
        // must agree, but cross-platform float math cannot promise bit parity.
        EXPECT_GT(RVX::dot(reference.normal,
                           RVX::normalize(RVX::cross(canonical.tangentZ,
                                                     canonical.tangentX))),
                  0.999999f);
        EXPECT_NEAR(reference.displacement.x, gpuMirror.displacement.x, 0.05f);
        EXPECT_NEAR(reference.displacement.y, gpuMirror.displacement.y, 0.05f);
        EXPECT_NEAR(reference.displacement.z, gpuMirror.displacement.z, 0.05f);
        EXPECT_GT(RVX::dot(reference.normal,
                           RVX::normalize(RVX::cross(gpuMirror.tangentZ,
                                                     gpuMirror.tangentX))),
                  0.995f);
    }
}

TEST(WaterGerstnerReferenceValidation, ShaderAbiConsumesPackedWaveNumberRatherThanRecomputingIt)
{
    const std::filesystem::path root{RVX_SOURCE_DIR};
    const std::string waterImplementation = ReadTextFile(
        root / "Source/Runtime/Render/Private/Passes/WaterFeaturePass.cpp");
    const std::string waterShader = ReadTextFile(
        root / "Source/Runtime/Render/Shaders/Water/WaterSurface.hlsl");
    const std::string mathContract = ReadTextFile(
        root / "Source/Runtime/RenderContracts/Include/RenderContracts/WaterGerstnerMath.h");
    EXPECT_NE(waterImplementation.find("PackWaterGerstnerWaveFrameTermsForGpu"),
              std::string::npos);
    EXPECT_NE(waterImplementation.find(
                  "wave.steepness, gpuTerms.waveNumber, gpuTerms.temporalPhase, 1.0f"),
              std::string::npos);
    EXPECT_NE(waterShader.find(
                  "const float k = max(steepnessTemporalPhase.y, 1.0e-6f);"),
              std::string::npos);
    EXPECT_EQ(waterShader.find("RVX_WATER_TWO_PI / wavelength"), std::string::npos);
    EXPECT_NE(mathContract.find("sole GPU quantization boundary"), std::string::npos);
    EXPECT_NE(mathContract.find("EvaluateWaterGerstnerGpuFloatMirror"),
              std::string::npos);
}

TEST(WaterOpticalReferenceValidation, ZeroAbsorptionAndZeroResponsePreserveOpaqueSnapshot)
{
    const RVX::Vec3 opaque{2.0f, 0.5f, 0.25f};
    const RVX::Vec3 transmitted = RVX::EvaluateWaterTransmission(
        opaque, {0.0f, 0.0f, 0.0f}, 42.0f);
    EXPECT_EQ(transmitted, opaque);
    // WaterSurface writes the fully composed pixel through an opaque RT0. A
    // zero response must stay exact; a second alpha blend would darken it.
    EXPECT_EQ(RVX::ComposeWaterSurfaceColor(
        opaque, {0.1f, 0.2f, 0.3f}, 0.0f), opaque);
}

TEST(WaterPreExposureValidation,
     AuthoredWaterRadianceReconstructsPhysicalResponseAtEV0AndEV85)
{
    // Mirror WaterSurface.hlsl: the opaque/refraction sample already carries
    // the producer scale, while every authored water term enters it once.
    constexpr float opaquePhysical = 3.75f;
    constexpr float authoredWaterPhysical = 1.25f;
    constexpr float transmission = 0.35f;
    const float expectedPhysical = opaquePhysical * transmission +
        authoredWaterPhysical * (1.0f - transmission);
    for (const float ev100 : std::array<float, 2>{0.0f, 8.5f})
    {
        float producerPreExposure = 0.0f;
        ASSERT_TRUE(RVX::TryCalculateRenderExposureScale(
            ev100, 0.0f, producerPreExposure));
        ASSERT_GT(producerPreExposure, 0.0f);

        const float opaqueSnapshotPreExposed = opaquePhysical * producerPreExposure;
        const float authoredWaterPreExposed =
            authoredWaterPhysical * producerPreExposure;
        const float composedPreExposed =
            opaqueSnapshotPreExposed * transmission +
            authoredWaterPreExposed * (1.0f - transmission);
        EXPECT_NEAR(composedPreExposed / producerPreExposure,
                    expectedPhysical, 1.0e-5f);
    }
}

TEST(WaterPreExposureValidation,
     RefractionSnapshotIsNotScaledAgainWhenComposingWaterResponse)
{
    constexpr float opaquePhysical = 4.0f;
    constexpr float authoredWaterPhysical = 1.5f;
    constexpr float transmission = 0.4f;
    float producerPreExposure = 0.0f;
    ASSERT_TRUE(RVX::TryCalculateRenderExposureScale(
        8.5f, 0.0f, producerPreExposure));

    const float refractedSnapshot = opaquePhysical * producerPreExposure;
    const float correctPreExposed = refractedSnapshot * transmission +
        authoredWaterPhysical * producerPreExposure * (1.0f - transmission);
    const float doubleScaledSnapshot = refractedSnapshot * producerPreExposure *
        transmission + authoredWaterPhysical * producerPreExposure *
        (1.0f - transmission);
    const float expectedPhysical = opaquePhysical * transmission +
        authoredWaterPhysical * (1.0f - transmission);

    EXPECT_NEAR(correctPreExposed / producerPreExposure,
                expectedPhysical, 1.0e-5f);
    EXPECT_GT(std::abs(doubleScaledSnapshot / producerPreExposure -
                       expectedPhysical),
              0.5f);
}

TEST(WaterPreExposureValidation,
     WaterRecordFreezesAndBindsTheExactCommittedExposureIdentity)
{
    const std::filesystem::path root{RVX_SOURCE_DIR};
    const std::string recordContext = ReadTextFile(
        root / "Source/Runtime/Render/Include/Render/Passes/RenderPassRecordContext.h");
    const std::string deferredAdapter = ReadTextFile(
        root / "Source/Runtime/Render/Private/Features/DeferredRecipeFeatureAdapter.cpp");
    const std::string forwardAdapter = ReadTextFile(
        root / "Source/Runtime/Render/Private/Features/LegacyForwardFeatureAdapter.cpp");
    const std::string water = ReadTextFile(
        root / "Source/Runtime/Render/Private/Passes/WaterFeaturePass.cpp");
    const std::string shader = ReadTextFile(
        root / "Source/Runtime/Render/Shaders/Water/WaterSurface.hlsl");

    EXPECT_NE(recordContext.find("RGBufferHandle exposureState{};"),
              std::string::npos);
    EXPECT_NE(recordContext.find("uint64 exposureStateContentHash = 0;"),
              std::string::npos);
    EXPECT_NE(recordContext.find("HasCurrentGraphProvenance(exposureState, expected)"),
              std::string::npos);
    for (const std::string* adapter : {&deferredAdapter, &forwardAdapter})
    {
        EXPECT_NE(adapter->find(
                      "outInput.exposureState = context.view.exposureState;"),
                  std::string::npos);
        EXPECT_NE(adapter->find(
                      "outInput.exposureStateContentHash = context.view.exposureStateContentHash;"),
                  std::string::npos);
    }
    EXPECT_NE(water.find("IsSameBufferHandle(data.waterSurface.exposureState,"),
              std::string::npos);
    EXPECT_NE(water.find("data.execution.view.exposureState)"),
              std::string::npos);
    EXPECT_NE(water.find("data.exposureStateReadHandle = builder.Read("),
              std::string::npos);
    EXPECT_NE(water.find(".BindBuffer(RVX_WATER_EXPOSURE_STATE_BINDING, exposureState)"),
              std::string::npos);
    EXPECT_NE(shader.find("#include \"../Include/Exposure.hlsli\""),
              std::string::npos);
    EXPECT_NE(shader.find(
                  "StructuredBuffer<RVXExposureState> gExposureState : register(t5, space0)"),
              std::string::npos);
    EXPECT_NE(shader.find("RVXExposureProducerScale(exposureState)"),
              std::string::npos);
}

TEST(WaterPreExposureValidation,
     HlslScalesOnlyAuthoredRadianceAndNeverTheOpaqueRefractionSnapshot)
{
    const std::filesystem::path root{RVX_SOURCE_DIR};
    const std::string shader = ReadTextFile(
        root / "Source/Runtime/Render/Shaders/Water/WaterSurface.hlsl");

    EXPECT_NE(shader.find("const float3 refracted = gOpaqueSceneSnapshot.Sample("),
              std::string::npos);
    EXPECT_NE(shader.find("* producerPreExposure;"), std::string::npos);
    EXPECT_NE(shader.find("FoamColorAndParameters.xyz * producerPreExposure"),
              std::string::npos);
    EXPECT_EQ(shader.find("refracted * producerPreExposure"), std::string::npos);
    EXPECT_NE(shader.find("RVXExposureStateIsValid(exposureState)"),
              std::string::npos);
    EXPECT_NE(shader.find("clip(-1.0f);"), std::string::npos);
}

TEST(WaterPassPriorityValidation, WaterRunsAfterSkyBeforeTransparency)
{
    EXPECT_GT(RVX::PassPriority::Water, RVX::PassPriority::Skybox);
    EXPECT_LT(RVX::PassPriority::Water, RVX::PassPriority::Transparent);
}

TEST(RenderClipConventionValidation, VulkanNegatesOnlyTheOutputClipYRow)
{
    RVX::Mat4 semantic = RVX::Mat4Identity();
    for (RVX::uint32 column = 0; column < 4u; ++column)
    {
        semantic[column][1] = static_cast<float>(column + 1u) * 0.25f;
    }

    const RVX::Mat4 dx12 = RVX::ApplyBackendClipConvention(
        semantic, RVX::RHIBackendType::DX12);
    const RVX::Mat4 metal = RVX::ApplyBackendClipConvention(
        semantic, RVX::RHIBackendType::Metal);
    const RVX::Mat4 vulkan = RVX::ApplyBackendClipConvention(
        semantic, RVX::RHIBackendType::Vulkan);

    EXPECT_EQ(dx12, semantic);
    EXPECT_EQ(metal, semantic);
    for (RVX::uint32 column = 0; column < 4u; ++column)
    {
        for (RVX::uint32 row = 0; row < 4u; ++row)
        {
            const float expected = row == 1u
                ? -semantic[column][row]
                : semantic[column][row];
            EXPECT_FLOAT_EQ(vulkan[column][row], expected);
        }
    }
}

TEST(RenderClipConventionValidation, WaterAndParticleUseTheSingleSharedPolicy)
{
    const std::filesystem::path root{RVX_SOURCE_DIR};
    const std::string convention = ReadTextFile(
        root / "Source/Runtime/Render/Include/Render/Renderer/RenderClipConvention.h");
    const std::string pipelineCache = ReadTextFile(
        root / "Source/Runtime/Render/Private/PipelineCache.cpp");
    const std::string water = ReadTextFile(
        root / "Source/Runtime/Render/Private/Passes/WaterFeaturePass.cpp");
    const std::string particles = ReadTextFile(
        root / "Source/Runtime/Render/Private/Passes/ParticleFeaturePass.cpp");

    EXPECT_NE(convention.find("ApplyBackendClipConvention"), std::string::npos);
    EXPECT_NE(pipelineCache.find("ApplyBackendClipConvention("), std::string::npos);
    EXPECT_NE(water.find("ApplyBackendClipConvention(\n                view.jitteredViewProjectionMatrix"),
              std::string::npos);
    EXPECT_NE(particles.find("ApplyBackendClipConvention(\n                view.jitteredViewProjectionMatrix"),
              std::string::npos);
    EXPECT_EQ(water.find("constants.viewProjection = view.jitteredViewProjectionMatrix"),
              std::string::npos);
    EXPECT_EQ(particles.find("constants.viewProjection = view.jitteredViewProjectionMatrix"),
              std::string::npos);
}

TEST(WaterFeaturePassValidation, EmptyAndDisabledSnapshotsDoNotScheduleWater)
{
    RVX::WaterFeaturePass pass;
    RVX::WaterRenderSnapshot empty;
    pass.SetSnapshot(&empty);
    EXPECT_FALSE(pass.GetRequirements().requested);
    EXPECT_TRUE(pass.GetStats().supported);
    EXPECT_FALSE(pass.GetStats().enabled);

    RVX::WaterRenderSnapshot disabled;
    disabled.BeginBuild(24u);
    disabled.items.emplace_back();
    disabled.MarkComplete();
    pass.SetSnapshot(&disabled);
    EXPECT_FALSE(pass.GetRequirements().requested);
    EXPECT_TRUE(pass.GetStats().supported);
    EXPECT_FALSE(pass.GetStats().graphPassScheduled);
}

TEST(WaterFeaturePassValidation, ValidV3SnapshotRequiresDistinctOpaqueDepthAndReactiveInputs)
{
    RVX::WaterFeaturePass pass;
    RVX::WaterRenderSnapshot snapshot = MakeReadySnapshot();
    ASSERT_TRUE(snapshot.items.front().IsStructurallyValid());
    pass.SetSnapshot(&snapshot);

    const RVX::WaterSurfaceRequirements requirements = pass.GetRequirements();
    EXPECT_TRUE(requirements.requested);
    EXPECT_TRUE(requirements.opaqueSceneSnapshotRequired);
    EXPECT_TRUE(requirements.depthSnapshotRequired);
    EXPECT_TRUE(requirements.reactiveMaskRequired);
    // A request without a graph/device must remain fail-closed rather than
    // being advertised as an enabled fallback path.
    EXPECT_FALSE(pass.GetStats().supported);
    EXPECT_FALSE(pass.GetStats().enabled);
}

TEST(WaterFeaturePassValidation,
     SnapshotCopiesMustCompleteBeforeAnyWaterDrawMayExecute)
{
    RVX::WaterSurfaceRecordInput input;
    input.opaqueSceneSnapshot.receipt =
        std::make_shared<RVX::OpaqueSceneSnapshotCopyReceipt>();
    input.depthSnapshot.receipt =
        std::make_shared<RVX::SceneDepthSnapshotCopyReceipt>();
    ASSERT_FALSE(RVX::AreWaterSnapshotCopiesReady(input));

    input.opaqueSceneSnapshot.receipt->copied.store(
        true, std::memory_order_release);
    EXPECT_FALSE(RVX::AreWaterSnapshotCopiesReady(input));
    input.depthSnapshot.receipt->copied.store(
        true, std::memory_order_release);
    EXPECT_TRUE(RVX::AreWaterSnapshotCopiesReady(input));

    const std::filesystem::path root{RVX_SOURCE_DIR};
    const std::string source = ReadTextFile(
        root / "Source/Runtime/Render/Private/Passes/WaterFeaturePass.cpp");
    const size_t receiptGate = source.find(
        "if (!AreWaterSnapshotCopiesReady(data.waterSurface))");
    const size_t beginRenderPass = source.find(
        "commands.BeginRenderPass(renderPassDesc)", receiptGate);
    ASSERT_NE(receiptGate, std::string::npos);
    ASSERT_NE(beginRenderPass, std::string::npos);
    EXPECT_LT(receiptGate, beginRenderPass);
}

TEST(WaterFeaturePassValidation, CorruptSchemaHashAndIncompleteSnapshotsFailClosed)
{
    RVX::WaterFeaturePass pass;

    RVX::WaterRenderSnapshot corruptHash = MakeReadySnapshot();
    corruptHash.items.front().contentHash ^= 1u;
    pass.SetSnapshot(&corruptHash);
    EXPECT_TRUE(pass.GetStats().requested);
    EXPECT_FALSE(pass.GetStats().supported);
    EXPECT_FALSE(pass.GetStats().enabled);

    RVX::WaterRenderSnapshot corruptSchema = MakeReadySnapshot();
    ++corruptSchema.items.front().header.schemaVersion;
    pass.SetSnapshot(&corruptSchema);
    EXPECT_TRUE(pass.GetStats().requested);
    EXPECT_FALSE(pass.GetStats().supported);
    EXPECT_FALSE(pass.GetStats().enabled);

    RVX::WaterRenderSnapshot incomplete = MakeReadySnapshot(false);
    pass.SetSnapshot(&incomplete);
    EXPECT_TRUE(pass.GetStats().requested);
    EXPECT_FALSE(pass.GetStats().supported);
    EXPECT_NE(pass.GetStats().unsupportedReason.find("incomplete"),
              std::string::npos);
}

TEST(WaterFeaturePassValidation, RendererCompositionAndCookedShaderCatalogAreHonest)
{
    const std::filesystem::path root{RVX_SOURCE_DIR};
    EXPECT_FALSE(std::filesystem::exists(
        root / "Source/Runtime/Render/Private/Passes/WaterPass.cpp"));

    const std::string renderCMake = ReadTextFile(
        root / "Source/Runtime/Render/CMakeLists.txt");
    EXPECT_NE(renderCMake.find("WaterFeaturePass.cpp"), std::string::npos);
    EXPECT_EQ(renderCMake.find("Private/Passes/WaterPass.cpp"), std::string::npos);

    const std::string defaultComposition = ReadTextFile(
        root / "Source/Runtime/Render/Private/Features/DefaultForwardComposition.cpp");
    const std::string renderComposition = ReadTextFile(
        root / "Source/Runtime/Render/Private/Features/DefaultRenderComposition.cpp");
    const std::string passBundle = ReadTextFile(
        root / "Source/Runtime/Render/Private/Features/DefaultRenderPassBundle.cpp");
    const std::string forwardAdapter = ReadTextFile(
        root / "Source/Runtime/Render/Private/Features/LegacyForwardFeatureAdapter.cpp");
    const std::string deferredAdapter = ReadTextFile(
        root / "Source/Runtime/Render/Private/Features/DeferredRecipeFeatureAdapter.cpp");
    const std::string depthSnapshotBuilder = ReadTextFile(
        root / "Source/Runtime/Render/Private/Features/SceneDepthSnapshotBuilder.cpp");
    EXPECT_NE(defaultComposition.find("CreateDefaultRenderPassBundle(dependencies)"),
              std::string::npos);
    EXPECT_NE(renderComposition.find("m_passBundle(std::make_unique<DefaultRenderPassBundle>())"),
              std::string::npos);
    EXPECT_NE(passBundle.find("auto waterFeaturePass = std::make_unique<WaterFeaturePass>();"),
              std::string::npos);
    EXPECT_NE(passBundle.find("waterFeaturePass->SetSnapshot"),
              std::string::npos);
    EXPECT_NE(passBundle.find("waterFeaturePass->SetShaderProgramProvider"),
              std::string::npos);
    EXPECT_NE(forwardAdapter.find("PassPriority::Water"), std::string::npos);
    EXPECT_NE(forwardAdapter.find("BuildWaterSurfaceRecordInput"), std::string::npos);
    EXPECT_NE(deferredAdapter.find("PassPriority::Water"), std::string::npos);
    EXPECT_NE(deferredAdapter.find("BuildWaterSurfaceRecordInput"), std::string::npos);
    EXPECT_NE(forwardAdapter.find("BuildSceneDepthSnapshot("), std::string::npos);
    EXPECT_NE(deferredAdapter.find("BuildSceneDepthSnapshot("), std::string::npos);
    const std::string legacyWaterDepthCopyName =
        "WaterDepth" "SnapshotCopy";
    EXPECT_EQ(forwardAdapter.find(legacyWaterDepthCopyName), std::string::npos);
    EXPECT_EQ(deferredAdapter.find(legacyWaterDepthCopyName), std::string::npos);
    EXPECT_NE(depthSnapshotBuilder.find("SceneDepthSnapshotCopy"),
              std::string::npos);
    EXPECT_NE(depthSnapshotBuilder.find("RHIResourceState::CopySource"),
              std::string::npos);

    const std::string waterImplementation = ReadTextFile(
        root / "Source/Runtime/Render/Private/Passes/WaterFeaturePass.cpp");
    const std::string waterShader = ReadTextFile(
        root / "Source/Runtime/Render/Shaders/Water/WaterSurface.hlsl");
    EXPECT_NE(waterImplementation.find("RVX_WATER_GRID_RESOLUTION = 64u"),
              std::string::npos);
    EXPECT_EQ(waterImplementation.find("item.resolution"), std::string::npos);
    EXPECT_NE(waterImplementation.find("if (m_initializationAttempted)"),
              std::string::npos);
    EXPECT_EQ(waterImplementation.find("WaitIdle("), std::string::npos);
    EXPECT_NE(waterImplementation.find("depthSnapshot.texture"),
              std::string::npos);
    EXPECT_NE(waterImplementation.find("depthSnapshot.receipt"),
              std::string::npos);
    EXPECT_NE(waterShader.find("WaveSteepnessTemporalPhase"), std::string::npos);
    EXPECT_NE(waterShader.find("fmod(k * dot"), std::string::npos);
    EXPECT_NE(waterShader.find("DetailTemporalPhases"), std::string::npos);
    EXPECT_EQ(waterShader.find("CameraPositionAndTime"), std::string::npos);
    EXPECT_EQ(waterShader.find("const float time ="), std::string::npos);
    EXPECT_NE(waterShader.find("EvaluateGerstner(localXZ"), std::string::npos);
    EXPECT_NE(waterShader.find("const float2 worldXZ = localXZ +"),
              std::string::npos);
    EXPECT_NE(waterShader.find("const float roughness = saturate("),
              std::string::npos);
    EXPECT_NE(waterShader.find("reflectionLobe"), std::string::npos);
    EXPECT_NE(waterShader.find(
                  "Texture3D<float4> gFroxelVolumetricLighting : register(t4, space0)"),
              std::string::npos);
    EXPECT_NE(waterShader.find("gFroxelVolumetricLighting.SampleLevel("),
              std::string::npos);
    EXPECT_NE(waterShader.find(
                  "const float3 volumetricComposed = refracted +"),
              std::string::npos);
    EXPECT_NE(waterShader.find("output.Color = float4(volumetricComposed, 1.0f)"),
              std::string::npos);

    const std::string shaderCatalog = ReadTextFile(
        root / "Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json");
    EXPECT_NE(shaderCatalog.find("Water/WaterSurface.hlsl"), std::string::npos);
    EXPECT_NE(shaderCatalog.find("\"VSMain\""), std::string::npos);
    EXPECT_NE(shaderCatalog.find("\"PSMain\""), std::string::npos);
    EXPECT_NE(shaderCatalog.find("\"DX11\",\"DX12\",\"Vulkan\",\"Metal\",\"OpenGL\""),
              std::string::npos);
}

TEST(WaterGerstnerReferenceValidation, ReducesHugeFiniteTimesWithoutFloatNarrowing)
{
    RVX::WaterGerstnerReferenceSample first;
    RVX::WaterGerstnerReferenceSample repeat;
    const double longRunningTime = 1.0e100;
    ASSERT_TRUE(RVX::EvaluateWaterGerstnerReference(
        MakeWaveSet(), {0.75f, -1.5f}, longRunningTime, first));
    ASSERT_TRUE(RVX::EvaluateWaterGerstnerReference(
        MakeWaveSet(), {0.75f, -1.5f}, longRunningTime, repeat));
    EXPECT_EQ(first.displacement, repeat.displacement);
    EXPECT_EQ(first.normal, repeat.normal);
    EXPECT_NEAR(RVX::length(first.normal), 1.0f, 1.0e-5f);
}

TEST(WaterDetailNormalValidation, ReducesEachScrollRateWithoutLongRunDiscontinuity)
{
    RVX::Vec4 first;
    RVX::Vec4 repeat;
    constexpr double longRunningTime = 1.0e100;
    ASSERT_TRUE(RVX::EvaluateWaterDetailTemporalPhases(longRunningTime, first));
    ASSERT_TRUE(RVX::EvaluateWaterDetailTemporalPhases(longRunningTime, repeat));
    EXPECT_EQ(first, repeat);
    constexpr float pi = 3.14159265358979323846f;
    EXPECT_LE(std::abs(first.x), pi);
    EXPECT_LE(std::abs(first.y), pi);
    EXPECT_LE(std::abs(first.z), pi);
    EXPECT_LE(std::abs(first.w), pi);
}

TEST(WaterDetailNormalValidation, HasNoSharedTimeWrapDiscontinuity)
{
    // The old shared `remainder(seconds, 2pi)` wrapped at pi before applying
    // non-integral scroll rates. Each independently reduced phase must remain
    // continuous across that former boundary.
    RVX::Vec4 before;
    RVX::Vec4 after;
    constexpr double pi = 3.14159265358979323846;
    constexpr double epsilon = 1.0e-4;
    ASSERT_TRUE(RVX::EvaluateWaterDetailTemporalPhases(pi - epsilon, before));
    ASSERT_TRUE(RVX::EvaluateWaterDetailTemporalPhases(pi + epsilon, after));
    EXPECT_NEAR(after.x - before.x, 0.07f * 2.0f * static_cast<float>(epsilon),
                1.0e-5f);
    EXPECT_NEAR(after.y - before.y, -0.05f * 2.0f * static_cast<float>(epsilon),
                1.0e-5f);
    EXPECT_NEAR(after.z - before.z, -0.11f * 2.0f * static_cast<float>(epsilon),
                1.0e-5f);
    EXPECT_NEAR(after.w - before.w, 0.09f * 2.0f * static_cast<float>(epsilon),
                1.0e-5f);
}
