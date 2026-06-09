/**
 * @file SceneRenderer.cpp
 * @brief SceneRenderer implementation
 */

#include "Render/Renderer/SceneRenderer.h"
#include "Core/Log.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/ToneMapping.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/TextureResource.h"
#include "Renderer/RenderFrameResourceBinder.h"
#include "Renderer/RenderPassRegistry.h"
#include "Renderer/RenderProxySceneBridge.h"
#include "Renderer/SceneSkyboxPassBridge.h"
#include "Runtime/Camera/Camera.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

#include <algorithm>
#include <filesystem>

namespace RVX
{
namespace
{
    PostProcessSettings MakeDefaultRuntimePostProcessSettings()
    {
        PostProcessSettings settings;
        settings.enableToneMapping = true;
        settings.exposure = 1.0f;
        settings.gamma = 2.2f;
        settings.enableBloom = true;
        settings.bloomIntensity = 0.0f;
        settings.enableFXAA = false;
        settings.enableDOF = false;
        settings.enableMotionBlur = false;
        settings.enableColorGrading = false;
        settings.enableVignette = false;
        settings.enableChromaticAberration = false;
        settings.enableFilmGrain = false;
        settings.enableVolumetricLighting = false;
        settings.enableSSAO = false;
        settings.enableSSR = false;
        settings.enableTAA = false;
        return settings;
    }
}

SceneRenderer::SceneRenderer() = default;

SceneRenderer::~SceneRenderer()
{
    Shutdown();
}

bool SceneRenderer::SupportsHDRSceneColor() const
{
    const IRHIDevice* device = m_renderContext ? m_renderContext->GetDevice() : nullptr;
    if (!device)
    {
        return false;
    }

    switch (device->GetBackendType())
    {
        case RHIBackendType::DX11:
        case RHIBackendType::DX12:
        case RHIBackendType::Vulkan:
        case RHIBackendType::Metal:
        case RHIBackendType::OpenGL:
            return true;
        case RHIBackendType::Auto:
        case RHIBackendType::None:
        default:
            return false;
    }
}

SceneColorFormatPolicy SceneRenderer::ResolveSceneColorFormatPolicy(RHIFormat backBufferFormat,
                                                                    bool postProcessActive) const
{
    SceneColorFormatPolicy policy;
    policy.backBufferFormat = backBufferFormat;
    policy.toneMappingOutputFormat = backBufferFormat;
    policy.actualSceneColorFormat = backBufferFormat;

    if (backBufferFormat == RHIFormat::Unknown)
    {
        policy.hdrFallbackReason = "back buffer format is unavailable";
        return policy;
    }

    if (!postProcessActive)
    {
        policy.hdrFallbackReason = "post-process stack has no supported enabled effects";
        return policy;
    }

    if (!SupportsHDRSceneColor())
    {
        policy.hdrFallbackReason = "current RHI backend does not advertise HDR scene color support";
        return policy;
    }

    policy.actualSceneColorFormat = policy.requestedSceneColorFormat;
    policy.hdrSceneColorEnabled = true;
    policy.hdrFallbackReason.clear();
    return policy;
}

void SceneRenderer::Initialize(RenderContext* renderContext)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("SceneRenderer already initialized");
        return;
    }

    if (!renderContext)
    {
        RVX_CORE_ERROR("SceneRenderer: Invalid render context");
        return;
    }

    m_renderContext = renderContext;
    m_passRegistry = std::make_unique<RenderPassRegistry>();
    m_proxyBridge = std::make_unique<RenderProxySceneBridge>();
    m_skyboxBridge = std::make_unique<SceneSkyboxPassBridge>();

    // Create render graph
    m_renderGraph = std::make_unique<RenderGraph>();
    m_renderGraph->SetDevice(m_renderContext->GetDevice());

    // Create GPU resource manager
    m_gpuResourceManager = std::make_unique<GPUResourceManager>();
    m_gpuResourceManager->Initialize(m_renderContext->GetDevice());

    // Create pipeline cache with shader reflection
    m_pipelineCache = std::make_unique<PipelineCache>();

    // Create material system after pipeline layouts are available.
    m_materialSystem = std::make_unique<MaterialSystem>();

    // Create transient resource pool for RenderGraph
    m_transientResourcePool = std::make_unique<TransientResourcePool>();
    m_transientResourcePool->Initialize(m_renderContext->GetDevice());

    // Create resource view cache for automatic view management
    m_resourceViewCache = std::make_unique<ResourceViewCache>();
    m_resourceViewCache->Initialize(m_renderContext->GetDevice());
    m_gpuResourceManager->SetTextureInvalidatedCallback(
        [this](RHITexture* texture)
        {
            if (m_resourceViewCache)
            {
                m_resourceViewCache->InvalidateTexture(texture);
            }
        });
    
    RVX_CORE_INFO("SceneRenderer: Searching for shader directory...");
    RVX_CORE_INFO("  Current working directory: {}", std::filesystem::current_path().string());
    
    // Determine shader directory
    std::string shaderDir = m_shaderDir;
    if (shaderDir.empty())
    {
        // Default shader directory - look for Render/Shaders in several locations
        std::vector<std::string> searchPaths = {
            "Render/Shaders",
            "../Render/Shaders",
            "../../Render/Shaders",
            "../../../Render/Shaders",
            "../../../../Render/Shaders",
            "../../../../../Render/Shaders"
        };
        
        for (const auto& path : searchPaths)
        {
            RVX_CORE_DEBUG("  Checking: {} -> exists: {}", path, std::filesystem::exists(path));
            if (std::filesystem::exists(path))
            {
                shaderDir = path;
                RVX_CORE_INFO("  Found shader directory: {}", path);
                break;
            }
        }
    }

    if (shaderDir.empty())
    {
        RVX_CORE_ERROR("SceneRenderer: Could not find shader directory!");
    }

    // Get render target format from swap chain
    RHIFormat rtFormat = RHIFormat::BGRA8_UNORM;
    if (m_renderContext->GetSwapChain())
    {
        rtFormat = m_renderContext->GetSwapChain()->GetFormat();
        RVX_CORE_INFO("SceneRenderer: Render target format from swap chain: {}", static_cast<int>(rtFormat));
    }
    else
    {
        RVX_CORE_WARN("SceneRenderer: Swap chain not ready, using default render target format: {}", static_cast<int>(rtFormat));
    }
    m_sceneColorFormatPolicy = ResolveSceneColorFormatPolicy(rtFormat, true);
    m_pipelineCache->SetRenderTargetFormats(m_sceneColorFormatPolicy.actualSceneColorFormat,
                                            m_sceneColorFormatPolicy.actualSceneColorFormat,
                                            m_sceneColorFormatPolicy.toneMappingOutputFormat);
    RVX_CORE_INFO("SceneRenderer: Pipeline formats scene={}, postProcessIntermediate={}, toneMappingOutput={}",
                  static_cast<int>(m_sceneColorFormatPolicy.actualSceneColorFormat),
                  static_cast<int>(m_sceneColorFormatPolicy.actualSceneColorFormat),
                  static_cast<int>(m_sceneColorFormatPolicy.toneMappingOutputFormat));
    if (!m_sceneColorFormatPolicy.hdrSceneColorEnabled)
    {
        RVX_CORE_WARN("SceneRenderer: HDR scene color fallback: {}",
                      m_sceneColorFormatPolicy.hdrFallbackReason);
    }

    // Initialize pipeline cache
    RVX_CORE_INFO("SceneRenderer: Initializing PipelineCache...");
    if (!shaderDir.empty() && m_pipelineCache->Initialize(m_renderContext->GetDevice(), shaderDir))
    {
        RVX_CORE_INFO("SceneRenderer: PipelineCache initialized successfully!");
        RVX_CORE_INFO("  OpaquePipeline: {}", m_pipelineCache->GetOpaquePipeline() ? "created" : "null");
    }
    else
    {
        RVX_CORE_ERROR("SceneRenderer: PipelineCache failed to initialize - rendering will be limited");
        RVX_CORE_ERROR("  shaderDir was: '{}'", shaderDir);
    }

    if (m_pipelineCache && m_pipelineCache->IsInitialized() && m_materialSystem)
    {
        if (!m_materialSystem->Initialize(m_renderContext->GetDevice(), m_gpuResourceManager.get(),
                                          m_pipelineCache->GetMaterialSetLayout()))
        {
            RVX_CORE_ERROR("SceneRenderer: MaterialSystem failed to initialize");
        }
    }

    // Setup default passes (can be customized later)
    SetupDefaultPasses();
    SetupDefaultPostProcess();

    m_initialized = true;
    RVX_CORE_DEBUG("SceneRenderer initialized");
}

