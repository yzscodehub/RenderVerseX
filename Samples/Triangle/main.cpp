#include "Core/Core.h"
#include "RHI/RHI.h"
#include "RenderGraph/RenderGraph.h"

#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <Windows.h>
#endif

#include <fstream>
#include <vector>
#include <string>
#include <cmath>

#include "ShaderCompiler/ShaderManager.h"
#include "ShaderCompiler/ShaderLayout.h"
#include "Engine/Engine.h"
#include "Engine/Systems/WindowSystem.h"
#include "Engine/Systems/RenderSystem.h"
#include "Engine/Systems/InputSystem.h"
#include "Engine/Systems/CameraSystem.h"
#include "Platform/InputBackend_GLFW.h"
#include "Camera/OrbitController.h"
#include "Core/Math.h"

// =============================================================================
// Transform Constant Buffer Data
// =============================================================================
struct TransformCB
{
    RVX::Mat4 worldMatrix;
    float tintColor[4];
};

// =============================================================================
// Vertex Structure
// =============================================================================
struct Vertex
{
    float position[3];
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

std::vector<uint8_t> LoadFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("Failed to open file: {}", path);
        return {};
    }

    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char *>(data.data()), size);
    return data;
}

std::string LoadTextFile(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("Failed to open file: {}", path);
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char *argv[])
{
    // Initialize logging
    RVX::Log::Initialize();
    RVX_CORE_INFO("RenderVerseX Triangle Sample - Interactive Rotation");

    // Initialize GLFW
    if (!glfwInit())
    {
        RVX_CORE_CRITICAL("Failed to initialize GLFW");
        return -1;
    }

    // Create window (no OpenGL context)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *window = glfwCreateWindow(1280, 720, "RenderVerseX - Interactive Triangle", nullptr, nullptr);
    if (!window)
    {
        RVX_CORE_CRITICAL("Failed to create window");
        glfwTerminate();
        return -1;
    }

    // Select backend
    RVX::RHIBackendType backend = RVX::RHIBackendType::DX12;

    // Parse command line for backend selection
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--dx11" || arg == "-d11")
            backend = RVX::RHIBackendType::DX11;
        else if (arg == "--dx12" || arg == "-d12")
            backend = RVX::RHIBackendType::DX12;
        else if (arg == "--vulkan" || arg == "-vk")
            backend = RVX::RHIBackendType::Vulkan;
    }

    RVX_CORE_INFO("Using backend: {}", RVX::ToString(backend));

    // Create RHI device
    RVX::RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.applicationName = "Triangle Sample";

    auto device = RVX::CreateRHIDevice(backend, deviceDesc);
    if (!device)
    {
        RVX_CORE_CRITICAL("Failed to create RHI device");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    RVX_CORE_INFO("Device capabilities:");
    RVX_CORE_INFO("  Adapter: {}", device->GetCapabilities().adapterName);
    RVX_CORE_INFO("  Bindless: {}", device->GetCapabilities().supportsBindless ? "Yes" : "No");
    RVX_CORE_INFO("  Raytracing: {}", device->GetCapabilities().supportsRaytracing ? "Yes" : "No");

    // Create swap chain
#ifdef _WIN32
    RVX::RHISwapChainDesc swapChainDesc;
    swapChainDesc.windowHandle = glfwGetWin32Window(window);
    swapChainDesc.width = 1280;
    swapChainDesc.height = 720;
    swapChainDesc.format = RVX::RHIFormat::BGRA8_UNORM_SRGB;
    swapChainDesc.bufferCount = 3;
    swapChainDesc.vsync = true;

    auto swapChain = device->CreateSwapChain(swapChainDesc);
    if (!swapChain)
    {
        RVX_CORE_CRITICAL("Failed to create swap chain");
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
#endif

    // Create per-frame command contexts
    std::vector<RVX::RHICommandContextRef> cmdContexts(RVX::RVX_MAX_FRAME_COUNT);
    for (RVX::uint32 i = 0; i < RVX::RVX_MAX_FRAME_COUNT; ++i)
    {
        cmdContexts[i] = device->CreateCommandContext(RVX::RHICommandQueueType::Graphics);
    }

    // =========================================================================
    // Create Vertex Buffer - Triangle in NDC
    // =========================================================================
    Vertex triangleVertices[] =
        {
            {{0.0f, 0.6f, 0.0f}, {1.0f, 0.2f, 0.3f, 1.0f}},   // Top - Coral
            {{0.5f, -0.4f, 0.0f}, {0.2f, 1.0f, 0.4f, 1.0f}},  // Right - Lime
            {{-0.5f, -0.4f, 0.0f}, {0.3f, 0.4f, 1.0f, 1.0f}}, // Left - Sky Blue
        };

    RVX::RHIBufferDesc vbDesc;
    vbDesc.size = sizeof(triangleVertices);
    vbDesc.usage = RVX::RHIBufferUsage::Vertex;
    vbDesc.memoryType = RVX::RHIMemoryType::Upload;
    vbDesc.stride = sizeof(Vertex);
    vbDesc.debugName = "Triangle VB";

    auto vertexBuffer = device->CreateBuffer(vbDesc);
    if (!vertexBuffer)
    {
        RVX_CORE_CRITICAL("Failed to create vertex buffer");
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    vertexBuffer->Upload(triangleVertices, 3);
    RVX_CORE_INFO("Created vertex buffer");

    // =========================================================================
    // Create Constant Buffer for Transform
    // =========================================================================
    RVX::RHIBufferDesc cbDesc;
    cbDesc.size = (sizeof(TransformCB) + 255) & ~255u;
    cbDesc.usage = RVX::RHIBufferUsage::Constant;
    cbDesc.memoryType = RVX::RHIMemoryType::Upload;
    cbDesc.debugName = "Transform CB";

    auto constantBuffer = device->CreateBuffer(cbDesc);
    if (!constantBuffer)
    {
        RVX_CORE_CRITICAL("Failed to create constant buffer");
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    RVX_CORE_INFO("Created constant buffer");

    // =========================================================================
    // Load and Compile Shaders
    // =========================================================================
    std::string exeDir = GetExecutableDir();
    std::string shaderPath = exeDir + "Shaders/Triangle.hlsl";
    RVX_CORE_INFO("Loading shader from: {}", shaderPath);

    std::string shaderSource = LoadTextFile(shaderPath);
    if (shaderSource.empty())
    {
        RVX_CORE_CRITICAL("Failed to load shader source");
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    RVX::ShaderManager shaderManager(RVX::CreateShaderCompiler());

    const bool useSrgbOutput = (swapChain->GetFormat() == RVX::RHIFormat::BGRA8_UNORM_SRGB ||
                                swapChain->GetFormat() == RVX::RHIFormat::RGBA8_UNORM_SRGB);

    RVX::ShaderLoadDesc vsLoad;
    vsLoad.path = shaderPath;
    vsLoad.entryPoint = "VSMain";
    vsLoad.stage = RVX::RHIShaderStage::Vertex;
    vsLoad.backend = backend;
    vsLoad.enableDebugInfo = true;
    vsLoad.enableOptimization = false;

    auto vsResult = shaderManager.LoadFromFile(device.get(), vsLoad);
    if (!vsResult.compileResult.success || !vsResult.shader)
    {
        RVX_CORE_CRITICAL("Failed to compile vertex shader: {}", vsResult.compileResult.errorMessage);
        shaderManager.ClearCache();
        constantBuffer = nullptr;
        cmdContexts.clear();
        vertexBuffer = nullptr;
        swapChain = nullptr;
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    auto vertexShader = vsResult.shader;
    RVX_CORE_INFO("VS bytecode size: {} bytes", vsResult.compileResult.bytecode.size());
    RVX_CORE_INFO("VS reflection: {} resources, {} inputs, {} push constants",
                  vsResult.compileResult.reflection.resources.size(),
                  vsResult.compileResult.reflection.inputs.size(),
                  vsResult.compileResult.reflection.pushConstants.size());
    if (vsResult.compileResult.bytecode.size() >= sizeof(RVX::uint32))
    {
        RVX::uint32 magic = 0;
        std::memcpy(&magic, vsResult.compileResult.bytecode.data(), sizeof(RVX::uint32));
        RVX_CORE_INFO("VS bytecode magic: 0x{:08X}", magic);
    }

    RVX::ShaderLoadDesc psLoad = vsLoad;
    psLoad.entryPoint = "PSMain";
    psLoad.stage = RVX::RHIShaderStage::Pixel;
    if (!useSrgbOutput)
    {
        psLoad.defines.push_back({"RVX_APPLY_SRGB_OUTPUT", "1"});
    }

    auto psResult = shaderManager.LoadFromFile(device.get(), psLoad);
    if (!psResult.compileResult.success || !psResult.shader)
    {
        RVX_CORE_CRITICAL("Failed to compile pixel shader: {}", psResult.compileResult.errorMessage);
        shaderManager.ClearCache();
        constantBuffer = nullptr;
        cmdContexts.clear();
        vertexBuffer = nullptr;
        swapChain = nullptr;
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    auto pixelShader = psResult.shader;
    RVX_CORE_INFO("PS bytecode size: {} bytes", psResult.compileResult.bytecode.size());
    RVX_CORE_INFO("PS reflection: {} resources, {} inputs, {} push constants",
                  psResult.compileResult.reflection.resources.size(),
                  psResult.compileResult.reflection.inputs.size(),
                  psResult.compileResult.reflection.pushConstants.size());
    if (psResult.compileResult.bytecode.size() >= sizeof(RVX::uint32))
    {
        RVX::uint32 magic = 0;
        std::memcpy(&magic, psResult.compileResult.bytecode.data(), sizeof(RVX::uint32));
        RVX_CORE_INFO("PS bytecode magic: 0x{:08X}", magic);
    }

    // =========================================================================
    // Create Descriptor Set Layout and Pipeline Layout (Auto)
    // =========================================================================
    std::vector<RVX::ReflectedShader> reflectedShaders = {
        {vsResult.compileResult.reflection, RVX::RHIShaderStage::Vertex},
        {psResult.compileResult.reflection, RVX::RHIShaderStage::Pixel}}; 

    auto autoLayout = RVX::BuildAutoPipelineLayout(reflectedShaders);
    RVX_CORE_INFO("AutoLayout: set count={}, push constants size={}, stages={}",
                  autoLayout.setLayouts.size(),
                  autoLayout.pipelineLayout.pushConstantSize,
                  static_cast<RVX::uint32>(autoLayout.pipelineLayout.pushConstantStages));

    std::vector<RVX::RHIDescriptorSetLayoutRef> setLayouts(autoLayout.setLayouts.size());
    for (size_t i = 0; i < autoLayout.setLayouts.size(); ++i)
    {
        if (autoLayout.setLayouts[i].entries.empty())
            continue;

        autoLayout.setLayouts[i].debugName = "TriangleSetLayout";
        RVX_CORE_INFO("Set {} bindings: {}", static_cast<RVX::uint32>(i),
                      static_cast<RVX::uint32>(autoLayout.setLayouts[i].entries.size()));
        setLayouts[i] = device->CreateDescriptorSetLayout(autoLayout.setLayouts[i]);
    }

    RVX::RHIPipelineLayoutDesc pipelineLayoutDesc = autoLayout.pipelineLayout;
    pipelineLayoutDesc.debugName = "TrianglePipelineLayout";
    for (const auto &layout : setLayouts)
    {
        pipelineLayoutDesc.setLayouts.push_back(layout.Get());
    }

    auto pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    if (!pipelineLayout)
    {
        RVX_CORE_CRITICAL("Failed to create pipeline layout");
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // =========================================================================
    // Create Descriptor Set
    // =========================================================================
    RVX::RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = setLayouts.empty() ? nullptr : setLayouts[0].Get();
    if (!descSetDesc.layout)
    {
        RVX_CORE_CRITICAL("Auto layout generation failed (set 0 missing)");
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    descSetDesc.debugName = "TriangleDescSet";
    descSetDesc.BindBuffer(0, constantBuffer.Get());
    auto descriptorSet = device->CreateDescriptorSet(descSetDesc);

    if (!descriptorSet)
    {
        RVX_CORE_CRITICAL("Failed to create descriptor set");
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // =========================================================================
    // Create Graphics Pipeline
    // =========================================================================
    RVX::RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader.Get();
    pipelineDesc.pixelShader = pixelShader.Get();
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.debugName = "TrianglePipeline";

    // Input layout
    pipelineDesc.inputLayout.AddElement("POSITION", RVX::RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("COLOR", RVX::RHIFormat::RGBA32_FLOAT, 0);

    // Rasterizer state - disable culling to see both sides
    pipelineDesc.rasterizerState = RVX::RHIRasterizerState::NoCull();

    // Depth stencil - disabled for simple 2D triangle
    pipelineDesc.depthStencilState = RVX::RHIDepthStencilState::Disabled();

    // Blend state
    pipelineDesc.blendState = RVX::RHIBlendState::Default();

    // Render target format
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = swapChain->GetFormat();
    pipelineDesc.depthStencilFormat = RVX::RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RVX::RHIPrimitiveTopology::TriangleList;

    auto pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        RVX_CORE_CRITICAL("Failed to create graphics pipeline");
        descriptorSet = nullptr;
        pipelineLayout = nullptr;
        setLayouts.clear();
        vertexShader = nullptr;
        pixelShader = nullptr;
        vsResult.shader = nullptr;
        psResult.shader = nullptr;
        shaderManager.ClearCache();
        constantBuffer = nullptr;
        cmdContexts.clear();
        vertexBuffer = nullptr;
        swapChain = nullptr;
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    RVX_CORE_INFO("Created graphics pipeline");

    // =========================================================================
    // Main Loop
    // =========================================================================
    RVX_CORE_INFO("Triangle sample initialized - entering main loop");
    RVX_CORE_INFO("Controls:");
    RVX_CORE_INFO("  Left Mouse Drag: Rotate X/Y");
    RVX_CORE_INFO("  Mouse Scroll: Rotate Z");
    RVX_CORE_INFO("  ESC: Exit");

    RVX::uint32 frameCount = 0;
    double lastTime = glfwGetTime();

    // Track back buffer states
    std::vector<RVX::RHIResourceState> backBufferStates(
        swapChain->GetBufferCount(),
        RVX::RHIResourceState::Undefined);

    // System-based rendering setup
    RVX::Engine engine;
    auto &systems = engine.GetSystemManager();
    auto *windowSystem = systems.RegisterSystem<RVX::WindowSystem>(
        []()
        { glfwPollEvents(); },
        [&]()
        { return glfwWindowShouldClose(window); },
        [&](RVX::uint32 &w, RVX::uint32 &h)
        {
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            w = static_cast<RVX::uint32>(width);
            h = static_cast<RVX::uint32>(height);
        });

    auto *inputSystem = systems.RegisterSystem<RVX::InputSystem>();
    inputSystem->SetBackend(std::make_unique<RVX::GlfwInputBackend>(window));

    auto *cameraSystem = systems.RegisterSystem<RVX::CameraSystem>();
    cameraSystem->SetInputSystem(inputSystem);
    cameraSystem->SetController(std::make_unique<RVX::OrbitController>());
    float aspect = static_cast<float>(swapChain->GetWidth()) / static_cast<float>(swapChain->GetHeight());
    cameraSystem->GetCamera().SetPerspective(60.0f * 3.14159265f / 180.0f, aspect, 0.1f, 100.0f);

    auto *renderSystem = systems.RegisterSystem<RVX::RenderSystem>();
    renderSystem->Initialize(device.get(), swapChain.Get(), &cmdContexts);
    // Track if we've logged render graph stats
    bool loggedRenderGraphStats = false;
    bool savedGraphviz = false;

    // Track memory aliasing demo state
    // Run the demo for a few frames, then WaitIdle before switching to normal rendering
    int memoryAliasingDemoFrameCount = 0;
    constexpr int DEMO_FRAMES = 3;  // Run demo for 3 frames to match triple buffering

    renderSystem->SetRenderCallback([&](RVX::RHICommandContext &ctx,
                                        RVX::RHISwapChain &swapChainRef,
                                        RVX::RenderGraph &graph,
                                        RVX::uint32 backBufferIndex)
                                    {
        auto* backBuffer = swapChainRef.GetCurrentBackBuffer();
        auto* backBufferView = swapChainRef.GetCurrentBackBufferView();

        // During demo phase and transition, wait for GPU to prevent destroying in-use resources
        // This is needed because demo creates new transient resources each frame
        if (memoryAliasingDemoFrameCount > 0 && memoryAliasingDemoFrameCount <= DEMO_FRAMES)
        {
            device->WaitIdle();
        }

        graph.Clear();

        auto backBufferHandle = graph.ImportTexture(backBuffer, backBufferStates[backBufferIndex]);
        graph.SetExportState(backBufferHandle, RVX::RHIResourceState::Present);

        // Only demonstrate memory aliasing on first few frames
        bool showMemoryAliasingDemo = (memoryAliasingDemoFrameCount < DEMO_FRAMES);
        
        if (showMemoryAliasingDemo)
        {
            // Enable memory aliasing for transient resources
            graph.SetMemoryAliasingEnabled(true);

            // Create some transient textures to demonstrate memory aliasing
            // These textures have non-overlapping lifetimes and can share memory
            RVX::RHITextureDesc transientDesc;
            transientDesc.width = swapChainRef.GetWidth() / 2;
            transientDesc.height = swapChainRef.GetHeight() / 2;
            transientDesc.format = RVX::RHIFormat::RGBA16_FLOAT;
            transientDesc.usage = RVX::RHITextureUsage::RenderTarget | RVX::RHITextureUsage::ShaderResource;
            
            transientDesc.debugName = "TransientA";
            auto transientA = graph.CreateTexture(transientDesc);

            transientDesc.debugName = "TransientB";
            auto transientB = graph.CreateTexture(transientDesc);

            transientDesc.debugName = "TransientC";
            auto transientC = graph.CreateTexture(transientDesc);

            struct PassAData { RVX::RGTextureHandle output; };
            struct PassBData { RVX::RGTextureHandle input; RVX::RGTextureHandle output; };
            struct PassCData { RVX::RGTextureHandle input; RVX::RGTextureHandle output; };

            // Pass 1: Write to TransientA
            graph.AddPass<PassAData>(
                "TransientPass_A",
                RVX::RenderGraphPassType::Graphics,
                [&](RVX::RenderGraphBuilder& builder, PassAData& data)
                {
                    data.output = builder.Write(transientA, RVX::RHIResourceState::RenderTarget);
                },
                [](const PassAData&, RVX::RHICommandContext&) { /* Dummy clear or render */ });

            // Pass 2: Read TransientA, Write TransientB
            graph.AddPass<PassBData>(
                "TransientPass_B",
                RVX::RenderGraphPassType::Graphics,
                [&](RVX::RenderGraphBuilder& builder, PassBData& data)
                {
                    data.input = builder.Read(transientA);
                    data.output = builder.Write(transientB, RVX::RHIResourceState::RenderTarget);
                },
                [](const PassBData&, RVX::RHICommandContext&) { /* Dummy process */ });

            // Pass 3: Read TransientB, Write TransientC
            graph.AddPass<PassCData>(
                "TransientPass_C",
                RVX::RenderGraphPassType::Graphics,
                [&](RVX::RenderGraphBuilder& builder, PassCData& data)
                {
                    data.input = builder.Read(transientB);
                    data.output = builder.Write(transientC, RVX::RHIResourceState::RenderTarget);
                },
                [](const PassCData&, RVX::RHICommandContext&) { /* Dummy process */ });

            // Main triangle pass - also reads TransientC to keep the chain alive
            struct TrianglePassDataFull
            {
                RVX::RGTextureHandle colorTarget;
                RVX::RGTextureHandle transientInput;
                RVX::RHITextureView* colorTargetView = nullptr;
            };

            graph.AddPass<TrianglePassDataFull>(
                "Triangle",
                RVX::RenderGraphPassType::Graphics,
                [&](RVX::RenderGraphBuilder& builder, TrianglePassDataFull& data)
                {
                    data.transientInput = builder.Read(transientC);
                    data.colorTarget = builder.Write(backBufferHandle, RVX::RHIResourceState::RenderTarget);
                    data.colorTargetView = backBufferView;
                },
                [&](const TrianglePassDataFull& data, RVX::RHICommandContext& ctx)
                {
                    RVX::RHIRenderPassDesc renderPass;
                    renderPass.AddColorAttachment(
                        data.colorTargetView,
                        RVX::RHILoadOp::Clear,
                        RVX::RHIStoreOp::Store,
                        {0.08f, 0.08f, 0.12f, 1.0f}
                    );

                    ctx.BeginRenderPass(renderPass);

                    RVX::RHIViewport viewport;
                    viewport.x = 0;
                    viewport.y = 0;
                    viewport.width = static_cast<float>(swapChainRef.GetWidth());
                    viewport.height = static_cast<float>(swapChainRef.GetHeight());
                    viewport.minDepth = 0.0f;
                    viewport.maxDepth = 1.0f;
                    ctx.SetViewport(viewport);

                    RVX::RHIRect scissor;
                    scissor.x = 0;
                    scissor.y = 0;
                    scissor.width = swapChainRef.GetWidth();
                    scissor.height = swapChainRef.GetHeight();
                    ctx.SetScissor(scissor);

                    ctx.SetPipeline(pipeline.Get());
                    ctx.SetDescriptorSet(0, descriptorSet.Get());
                    ctx.SetVertexBuffer(0, vertexBuffer.Get());
                    ctx.Draw(3, 1, 0, 0);
                    ctx.EndRenderPass();
                });

            graph.Compile();

            // Log render graph statistics once
            if (!loggedRenderGraphStats)
            {
                const auto& stats = graph.GetCompileStats();
                RVX_CORE_INFO("=== RenderGraph Statistics (Memory Aliasing Demo) ===");
                RVX_CORE_INFO("  Total passes: {}", stats.totalPasses);
                RVX_CORE_INFO("  Culled passes: {}", stats.culledPasses);
                RVX_CORE_INFO("  Barriers: {} (tex: {}, buf: {})", 
                    stats.barrierCount, stats.textureBarrierCount, stats.bufferBarrierCount);
                RVX_CORE_INFO("  Merged barriers: {}", stats.mergedBarrierCount);
                RVX_CORE_INFO("  Transient textures: {}, buffers: {}", 
                    stats.totalTransientTextures, stats.totalTransientBuffers);
                RVX_CORE_INFO("  Aliased textures: {}, buffers: {}", 
                    stats.aliasedTextureCount, stats.aliasedBufferCount);
                RVX_CORE_INFO("  Memory without aliasing: {} KB", stats.memoryWithoutAliasing / 1024);
                RVX_CORE_INFO("  Memory with aliasing: {} KB", stats.memoryWithAliasing / 1024);
                RVX_CORE_INFO("  Memory savings: {:.1f}%", stats.GetMemorySavingsPercent());
                RVX_CORE_INFO("  Transient heaps: {}", stats.transientHeapCount);
                loggedRenderGraphStats = true;
            }

            // Save Graphviz DOT file once
            if (!savedGraphviz)
            {
                std::string dotPath = exeDir + "render_graph.dot";
                if (graph.SaveGraphviz(dotPath.c_str()))
                {
                    RVX_CORE_INFO("Saved RenderGraph visualization to: {}", dotPath);
                    RVX_CORE_INFO("  To view: dot -Tpng {} -o render_graph.png", dotPath);
                }
                savedGraphviz = true;
            }

            graph.Execute(ctx);
            
            // Increment demo frame counter
            memoryAliasingDemoFrameCount++;
        }
        else
        {
            // Simple rendering without transient resources (normal per-frame path)
            struct TrianglePassData
            {
                RVX::RGTextureHandle colorTarget;
                RVX::RHITextureView* colorTargetView = nullptr;
            };

            graph.AddPass<TrianglePassData>(
                "Triangle",
                RVX::RenderGraphPassType::Graphics,
                [&](RVX::RenderGraphBuilder& builder, TrianglePassData& data)
                {
                    data.colorTarget = builder.Write(backBufferHandle, RVX::RHIResourceState::RenderTarget);
                    data.colorTargetView = backBufferView;
                },
                [&](const TrianglePassData& data, RVX::RHICommandContext& ctx)
                {
                    RVX::RHIRenderPassDesc renderPass;
                    renderPass.AddColorAttachment(
                        data.colorTargetView,
                        RVX::RHILoadOp::Clear,
                        RVX::RHIStoreOp::Store,
                        {0.08f, 0.08f, 0.12f, 1.0f}
                    );

                    ctx.BeginRenderPass(renderPass);

                    RVX::RHIViewport viewport;
                    viewport.x = 0;
                    viewport.y = 0;
                    viewport.width = static_cast<float>(swapChainRef.GetWidth());
                    viewport.height = static_cast<float>(swapChainRef.GetHeight());
                    viewport.minDepth = 0.0f;
                    viewport.maxDepth = 1.0f;
                    ctx.SetViewport(viewport);

                    RVX::RHIRect scissor;
                    scissor.x = 0;
                    scissor.y = 0;
                    scissor.width = swapChainRef.GetWidth();
                    scissor.height = swapChainRef.GetHeight();
                    ctx.SetScissor(scissor);

                    ctx.SetPipeline(pipeline.Get());
                    ctx.SetDescriptorSet(0, descriptorSet.Get());
                    ctx.SetVertexBuffer(0, vertexBuffer.Get());
                    ctx.Draw(3, 1, 0, 0);
                    ctx.EndRenderPass();
                });

            graph.Compile();
            graph.Execute(ctx);
        }

        backBufferStates[backBufferIndex] = RVX::RHIResourceState::Present; });

    systems.AddDependency("InputSystem", "WindowSystem");
    systems.AddDependency("CameraSystem", "InputSystem");
    systems.AddDependency("RenderSystem", "WindowSystem");
    engine.Init();

    double lastFrameTime = glfwGetTime();
    float modelRoll = 0.0f;
    while (!windowSystem->ShouldClose())
    {
        // Check for ESC key
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        const auto &inputState = inputSystem->GetState();
        if (inputState.mouseWheel != 0.0f)
        {
            modelRoll += inputState.mouseWheel * 0.1f;
        }

        // Update constant buffer with transform
        TransformCB transformData;
        auto rotation = cameraSystem->GetCamera().GetRotation();
        transformData.worldMatrix = RVX::Mat4::RotationXYZ({rotation.x, rotation.y, modelRoll});

        // Pulsing tint color based on time
        double currentTime = glfwGetTime();
        float pulse = (std::sin(static_cast<float>(currentTime) * 2.0f) + 1.0f) * 0.2f + 0.8f;
        transformData.tintColor[0] = pulse;
        transformData.tintColor[1] = pulse;
        transformData.tintColor[2] = pulse;
        transformData.tintColor[3] = 1.0f;

        constantBuffer->Upload(&transformData, 1);

        double now = glfwGetTime();
        float deltaTime = static_cast<float>(now - lastFrameTime);
        lastFrameTime = now;
        engine.Tick(deltaTime);

        frameCount++;

        // FPS counter
        if (currentTime - lastTime >= 1.0)
        {
            RVX_CORE_DEBUG("FPS: {}", frameCount);
            frameCount = 0;
            lastTime = currentTime;
        }

        // Handle resize
        RVX::uint32 width = 0;
        RVX::uint32 height = 0;
        if (windowSystem->ConsumeResize(width, height))
        {
            renderSystem->HandleResize(width, height);
            float newAspect = static_cast<float>(swapChain->GetWidth()) / static_cast<float>(swapChain->GetHeight());
            cameraSystem->GetCamera().SetPerspective(60.0f * 3.14159265f / 180.0f, newAspect, 0.1f, 100.0f);
            backBufferStates.assign(swapChain->GetBufferCount(), RVX::RHIResourceState::Undefined);
            RVX_CORE_INFO("Resized to {}x{}", width, height);
        }
    }

    // Cleanup
    engine.Shutdown();
    device->WaitIdle();

    descriptorSet = nullptr;
    pipeline = nullptr;
    pipelineLayout = nullptr;
    setLayouts.clear();
    vertexShader = nullptr;
    pixelShader = nullptr;
    vsResult.shader = nullptr;
    psResult.shader = nullptr;
    shaderManager.ClearCache();
    constantBuffer = nullptr;
    cmdContexts.clear();
    vertexBuffer = nullptr;
    swapChain = nullptr;
    device.reset();

    glfwDestroyWindow(window);
    glfwTerminate();

    RVX::Log::Shutdown();
    return 0;
}
