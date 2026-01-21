/**
 * @file SceneRenderer.cpp
 * @brief SceneRenderer implementation
 */

#include "Render/Renderer/SceneRenderer.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Runtime/Camera/Camera.h"
#include "Core/Log.h"
#include <algorithm>
#include <filesystem>

namespace RVX
{

SceneRenderer::~SceneRenderer()
{
    Shutdown();
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

    // Create render graph
    m_renderGraph = std::make_unique<RenderGraph>();
    m_renderGraph->SetDevice(m_renderContext->GetDevice());

    // Create GPU resource manager
    m_gpuResourceManager = std::make_unique<GPUResourceManager>();
    m_gpuResourceManager->Initialize(m_renderContext->GetDevice());

    // Create pipeline cache with shader reflection
    m_pipelineCache = std::make_unique<PipelineCache>();
    
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
    m_pipelineCache->SetRenderTargetFormat(rtFormat);

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

    // Setup default passes (can be customized later)
    SetupDefaultPasses();

    m_initialized = true;
    RVX_CORE_DEBUG("SceneRenderer initialized");
}

void SceneRenderer::Shutdown()
{
    if (!m_initialized)
        return;

    ClearPasses();
    m_opaquePass = nullptr;
    
    if (m_pipelineCache)
    {
        m_pipelineCache->Shutdown();
        m_pipelineCache.reset();
    }
    
    if (m_gpuResourceManager)
    {
        m_gpuResourceManager->Shutdown();
        m_gpuResourceManager.reset();
    }
    
    m_renderGraph.reset();
    m_renderContext = nullptr;
    m_initialized = false;

    RVX_CORE_DEBUG("SceneRenderer shutdown");
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

    // Collect scene data from world
    m_renderScene.CollectFromWorld(world);

    // Perform visibility culling
    m_renderScene.CullAgainstCamera(camera, m_visibleObjectIndices);

    // Sort visible objects for optimal rendering
    m_renderScene.SortVisibleObjects(m_visibleObjectIndices, m_viewData.cameraPosition);

    // Mark visible meshes as used for GPU resource management
    if (m_gpuResourceManager)
    {
        for (uint32_t idx : m_visibleObjectIndices)
        {
            const auto& obj = m_renderScene.GetObject(idx);
            m_gpuResourceManager->MarkUsed(obj.meshId);
        }
    }
}

void SceneRenderer::Render()
{
    if (!m_initialized || !m_renderGraph || !m_renderContext)
        return;

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

    // Update pass resources (render scene, visible indices, etc.)
    UpdatePassResources();

    // Clear the render graph for this frame
    m_renderGraph->Clear();

    // Build the render graph by calling Setup on all passes
    BuildRenderGraph();

    // Compile the render graph
    m_renderGraph->Compile();

    // Get command context and execute
    RHICommandContext* ctx = m_renderContext->GetGraphicsContext();
    if (ctx)
    {
        // For now, execute passes directly instead of through render graph
        // TODO: Integrate IRenderPass with RenderGraph's callback-based system
        ExecutePasses(*ctx);
    }
}

void SceneRenderer::ExecutePasses(RHICommandContext& ctx)
{
    // Per-frame log disabled to reduce spam
    // RVX_CORE_DEBUG("SceneRenderer: Executing {} passes", m_passes.size());
    
    // Get swap chain info
    RHISwapChain* swapChain = m_renderContext->GetSwapChain();
    if (!swapChain)
        return;
    
    // Check if swap chain was resized and reset state tracking
    uint32_t currentWidth = swapChain->GetWidth();
    uint32_t currentHeight = swapChain->GetHeight();
    uint32_t bufferCount = swapChain->GetBufferCount();
    
    if (m_backBufferStates.size() != bufferCount ||
        m_lastSwapChainWidth != currentWidth ||
        m_lastSwapChainHeight != currentHeight)
    {
        // Swap chain was recreated, reset all buffer states to Undefined
        m_backBufferStates.assign(bufferCount, RHIResourceState::Undefined);
        m_depthBufferState = RHIResourceState::Undefined;  // Depth buffer also recreated on resize
        m_lastSwapChainWidth = currentWidth;
        m_lastSwapChainHeight = currentHeight;
    }
    
    // Get current back buffer and its state
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
    
    for (auto& pass : m_passes)
    {
        if (pass && pass->IsEnabled())
        {
            // RVX_CORE_DEBUG("SceneRenderer: Executing pass '{}'", pass->GetName());
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
    // Update OpaquePass with current frame data
    if (m_opaquePass)
    {
        m_opaquePass->SetRenderScene(&m_renderScene, &m_visibleObjectIndices);
        
        // Ensure depth buffer exists and is correct size
        EnsureDepthBuffer(m_viewData.viewportWidth, m_viewData.viewportHeight);
        
        // Set render target views
        RHITextureView* colorTargetView = m_renderContext->GetCurrentBackBufferView();
        m_opaquePass->SetRenderTargets(colorTargetView, m_depthTextureView.Get());
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
        depthDesc.format = RHIFormat::D24_UNORM_S8_UINT;
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
        viewDesc.format = RHIFormat::D24_UNORM_S8_UINT;
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange::All();
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
    // Import back buffer from swap chain
    if (m_renderContext->GetSwapChain())
    {
        RHITexture* backBuffer = m_renderContext->GetCurrentBackBuffer();
        if (backBuffer)
        {
            m_viewData.colorTarget = m_renderGraph->ImportTexture(backBuffer, RHIResourceState::Present);
            m_renderGraph->SetExportState(m_viewData.colorTarget, RHIResourceState::Present);
        }
    }

    // Create depth buffer if needed
    if (!m_viewData.depthTarget.IsValid() && m_viewData.viewportWidth > 0 && m_viewData.viewportHeight > 0)
    {
        RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(
            m_viewData.viewportWidth,
            m_viewData.viewportHeight,
            RHIFormat::D24_UNORM_S8_UINT);
        depthDesc.debugName = "SceneDepth";
        m_viewData.depthTarget = m_renderGraph->CreateTexture(depthDesc);
    }

    // Call Setup on each render pass
    for (auto& pass : m_passes)
    {
        if (pass && pass->IsEnabled())
        {
            // Note: Actual pass setup would use RenderGraphBuilder
            // For now, passes will be integrated in Phase 4
        }
    }
}

void SceneRenderer::AddPass(std::unique_ptr<IRenderPass> pass)
{
    if (!pass)
        return;

    RVX_CORE_DEBUG("SceneRenderer: Adding pass '{}'", pass->GetName());
    m_passes.push_back(std::move(pass));

    // Re-sort passes by priority
    std::stable_sort(m_passes.begin(), m_passes.end(),
        [](const std::unique_ptr<IRenderPass>& a, const std::unique_ptr<IRenderPass>& b)
        {
            return a->GetPriority() < b->GetPriority();
        });
}

bool SceneRenderer::RemovePass(const char* name)
{
    auto it = std::find_if(m_passes.begin(), m_passes.end(),
        [name](const std::unique_ptr<IRenderPass>& pass)
        {
            return pass && strcmp(pass->GetName(), name) == 0;
        });

    if (it != m_passes.end())
    {
        RVX_CORE_DEBUG("SceneRenderer: Removing pass '{}'", name);
        m_passes.erase(it);
        return true;
    }

    return false;
}

void SceneRenderer::ClearPasses()
{
    m_passes.clear();
}

void SceneRenderer::SetupDefaultPasses()
{
    // Create opaque pass
    auto opaquePass = std::make_unique<OpaquePass>();
    
    // Set resource dependencies
    opaquePass->SetResources(m_gpuResourceManager.get(), m_pipelineCache.get());
    
    // Store raw pointer for quick access
    m_opaquePass = opaquePass.get();
    
    // Add to pass list
    AddPass(std::move(opaquePass));
    
    RVX_CORE_DEBUG("SceneRenderer: Default passes setup complete");
}

} // namespace RVX