void SceneRenderer::Shutdown()
{
    if (!m_initialized)
        return;

    ClearPasses();
    m_depthPrepass = nullptr;
    m_opaquePass = nullptr;
    m_shadowPass = nullptr;
    m_transparentPass = nullptr;
    m_skyboxPass = nullptr;
    m_bloomPostProcess = nullptr;
    m_toneMappingPostProcess = nullptr;
    if (m_postProcessStack)
    {
        m_postProcessStack->Shutdown();
        m_postProcessStack.reset();
    }
    
    if (m_materialSystem)
    {
        m_materialSystem->Shutdown();
        m_materialSystem.reset();
    }

    if (m_pipelineCache)
    {
        m_pipelineCache->Shutdown();
        m_pipelineCache.reset();
    }

    if (m_transientResourcePool)
    {
        m_transientResourcePool->Shutdown();
        m_transientResourcePool.reset();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->Shutdown();
        m_resourceViewCache.reset();
    }
    
    if (m_gpuResourceManager)
    {
        m_gpuResourceManager->Shutdown();
        m_gpuResourceManager.reset();
    }
    
    m_renderGraph.reset();
    m_passRegistry.reset();
    m_proxyBridge.reset();
    m_skyboxBridge.reset();
    m_renderContext = nullptr;
    m_initialized = false;

    RVX_CORE_DEBUG("SceneRenderer shutdown");
}

