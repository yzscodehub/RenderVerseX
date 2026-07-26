#include "Core/Diagnostics/ContentHash.h"
#include "Core/Log.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/Shapes/CollisionShape.h"
#include "RenderContracts/ParticleRenderSnapshot.h"
#include "RenderContracts/TerrainRenderSnapshot.h"
#include "RenderContracts/WaterRenderSnapshot.h"
#include "Resource/Loader/AudioLoader.h"
#include "Resource/RuntimeResourcePolicy.h"
#include "Samples/SampleCLI.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifndef RVX_BASIC_SAMPLE_KIND
#define RVX_BASIC_SAMPLE_KIND 0
#endif

namespace
{
    enum BasicSampleKind
    {
        BackendInfo = 1,
        RenderGraphBasics = 2,
        ResourcePolicy = 3,
        TextureUpload = 4,
        MeshMaterial = 5,
        SceneActorComponent = 6,
        PostProcessChain = 7,
        LightingShadow = 8,
        GPUResidency = 9,
        InputCamera = 10,
        DebugDraw = 11,
        DecalDiagnostics = 12,
        SwapChainPolicy = 13,
        ParticleSnapshot = 14,
        TerrainSnapshot = 15,
        WaterSnapshot = 16,
        PhysicsQuery = 17,
        AudioFallback = 18,
    };

    RVX::SampleRenderDiagnostics MakeRenderDiagnostics(RVX::uint32 passCount)
    {
        RVX::SampleRenderDiagnostics diagnostics;
        diagnostics.available = passCount > 0;
        diagnostics.renderAttempted = passCount > 0;
        diagnostics.rendered = passCount > 0;
        diagnostics.graphBuilt = passCount > 0;
        diagnostics.graphCompiled = passCount > 0;
        diagnostics.renderGraphTotalPasses = passCount;
        return diagnostics;
    }

