/**
 * @file Cube3D Sample
 * @brief 3D cube rendering demo using Engine + Subsystem architecture
 *
 * This sample demonstrates:
 * - Engine and Subsystem pattern usage
 * - Engine World/Camera management
 * - WindowSubsystem for window management
 * - RenderSubsystem with manual render control
 * - Custom render pass with depth buffer
 * - 3D transforms and basic lighting
 */

#include "Core/Core.h"
#include "Core/MathTypes.h"
#include "Engine/Engine.h"
#include "Runtime/Runtime.h"
#include "Render/RenderSubsystem.h"
#include "Render/Graph/RenderGraph.h"
#include "World/World.h"
#include "Runtime/Camera/Camera.h"
#include "RHI/RHI.h"
#include "ShaderCompiler/ShaderManager.h"
#include "ShaderCompiler/ShaderLayout.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <vector>
#include <string>
#include <cmath>

// =============================================================================
// Constant Buffer
// =============================================================================
struct TransformCB
{
    RVX::Mat4 worldViewProj;
    RVX::Mat4 world;
    float lightDir[4];
};

// =============================================================================
// Vertex Structure
// =============================================================================
struct Vertex
{
    float position[3];
    float normal[3];
    float color[4];
};

// =============================================================================
// File Loading
// =============================================================================
std::string GetExecutableDir()
{
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string fullPath(path);
    size_t pos = fullPath.find_last_of("\\/");
    if (pos != std::string::npos)
    {
        return fullPath.substr(0, pos + 1);
    }
    return "";
#else
    return "";
#endif
}