void SceneRenderer::UpdateEnvironmentIBL(World* world)
{
    ++m_environmentIBLStats.frameCount;
    m_environmentIBLStats.skyboxFound = false;
    m_environmentIBLStats.uploadRequested = false;
    m_environmentIBLStats.textureIBLEnabled = false;
    m_environmentIBLStats.prefilteredMipLevels = 1;
    m_environmentIBLStats.intensity = 1.0f;
    m_environmentIBLStats.fallbackReason.clear();

    m_viewData.textureIBLEnabled = 0;
    m_viewData.textureIBLPrefilteredMipLevels = 1;
    m_viewData.textureIBLIntensity = 1.0f;
    m_viewData.ambientFloorIntensity = 0.08f;

    auto disableTextureIBL = [this](const char* reason)
    {
        m_environmentIBLStats.fallbackReason = reason ? reason : "Unknown";
        if (m_materialSystem)
        {
            m_materialSystem->ClearEnvironmentIBLResources();
        }
    };

    if (!m_gpuResourceManager || !m_materialSystem || !m_resourceViewCache)
    {
        disableTextureIBL("RendererIBLDependenciesMissing");
        return;
    }

    if (!world)
    {
        disableTextureIBL("NoWorld");
        return;
    }

    SceneManager* sceneManager = world->GetSceneManager();
    if (!sceneManager)
    {
        disableTextureIBL("NoSceneManager");
        return;
    }

    SkyboxComponent* skybox = nullptr;
    sceneManager->ForEachActiveEntity(
        [&skybox](SceneEntity* entity)
        {
            if (skybox || !entity)
                return;

            auto* candidate = entity->GetComponent<SkyboxComponent>();
            if (candidate && candidate->IsEnabled() && candidate->ContributesToLighting())
            {
                skybox = candidate;
            }
        });

    if (!skybox)
    {
        disableTextureIBL("NoLightingSkybox");
        return;
    }

    m_environmentIBLStats.skyboxFound = true;
    m_environmentIBLStats.intensity = skybox->GetExposure();

    Resource::TextureResource* irradiance = skybox->GetIrradianceMap().Get();
    Resource::TextureResource* prefiltered = skybox->GetPrefilteredMap().Get();
    Resource::TextureResource* brdfLUT = skybox->GetBRDFLUT().Get();
    if (!irradiance || !prefiltered || !brdfLUT)
    {
        disableTextureIBL("SkyboxIBLResourcesMissing");
        return;
    }

    m_environmentIBLStats.prefilteredMipLevels = std::max(1u, prefiltered->GetMipLevels());

    auto requestAndResolveView = [this](Resource::TextureResource* texture,
                                        const char* reason) -> bool
    {
        if (!texture)
        {
            m_environmentIBLStats.fallbackReason = reason;
            return false;
        }

        const Resource::ResourceId textureId = texture->GetId();
        if (!m_gpuResourceManager->IsResident(textureId))
        {
            m_gpuResourceManager->RequestUpload(texture, UploadPriority::High);
            m_environmentIBLStats.uploadRequested = true;
        }

        m_gpuResourceManager->MarkUsed(textureId);
        if (!m_gpuResourceManager->IsGPUReady(textureId))
        {
            m_environmentIBLStats.fallbackReason = reason;
            return false;
        }

        RHITexture* rhiTexture = m_gpuResourceManager->GetTexture(textureId);
        if (!rhiTexture || !m_resourceViewCache->GetDefaultSRV(rhiTexture))
        {
            m_environmentIBLStats.fallbackReason = reason;
            return false;
        }

        return true;
    };

    bool ready = true;
    ready = requestAndResolveView(irradiance, "IrradianceNotReady") && ready;
    ready = requestAndResolveView(prefiltered, "PrefilteredEnvironmentNotReady") && ready;
    ready = requestAndResolveView(brdfLUT, "BRDFLUTNotReady") && ready;

    if (!ready)
    {
        if (m_environmentIBLStats.fallbackReason.empty())
        {
            m_environmentIBLStats.fallbackReason = "IBLResourcesNotReady";
        }
        m_materialSystem->ClearEnvironmentIBLResources();
        return;
    }

    MaterialSystem::EnvironmentIBLResources resources;
    resources.irradianceMap = irradiance;
    resources.prefilteredMap = prefiltered;
    resources.brdfLUT = brdfLUT;
    resources.prefilteredMipLevels = m_environmentIBLStats.prefilteredMipLevels;
    resources.intensity = m_environmentIBLStats.intensity;
    resources.textureIBLEnabled = true;
    m_materialSystem->SetEnvironmentIBLResources(resources);

    m_viewData.textureIBLEnabled = 1;
    m_viewData.textureIBLPrefilteredMipLevels = resources.prefilteredMipLevels;
    m_viewData.textureIBLIntensity = resources.intensity;
    m_viewData.ambientFloorIntensity = 0.0f;
    m_environmentIBLStats.textureIBLEnabled = true;
}

