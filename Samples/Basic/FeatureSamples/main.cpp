#include "Samples/SampleCLI.h"

#include "Core/Diagnostics/ContentHash.h"
#include "Render/GPUResourceManager.h"
#include "Resource/RuntimeResourcePolicy.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

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
        RVX::GPUResourceManager manager;
        manager.SetMemoryBudget(64ull * 1024ull * 1024ull);
        const RVX::GPUResourceManager::Stats stats = manager.GetStats();
        desc.resourceDiagnostics.push_back("gpu memory budget=" + std::to_string(stats.memoryBudget));
        desc.resourceDiagnostics.push_back("resident meshes=" + std::to_string(stats.residentMeshCount));
        desc.resourceDiagnostics.push_back("resident textures=" + std::to_string(stats.residentTextureCount));
        desc.resourceDiagnostics.push_back("pending uploads=" + std::to_string(stats.pendingUploadCount));
        desc.resourceDiagnostics.push_back("eviction reason=none; no resident resources in this smoke fixture");
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
            "GPUResourceManager",
            "MemoryBudget",
            "UploadState",
            "ResidencyState",
            "EvictionDiagnostics",
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