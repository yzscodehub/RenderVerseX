#pragma once

/**
 * @file HybridRayTracingNativeCases.h
 * @brief Backend-parametric native evidence for the GFX21 hybrid RT contract.
 *
 * The two backend executables intentionally share this production-only test
 * body.  Their CMake wrappers select exactly one backend at compile time and
 * stage an independent cooked catalog/evidence destination for that backend.
 */

#include "Common/GpuTestUtils.h"
#include "Common/RenderGraphValidationAccess.h"
#include "Common/RenderRuntimeTestHarness.h"
#include "Geometry/Asset/Mesh.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "Render/PipelineCache.h"
#include "Render/PostProcess/Exposure.h"
#include "Render/RayTracing/RayTracingScene.h"
#include "Render/RayTracing/RayTracingSceneManager.h"
#include "RenderContracts/RenderExposure.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHI.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "ShaderRuntime/CookedShaderProgramProvider.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace RVX::GFX21NativeValidation
{
    namespace fs = std::filesystem;

#if defined(RVX_GFX21_NATIVE_BACKEND_DX12)
    inline constexpr RHIBackendType kBackend = RHIBackendType::DX12;
    inline constexpr const char* kBackendName = "DX12";
    inline constexpr const char* kShaderRootEnvironment =
        "RVX_GFX21_HYBRID_RT_SHADER_ROOT_DX12";
    inline constexpr const char* kEvidenceEnvironment =
        "RVX_GFX21_HYBRID_RT_EVIDENCE_DX12";
#elif defined(RVX_GFX21_NATIVE_BACKEND_VULKAN)
    inline constexpr RHIBackendType kBackend = RHIBackendType::Vulkan;
    inline constexpr const char* kBackendName = "Vulkan";
    inline constexpr const char* kShaderRootEnvironment =
        "RVX_GFX21_HYBRID_RT_SHADER_ROOT_VULKAN";
    inline constexpr const char* kEvidenceEnvironment =
        "RVX_GFX21_HYBRID_RT_EVIDENCE_VULKAN";
#else
#error "A GFX21 native validation backend must be selected by CMake."
#endif

    inline constexpr uint32 kExtent = 64u;
    inline constexpr uint32 kHalfExtent = 32u;
    inline constexpr std::string_view kStableSceneIdentity =
        "gfx21.masked-triangle.scene.v1";

    struct NativeEvidenceFacts
    {
        bool maskedAnyHit = false;
        bool shadowDispatch = false;
        bool reflectionDispatch = false;
        bool spatialDenoise = false;
        bool temporalResolve = false;
        bool temporalAccumulated = false;
        bool denoisedComposite = false;
        uint32 shadowWidth = 0;
        uint32 shadowHeight = 0;
        uint32 reflectionWidth = 0;
        uint32 reflectionHeight = 0;
        uint64 shadowDispatchPixels = 0;
        uint64 reflectionDispatchPixels = 0;
    };

    inline NativeEvidenceFacts s_evidenceFacts{};

    [[nodiscard]] inline std::string ReadEnvironment(const char* name)
    {
        const char* const value = std::getenv(name);
        return value != nullptr ? value : "";
    }

    [[nodiscard]] inline fs::path CookedShaderRoot()
    {
        return fs::path(ReadEnvironment(kShaderRootEnvironment));
    }

    [[nodiscard]] inline std::string EscapeJson(std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char character : value)
        {
            switch (character)
            {
                case '\\': escaped += "\\\\"; break;
                case '\"': escaped += "\\\""; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped += character; break;
            }
        }
        return escaped;
    }

    [[nodiscard]] inline GPUCompletionToken MakeCompletion(uint64 fenceValue)
    {
        GPUCompletionToken completion;
        completion.count = 1u;
        completion.points[0] = {GPUQueueDomain::Graphics, fenceValue};
        return completion;
    }

    [[nodiscard]] inline RenderViewHistoryIdentity MakeHistoryIdentity(
        uint64 frameSequence)
    {
        return {
            .previousFrame = {
                .frameSequence = frameSequence,
                .sceneRevision = 1u,
                .surfaceGeneration = 1u,
                .configurationRevision = 1u,
            },
            .view = {
                .sceneRuntimeId = 0x4758463231u,
                .cameraEntityPacked = 1u,
                .subViewIndex = 0u,
                .role = ViewRole::Main,
            },
            .extent = {.width = kExtent, .height = kExtent},
            .familyId = 1u,
            .worldRevision = 1u,
            .temporalEpoch = 1u,
            .formatRevision = 1u,
            .surfaceGeneration = 1u,
            .cameraCutRevision = 1u,
        };
    }

    struct ExposureGraphState
    {
        RHIBufferRef buffer{};
        RGBufferHandle handle{};
        uint64 contentHash = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return buffer && handle.IsValid() && contentHash != 0u;
        }
    };

    [[nodiscard]] inline ExposureGraphState CreateExposureGraphState(
        IRHIDevice& device,
        RenderGraph& graph,
        uint64 contentHash)
    {
        ExposureGraphState state;
        RHIBufferDesc desc;
        desc.size = RVX_EXPOSURE_STATE_STRIDE;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = RVX_EXPOSURE_STATE_STRIDE;
        desc.debugName = "GFX21NativeExposureState";
        state.buffer = device.CreateBuffer(desc);
        if (!state.buffer)
        {
            return {};
        }

        std::array<uint8, RVX_EXPOSURE_STATE_STRIDE> bytes{};
        const std::array<float32, 4> values{
            1.0f / RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION,
            0.5f / RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION,
            0.25f / RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION,
            0.0f};
        constexpr uint32 validState = 1u;
        std::memcpy(bytes.data(), values.data(), sizeof(values));
        std::memcpy(bytes.data() + 32u, &contentHash, sizeof(contentHash));
        std::memcpy(bytes.data() + 44u, &validState, sizeof(validState));
        if (!state.buffer->Upload(bytes.data(), bytes.size()))
        {
            return {};
        }

        state.handle = RenderGraphValidationAccess::ImportBuffer(
            graph, state.buffer.Get(), RHIResourceState::ShaderResource);
        state.contentHash = contentHash;
        return state;
    }

    struct NativeRuntime final
    {
        std::unique_ptr<IRHIDevice> device;
        std::unique_ptr<IShaderProgramProvider> shaderProvider;
        std::unique_ptr<PipelineCache> pipelineCache;
        RenderRuntimeTestHarness resources;
        ResourceViewCache viewCache;
        std::unique_ptr<Resource::MeshResource> mesh;
        std::unique_ptr<Resource::MaterialResource> material;
        RenderScene scene;
        RayTracingSceneBuildPlan scenePlan;
        RayTracingSceneManager sceneManager;
        RHINativeValidationDiagnostics nativeValidationBefore{};

        ~NativeRuntime()
        {
            sceneManager.Shutdown();
            viewCache.Shutdown();
            resources.Shutdown();
            if (pipelineCache)
            {
                pipelineCache->Shutdown();
            }
        }
    };

    inline void AssertRequiredCookedArtifacts(const fs::path& root)
    {
        ASSERT_FALSE(root.empty())
            << "The backend-specific cooked production shader root is required";
        ASSERT_TRUE(fs::is_directory(root))
            << "The GFX21 cooked fixture was not staged: "
            << root.generic_string();

        constexpr std::array<std::pair<std::string_view, std::string_view>, 8>
            artifacts = {{
                {"RayTracing/RayTracedShadow.hlsl", "RayGen"},
                {"RayTracing/RayTracedShadow.hlsl", "ShadowMiss"},
                {"RayTracing/RayTracedShadow.hlsl", "ShadowClosestHit"},
                {"RayTracing/RayTracedShadow.hlsl", "ShadowAnyHit"},
                {"RayTracing/RayTracedReflection.hlsl", "ReflectionRayGen"},
                {"RayTracing/RayTracedReflection.hlsl", "ReflectionMiss"},
                {"RayTracing/RayTracedReflection.hlsl", "ReflectionClosestHit"},
                {"RayTracing/RayTracedReflection.hlsl", "ReflectionAnyHit"},
            }};
        for (const auto& [logicalAssetId, entryPoint] : artifacts)
        {
            ShaderProgramRequest request;
            request.logicalAssetId = std::string(logicalAssetId);
            request.stage = entryPoint.find("RayGen") != std::string_view::npos
                ? RHIShaderStage::RayGeneration
                : entryPoint.find("Miss") != std::string_view::npos
                    ? RHIShaderStage::Miss
                    : entryPoint.find("Closest") != std::string_view::npos
                        ? RHIShaderStage::ClosestHit
                        : RHIShaderStage::AnyHit;
            request.entryPoint = std::string(entryPoint);
            request.backend = kBackend;
            const fs::path artifact = root / BuildShaderArtifactRelativePath(request);
            ASSERT_TRUE(fs::is_regular_file(artifact))
                << "Missing cooked production RT shader artifact: "
                << artifact.generic_string();
        }
    }

    inline void InitializeRuntime(NativeRuntime& runtime)
    {
        const fs::path shaderRoot = CookedShaderRoot();
        AssertRequiredCookedArtifacts(shaderRoot);

        RHIDeviceDesc desc;
        desc.enableDebugLayer = true;
        desc.allowSoftwareAdapter = false;
        runtime.device = Test::CreateNativeValidationDevice(kBackend, desc);
        ASSERT_NE(runtime.device.get(), nullptr)
            << kBackendName << " hardware is required for GFX21 native validation";
        ASSERT_EQ(runtime.device->GetBackendType(), kBackend);
        ASSERT_TRUE(Test::IsHardwareGpuDevice(runtime.device))
            << kBackendName << " GFX21 validation forbids software adapters";
        const RHICapabilities& capabilities = runtime.device->GetCapabilities();
        ASSERT_TRUE(capabilities.supportsRaytracing)
            << kBackendName << " does not expose hardware ray tracing";
        ASSERT_TRUE(capabilities.supportsRaytracingPipeline)
            << kBackendName << " does not expose a ray-tracing pipeline";
        ASSERT_TRUE(capabilities.supportsAccelerationStructureUpdate)
            << kBackendName << " does not expose TLAS update support";

        runtime.nativeValidationBefore =
            runtime.device->GetNativeValidationDiagnostics();
        ASSERT_TRUE(runtime.nativeValidationBefore.available)
            << kBackendName << " native validation telemetry is required";
        ASSERT_TRUE(runtime.nativeValidationBefore.enabled)
            << kBackendName << " native validation must be enabled";
        ASSERT_TRUE(runtime.nativeValidationBefore.readComplete)
            << kBackendName << " native validation telemetry is incomplete";

        runtime.shaderProvider =
            CreateCookedShaderProgramProviderFactory(shaderRoot)();
        ASSERT_NE(runtime.shaderProvider.get(), nullptr);
        runtime.pipelineCache =
            std::make_unique<PipelineCache>(*runtime.shaderProvider);
        const fs::path sourceShaderDirectory = fs::path(
            ReadEnvironment("RVX_GFX21_HYBRID_RT_SOURCE_SHADER_ROOT"));
        ASSERT_FALSE(sourceShaderDirectory.empty())
            << "The production shader source root is required to preserve "
               "logical cooked-artifact identities";
        ASSERT_TRUE(fs::is_directory(sourceShaderDirectory));
        ASSERT_TRUE(runtime.pipelineCache->Initialize(
            runtime.device.get(), sourceShaderDirectory.string()))
            << runtime.pipelineCache->GetLastError();
        ASSERT_NE(runtime.pipelineCache->GetRayTracedShadowPipeline(), nullptr);
        ASSERT_NE(runtime.pipelineCache->GetRayTracedShadowShaderTable(), nullptr);
        ASSERT_NE(runtime.pipelineCache->GetRayTracedReflectionPipeline(), nullptr);
        ASSERT_NE(runtime.pipelineCache->GetRayTracedReflectionShaderTable(), nullptr);
        ASSERT_NE(runtime.pipelineCache->GetRayTracedReflectionDenoisePipeline(), nullptr);
        ASSERT_NE(runtime.pipelineCache->GetRayTracedReflectionCompositePipeline(), nullptr);

        ASSERT_TRUE(runtime.resources.Initialize(runtime.device.get()));
        runtime.viewCache.Initialize(runtime.device.get());

        runtime.mesh = std::make_unique<Resource::MeshResource>();
        runtime.mesh->SetId(0x2101u);
        runtime.mesh->SetName("GFX21MaskedTriangleMesh");
        runtime.mesh->SetMesh(MeshFactory::CreateTriangle());
        ASSERT_TRUE(runtime.resources.UploadImmediate(runtime.mesh.get()));
        ASSERT_TRUE(runtime.resources.IsGPUReady(runtime.mesh->GetId()));

        runtime.material = std::make_unique<Resource::MaterialResource>();
        runtime.material->SetId(0x2102u);
        runtime.material->SetName("GFX21MaskedAnyHitMaterial");
        auto material = std::make_shared<Material>("GFX21MaskedAnyHitMaterial");
        material->SetAlphaMode(Material::AlphaMode::Mask);
        material->SetAlphaCutoff(0.5f);
        material->SetBaseColor(Vec4(1.0f, 1.0f, 1.0f, 0.0f));
        ASSERT_TRUE(runtime.material->SetMaterialData(std::move(material)));
        ASSERT_TRUE(runtime.resources.UploadImmediate(runtime.material.get()));
        ASSERT_TRUE(runtime.resources.IsGPUReady(runtime.material->GetId()));

        RenderObject object;
        object.mesh = runtime.resources.GetHandle(runtime.mesh->GetId());
        object.material = runtime.resources.GetHandle(runtime.material->GetId());
        object.entityId = 0x4758463231u;
        object.worldMatrix = Mat4Identity();
        object.normalMatrix = Mat4Identity();
        object.drawable = true;
        object.visible = true;
        object.castsShadow = true;
        object.materialModes = {RenderMaterialMode::Masked};
        ASSERT_TRUE(object.mesh.IsValid());
        ASSERT_TRUE(object.material.IsValid());
        runtime.scene.AddObject(object);

        const std::array<uint32, 1> candidates = {0u};
        runtime.scenePlan = BuildRayTracingSceneBuildPlan(
            runtime.scene, candidates, runtime.resources.GetRegistry());
        ASSERT_TRUE(runtime.scenePlan.HasWork());
        ASSERT_TRUE(runtime.scenePlan.maskedMetadataComplete);
        ASSERT_EQ(runtime.scenePlan.instances.size(), 1u);
        ASSERT_TRUE(runtime.scenePlan.instances.front().alphaTest.enabled);
        ASSERT_EQ(runtime.scenePlan.instances.front().alphaTest.status,
                  RayTracingMaskedMetadataStatus::Complete);
        ASSERT_NE(static_cast<uint32>(runtime.scenePlan.instances.front().desc.flags) &
                      static_cast<uint32>(RHIRayTracingInstanceFlags::ForceNoOpaque),
                  0u);

        runtime.sceneManager.Initialize(runtime.device.get());
        runtime.sceneManager.SetBLASCompactionEnabled(false);
        ASSERT_TRUE(runtime.sceneManager.IsSupported());
        ASSERT_TRUE(runtime.sceneManager.Prepare(runtime.scenePlan))
            << runtime.sceneManager.GetStats().fallbackReason;

        RHICommandContextRef context = runtime.device->CreateCommandContext(
            RHICommandQueueType::Graphics);
        RHIFenceRef fence = runtime.device->CreateFence(0u);
        ASSERT_NE(context.Get(), nullptr);
        ASSERT_NE(fence.Get(), nullptr);
        context->Begin();
        RayTracingSceneRecordingToken recording;
        ASSERT_TRUE(runtime.sceneManager.RecordBuildCommands(*context, recording));
        context->End();
        const uint64 submittedValue = runtime.device->SubmitCommandContext(
            context.Get(), fence.Get());
        ASSERT_GT(submittedValue, 0u);
        ASSERT_TRUE(runtime.sceneManager.NotifySubmission(recording));
        runtime.device->WaitForFence(fence.Get(), submittedValue);
        ASSERT_GE(fence->GetCompletedValue(), submittedValue);
        ASSERT_NE(runtime.sceneManager.GetTopLevelAS(), nullptr);
        ASSERT_TRUE(runtime.sceneManager.GetStats().submittedTLASBuild);
        ASSERT_EQ(runtime.sceneManager.GetStats().alphaTestedInstanceCount, 1u);
    }

    [[nodiscard]] inline ViewData MakeView(
        RenderGraph& graph,
        IRHIDevice& device,
        uint64 frameNumber,
        std::vector<RHIBufferRef>& retainedBuffers,
        std::vector<RHITextureRef>& retainedTextures)
    {
        RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(
            kExtent, kExtent, RHIFormat::RGBA16_FLOAT);
        colorDesc.debugName = "GFX21NativeSceneColor";
        RHITextureRef color = device.CreateTexture(colorDesc);

        RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(
            kExtent, kExtent, PipelineCache::GetDefaultDepthStencilFormat());
        depthDesc.debugName = "GFX21NativeDepth";
        RHITextureRef depth = device.CreateTexture(depthDesc);

        RHITextureDesc velocityDesc = RHITextureDesc::RenderTarget(
            kExtent, kExtent, RHIFormat::RG16_FLOAT);
        velocityDesc.debugName = "GFX21NativeVelocity";
        RHITextureRef velocity = device.CreateTexture(velocityDesc);

        ViewData view;
        view.frameNumber = frameNumber;
        view.viewportWidth = kExtent;
        view.viewportHeight = kExtent;
        view.aspectRatio = 1.0f;
        view.nearPlane = 0.1f;
        view.farPlane = 100.0f;
        view.cameraPosition = {0.0f, 0.0f, 5.0f};
        view.cameraForward = {0.0f, 0.0f, -1.0f};
        view.viewMatrix = Mat4Identity();
        view.projectionMatrix = Mat4Identity();
        view.viewProjectionMatrix = Mat4Identity();
        view.jitteredViewProjectionMatrix = Mat4Identity();
        view.inverseViewMatrix = Mat4Identity();
        view.inverseProjectionMatrix = Mat4Identity();
        view.previousViewProjectionMatrix = Mat4Identity();
        view.directionalLightDirection = {-0.3f, -1.0f, -0.2f};
        view.directionalLightColor = {1.0f, 1.0f, 1.0f};
        view.directionalLightIntensity = 5.0f;
        view.colorTarget = RenderGraphValidationAccess::ImportTexture(
            graph, color.Get(), RHIResourceState::Undefined);
        view.depthTarget = RenderGraphValidationAccess::ImportTexture(
            graph, depth.Get(), RHIResourceState::Undefined);
        view.velocityTarget = RenderGraphValidationAccess::ImportTexture(
            graph, velocity.Get(), RHIResourceState::Undefined);
        graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);
        graph.SetExportState(view.depthTarget, RHIResourceState::DepthRead);
        graph.SetExportState(view.velocityTarget, RHIResourceState::ShaderResource);

        // Importing a raw graph resource intentionally does not retain its
        // owner. Keep the allocations alive through this graph's submission
        // and fence completion so production passes cannot observe dangling
        // native objects.
        retainedTextures.clear();
        retainedTextures.emplace_back(std::move(color));
        retainedTextures.emplace_back(std::move(depth));
        retainedTextures.emplace_back(std::move(velocity));

        ExposureGraphState exposure = CreateExposureGraphState(
            device, graph, 0xA5219B3C4D5E6F70ull ^ frameNumber);
        retainedBuffers.push_back(exposure.buffer);
        if (!exposure.IsValid())
        {
            return {};
        }
        view.exposureState = exposure.handle;
        view.exposureStateSchemaVersion = 1u;
        view.exposureStateStride = RVX_EXPOSURE_STATE_STRIDE;
        view.exposureStateMode = RenderExposureMode::ManualEV100;
        view.exposureStateConfigurationHash = 0x91DCE47A51ull ^ frameNumber;
        view.exposureStateContentHash = exposure.contentHash;
        return view;
    }

    [[nodiscard]] inline RenderPassRecordContext MakeContext(
        RenderGraph& graph,
        const ViewData& view)
    {
        RenderPassRecordContext context = MakeRenderPassRecordContext(graph, view);
        context.legacyAdapter = false;
        context.primaryDirectionalLight.selected = true;
        context.primaryDirectionalLight.castsShadow = true;
        context.primaryDirectionalLight.direction = view.directionalLightDirection;
        context.primaryDirectionalLight.color = view.directionalLightColor;
        context.primaryDirectionalLight.intensity = view.directionalLightIntensity;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        return context;
    }

    /**
     * @brief Bind the completed native TLAS as an exact graph-owned trace input.
     *
     * InitializeRuntime has already submitted and completed the scene build.
     * This graph therefore imports the real backing storage in AS-read state;
     * it must not invent a second build write merely to satisfy a test fixture.
     */
    [[nodiscard]] inline bool AttachSubmittedRayTracingScene(
        RenderPassRecordContext& context,
        RayTracingSceneManager& sceneManager)
    {
        if (!context.identity.IsValid() || context.identity.graph == nullptr)
        {
            return false;
        }

        RHIAccelerationStructureRef topLevelAS(sceneManager.GetTopLevelAS());
        RHIBuffer* const storage =
            topLevelAS ? topLevelAS->GetStorageBuffer() : nullptr;
        const RayTracingSceneManagerStats& sceneStats = sceneManager.GetStats();
        const uint64 structuralHash =
            sceneManager.GetPreparedTLASStructuralSignature().hash;
        if (!topLevelAS || !storage ||
            !HasFlag(storage->GetUsage(),
                     RHIBufferUsage::AccelerationStructureStorage) ||
            !storage->GetResourceInstanceId().IsValid() ||
            sceneStats.preparationGeneration == 0 || structuralHash == 0)
        {
            return false;
        }

        RenderGraph& graph = *context.identity.graph;
        const RGBufferHandle storageHandle = graph.ImportBuffer(
            RHIBufferRef(storage), RHIResourceState::AccelerationStructureRead);
        if (!storageHandle.IsValid())
        {
            return false;
        }
        graph.SetExportState(storageHandle,
                             RHIResourceState::AccelerationStructureRead);
        context.rayTracingScene = {
            .identity = context.identity,
            .topLevelAS = std::move(topLevelAS),
            .storageResourceId = storage->GetResourceInstanceId(),
            .topLevelASStorage = storageHandle,
            .preparationGeneration = sceneStats.preparationGeneration,
            .structuralHash = structuralHash,
            .required = true,
        };
        return context.rayTracingScene.IsCompatibleWith(context.identity);
    }

    inline void AssertNativeValidationUnchanged(const NativeRuntime& runtime)
    {
        const RHINativeValidationDiagnostics after =
            runtime.device->GetNativeValidationDiagnostics();
        ASSERT_TRUE(after.available);
        ASSERT_TRUE(after.enabled);
        ASSERT_TRUE(after.readComplete);
        EXPECT_EQ(after.warningCount,
                  runtime.nativeValidationBefore.warningCount);
        EXPECT_EQ(after.errorCount, runtime.nativeValidationBefore.errorCount);
        EXPECT_EQ(after.corruptionCount,
                  runtime.nativeValidationBefore.corruptionCount);
    }

    inline void WriteEvidence()
    {
        const fs::path evidencePath(ReadEnvironment(kEvidenceEnvironment));
        ASSERT_FALSE(evidencePath.empty());
        ASSERT_TRUE(evidencePath.filename() ==
                    (std::string(kBackend == RHIBackendType::DX12
                                     ? "dx12"
                                     : "vulkan") +
                     "-hybrid-ray-tracing-evidence.json"));
        const std::string runId = ReadEnvironment("RVX_VALIDATION_RUN_ID");
        const std::string sourceCommit =
            ReadEnvironment("RVX_VALIDATION_SOURCE_COMMIT");
        const std::string preset = ReadEnvironment("RVX_VALIDATION_PRESET");
        if (!runId.empty())
        {
            ASSERT_FALSE(sourceCommit.empty());
            ASSERT_FALSE(preset.empty());
        }
        ASSERT_TRUE(s_evidenceFacts.maskedAnyHit);
        ASSERT_TRUE(s_evidenceFacts.shadowDispatch);
        ASSERT_TRUE(s_evidenceFacts.reflectionDispatch);
        ASSERT_TRUE(s_evidenceFacts.spatialDenoise);
        ASSERT_TRUE(s_evidenceFacts.temporalResolve);
        ASSERT_TRUE(s_evidenceFacts.temporalAccumulated);
        ASSERT_TRUE(s_evidenceFacts.denoisedComposite);

        std::error_code error;
        fs::create_directories(evidencePath.parent_path(), error);
        ASSERT_FALSE(error) << error.message();
        std::ofstream output(evidencePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << "{\n"
               << "  \"schema\": \"RVX.GFX21HybridRayTracingEvidence\",\n"
               << "  \"schemaVersion\": 1,\n"
               << "  \"backend\": \"" << kBackendName << "\",\n"
               << "  \"runId\": \"" << EscapeJson(
                      runId.empty() ? "standalone" : runId) << "\",\n"
               << "  \"sourceCommit\": \"" << EscapeJson(sourceCommit) << "\",\n"
               << "  \"preset\": \"" << EscapeJson(preset) << "\",\n"
               << "  \"sceneIdentity\": \"" << kStableSceneIdentity << "\",\n"
               << "  \"extent\": {\"width\": " << kExtent
               << ", \"height\": " << kExtent << "},\n"
               << "  \"shadow\": {\"dispatch\": "
               << (s_evidenceFacts.shadowDispatch ? "true" : "false")
               << ", \"maskedAnyHit\": "
               << (s_evidenceFacts.maskedAnyHit ? "true" : "false")
               << ", \"width\": " << s_evidenceFacts.shadowWidth
               << ", \"height\": " << s_evidenceFacts.shadowHeight
               << ", \"pixels\": " << s_evidenceFacts.shadowDispatchPixels
               << "},\n"
               << "  \"reflection\": {\"dispatch\": "
               << (s_evidenceFacts.reflectionDispatch ? "true" : "false")
               << ", \"halfResolution\": {\"width\": "
               << s_evidenceFacts.reflectionWidth << ", \"height\": "
               << s_evidenceFacts.reflectionHeight << "}, \"pixels\": "
               << s_evidenceFacts.reflectionDispatchPixels
               << ", \"spatialDenoise\": "
               << (s_evidenceFacts.spatialDenoise ? "true" : "false")
               << ", \"temporalResolve\": "
               << (s_evidenceFacts.temporalResolve ? "true" : "false")
               << ", \"temporalAccumulated\": "
               << (s_evidenceFacts.temporalAccumulated ? "true" : "false")
               << ", \"denoisedComposite\": "
               << (s_evidenceFacts.denoisedComposite ? "true" : "false")
               << "},\n"
               << "  \"success\": true\n"
               << "}\n";
        ASSERT_TRUE(output.good());
    }

    inline void RunMaskedAnyHitShadowCase()
    {
        NativeRuntime runtime;
        InitializeRuntime(runtime);
        ASSERT_NE(runtime.device.get(), nullptr);
        ASSERT_NE(runtime.shaderProvider.get(), nullptr);
        ASSERT_NE(runtime.pipelineCache.get(), nullptr);
        ASSERT_NE(runtime.pipelineCache->GetRayTracedShadowPipeline(), nullptr);
        ASSERT_NE(runtime.mesh.get(), nullptr);
        ASSERT_NE(runtime.material.get(), nullptr);
        ASSERT_NE(runtime.sceneManager.GetTopLevelAS(), nullptr);
        ASSERT_TRUE(runtime.sceneManager.GetStats().submittedTLASBuild);

        RenderGraph graph;
        RenderGraphValidationAccess::SetDevice(graph, runtime.device.get());
        std::vector<RHIBufferRef> retainedBuffers;
        std::vector<RHITextureRef> retainedTextures;
        ViewData view = MakeView(
            graph, *runtime.device, 1u, retainedBuffers, retainedTextures);
        ASSERT_TRUE(view.colorTarget.IsValid());
        ASSERT_TRUE(view.depthTarget.IsValid());
        ASSERT_TRUE(view.exposureState.IsValid());
        RenderPassRecordContext context = MakeContext(graph, view);
        ASSERT_TRUE(context.MatchesTargetGraph(graph));
        ASSERT_TRUE(AttachSubmittedRayTracingScene(
            context, runtime.sceneManager));

        RayTracedShadowPass shadow;
        shadow.SetEnabled(true);
        shadow.OnAdd(runtime.device.get());
        shadow.SetResources(runtime.pipelineCache.get(), &runtime.viewCache);
        shadow.SetResourceRegistry(&runtime.resources.GetRegistry());
        shadow.SetRayTracingScene(&runtime.sceneManager);
        ASSERT_TRUE(shadow.IsSupported()) << shadow.GetUnsupportedReason();
        shadow.AddToGraph(graph, context);
        ASSERT_TRUE(context.results->rayTracedShadowOutput.enabled);
        ASSERT_TRUE(context.results->rayTracedShadowOutput.shadowMask.IsValid());
        ASSERT_TRUE(context.results->rayTracedShadowOutput.executionState);

        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid)
            << graph.ExportDiagnosticsText();
        RHICommandContextRef command = runtime.device->CreateCommandContext(
            RHICommandQueueType::Graphics);
        RHIFenceRef fence = runtime.device->CreateFence(0u);
        ASSERT_NE(command.Get(), nullptr);
        ASSERT_NE(fence.Get(), nullptr);
        command->Begin();
        RenderGraphValidationAccess::Execute(graph, *command);
        command->End();
        const uint64 submittedValue = runtime.device->SubmitCommandContext(
            command.Get(), fence.Get());
        ASSERT_GT(submittedValue, 0u);
        runtime.device->WaitForFence(fence.Get(), submittedValue);
        ASSERT_GE(fence->GetCompletedValue(), submittedValue);
        shadow.NotifySubmission(context.identity, MakeCompletion(submittedValue));

        const RayTracedShadowPassStats& stats =
            context.results->rayTracedShadowStats;
        ASSERT_TRUE(stats.requested);
        ASSERT_TRUE(stats.supported) << shadow.GetUnsupportedReason();
        ASSERT_TRUE(stats.tlasAvailable);
        ASSERT_TRUE(stats.materialMetadataAvailable);
        ASSERT_TRUE(stats.alphaMetadataAvailable);
        ASSERT_TRUE(stats.alphaGeometryTableAvailable);
        ASSERT_TRUE(stats.resourceViewsAvailable);
        ASSERT_TRUE(stats.descriptorSetAvailable);
        ASSERT_TRUE(stats.constantsUploaded);
        ASSERT_TRUE(stats.dispatchRecorded);
        ASSERT_FALSE(stats.executionFailed);
        ASSERT_EQ(stats.width, kExtent);
        ASSERT_EQ(stats.height, kExtent);
        ASSERT_EQ(stats.dispatchPixelCount,
                  static_cast<uint64>(kExtent) * kExtent);
        ASSERT_GT(stats.estimatedRayCount, 0u);
        ASSERT_TRUE(context.results->rayTracedShadowOutput.executionState
                        ->dispatchReady.load());
        ASSERT_FALSE(context.results->rayTracedShadowOutput.executionState
                         ->executionFailed.load());
        AssertNativeValidationUnchanged(runtime);

        s_evidenceFacts.maskedAnyHit = true;
        s_evidenceFacts.shadowDispatch = true;
        s_evidenceFacts.shadowWidth = stats.width;
        s_evidenceFacts.shadowHeight = stats.height;
        s_evidenceFacts.shadowDispatchPixels = stats.dispatchPixelCount;
        shadow.OnRemove();
    }

    inline void RunHalfResolutionReflectionCase()
    {
        // gtest_discover_tests registers each TEST as an independent CTest
        // process. Re-establish the native masked any-hit precondition here
        // instead of depending on the first test's process-local static facts.
        // This makes the evidence emitted by this case self-contained.
        s_evidenceFacts = {};
        RunMaskedAnyHitShadowCase();
        ASSERT_TRUE(s_evidenceFacts.maskedAnyHit);
        ASSERT_TRUE(s_evidenceFacts.shadowDispatch);

        NativeRuntime runtime;
        InitializeRuntime(runtime);
        ASSERT_NE(runtime.device.get(), nullptr);
        ASSERT_NE(runtime.shaderProvider.get(), nullptr);
        ASSERT_NE(runtime.pipelineCache.get(), nullptr);
        ASSERT_NE(runtime.pipelineCache->GetRayTracedReflectionPipeline(), nullptr);
        ASSERT_NE(runtime.pipelineCache->GetRayTracedReflectionDenoisePipeline(), nullptr);
        ASSERT_NE(runtime.pipelineCache->GetRayTracedReflectionCompositePipeline(), nullptr);
        ASSERT_NE(runtime.mesh.get(), nullptr);
        ASSERT_NE(runtime.material.get(), nullptr);
        ASSERT_NE(runtime.sceneManager.GetTopLevelAS(), nullptr);
        ASSERT_TRUE(runtime.sceneManager.GetStats().submittedTLASBuild);
        RayTracedReflectionPass reflection;
        reflection.SetEnabled(true);
        reflection.OnAdd(runtime.device.get());
        reflection.SetResources(runtime.pipelineCache.get(), &runtime.viewCache);
        reflection.SetResourceRegistry(&runtime.resources.GetRegistry());
        reflection.SetRayTracingScene(&runtime.sceneManager);
        RayTracedReflectionPassConfig reflectionConfig;
        reflectionConfig.fixedHalfResolution = true;
        reflectionConfig.temporalAccumulation = true;
        reflection.SetConfig(reflectionConfig);
        ASSERT_TRUE(reflection.IsSupported()) << reflection.GetUnsupportedReason();

        RayTracedReflectionDenoisePass denoise;
        denoise.SetEnabled(true);
        denoise.OnAdd(runtime.device.get());
        denoise.SetResources(runtime.pipelineCache.get(), &runtime.viewCache);
        denoise.SetReflectionSource(&reflection);
        ASSERT_TRUE(denoise.IsSupported()) << denoise.GetUnsupportedReason();

        RayTracedReflectionCompositePass composite;
        composite.SetEnabled(true);
        composite.OnAdd(runtime.device.get());
        composite.SetResources(runtime.pipelineCache.get(), &runtime.viewCache);
        composite.SetReflectionSource(&reflection);
        composite.SetDenoisedReflectionSource(&denoise);
        ASSERT_TRUE(composite.IsSupported()) << composite.GetUnsupportedReason();

        RayTracedReflectionPassStats lastReflectionStats{};
        RayTracedReflectionDenoisePassStats lastDenoiseStats{};
        RayTracedReflectionCompositePassStats lastCompositeStats{};
        for (uint64 frame = 1u; frame <= 2u; ++frame)
        {
            RenderGraph graph;
            RenderGraphValidationAccess::SetDevice(graph, runtime.device.get());
            std::vector<RHIBufferRef> retainedBuffers;
            std::vector<RHITextureRef> retainedTextures;
            ViewData view = MakeView(
                graph, *runtime.device, frame, retainedBuffers, retainedTextures);
            ASSERT_TRUE(view.colorTarget.IsValid());
            ASSERT_TRUE(view.depthTarget.IsValid());
            ASSERT_TRUE(view.velocityTarget.IsValid());
            ASSERT_TRUE(view.exposureState.IsValid());
            view.resetTemporalHistory = false;
            RenderPassRecordContext context = MakeContext(graph, view);
            ASSERT_TRUE(context.MatchesTargetGraph(graph));
            ASSERT_TRUE(AttachSubmittedRayTracingScene(
                context, runtime.sceneManager));

            reflection.AddToGraph(graph, context);
            denoise.AddToGraph(graph, context);
            composite.AddToGraph(graph, context);
            ASSERT_TRUE(reflection.GetStats().outputDeclared);
            ASSERT_TRUE(denoise.GetStats().outputDeclared);
            ASSERT_TRUE(composite.GetStats().outputDeclared);
            ASSERT_TRUE(composite.GetStats().denoisedSourceUsed);
            ASSERT_FALSE(composite.GetStats().denoiseFallbackToRaw);

            RenderGraphValidationAccess::Compile(graph);
            ASSERT_TRUE(graph.GetCompileStats().compileValid)
                << graph.ExportDiagnosticsText();
            RHICommandContextRef command = runtime.device->CreateCommandContext(
                RHICommandQueueType::Graphics);
            RHIFenceRef fence = runtime.device->CreateFence(0u);
            ASSERT_NE(command.Get(), nullptr);
            ASSERT_NE(fence.Get(), nullptr);
            command->Begin();
            RenderGraphValidationAccess::Execute(graph, *command);
            command->End();
            const uint64 submittedValue = runtime.device->SubmitCommandContext(
                command.Get(), fence.Get());
            ASSERT_GT(submittedValue, 0u);
            runtime.device->WaitForFence(fence.Get(), submittedValue);
            ASSERT_GE(fence->GetCompletedValue(), submittedValue);

            const RenderViewHistoryIdentity historyIdentity =
                MakeHistoryIdentity(context.identity.frameSequence);
            ASSERT_TRUE(reflection.NotifySubmission(
                context.identity, historyIdentity, MakeCompletion(submittedValue)));
            ASSERT_TRUE(reflection.CanCommitHistoryAfterPresent(historyIdentity));
            ASSERT_TRUE(reflection.CommitHistoryAfterPresent(historyIdentity));

            lastReflectionStats =
                context.results->rayTracedReflectionStats;
            lastDenoiseStats =
                context.results->rayTracedReflectionDenoiseStats;
            lastCompositeStats =
                context.results->rayTracedReflectionCompositeStats;
            ASSERT_TRUE(lastReflectionStats.dispatchRecorded);
            ASSERT_TRUE(lastReflectionStats.temporalResolveRecorded);
            ASSERT_TRUE(lastDenoiseStats.denoiseRecorded);
            ASSERT_TRUE(lastCompositeStats.compositeRecorded);
            ASSERT_TRUE(lastCompositeStats.denoisedSourceUsed);
        }

        ASSERT_EQ(lastReflectionStats.width, kHalfExtent);
        ASSERT_EQ(lastReflectionStats.height, kHalfExtent);
        ASSERT_EQ(lastReflectionStats.dispatchPixelCount,
                  static_cast<uint64>(kHalfExtent) * kHalfExtent);
        ASSERT_TRUE(lastReflectionStats.historyAvailable);
        ASSERT_TRUE(lastReflectionStats.temporalResolveRecorded);
        ASSERT_TRUE(lastReflectionStats.temporalAccumulated);
        ASSERT_EQ(lastDenoiseStats.width, kHalfExtent);
        ASSERT_EQ(lastDenoiseStats.height, kHalfExtent);
        ASSERT_EQ(lastCompositeStats.width, kExtent);
        ASSERT_EQ(lastCompositeStats.height, kExtent);
        AssertNativeValidationUnchanged(runtime);

        s_evidenceFacts.reflectionDispatch = true;
        s_evidenceFacts.spatialDenoise = true;
        s_evidenceFacts.temporalResolve =
            lastReflectionStats.temporalResolveRecorded;
        s_evidenceFacts.temporalAccumulated =
            lastReflectionStats.temporalAccumulated;
        s_evidenceFacts.denoisedComposite =
            lastCompositeStats.denoisedSourceUsed &&
            lastCompositeStats.compositeRecorded;
        s_evidenceFacts.reflectionWidth = lastReflectionStats.width;
        s_evidenceFacts.reflectionHeight = lastReflectionStats.height;
        s_evidenceFacts.reflectionDispatchPixels =
            lastReflectionStats.dispatchPixelCount;
        WriteEvidence();

        composite.OnRemove();
        denoise.OnRemove();
        reflection.OnRemove();
    }
} // namespace RVX::GFX21NativeValidation

#if defined(RVX_GFX21_NATIVE_BACKEND_DX12)
#define RVX_GFX21_NATIVE_TEST(testName) \
    TEST(HybridRayTracingDX12NativeValidation, testName)
#elif defined(RVX_GFX21_NATIVE_BACKEND_VULKAN)
#define RVX_GFX21_NATIVE_TEST(testName) \
    TEST(HybridRayTracingVulkanNativeValidation, testName)
#endif

RVX_GFX21_NATIVE_TEST(MaskedAnyHitShadowUsesCookedPipelineAndNativeDispatch)
{
    RVX::GFX21NativeValidation::RunMaskedAnyHitShadowCase();
}

RVX_GFX21_NATIVE_TEST(
    HalfResolutionReflectionUsesSpatialTemporalDenoisedCompositeAndWritesEvidence)
{
    RVX::GFX21NativeValidation::RunHalfResolutionReflectionCase();
}

#undef RVX_GFX21_NATIVE_TEST