void SceneRenderer::UpdateSkyboxPass(World* world)
{
    if (!m_skyboxPass || !m_skyboxBridge)
        return;

    SceneSkyboxPassActions passActions;
    passActions.setProcedural =
        [this](const Vec3& sunDirection,
               const Vec3& skyColor,
               const Vec3& horizonColor,
               const Vec3& groundColor,
               const Vec3& sunColor,
               float exposure,
               float scatteringIntensity)
        {
            m_skyboxPass->SetProceduralSkyParams(sunDirection,
                                                 skyColor,
                                                 horizonColor,
                                                 groundColor,
                                                 sunColor,
                                                 exposure,
                                                 scatteringIntensity);
        };
    passActions.setSolidColor =
        [this](const Vec3& color, float exposure)
        {
            m_skyboxPass->SetSolidColor(color, exposure);
        };
    passActions.setCubemap =
        [this](RHITexture* cubemap, float exposure, float rotation, float blurLevel)
        {
            m_skyboxPass->SetCubemap(cubemap, exposure, rotation, blurLevel);
        };
    passActions.clear =
        [this](const char* reason)
        {
            m_skyboxPass->ClearSkybox(reason);
        };

    SceneSkyboxTextureAccess textureAccess;
    textureAccess.requestUpload =
        [this](Resource::TextureResource* texture)
        {
            if (m_gpuResourceManager)
            {
                m_gpuResourceManager->RequestUpload(texture, UploadPriority::High);
            }
        };
    textureAccess.isGPUReady =
        [this](Resource::ResourceId id) -> bool
        {
            return m_gpuResourceManager && m_gpuResourceManager->IsGPUReady(id);
        };
    textureAccess.getTexture =
        [this](Resource::ResourceId id) -> RHITexture*
        {
            return m_gpuResourceManager ? m_gpuResourceManager->GetTexture(id) : nullptr;
        };

    m_skyboxBridge->Update(world, passActions, textureAccess);
}

void SceneRenderer::SetupView(const Camera& camera, World* world)
{
    if (!m_initialized)
        return;

    // Get viewport dimensions from swap chain
    uint32_t width = 1280;
    uint32_t height = 720;

    if (m_renderContext && m_renderContext->GetSwapChain())
    {
        width = m_renderContext->GetSwapChain()->GetWidth();
        height = m_renderContext->GetSwapChain()->GetHeight();
    }

    // Setup view data from camera
    m_viewData.SetupFromCamera(camera, width, height);
    UpdateEnvironmentIBL(world);
    UpdateSkyboxPass(world);

    // Collect scene data through the proxy bridge first; legacy collection is audited fallback only.
    RenderProxySceneBridgeResult proxyResult;
    if (m_proxyBridge && m_proxyBridge->BuildSnapshot(world, m_proxySnapshot, &proxyResult))
    {
        m_renderScene.ApplyProxySnapshot(m_proxySnapshot);
        m_collectionStats.lastPath = SceneRenderCollectionPath::Proxy;
        ++m_collectionStats.proxyFrameCount;
        m_collectionStats.lastProxyPrimitiveCount = proxyResult.primitiveCount;
        m_collectionStats.lastProxyLightCount = proxyResult.lightCount;
        m_collectionStats.lastFallbackOwnerId = 0;
        m_collectionStats.lastFallbackReason.clear();
    }
    else
    {
        m_renderScene.CollectFromWorld(world);
        m_collectionStats.lastPath = SceneRenderCollectionPath::LegacyFallback;
        ++m_collectionStats.legacyFallbackFrameCount;
        m_collectionStats.lastProxyPrimitiveCount = 0;
        m_collectionStats.lastProxyLightCount = 0;
        m_collectionStats.lastFallbackOwnerId = proxyResult.fallbackOwnerId;
        m_collectionStats.lastFallbackReason = ToString(proxyResult.fallbackReason);

        RVX_CORE_WARN("SceneRenderer: using legacy RenderSceneCollector fallback, reason={}, ownerId={}",
                      m_collectionStats.lastFallbackReason,
                      m_collectionStats.lastFallbackOwnerId);
    }

    // Perform visibility culling
    m_renderScene.CullAgainstCamera(camera, m_visibleObjectIndices);

    // Sort visible objects for optimal rendering
    m_renderScene.SortVisibleObjects(m_visibleObjectIndices, m_viewData.cameraPosition);
    BuildMaterialDrawLists();

    // Mark visible meshes as used for GPU resource management
    if (m_gpuResourceManager)
    {
        for (uint32_t idx : m_visibleObjectIndices)
        {
            const auto& obj = m_renderScene.GetObject(idx);
            if (!m_gpuResourceManager->IsResident(obj.meshId) && obj.meshResource)
            {
                m_gpuResourceManager->RequestUpload(obj.meshResource, UploadPriority::High);
            }
            m_gpuResourceManager->MarkUsed(obj.meshId);

            for (auto* material : obj.materialResources)
            {
                if (!material)
                    continue;

                const auto requestTexture = [this](Resource::ResourceHandle<Resource::TextureResource> textureHandle)
                {
                    auto* texture = textureHandle.Get();
                    if (!texture)
                        return;

                    if (!m_gpuResourceManager->IsResident(texture->GetId()))
                    {
                        m_gpuResourceManager->RequestUpload(texture, UploadPriority::High);
                    }
                    m_gpuResourceManager->MarkUsed(texture->GetId());
                };

                requestTexture(material->GetAlbedoTexture());
                requestTexture(material->GetNormalTexture());
                requestTexture(material->GetMetallicRoughnessTexture());
                requestTexture(material->GetAOTexture());
                requestTexture(material->GetEmissiveTexture());
            }
        }
    }
}