// =============================================================================
// Cube Geometry
// =============================================================================
void GenerateCubeVertices(std::vector<Vertex> &vertices, std::vector<RVX::uint16> &indices)
{
    auto addVertex = [&](float px, float py, float pz, float nx, float ny, float nz,
                         float cr, float cg, float cb, float ca)
    {
        Vertex v;
        v.position[0] = px;
        v.position[1] = py;
        v.position[2] = pz;
        v.normal[0] = nx;
        v.normal[1] = ny;
        v.normal[2] = nz;
        v.color[0] = cr;
        v.color[1] = cg;
        v.color[2] = cb;
        v.color[3] = ca;
        vertices.push_back(v);
    };

    // Right face (+X) - Red
    addVertex(0.5f, -0.5f, -0.5f, 1, 0, 0, 1.0f, 0.3f, 0.3f, 1.0f);
    addVertex(0.5f, 0.5f, -0.5f, 1, 0, 0, 1.0f, 0.3f, 0.3f, 1.0f);
    addVertex(0.5f, 0.5f, 0.5f, 1, 0, 0, 1.0f, 0.3f, 0.3f, 1.0f);
    addVertex(0.5f, -0.5f, 0.5f, 1, 0, 0, 1.0f, 0.3f, 0.3f, 1.0f);

    // Left face (-X) - Green
    addVertex(-0.5f, -0.5f, 0.5f, -1, 0, 0, 0.3f, 1.0f, 0.3f, 1.0f);
    addVertex(-0.5f, 0.5f, 0.5f, -1, 0, 0, 0.3f, 1.0f, 0.3f, 1.0f);
    addVertex(-0.5f, 0.5f, -0.5f, -1, 0, 0, 0.3f, 1.0f, 0.3f, 1.0f);
    addVertex(-0.5f, -0.5f, -0.5f, -1, 0, 0, 0.3f, 1.0f, 0.3f, 1.0f);

    // Top face (+Y) - Blue
    addVertex(-0.5f, 0.5f, -0.5f, 0, 1, 0, 0.3f, 0.3f, 1.0f, 1.0f);
    addVertex(-0.5f, 0.5f, 0.5f, 0, 1, 0, 0.3f, 0.3f, 1.0f, 1.0f);
    addVertex(0.5f, 0.5f, 0.5f, 0, 1, 0, 0.3f, 0.3f, 1.0f, 1.0f);
    addVertex(0.5f, 0.5f, -0.5f, 0, 1, 0, 0.3f, 0.3f, 1.0f, 1.0f);

    // Bottom face (-Y) - Yellow
    addVertex(-0.5f, -0.5f, 0.5f, 0, -1, 0, 1.0f, 1.0f, 0.3f, 1.0f);
    addVertex(-0.5f, -0.5f, -0.5f, 0, -1, 0, 1.0f, 1.0f, 0.3f, 1.0f);
    addVertex(0.5f, -0.5f, -0.5f, 0, -1, 0, 1.0f, 1.0f, 0.3f, 1.0f);
    addVertex(0.5f, -0.5f, 0.5f, 0, -1, 0, 1.0f, 1.0f, 0.3f, 1.0f);

    // Front face (+Z) - Magenta
    addVertex(-0.5f, -0.5f, 0.5f, 0, 0, 1, 1.0f, 0.3f, 1.0f, 1.0f);
    addVertex(0.5f, -0.5f, 0.5f, 0, 0, 1, 1.0f, 0.3f, 1.0f, 1.0f);
    addVertex(0.5f, 0.5f, 0.5f, 0, 0, 1, 1.0f, 0.3f, 1.0f, 1.0f);
    addVertex(-0.5f, 0.5f, 0.5f, 0, 0, 1, 1.0f, 0.3f, 1.0f, 1.0f);

    // Back face (-Z) - Cyan
    addVertex(0.5f, -0.5f, -0.5f, 0, 0, -1, 0.3f, 1.0f, 1.0f, 1.0f);
    addVertex(-0.5f, -0.5f, -0.5f, 0, 0, -1, 0.3f, 1.0f, 1.0f, 1.0f);
    addVertex(-0.5f, 0.5f, -0.5f, 0, 0, -1, 0.3f, 1.0f, 1.0f, 1.0f);
    addVertex(0.5f, 0.5f, -0.5f, 0, 0, -1, 0.3f, 1.0f, 1.0f, 1.0f);

    // Indices for 6 faces (2 triangles each)
    for (RVX::uint16 face = 0; face < 6; ++face)
    {
        RVX::uint16 base = face * 4;
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char *argv[])
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("RenderVerseX 3D Cube Sample (Engine Architecture)");
    RVX_CORE_INFO("Demonstrates Engine + World/Camera management with custom rendering");

    // =========================================================================
    // Parse command line for backend selection (default: Auto)
    // =========================================================================
    RVX::RHIBackendType backend = RVX::RHIBackendType::Auto;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--dx11" || arg == "-d11")
            backend = RVX::RHIBackendType::DX11;
        else if (arg == "--dx12" || arg == "-d12")
            backend = RVX::RHIBackendType::DX12;
        else if (arg == "--vulkan" || arg == "-vk")
            backend = RVX::RHIBackendType::Vulkan;
        else if (arg == "--metal" || arg == "-mtl")
            backend = RVX::RHIBackendType::Metal;
    }

    RVX_CORE_INFO("Using backend: {}", RVX::ToString(backend));

    // =========================================================================
    // Setup Engine with Subsystems
    // =========================================================================
    RVX::Engine engine;

    RVX::EngineConfig engineConfig;
    engineConfig.appName = "Cube3D Sample";
    engineConfig.windowWidth = 1280;
    engineConfig.windowHeight = 720;
    engineConfig.vsync = true;
    engine.SetConfig(engineConfig);

    // Add WindowSubsystem
    auto *windowSubsystem = engine.AddSubsystem<RVX::WindowSubsystem>();
    RVX::WindowConfig windowConfig;
    windowConfig.width = 1280;
    windowConfig.height = 720;
    windowConfig.title = "RenderVerseX - 3D Cube (Engine)";
    windowConfig.resizable = true;
    windowSubsystem->SetConfig(windowConfig);

    // Add InputSubsystem
    engine.AddSubsystem<RVX::InputSubsystem>();

    // Add RenderSubsystem with configuration
    auto *renderSubsystem = engine.AddSubsystem<RVX::RenderSubsystem>();
    RVX::RenderConfig renderConfig;
    renderConfig.backendType = backend;
    renderConfig.enableValidation = true;
    renderConfig.vsync = true;
    renderConfig.frameBuffering = 2;
    renderConfig.autoBindWindow = true;  // Automatically bind to WindowSubsystem
    renderConfig.autoRender = false;     // We do custom rendering in this sample
    renderSubsystem->SetConfig(renderConfig);

    // Initialize engine (initializes all subsystems in dependency order)
    engine.Initialize();

    // =========================================================================
    // Create World and Camera using Engine APIs
    // =========================================================================
    auto* world = engine.CreateWorld("Main");
    auto* camera = world->CreateCamera("Main");
    
    // Setup camera
    float aspect = 1280.0f / 720.0f;
    camera->SetPerspective(60.0f * 3.14159f / 180.0f, aspect, 0.1f, 100.0f);
    camera->SetPosition(RVX::Vec3(0.0f, 0.0f, -3.0f));
    // Camera looks down +Z by default when rotation is zero
    camera->UpdateMatrices();
    
    world->SetActiveCamera(camera);
    engine.SetActiveWorld(world);

    // =========================================================================
    // Get RHI Device
    // =========================================================================
    RVX::IRHIDevice *device = renderSubsystem->GetDevice();
    if (!device)
    {
        RVX_CORE_CRITICAL("Failed to get RHI device from RenderSubsystem");
        engine.Shutdown();
        return -1;
    }

    RVX_CORE_INFO("Adapter: {}", device->GetCapabilities().adapterName);

    RVX::RHISwapChain *swapChain = renderSubsystem->GetSwapChain();
    if (!swapChain)
    {
        RVX_CORE_CRITICAL("Failed to get swap chain");
        engine.Shutdown();
        return -1;
    }

    // =========================================================================
    // Create Depth Buffer
    // =========================================================================
    RVX::RHITextureDesc depthDesc = RVX::RHITextureDesc::DepthStencil(
        swapChain->GetWidth(), swapChain->GetHeight(), RVX::RHIFormat::D24_UNORM_S8_UINT);
    depthDesc.debugName = "DepthBuffer";
    auto depthBuffer = device->CreateTexture(depthDesc);

    RVX::RHITextureViewDesc depthViewDesc;
    depthViewDesc.format = RVX::RHIFormat::D24_UNORM_S8_UINT;
    auto depthView = device->CreateTextureView(depthBuffer.Get(), depthViewDesc);
    RVX_CORE_INFO("Created depth buffer");

    // =========================================================================
    // Create Command Contexts
    // =========================================================================
    std::vector<RVX::RHICommandContextRef> cmdContexts(RVX::RVX_MAX_FRAME_COUNT);
    for (RVX::uint32 i = 0; i < RVX::RVX_MAX_FRAME_COUNT; ++i)
    {
        cmdContexts[i] = device->CreateCommandContext(RVX::RHICommandQueueType::Graphics);
    }

    // =========================================================================
    // Create Cube Geometry
    // =========================================================================
    std::vector<Vertex> vertices;
    std::vector<RVX::uint16> indices;
    GenerateCubeVertices(vertices, indices);

    RVX::RHIBufferDesc vbDesc;
    vbDesc.size = vertices.size() * sizeof(Vertex);
    vbDesc.usage = RVX::RHIBufferUsage::Vertex;
    vbDesc.memoryType = RVX::RHIMemoryType::Upload;
    vbDesc.stride = sizeof(Vertex);
    vbDesc.debugName = "Cube VB";

    auto vertexBuffer = device->CreateBuffer(vbDesc);
    std::memcpy(vertexBuffer->Map(), vertices.data(), vbDesc.size);
    vertexBuffer->Unmap();

    RVX::RHIBufferDesc ibDesc;
    ibDesc.size = indices.size() * sizeof(RVX::uint16);
    ibDesc.usage = RVX::RHIBufferUsage::Index;
    ibDesc.memoryType = RVX::RHIMemoryType::Upload;
    ibDesc.debugName = "Cube IB";

    auto indexBuffer = device->CreateBuffer(ibDesc);
    std::memcpy(indexBuffer->Map(), indices.data(), ibDesc.size);
    indexBuffer->Unmap();

    RVX_CORE_INFO("Created cube geometry: {} vertices, {} indices",
                  vertices.size(), indices.size());

    // =========================================================================
    // Create Constant Buffer
    // =========================================================================
    RVX::RHIBufferDesc cbDesc;
    cbDesc.size = (sizeof(TransformCB) + 255) & ~255u;
    cbDesc.usage = RVX::RHIBufferUsage::Constant;
    cbDesc.memoryType = RVX::RHIMemoryType::Upload;
    cbDesc.debugName = "Transform CB";

    auto constantBuffer = device->CreateBuffer(cbDesc);

    // =========================================================================
    // Load and Compile Shaders
    // =========================================================================
    std::string exeDir = GetExecutableDir();
    RVX::ShaderManager shaderManager(RVX::CreateShaderCompiler());

    // Get actual backend type (in case Auto was selected)
    RVX::RHIBackendType actualBackend = device->GetBackendType();

    RVX::ShaderLoadDesc vsLoad;
    vsLoad.path = exeDir + "Shaders/Cube3D.hlsl";
    vsLoad.entryPoint = "VSMain";
    vsLoad.stage = RVX::RHIShaderStage::Vertex;
    vsLoad.backend = actualBackend;

    auto vsResult = shaderManager.LoadFromFile(device, vsLoad);
    if (!vsResult.compileResult.success)
    {
        RVX_CORE_CRITICAL("Failed to compile vertex shader: {}", vsResult.compileResult.errorMessage);
        engine.Shutdown();
        return -1;
    }

    RVX::ShaderLoadDesc psLoad = vsLoad;
    psLoad.entryPoint = "PSMain";
    psLoad.stage = RVX::RHIShaderStage::Pixel;

    auto psResult = shaderManager.LoadFromFile(device, psLoad);
    if (!psResult.compileResult.success)
    {
        RVX_CORE_CRITICAL("Failed to compile pixel shader: {}", psResult.compileResult.errorMessage);
        engine.Shutdown();
        return -1;
    }

    RVX_CORE_INFO("Compiled shaders successfully");

    // =========================================================================
    // Create Pipeline Layout
    // =========================================================================
    std::vector<RVX::ReflectedShader> reflectedShaders = {
        {vsResult.compileResult.reflection, RVX::RHIShaderStage::Vertex},
        {psResult.compileResult.reflection, RVX::RHIShaderStage::Pixel}};

    auto autoLayout = RVX::BuildAutoPipelineLayout(reflectedShaders);

    std::vector<RVX::RHIDescriptorSetLayoutRef> setLayouts(autoLayout.setLayouts.size());
    for (size_t i = 0; i < autoLayout.setLayouts.size(); ++i)
    {
        if (autoLayout.setLayouts[i].entries.empty())
            continue;
        setLayouts[i] = device->CreateDescriptorSetLayout(autoLayout.setLayouts[i]);
    }

    RVX::RHIPipelineLayoutDesc pipelineLayoutDesc = autoLayout.pipelineLayout;
    for (const auto &layout : setLayouts)
    {
        pipelineLayoutDesc.setLayouts.push_back(layout.Get());
    }

    auto pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);

    // Create descriptor set
    RVX::RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = setLayouts.empty() ? nullptr : setLayouts[0].Get();
    descSetDesc.BindBuffer(0, constantBuffer.Get());
    auto descriptorSet = device->CreateDescriptorSet(descSetDesc);

    // =========================================================================
    // Create Graphics Pipeline with Depth Test
    // =========================================================================
    RVX::RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vsResult.shader.Get();
    pipelineDesc.pixelShader = psResult.shader.Get();
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.debugName = "Cube3DPipeline";

    pipelineDesc.inputLayout.AddElement("POSITION", RVX::RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("NORMAL", RVX::RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("COLOR", RVX::RHIFormat::RGBA32_FLOAT, 0);

    pipelineDesc.rasterizerState = RVX::RHIRasterizerState::Default();
    pipelineDesc.depthStencilState = RVX::RHIDepthStencilState::Default();
    pipelineDesc.blendState = RVX::RHIBlendState::Default();

    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = swapChain->GetFormat();
    pipelineDesc.depthStencilFormat = RVX::RHIFormat::D24_UNORM_S8_UINT;
    pipelineDesc.primitiveTopology = RVX::RHIPrimitiveTopology::TriangleList;

    auto pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        RVX_CORE_CRITICAL("Failed to create graphics pipeline");
        engine.Shutdown();
        return -1;
    }
    RVX_CORE_INFO("Created graphics pipeline with depth testing");

    // =========================================================================
    // RenderGraph setup
    // =========================================================================
    RVX::RenderGraph renderGraph;
    renderGraph.SetDevice(device);

    // =========================================================================
    // Main Loop
    // =========================================================================
    RVX_CORE_INFO("3D Cube sample initialized - press ESC to exit");
    RVX_CORE_INFO("The cube will rotate automatically");

    std::vector<RVX::RHIResourceState> backBufferStates(
        swapChain->GetBufferCount(),
        RVX::RHIResourceState::Undefined);
    RVX::RHIResourceState depthState = RVX::RHIResourceState::Undefined;

    RVX::uint32 lastWidth = swapChain->GetWidth();
    RVX::uint32 lastHeight = swapChain->GetHeight();

    while (!engine.ShouldShutdown())
    {
        // Engine tick handles window events, input, time, world updates, etc.
        // Note: autoRender = false, so no automatic rendering
        engine.Tick();

        // Check ESC key via InputSubsystem
        auto *inputSubsystem = engine.GetSubsystem<RVX::InputSubsystem>();
        if (inputSubsystem && inputSubsystem->IsKeyPressed(RVX::HAL::Key::Escape))
        {
            engine.RequestShutdown();
            continue;
        }

        // Handle resize
        RVX::uint32 currentWidth, currentHeight;
        windowSubsystem->GetFramebufferSize(currentWidth, currentHeight);

        if (currentWidth != lastWidth || currentHeight != lastHeight)
        {
            if (currentWidth > 0 && currentHeight > 0)
            {
                device->WaitIdle();
                renderGraph.Clear();

                // Recreate depth buffer
                depthDesc = RVX::RHITextureDesc::DepthStencil(
                    currentWidth, currentHeight, RVX::RHIFormat::D24_UNORM_S8_UINT);
                depthDesc.debugName = "DepthBuffer";
                depthBuffer = device->CreateTexture(depthDesc);
                depthView = device->CreateTextureView(depthBuffer.Get(), depthViewDesc);
                depthState = RVX::RHIResourceState::Undefined;

                backBufferStates.assign(swapChain->GetBufferCount(), RVX::RHIResourceState::Undefined);

                // Update camera aspect ratio
                float newAspect = static_cast<float>(currentWidth) / static_cast<float>(currentHeight);
                camera->SetPerspective(60.0f * 3.14159f / 180.0f, newAspect, 0.1f, 100.0f);

                lastWidth = currentWidth;
                lastHeight = currentHeight;
                RVX_CORE_INFO("Resized to {}x{}", currentWidth, currentHeight);
            }
            continue;
        }

        // Skip rendering if window is minimized
        if (currentWidth == 0 || currentHeight == 0)
            continue;

        float time = static_cast<float>(RVX::Time::ElapsedTime());

        // Update transform using cube rotation (camera is fixed)
        RVX::Mat4 worldMatrix = RVX::MakeRotationXYZ(RVX::Vec3(time * 0.5f, time * 0.7f, time * 0.3f));

        // Calculate view/projection matrices directly (like Triangle sample)
        float currentAspect = static_cast<float>(swapChain->GetWidth()) / 
                              static_cast<float>(swapChain->GetHeight());
        RVX::Vec3 eye(0.0f, 0.0f, -3.0f);
        RVX::Vec3 at(0.0f, 0.0f, 0.0f);
        RVX::Vec3 up(0.0f, 1.0f, 0.0f);
        RVX::Mat4 view = RVX::lookAt(eye, at, up);
        RVX::Mat4 proj = RVX::MakePerspective(60.0f * 3.14159f / 180.0f, currentAspect, 0.1f, 100.0f);

        TransformCB cbData;
        cbData.worldViewProj = proj * view * worldMatrix;
        cbData.world = worldMatrix;
        cbData.lightDir[0] = 0.577f;
        cbData.lightDir[1] = 0.577f;
        cbData.lightDir[2] = -0.577f;
        cbData.lightDir[3] = 0.0f;

        constantBuffer->Upload(&cbData, 1);

        // =====================================================================
        // Render Frame using RenderGraph (manual control)
        // =====================================================================
        device->BeginFrame();

        RVX::uint32 backBufferIndex = swapChain->GetCurrentBackBufferIndex();
        RVX::uint32 frameIndex = device->GetCurrentFrameIndex();
        auto *backBuffer = swapChain->GetCurrentBackBuffer();
        auto *backBufferView = swapChain->GetCurrentBackBufferView();

        auto &cmdContext = cmdContexts[frameIndex];
        cmdContext->Begin();

        // Build RenderGraph
        renderGraph.Clear();

        auto backBufferHandle = renderGraph.ImportTexture(backBuffer, backBufferStates[backBufferIndex]);
        auto depthBufferHandle = renderGraph.ImportTexture(depthBuffer.Get(), depthState);
        renderGraph.SetExportState(backBufferHandle, RVX::RHIResourceState::Present);

        struct CubePassData
        {
            RVX::RGTextureHandle colorTarget;
            RVX::RGTextureHandle depthTarget;
        };

        // Capture pointers for lambda
        auto *pPipeline = pipeline.Get();
        auto *pDescSet = descriptorSet.Get();
        auto *pVB = vertexBuffer.Get();
        auto *pIB = indexBuffer.Get();
        auto *pSwapChain = swapChain;
        auto *pBackBufferView = backBufferView;
        auto *pDepthView = depthView.Get();
        RVX::uint32 indexCount = static_cast<RVX::uint32>(indices.size());

        renderGraph.AddPass<CubePassData>(
            "CubePass",
            RVX::RenderGraphPassType::Graphics,
            [&](RVX::RenderGraphBuilder &builder, CubePassData &data)
            {
                data.colorTarget = builder.Write(backBufferHandle, RVX::RHIResourceState::RenderTarget);
                builder.SetDepthStencil(depthBufferHandle, true, false);
                data.depthTarget = depthBufferHandle;
            },
            [=](const CubePassData &, RVX::RHICommandContext &ctx)
            {
                RVX::RHIRenderPassDesc renderPass;
                renderPass.AddColorAttachment(pBackBufferView,
                                              RVX::RHILoadOp::Clear, RVX::RHIStoreOp::Store,
                                              {0.1f, 0.1f, 0.15f, 1.0f});
                renderPass.SetDepthStencil(pDepthView,
                                           RVX::RHILoadOp::Clear, RVX::RHIStoreOp::Store,
                                           1.0f, 0);

                ctx.BeginRenderPass(renderPass);

                RVX::RHIViewport viewport = {0, 0,
                                             static_cast<float>(pSwapChain->GetWidth()),
                                             static_cast<float>(pSwapChain->GetHeight()),
                                             0.0f, 1.0f};
                ctx.SetViewport(viewport);

                RVX::RHIRect scissor = {0, 0, pSwapChain->GetWidth(), pSwapChain->GetHeight()};
                ctx.SetScissor(scissor);

                ctx.SetPipeline(pPipeline);
                ctx.SetDescriptorSet(0, pDescSet);
                ctx.SetVertexBuffer(0, pVB);
                ctx.SetIndexBuffer(pIB, RVX::RHIFormat::R16_UINT);
                ctx.DrawIndexed(indexCount, 1, 0, 0, 0);

                ctx.EndRenderPass();
            });

        renderGraph.Compile();
        renderGraph.Execute(*cmdContext);

        backBufferStates[backBufferIndex] = RVX::RHIResourceState::Present;
        depthState = RVX::RHIResourceState::DepthWrite;

        cmdContext->End();

        device->SubmitCommandContext(cmdContext.Get(), nullptr);
        swapChain->Present();
        device->EndFrame();
    }

    // =========================================================================
    // Cleanup
    // =========================================================================
    device->WaitIdle();

    descriptorSet = nullptr;
    pipeline = nullptr;
    pipelineLayout = nullptr;
    setLayouts.clear();
    vsResult.shader = nullptr;
    psResult.shader = nullptr;
    shaderManager.ClearCache();
    constantBuffer = nullptr;
    indexBuffer = nullptr;
    vertexBuffer = nullptr;
    depthView = nullptr;
    depthBuffer = nullptr;
    cmdContexts.clear();

    engine.Shutdown();

    RVX::Log::Shutdown();
    return 0;
}