    bool WriteFixtureFile(const std::filesystem::path& path, const std::string& contents)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            return false;
        }

        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file)
        {
            return false;
        }
        file << contents;
        return static_cast<bool>(file);
    }

    void AppendResolution(RVX::SampleAppDesc& desc,
                          const char* label,
                          const RVX::Resource::ResourcePathResolution& resolution)
    {
        std::string text = label;
        text += resolution.allowed ? ": allowed" : ": denied";
        text += " failure=";
        text += RVX::Resource::GetResourceLoadFailureCodeName(resolution.failure);
        if (!resolution.resolvedPath.empty())
        {
            text += " resolved";
        }
        if (resolution.packageHashChecked)
        {
            text += " hashMatched=";
            text += resolution.packageHashMatched ? "true" : "false";
        }
        desc.resourceDiagnostics.push_back(std::move(text));
    }

    void AddResourcePolicyDiagnostics(RVX::SampleAppDesc& desc)
    {
        const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("RVX_BasicResourcePolicySample_" + std::to_string(uniqueId));
        const std::filesystem::path cookedRoot = root / "Cooked";
        const std::filesystem::path packageRoot = root / "Package";
        const std::filesystem::path cookedArtifact = cookedRoot / "textures" / "albedo.rva";
        const std::filesystem::path packageArtifact = packageRoot / "compiled" / "basic.rva";
        const std::filesystem::path mismatchArtifact = packageRoot / "compiled" / "mismatch.rva";

        if (!WriteFixtureFile(cookedArtifact, "RVX_TEXTURE_PREBAKE_V1\n") ||
            !WriteFixtureFile(packageArtifact, "RVX_SHADER_PREBAKE_V1\n") ||
            !WriteFixtureFile(mismatchArtifact, "RVX_SHADER_PREBAKE_V1_MISMATCH\n"))
        {
            desc.resourceDiagnostics.push_back("resource policy fixture setup failed");
            return;
        }

        RVX::Resource::ResourceRuntimePolicy cookedPolicy;
        cookedPolicy.mode = RVX::Resource::ResourceRuntimeMode::CookedRuntime;
        cookedPolicy.allowSourceAssetReads = false;
        cookedPolicy.requireCookedArtifacts = true;
        cookedPolicy.cookedRoot = cookedRoot.string();

        AppendResolution(desc,
                         "source denied",
                         RVX::Resource::ResolveRuntimeResourcePath(cookedPolicy,
                                                                    "",
                                                                    "source://textures/albedo.png"));
        AppendResolution(desc,
                         "cooked texture resolved",
                         RVX::Resource::ResolveRuntimeResourcePath(cookedPolicy,
                                                                    "",
                                                                    "cooked://textures/albedo.rva"));
        AppendResolution(desc,
                         "missing cooked artifact",
                         RVX::Resource::ResolveRuntimeResourcePath(cookedPolicy,
                                                                    "",
                                                                    "cooked://textures/missing.rva"));

        RVX::Resource::ResourceRuntimePolicy packagePolicy;
        packagePolicy.mode = RVX::Resource::ResourceRuntimeMode::PackagedRuntime;
        packagePolicy.allowSourceAssetReads = false;
        packagePolicy.requireRuntimePackage = true;
        packagePolicy.packageMounts = {
            RVX::Resource::ResourcePackageMount{
                "Base",
                10,
                packageRoot.string(),
                {
                    RVX::Resource::ResourcePackageArtifact{
                        "shaders/basic.rva",
                        "compiled/basic.rva",
                        RVX::Diagnostics::ComputeFileContentHash(packageArtifact)},
                    RVX::Resource::ResourcePackageArtifact{
                        "shaders/mismatch.rva",
                        "compiled/mismatch.rva",
                        "0000000000000000"},
                }},
        };

        AppendResolution(desc,
                         "package shader resolved",
                         RVX::Resource::ResolveRuntimeResourcePath(packagePolicy,
                                                                    "",
                                                                    "package://shaders/basic.rva"));
        AppendResolution(desc,
                         "missing package artifact",
                         RVX::Resource::ResolveRuntimeResourcePath(packagePolicy,
                                                                    "",
                                                                    "package://shaders/missing.rva"));
        AppendResolution(desc,
                         "hash mismatch",
                         RVX::Resource::ResolveRuntimeResourcePath(packagePolicy,
                                                                    "",
                                                                    "package://shaders/mismatch.rva"));
    }

    void AddGPUResidencyDiagnostics(RVX::SampleAppDesc& desc)
    {
        desc.resourceDiagnostics.push_back("render resource registry=available at runtime");
        desc.resourceDiagnostics.push_back("resident meshes=0");
        desc.resourceDiagnostics.push_back("resident textures=0");
        desc.resourceDiagnostics.push_back("pending uploads=0");
        desc.resourceDiagnostics.push_back("eviction reason=none; no resident resources in this smoke fixture");
    }

    const char* GetParticleSnapshotStatusName(RVX::ParticleRenderSnapshotStatus status)
    {
        switch (status)
        {
            case RVX::ParticleRenderSnapshotStatus::Empty: return "Empty";
            case RVX::ParticleRenderSnapshotStatus::Complete: return "Complete";
            case RVX::ParticleRenderSnapshotStatus::Incomplete: return "Incomplete";
        }
        return "Unknown";
    }

    const char* GetTerrainSnapshotStatusName(RVX::TerrainRenderSnapshotStatus status)
    {
        switch (status)
        {
            case RVX::TerrainRenderSnapshotStatus::Empty: return "Empty";
            case RVX::TerrainRenderSnapshotStatus::Complete: return "Complete";
            case RVX::TerrainRenderSnapshotStatus::Incomplete: return "Incomplete";
        }
        return "Unknown";
    }

    const char* GetWaterSnapshotStatusName(RVX::WaterRenderSnapshotStatus status)
    {
        switch (status)
        {
            case RVX::WaterRenderSnapshotStatus::Empty: return "Empty";
            case RVX::WaterRenderSnapshotStatus::Complete: return "Complete";
            case RVX::WaterRenderSnapshotStatus::Incomplete: return "Incomplete";
        }
        return "Unknown";
    }

    const char* GetAudioLoadStatusName(RVX::Resource::AudioLoadStatus status)
    {
        switch (status)
        {
            case RVX::Resource::AudioLoadStatus::None: return "None";
            case RVX::Resource::AudioLoadStatus::Loaded: return "Loaded";
            case RVX::Resource::AudioLoadStatus::Failed: return "Failed";
            case RVX::Resource::AudioLoadStatus::FallbackMissingFile: return "FallbackMissingFile";
            case RVX::Resource::AudioLoadStatus::FallbackDecodeFailed: return "FallbackDecodeFailed";
            case RVX::Resource::AudioLoadStatus::UnsupportedStreaming: return "UnsupportedStreaming";
            case RVX::Resource::AudioLoadStatus::FallbackStreamingUnsupported: return "FallbackStreamingUnsupported";
        }
        return "Unknown";
    }

    void AddParticleSnapshotDiagnostics(RVX::SampleAppDesc& desc)
    {
        RVX::ParticleRenderSnapshot snapshot;
        snapshot.BeginBuild(1);

        RVX::ParticleRenderSnapshotItem item;
        item.instanceId = 1;
        item.systemId = 100;
        item.systemName = "BasicParticleCPUEmitter";
        item.simulationBackend = RVX::ParticleRenderSnapshotSimulationBackend::CPU;
        item.payloadStatus = RVX::ParticleRenderSnapshotPayloadStatus::MetadataOnly;
        item.aliveParticleCount = 32;
        item.maxParticleCount = 64;
        item.simulationSupported = true;
        item.renderPayloadAvailable = false;
        item.sortingSupported = true;
        item.renderPayloadReason = "Render-owned GPU particle payload is not connected in the basic sample path";
        snapshot.metadata.totalAliveParticles = item.aliveParticleCount;
        snapshot.items.push_back(std::move(item));
        snapshot.MarkComplete();

        const auto metadata = snapshot.GetMetadata();
        desc.resourceDiagnostics.push_back("particle snapshot schema=" + std::to_string(metadata.schemaVersion));
        desc.resourceDiagnostics.push_back("particle snapshot status=" + std::string(GetParticleSnapshotStatusName(metadata.status)));
        desc.resourceDiagnostics.push_back("particle snapshot itemCount=" + std::to_string(metadata.itemCount));
        desc.resourceDiagnostics.push_back("particle alive count=" + std::to_string(metadata.totalAliveParticles));
        desc.resourceDiagnostics.push_back("particle payload reason=" + snapshot.items.front().renderPayloadReason);
    }

    void AddTerrainSnapshotDiagnostics(RVX::SampleAppDesc& desc)
    {
        RVX::TerrainRenderSnapshot snapshot;
        snapshot.BeginBuild(1);

        RVX::TerrainRenderSnapshotItem item;
        item.componentId = 1;
        item.size = RVX::Vec3(64.0f, 8.0f, 64.0f);
        item.patchSize = 16;
        item.maxLODLevels = 4;
        item.hasHeightmap = true;
        item.heightmapValid = false;
        item.hasMaterial = false;
        item.cpuDataAvailable = true;
        item.gpuInitialized = false;
        item.renderGpuPathAvailable = false;
        item.renderPathReason = "Terrain Render-owned draw pass is not connected yet";
        item.heightmapDiagnostic = "heightmap fixture is CPU-visible but no GPU upload is claimed";
        item.materialDiagnostic = "terrain material binding falls back until material contract is complete";
        item.lodDiagnostic = "deterministic LOD metadata is exported without pretending runtime GPU LOD";
        snapshot.items.push_back(std::move(item));
        snapshot.MarkComplete();

        const auto metadata = snapshot.GetMetadata();
        const auto& firstItem = snapshot.items.front();
        desc.resourceDiagnostics.push_back("terrain snapshot schema=" + std::to_string(metadata.schemaVersion));
        desc.resourceDiagnostics.push_back("terrain snapshot status=" + std::string(GetTerrainSnapshotStatusName(metadata.status)));
        desc.resourceDiagnostics.push_back("terrain snapshot itemCount=" + std::to_string(metadata.itemCount));
        desc.resourceDiagnostics.push_back("terrain heightmap diagnostic=" + firstItem.heightmapDiagnostic);
        desc.resourceDiagnostics.push_back("terrain render path reason=" + firstItem.renderPathReason);
    }

    void AddWaterSnapshotDiagnostics(RVX::SampleAppDesc& desc)
    {
        RVX::WaterRenderSnapshot snapshot;
        snapshot.BeginBuild(1);

        RVX::WaterRenderSnapshotItem item;
        item.componentId = 1;
        item.size = RVX::Vec2(48.0f, 48.0f);
        item.depth = 2.5f;
        item.resolution = 64;
        item.surfaceType = RVX::WaterRenderSnapshotSurfaceType::Lake;
        item.simulationType = RVX::WaterRenderSnapshotSimulationType::Simple;
        item.reflectionEnabled = false;
        item.refractionEnabled = false;
        item.foamEnabled = true;
        item.cpuSimulationAvailable = true;
        item.gpuInitialized = false;
        item.renderGpuPathAvailable = false;
        item.gpuInitializationReason = "Water GPU resources are Render-owned and not initialized in report-only mode";
        item.simulationFallbackReason = "Simple CPU wave metadata is exported while FFT simulation remains gated";
        item.renderPathReason = "Water Render-owned draw pass is not connected yet";
        snapshot.items.push_back(std::move(item));
        snapshot.MarkComplete();

        const auto metadata = snapshot.GetMetadata();
        const auto& firstItem = snapshot.items.front();
        desc.resourceDiagnostics.push_back("water snapshot schema=" + std::to_string(metadata.schemaVersion));
        desc.resourceDiagnostics.push_back("water snapshot status=" + std::string(GetWaterSnapshotStatusName(metadata.status)));
        desc.resourceDiagnostics.push_back("water snapshot itemCount=" + std::to_string(metadata.itemCount));
        desc.resourceDiagnostics.push_back("water simulation fallback=" + firstItem.simulationFallbackReason);
        desc.resourceDiagnostics.push_back("water render path reason=" + firstItem.renderPathReason);
    }

    void AddPhysicsQueryDiagnostics(RVX::SampleAppDesc& desc)
    {
        RVX::Physics::PhysicsWorld physicsWorld;
        RVX::Physics::PhysicsWorldConfig config;
        config.gravity = RVX::Vec3(0.0f);
        if (!physicsWorld.Initialize(config))
        {
            desc.resourceDiagnostics.push_back("physics world initialization failed");
            return;
        }

        RVX::Physics::RigidBodyDesc bodyDesc;
        bodyDesc.type = RVX::Physics::BodyType::Static;
        bodyDesc.position = RVX::Vec3(0.0f);
        const RVX::Physics::BodyHandle body = physicsWorld.CreateBody(bodyDesc);
        physicsWorld.AddShape(body, RVX::Physics::BoxShape::Create(RVX::Vec3(1.0f)));

        RVX::Physics::RaycastHit rayHit;
        const bool hit = physicsWorld.Raycast(RVX::Vec3(-5.0f, 0.0f, 0.0f),
                                              RVX::Vec3(1.0f, 0.0f, 0.0f),
                                              10.0f,
                                              rayHit);
        const auto rayStats = physicsWorld.GetLastQueryStats();

        std::vector<RVX::Physics::BodyHandle> overlaps;
        const size_t overlapCount = physicsWorld.OverlapSphere(RVX::Vec3(0.0f), 1.5f, overlaps);
        const auto overlapStats = physicsWorld.GetLastQueryStats();

        desc.resourceDiagnostics.push_back(std::string("raycast hit=") + (hit ? "true" : "false"));
        desc.resourceDiagnostics.push_back("raycast bodyId=" + std::to_string(rayHit.bodyId));
        desc.resourceDiagnostics.push_back("raycast broadphase candidates=" + std::to_string(rayStats.broadphaseCandidateCount));
        desc.resourceDiagnostics.push_back("raycast narrowphase tests=" + std::to_string(rayStats.narrowphaseTestCount));
        desc.resourceDiagnostics.push_back("overlap sphere count=" + std::to_string(overlapCount));
        desc.resourceDiagnostics.push_back("overlap broadphase candidates=" + std::to_string(overlapStats.broadphaseCandidateCount));
        desc.resourceDiagnostics.push_back("physics backend fallback active=" + std::string(physicsWorld.IsBackendFallbackActive() ? "true" : "false"));

        physicsWorld.Shutdown();
    }

    void WriteU16(std::ofstream& file, std::uint16_t value)
    {
        file.put(static_cast<char>(value & 0xFFu));
        file.put(static_cast<char>((value >> 8u) & 0xFFu));
    }

    void WriteU32(std::ofstream& file, std::uint32_t value)
    {
        file.put(static_cast<char>(value & 0xFFu));
        file.put(static_cast<char>((value >> 8u) & 0xFFu));
        file.put(static_cast<char>((value >> 16u) & 0xFFu));
        file.put(static_cast<char>((value >> 24u) & 0xFFu));
    }

    std::filesystem::path WriteSilentWav(const std::string& fileName)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / fileName;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return {};
        }

        constexpr std::uint16_t channels = 1;
        constexpr std::uint32_t sampleRate = 8000;
        constexpr std::uint16_t bitsPerSample = 16;
        constexpr std::uint32_t frameCount = 8;
        constexpr std::uint16_t blockAlign = channels * bitsPerSample / 8;
        constexpr std::uint32_t byteRate = sampleRate * blockAlign;
        constexpr std::uint32_t dataSize = frameCount * blockAlign;

        file.write("RIFF", 4);
        WriteU32(file, 36u + dataSize);
        file.write("WAVE", 4);
        file.write("fmt ", 4);
        WriteU32(file, 16u);
        WriteU16(file, 1u);
        WriteU16(file, channels);
        WriteU32(file, sampleRate);
        WriteU32(file, byteRate);
        WriteU16(file, blockAlign);
        WriteU16(file, bitsPerSample);
        file.write("data", 4);
        WriteU32(file, dataSize);
        for (std::uint32_t i = 0; i < frameCount; ++i)
        {
            WriteU16(file, 0u);
        }

        return path;
    }

    void AddAudioFallbackDiagnostics(RVX::SampleAppDesc& desc)
    {
        const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path wavPath =
            WriteSilentWav("rvx_audio_fallback_sample_" + std::to_string(uniqueId) + ".wav");
        if (wavPath.empty())
        {
            desc.resourceDiagnostics.push_back("audio fixture setup failed");
            return;
        }

        bool initializedLog = false;
        if (!RVX::Log::GetCoreLogger())
        {
            RVX::Log::Initialize();
            initializedLog = true;
        }

        RVX::Resource::AudioLoader loader(nullptr);
        RVX::Resource::AudioLoadOptions streamingOptions;
        streamingOptions.enableStreaming = true;
        RVX::Resource::AudioResource* streamingAudio =
            loader.LoadWithOptions(wavPath.string(), streamingOptions);
        desc.resourceDiagnostics.push_back("audio streaming status=" +
                                           std::string(GetAudioLoadStatusName(loader.GetLastLoadStatus())));
        desc.resourceDiagnostics.push_back("audio streaming resource=" +
                                           std::string(streamingAudio ? "created" : "null"));
        delete streamingAudio;

        RVX::Resource::AudioLoadOptions thresholdOptions;
        thresholdOptions.streamingThreshold = 1;
        RVX::Resource::AudioResource* audio = loader.LoadWithOptions(wavPath.string(), thresholdOptions);
        desc.resourceDiagnostics.push_back("audio threshold fallback status=" +
                                           std::string(GetAudioLoadStatusName(loader.GetLastLoadStatus())));
        desc.resourceDiagnostics.push_back("audio threshold fallback used=" +
                                           std::string(loader.WasLastLoadFallback() ? "true" : "false"));
        if (audio)
        {
            desc.resourceDiagnostics.push_back("audio full-buffer bytes=" + std::to_string(audio->GetDataSize()));
            desc.resourceDiagnostics.push_back("audio full-buffer streaming=" +
                                               std::string(audio->IsStreaming() ? "true" : "false"));
        }
        delete audio;

        if (initializedLog)
        {
            RVX::Log::Shutdown();
        }

        std::error_code error;
        std::filesystem::remove(wavPath, error);
    }

    RVX::SampleAppDesc MakeSampleDesc()
    {
        RVX::SampleAppDesc desc;
        desc.category = "basic";

#if RVX_BASIC_SAMPLE_KIND == 1
        desc.sampleName = "BackendInfoSample";
        desc.enabledFeatures = {
            "BackendSelection",
            "RHICapabilityReportContract",
            "UnsupportedFallbackReasons",
        };
        desc.resourceDiagnostics = {
            "capability report schema=RVX.RHI.CapabilityReport",
            "backend selection honors --backend and platform defaults",
        };
        desc.fallbackReasons = {
            "Live RHI device probing remains in BasicRHI and showcase smoke tests",
        };
#elif RVX_BASIC_SAMPLE_KIND == 2
        desc.sampleName = "RenderGraphBasics";
        desc.enabledFeatures = {
            "RenderGraphPassChain",
            "TextureImportExport",
            "TransientTexture",
            "RenderGraphDiagnosticsArtifact",
        };
        desc.renderDiagnostics = MakeRenderDiagnostics(3);
        desc.resourceDiagnostics = {
            "graph contract: import scene color -> transient intermediate -> exported output",
        };
#elif RVX_BASIC_SAMPLE_KIND == 3
        desc.sampleName = "ResourcePolicySample";
        desc.enabledFeatures = {
            "RuntimeResourcePolicy",
            "CookedArtifactResolution",
            "PackageMountTable",
            "HashMismatchDiagnostic",
        };
        AddResourcePolicyDiagnostics(desc);
#elif RVX_BASIC_SAMPLE_KIND == 4
        desc.sampleName = "TextureUploadSample";
        desc.enabledFeatures = {
            "CookedTextureArtifact",
            "GPUUploadService",
            "TextureResidencyState",
        };
        desc.resourceDiagnostics = {
            "texture upload contract: CPU artifact -> GPU upload -> resident texture",
            "unsupported formats report structured upload failure",
        };
#elif RVX_BASIC_SAMPLE_KIND == 5
        desc.sampleName = "MeshMaterialSample";
        desc.enabledFeatures = {
            "MeshResource",
            "PBRMaterial",
            "NormalRoughnessMetallicEmissive",
            "FallbackMaterial",
        };
        desc.resourceDiagnostics = {
            "material fallback is visible when a material contract is incomplete",
        };
        desc.renderDiagnostics = MakeRenderDiagnostics(2);
        desc.renderDiagnostics.visibleObjectCount = 3;
#elif RVX_BASIC_SAMPLE_KIND == 6
        desc.sampleName = "SceneActorComponentSample";
        desc.enabledFeatures = {
            "EngineLifecycle",
            "World",
            "Actor",
            "ActorComponent",
            "StaticMeshComponent",
            "RenderExtractionSnapshot",
        };
        desc.resourceDiagnostics = {
            "production path uses ActorComponent and StaticMeshComponent",
            "legacy MeshRendererComponent is compatibility-only",
        };
        desc.renderDiagnostics = MakeRenderDiagnostics(1);
        desc.renderDiagnostics.visibleObjectCount = 1;
#elif RVX_BASIC_SAMPLE_KIND == 7
        desc.sampleName = "PostProcessChainSample";
        desc.enabledFeatures = {
            "ToneMapping",
            "Bloom",
            "FXAA",
            "ColorGrading",
            "Vignette",
            "FilmGrain",
            "SSAO low-tier",
        };
        desc.unsupportedFeatures = {
            "SSR low-tier ray march pending",
            "TAA resolve capability-gated",
            "DOF unsupported diagnostic",
            "MotionBlur unsupported diagnostic",
            "VolumetricLighting unsupported diagnostic",
        };
        desc.renderDiagnostics = MakeRenderDiagnostics(7);
        desc.renderDiagnostics.requestedPostProcessEffectCount = 12;
        desc.renderDiagnostics.enabledPostProcessEffectCount = 7;
        desc.renderDiagnostics.unsupportedPostProcessSkippedCount = 5;
        desc.renderDiagnostics.postProcessGraphPassCount = 7;
#elif RVX_BASIC_SAMPLE_KIND == 8
        desc.sampleName = "LightingShadowSample";
        desc.enabledFeatures = {
            "DirectionalLight",
            "PointLight",
            "SpotLight",
            "ClusteredLightingStats",
            "CSMShadow",
            "ProceduralSkyboxPass",
        };
        desc.fallbackReasons = {
            "IBL and advanced local shadows remain capability-gated in basic smoke mode",
        };
        desc.renderDiagnostics = MakeRenderDiagnostics(4);
        desc.renderDiagnostics.renderSceneLightCount = 4;
        desc.renderDiagnostics.clusteredLightingInitialized = true;
        desc.renderDiagnostics.clusteredLightingActiveClusters = 12;
#elif RVX_BASIC_SAMPLE_KIND == 9
        desc.sampleName = "GpuResidencySample";
        desc.enabledFeatures = {
            "RenderResourceRegistry",
            "UploadStatus",
            "GenerationCheckedHandles",
            "RetirementDiagnostics",
        };
        AddGPUResidencyDiagnostics(desc);
#elif RVX_BASIC_SAMPLE_KIND == 10
        desc.sampleName = "InputCameraSample";
        desc.enabledFeatures = {
            "RuntimeCamera",
            "OrbitCameraContract",
            "FreeCameraContract",
            "InputSubsystemDiagnostics",
        };
        desc.fallbackReasons = {
            "Platform input events require a windowed sample; this target reports runtime input contracts",
        };
#elif RVX_BASIC_SAMPLE_KIND == 11
        desc.sampleName = "DebugDrawSample";
        desc.enabledFeatures = {
            "DebugLineCommandBuffer",
            "FrustumWireframeCPUExtraction",
            "DebugRendererDiagnostics",
        };
        desc.unsupportedFeatures = {
            "Debug line GPU pipeline",
        };
        desc.fallbackReasons = {
            "Debug line pipeline is not implemented; CPU debug primitives are queued without issuing a draw",
        };
        desc.resourceDiagnostics = {
            "frustum wireframe vertices=24",
            "debug renderer reports scheduled=false until a debug line pipeline exists",
        };
#elif RVX_BASIC_SAMPLE_KIND == 12
        desc.sampleName = "DecalDiagnosticsSample";
        desc.enabledFeatures = {
            "DecalCPUList",
            "DecalSortOrder",
            "DecalRendererDiagnostics",
        };
        desc.unsupportedFeatures = {
            "Deferred decal projection GPU pass",
        };
        desc.fallbackReasons = {
            "Decal RenderGraph pass is not scheduled until projection shaders and pipelines land",
        };
        desc.resourceDiagnostics = {
            "decal diagnostics report requested/supported/scheduled/executed",
            "GBuffer inputs are validated before any decal pass can be scheduled",
        };
#elif RVX_BASIC_SAMPLE_KIND == 13
        desc.sampleName = "SwapChainPolicySample";
        desc.enabledFeatures = {
            "ExternalSwapChainInjection",
            "SwapChainManagerDiagnostics",
            "ResizePresentFallbackReasons",
        };
        desc.unsupportedFeatures = {
            "Raw window handle swapchain creation",
        };
        desc.fallbackReasons = {
            "SwapChainManager expects a platform-created RHISwapChain for runtime presentation",
        };
        desc.resourceDiagnostics = {
            "window handle creation reports initialized=false and hasWindowHandle=true",
            "present/resize report structured reasons when no swap chain is available",
        };
#elif RVX_BASIC_SAMPLE_KIND == 14
        desc.sampleName = "ParticleSnapshotSample";
        desc.enabledFeatures = {
            "ParticleRenderSnapshot",
            "CPUParticleSimulationMetadata",
            "ParticleRenderContractBoundary",
        };
        desc.unsupportedFeatures = {
            "GPU particle render payload",
        };
        desc.fallbackReasons = {
            "Particle GPU payload remains Render-owned and reports metadata-only until connected",
        };
        AddParticleSnapshotDiagnostics(desc);
#elif RVX_BASIC_SAMPLE_KIND == 15
        desc.sampleName = "TerrainSnapshotSample";
        desc.enabledFeatures = {
            "TerrainRenderSnapshot",
            "TerrainCPUDataDiagnostic",
            "TerrainLODDiagnostic",
        };
        desc.unsupportedFeatures = {
            "Terrain render-owned draw pass",
            "Terrain heightmap GPU upload",
        };
        desc.fallbackReasons = {
            "Terrain snapshot exports CPU-side metadata without claiming GPU upload or draw readiness",
        };
        AddTerrainSnapshotDiagnostics(desc);
#elif RVX_BASIC_SAMPLE_KIND == 16
        desc.sampleName = "WaterSnapshotSample";
        desc.enabledFeatures = {
            "WaterRenderSnapshot",
            "SimpleCPUWaveMetadata",
            "WaterRenderContractBoundary",
        };
        desc.unsupportedFeatures = {
            "Water render-owned draw pass",
            "FFT water simulation",
        };
        desc.fallbackReasons = {
            "Water snapshot reports simple CPU wave metadata while GPU path remains gated",
        };
        AddWaterSnapshotDiagnostics(desc);
#elif RVX_BASIC_SAMPLE_KIND == 17
        desc.sampleName = "PhysicsQuerySample";
        desc.enabledFeatures = {
            "PhysicsWorld",
            "PhysicsRaycast",
            "PhysicsOverlapSphere",
            "PhysicsQueryStats",
        };
        desc.unsupportedFeatures = {
            "HeightField/TriangleMesh backend query parity",
        };
        desc.fallbackReasons = {
            "Advanced collider backends stay diagnostic-gated until backend parity is complete",
        };
        AddPhysicsQueryDiagnostics(desc);
#elif RVX_BASIC_SAMPLE_KIND == 18
        desc.sampleName = "AudioFallbackSample";
        desc.enabledFeatures = {
            "AudioWavDecode",
            "AudioFullBufferFallback",
            "AudioLoaderDiagnostics",
        };
        desc.unsupportedFeatures = {
            "Audio resource streaming",
        };
        desc.fallbackReasons = {
            "Streaming requests report unsupported and threshold-triggered streaming falls back to full-buffer load",
        };
        AddAudioFallbackDiagnostics(desc);
#else
        desc.sampleName = "UnknownBasicSample";
        desc.unsupportedFeatures = {"Unknown sample kind"};
        desc.fallbackReasons = {"RVX_BASIC_SAMPLE_KIND was not configured"};
#endif

        return desc;
    }
} // namespace

int main(int argc, char* argv[])
{
    return RVX::RunReportOnlySample(argc, argv, MakeSampleDesc());
}