void SceneRenderer::BuildMaterialDrawLists()
{
    RVX::BuildMaterialDrawLists(m_renderScene,
                                m_visibleObjectIndices,
                                m_viewData.cameraPosition,
                                m_opaqueDrawItems,
                                m_maskedDrawItems,
                                m_transparentDrawItems);
}

void SceneRenderer::ApplyPostProcessSettings(const PostProcessSettings& settings)
{
    m_postProcessSettings = settings;
    if (m_postProcessStack)
    {
        m_postProcessStack->ApplySettings(m_postProcessSettings);
    }
}

void SceneRenderer::Render()
{
    if (!m_initialized || !m_renderGraph || !m_renderContext)
        return;

    // Begin new frame for transient resource pool and view cache
    if (m_transientResourcePool)
    {
        m_transientResourcePool->BeginFrame();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->BeginFrame();
    }

    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        m_pipelineCache->BeginFrame();
    }

    if (m_materialSystem && m_materialSystem->IsInitialized())
    {
        m_materialSystem->BeginFrame();
    }

    // Process pending GPU uploads with time budget
    if (m_gpuResourceManager)
    {
        m_gpuResourceManager->ProcessPendingUploads(2.0f);  // 2ms budget
    }

    // Update view constants in pipeline cache
    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        m_pipelineCache->UpdateViewConstants(m_viewData);
    }

    PreparePassesForFrame();

    // Clear the render graph for this frame
    m_renderGraph->Clear();

    // Build the render graph (creates depth buffer if needed, imports resources)
    BuildRenderGraph();

    // Update pass resources AFTER BuildRenderGraph creates resources
    // This provides passes with render scene data and texture views
    UpdatePassResources();

    // Compile the render graph (computes barriers, memory aliasing, pass culling)
    m_renderGraph->Compile();

    // Execute through RenderGraph for automatic barrier management
    RHICommandContext* ctx = m_renderContext->GetGraphicsContext();
    if (ctx)
    {
        m_renderGraph->Execute(*ctx);
    }

    // Update depth buffer state after RenderGraph execution
    // RenderGraph may have transitioned it to DepthWrite
    if (m_depthTexture)
    {
        m_depthBufferState = RHIResourceState::DepthWrite;
    }

    // Log compile stats periodically for debugging
    static uint64_t frameCount = 0;
    if (++frameCount == 1)
    {
        const auto& stats = m_renderGraph->GetCompileStats();
        RVX_CORE_INFO("RenderGraph stats: {} passes ({} culled), {} barriers, memory savings: {:.1f}%",
                      stats.totalPasses, stats.culledPasses, stats.barrierCount,
                      stats.GetMemorySavingsPercent());
    }

    // End frame for transient resource pool
    if (m_transientResourcePool)
    {
        m_transientResourcePool->EndFrame();
        
        // Evict unused resources every 60 frames (approx 1 second at 60fps)
        if (frameCount % 60 == 0)
        {
            m_transientResourcePool->EvictUnused(3);  // Evict resources unused for 3 frames
        }
    }
}

void SceneRenderer::ExecutePasses(RHICommandContext& ctx)
{
    // NOTE: This is a legacy path for manual pass execution without RenderGraph.
    // The preferred path is Render() -> BuildRenderGraph() -> RenderGraph::Execute()
    // which handles barrier management automatically.
    
    RHISwapChain* swapChain = m_renderContext->GetSwapChain();
    if (!swapChain)
        return;
    
    // Get current back buffer and its tracked state
    // State tracking is managed by BuildRenderGraph(), but if called standalone,
    // ensure we have valid state tracking
    uint32_t bufferCount = swapChain->GetBufferCount();
    if (m_backBufferStates.size() != bufferCount)
    {
        m_backBufferStates.assign(bufferCount, RHIResourceState::Undefined);
    }
    
    uint32_t backBufferIndex = swapChain->GetCurrentBackBufferIndex();
    RHITexture* backBuffer = m_renderContext->GetCurrentBackBuffer();
    RHIResourceState& backBufferState = m_backBufferStates[backBufferIndex];
    
    // Transition back buffer to RenderTarget (from Undefined on first use, Present thereafter)
    if (backBuffer && backBufferState != RHIResourceState::RenderTarget)
    {
        ctx.TextureBarrier(backBuffer, backBufferState, RHIResourceState::RenderTarget);
        backBufferState = RHIResourceState::RenderTarget;
    }
    
    // Transition depth buffer to DepthWrite if it exists
    if (m_depthTexture && m_depthBufferState != RHIResourceState::DepthWrite)
    {
        ctx.TextureBarrier(m_depthTexture.Get(), m_depthBufferState, RHIResourceState::DepthWrite);
        m_depthBufferState = RHIResourceState::DepthWrite;
    }
    
    if (!m_passRegistry)
        return;

    for (auto& pass : m_passRegistry->GetPasses())
    {
        if (pass && pass->IsEnabled())
        {
            pass->Execute(ctx, m_viewData);
        }
    }
    
    // Transition back buffer from RenderTarget back to Present
    if (backBuffer && backBufferState != RHIResourceState::Present)
    {
        ctx.TextureBarrier(backBuffer, backBufferState, RHIResourceState::Present);
        backBufferState = RHIResourceState::Present;
    }
}

void SceneRenderer::UpdatePassResources()
{
    if (!m_renderContext)
        return;

    RenderFrameResourceBinder::BindScenePassResources(
        *m_renderContext,
        m_renderScene,
        m_opaqueDrawItems,
        m_maskedDrawItems,
        m_transparentDrawItems,
        m_depthTextureView.Get(),
        m_depthPrepass,
        m_opaquePass,
        m_shadowPass,
        m_transparentPass,
        m_skyboxPass);
}

void SceneRenderer::PreparePassesForFrame()
{
    m_viewData.directionalLightDirection = Vec3{0.5f, -0.8f, 0.3f};
    m_viewData.directionalLightIntensity = 4.0f;
    m_viewData.directionalShadowEnabled = 0;
    m_viewData.directionalShadowViewProjection = Mat4Identity();
    m_viewData.directionalShadowInvMapSize = 0.0f;
    m_viewData.directionalShadowFilterRadiusTexels = 1.0f;

    if (m_shadowPass)
    {
        m_shadowPass->SetEnabled(false);
    }

    if (m_opaquePass)
    {
        m_opaquePass->SetDirectionalShadowSource(m_shadowPass);
    }

    for (const RenderLight& light : m_renderScene.GetLights())
    {
        if (light.type != RenderLight::Type::Directional || light.intensity <= 0.0f)
            continue;

        m_viewData.directionalLightDirection = light.direction;
        m_viewData.directionalLightIntensity = light.intensity;

        if (m_shadowPass && light.castsShadow)
        {
            m_shadowPass->SetDirectionalLight(light.direction, light.color, light.intensity);
        }
        break;
    }
}

void SceneRenderer::EnsureDepthBuffer(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return;
    
    // Check if we need to create or resize the depth buffer
    if (!m_depthTexture || m_depthWidth != width || m_depthHeight != height)
    {
        IRHIDevice* device = m_renderContext->GetDevice();
        if (!device)
            return;
        
        RVX_CORE_DEBUG("SceneRenderer: Creating depth buffer {}x{}", width, height);
        
        // Create depth texture
        RHITextureDesc depthDesc;
        depthDesc.width = width;
        depthDesc.height = height;
        depthDesc.depth = 1;
        depthDesc.mipLevels = 1;
        depthDesc.arraySize = 1;
        depthDesc.format = PipelineCache::GetDefaultDepthStencilFormat();
        depthDesc.dimension = RHITextureDimension::Texture2D;
        depthDesc.usage = RHITextureUsage::DepthStencil;
        depthDesc.debugName = "SceneDepthBuffer";
        
        m_depthTexture = device->CreateTexture(depthDesc);
        if (!m_depthTexture)
        {
            RVX_CORE_ERROR("SceneRenderer: Failed to create depth buffer");
            return;
        }
        
        // Create depth texture view
        RHITextureViewDesc viewDesc;
        viewDesc.format = PipelineCache::GetDefaultDepthStencilFormat();
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        viewDesc.type = RHITextureViewType::DepthStencil;
        viewDesc.debugName = "SceneDepthBufferView";
        
        m_depthTextureView = device->CreateTextureView(m_depthTexture.Get(), viewDesc);
        if (!m_depthTextureView)
        {
            RVX_CORE_ERROR("SceneRenderer: Failed to create depth buffer view");
            m_depthTexture.Reset();
            return;
        }
        
        m_depthWidth = width;
        m_depthHeight = height;
        m_depthBufferState = RHIResourceState::Undefined;  // Reset state for new buffer
        
        RVX_CORE_INFO("SceneRenderer: Created depth buffer {}x{}", width, height);
    }
}

void SceneRenderer::BuildRenderGraph()
{
    // Store RenderGraph and ViewCache pointers in ViewData so passes can access resources
    m_viewData.renderGraph = m_renderGraph.get();
    m_viewData.viewCache = m_resourceViewCache.get();
    const uint64 postProcessFrameCount = m_postProcessStats.frameCount + 1;
    m_postProcessStats = {};
    m_postProcessStats.frameCount = postProcessFrameCount;

    RGTextureHandle backBufferTarget;
    RGTextureHandle sceneColorTarget;

    // Import back buffer from swap chain
    RHISwapChain* swapChain = m_renderContext->GetSwapChain();
    if (swapChain)
    {
        // Track swapchain state - reset if resized or recreated
        uint32_t currentWidth = swapChain->GetWidth();
        uint32_t currentHeight = swapChain->GetHeight();
        uint32_t bufferCount = swapChain->GetBufferCount();
        
        if (m_backBufferStates.size() != bufferCount ||
            m_lastSwapChainWidth != currentWidth ||
            m_lastSwapChainHeight != currentHeight)
        {
            // Swap chain was recreated, reset all buffer states to Undefined
            m_backBufferStates.assign(bufferCount, RHIResourceState::Undefined);
            m_lastSwapChainWidth = currentWidth;
            m_lastSwapChainHeight = currentHeight;
        }
        
        RHITexture* backBuffer = m_renderContext->GetCurrentBackBuffer();
        if (backBuffer)
        {
            // Get the current state for this back buffer
            // First use: Undefined, subsequent uses: Present (after presentation)
            uint32_t backBufferIndex = swapChain->GetCurrentBackBufferIndex();
            RHIResourceState currentState = m_backBufferStates[backBufferIndex];
            
            // Import with actual current state (Undefined on first use, Present after presentation)
            backBufferTarget = m_renderGraph->ImportTexture(backBuffer, currentState);
            m_viewData.colorTarget = backBufferTarget;
            // Export back to Present state for display
            m_renderGraph->SetExportState(backBufferTarget, RHIResourceState::Present);
            
            // After RenderGraph executes, the back buffer will be in Present state
            m_backBufferStates[backBufferIndex] = RHIResourceState::Present;
        }
    }

    // Ensure we have a depth buffer and import it into the RenderGraph
    EnsureDepthBuffer(m_viewData.viewportWidth, m_viewData.viewportHeight);
    if (m_depthTexture)
    {
        // Import the existing depth buffer so RenderGraph can manage its barriers
        m_viewData.depthTarget = m_renderGraph->ImportTexture(
            m_depthTexture.Get(), 
            m_depthBufferState);
    }

    if (m_postProcessStack)
    {
        m_postProcessStats.stackStats = m_postProcessStack->EvaluateEffects();
    }

    const RHITextureDesc* backBufferDesc = backBufferTarget.IsValid()
                                               ? m_renderGraph->GetTextureDesc(backBufferTarget)
                                               : nullptr;
    const RHIFormat backBufferFormat = backBufferDesc ? backBufferDesc->format : RHIFormat::Unknown;
    const bool postProcessActive = m_postProcessStats.stackStats.enabledEffectCount > 0;
    m_sceneColorFormatPolicy = ResolveSceneColorFormatPolicy(backBufferFormat, postProcessActive);
    m_postProcessStats.requestedSceneColorFormat = m_sceneColorFormatPolicy.requestedSceneColorFormat;
    m_postProcessStats.actualSceneColorFormat = m_sceneColorFormatPolicy.actualSceneColorFormat;
    m_postProcessStats.backBufferFormat = m_sceneColorFormatPolicy.backBufferFormat;
    m_postProcessStats.toneMappingOutputFormat = m_sceneColorFormatPolicy.toneMappingOutputFormat;
    m_postProcessStats.hdrSceneColorEnabled = m_sceneColorFormatPolicy.hdrSceneColorEnabled;
    m_postProcessStats.hdrFallbackReason = m_sceneColorFormatPolicy.hdrFallbackReason;

    if (backBufferTarget.IsValid() && m_postProcessStats.stackStats.enabledEffectCount > 0)
    {
        if (backBufferDesc)
        {
            RHITextureDesc sceneColorDesc = *backBufferDesc;
            sceneColorDesc.format = m_sceneColorFormatPolicy.actualSceneColorFormat;
            sceneColorDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
            sceneColorDesc.debugName = "SceneColorPostProcessInput";

            sceneColorTarget = m_renderGraph->CreateTexture(sceneColorDesc);
            m_viewData.colorTarget = sceneColorTarget;
            m_postProcessStats.sceneColorStagingUsed = true;
            m_postProcessStats.directToBackBuffer = false;
            m_postProcessStats.sceneColorWidth = sceneColorDesc.width;
            m_postProcessStats.sceneColorHeight = sceneColorDesc.height;
            m_postProcessStats.sceneColorFormat = sceneColorDesc.format;
        }
        else
        {
            RVX_CORE_WARN("SceneRenderer: post-process requested but back buffer description is unavailable");
        }
    }

    // Register each render pass with the RenderGraph
    // Passes are sorted by priority, so they will be added in correct order
    if (!m_passRegistry)
        return;

    m_passChainStats.frameCount++;
    m_passChainStats.passStatuses = m_passRegistry->GetPassStatuses();
    m_passChainStats.registeredPassCount = m_passChainStats.passStatuses.size();
    m_passChainStats.graphPassCount = 0;
    m_passChainStats.skippedDisabledPassCount = 0;
    m_passChainStats.skippedUnsupportedPassCount = 0;

    for (auto& pass : m_passRegistry->GetPasses())
    {
        if (!pass)
            continue;

        const RenderPassStatus status = pass->GetStatus();
        if (!status.requestedEnabled)
        {
            m_passChainStats.skippedDisabledPassCount++;
            continue;
        }

        if (!status.supported)
        {
            m_passChainStats.skippedUnsupportedPassCount++;
            const bool alreadyLogged = std::find(m_loggedUnsupportedPassNames.begin(),
                                                 m_loggedUnsupportedPassNames.end(),
                                                 status.name) != m_loggedUnsupportedPassNames.end();
            if (!alreadyLogged)
            {
                RVX_CORE_WARN("SceneRenderer: Skipping unsupported requested pass '{}': {}",
                              status.name,
                              status.unsupportedReason.empty() ? "Unsupported" : status.unsupportedReason);
                m_loggedUnsupportedPassNames.push_back(status.name);
            }
            continue;
        }

        if (!status.enabled)
        {
            m_passChainStats.skippedDisabledPassCount++;
            continue;
        }

        // AddToGraph wraps Setup/Execute into RenderGraph callbacks.
        pass->AddToGraph(*m_renderGraph, m_viewData);
        m_passChainStats.graphPassCount++;
    }

    if (m_postProcessStats.sceneColorStagingUsed && sceneColorTarget.IsValid() && backBufferTarget.IsValid() &&
        m_postProcessStack)
    {
        m_postProcessStack->Execute(*m_renderGraph, sceneColorTarget, backBufferTarget);
        m_postProcessStats.stackStats = m_postProcessStack->GetLastExecuteStats();
    }
}

void SceneRenderer::AddPass(std::unique_ptr<IRenderPass> pass)
{
    if (!m_passRegistry)
        m_passRegistry = std::make_unique<RenderPassRegistry>();

    m_passRegistry->AddPass(std::move(pass), m_renderContext ? m_renderContext->GetDevice() : nullptr);
}

bool SceneRenderer::RemovePass(const char* name)
{
    return m_passRegistry ? m_passRegistry->RemovePass(name) : false;
}

void SceneRenderer::ClearPasses()
{
    if (m_passRegistry)
        m_passRegistry->Clear();

    m_depthPrepass = nullptr;
    m_opaquePass = nullptr;
    m_shadowPass = nullptr;
    m_transparentPass = nullptr;
    m_skyboxPass = nullptr;
    m_passChainStats = {};
    m_loggedUnsupportedPassNames.clear();
}

size_t SceneRenderer::GetPassCount() const
{
    return m_passRegistry ? m_passRegistry->GetPassCount() : 0;
}

void SceneRenderer::SetupDefaultPostProcess()
{
    if (!m_renderContext || !m_renderContext->GetDevice())
        return;

    m_postProcessStack = std::make_unique<PostProcessStack>();
    m_postProcessStack->Initialize(m_renderContext->GetDevice());
    m_bloomPostProcess = m_postProcessStack->AddEffect<BloomPass>();
    m_toneMappingPostProcess = m_postProcessStack->AddEffect<ToneMappingPass>();

    if (m_bloomPostProcess)
    {
        m_bloomPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_toneMappingPostProcess)
    {
        m_toneMappingPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    ApplyPostProcessSettings(MakeDefaultRuntimePostProcessSettings());
    if (m_toneMappingPostProcess)
    {
        m_toneMappingPostProcess->SetOperator(ToneMappingOperator::None);
    }
}

void SceneRenderer::SetupDefaultPasses()
{
    auto depthPrepass = std::make_unique<DepthPrepass>();
    depthPrepass->SetResources(m_gpuResourceManager.get(), m_pipelineCache.get());
    m_depthPrepass = depthPrepass.get();
    AddPass(std::move(depthPrepass));

    auto shadowPass = std::make_unique<ShadowPass>();
    shadowPass->SetResources(m_gpuResourceManager.get(), m_pipelineCache.get());
    m_shadowPass = shadowPass.get();
    AddPass(std::move(shadowPass));

    auto opaquePass = std::make_unique<OpaquePass>();
    opaquePass->SetResources(m_gpuResourceManager.get(), m_pipelineCache.get(), m_materialSystem.get());
    m_opaquePass = opaquePass.get();
    AddPass(std::move(opaquePass));

    auto skyboxPass = std::make_unique<SkyboxPass>();
    skyboxPass->SetResources(m_pipelineCache.get());
    m_skyboxPass = skyboxPass.get();
    AddPass(std::move(skyboxPass));

    auto transparentPass = std::make_unique<TransparentPass>();
    transparentPass->SetResources(m_gpuResourceManager.get(), m_pipelineCache.get(), m_materialSystem.get());
    m_transparentPass = transparentPass.get();
    AddPass(std::move(transparentPass));

    RVX_CORE_DEBUG("SceneRenderer: Default passes setup complete");
}

} // namespace RVX
