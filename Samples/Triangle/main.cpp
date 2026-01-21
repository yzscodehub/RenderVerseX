/**
 * @file Triangle Sample
 * @brief Interactive triangle rendering demo using RHI and RenderGraph
 *
 * This sample demonstrates:
 * - Direct RHI device and swapchain creation
 * - RenderGraph usage for automatic barrier management
 * - Camera controls with mouse/keyboard input
 */

#include "Core/Core.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include "Render/Graph/RenderGraph.h"
#include "ShaderCompiler/ShaderManager.h"
#include "ShaderCompiler/ShaderLayout.h"

#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <Windows.h>
#elif __APPLE__
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

#include <fstream>
#include <vector>
#include <string>
#include <cmath>

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
// File Loading Utilities
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
// Main Application
// =============================================================================
int main(int argc, char *argv[])
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("Triangle Sample - RHI + RenderGraph Demo");

    // =========================================================================
    // GLFW Window Setup
    // =========================================================================
    if (!glfwInit())
    {
        RVX_CORE_ERROR("Failed to initialize GLFW");
        return -1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *window = glfwCreateWindow(1280, 720, "Triangle - RenderVerseX", nullptr, nullptr);
    if (!window)
    {
        RVX_CORE_ERROR("Failed to create GLFW window");
        glfwTerminate();
        return -1;
    }

    // Select backend
#if defined(__APPLE__)
    RVX::RHIBackendType backend = RVX::RHIBackendType::Metal;
#elif defined(_WIN32)
    RVX::RHIBackendType backend = RVX::RHIBackendType::DX12;
#else
    RVX::RHIBackendType backend = RVX::RHIBackendType::Vulkan;
#endif

    // Command line override
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
    // RHI Device Creation
    // =========================================================================
    RVX::RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.applicationName = "Triangle Sample";

    auto device = RVX::CreateRHIDevice(backend, deviceDesc);
    if (!device)
    {
        RVX_CORE_ERROR("Failed to create RHI device");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    RVX_CORE_INFO("Adapter: {}", device->GetCapabilities().adapterName);

    // =========================================================================
    // Swap Chain Creation
    // =========================================================================
    RVX::RHISwapChainDesc swapChainDesc;
#ifdef _WIN32
    swapChainDesc.windowHandle = glfwGetWin32Window(window);
#elif __APPLE__
    swapChainDesc.windowHandle = glfwGetCocoaWindow(window);
#endif
    swapChainDesc.width = 1280;
    swapChainDesc.height = 720;
    swapChainDesc.bufferCount = 3;
    swapChainDesc.format = RVX::RHIFormat::RGBA8_UNORM;
    swapChainDesc.vsync = true;

    auto swapChain = device->CreateSwapChain(swapChainDesc);
    if (!swapChain)
    {
        RVX_CORE_ERROR("Failed to create swap chain");
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // =========================================================================
    // Command Contexts (per frame)
    // =========================================================================
    std::vector<RVX::RHICommandContextRef> cmdContexts(RVX::RVX_MAX_FRAME_COUNT);
    for (RVX::uint32 i = 0; i < RVX::RVX_MAX_FRAME_COUNT; ++i)
    {
        cmdContexts[i] = device->CreateCommandContext(RVX::RHICommandQueueType::Graphics);
        if (!cmdContexts[i])
        {
            RVX_CORE_ERROR("Failed to create command context {}", i);
            return -1;
        }
    }

    // =========================================================================
    // Vertex Buffer
    // =========================================================================
    Vertex triangleVertices[] = {
        {{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},  // Top (Red)
        {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}}, // Right (Green)
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}} // Left (Blue)
    };

    RVX::RHIBufferDesc vertexBufferDesc;
    vertexBufferDesc.size = sizeof(triangleVertices);
    vertexBufferDesc.usage = RVX::RHIBufferUsage::Vertex;
    vertexBufferDesc.memoryType = RVX::RHIMemoryType::Upload;
    vertexBufferDesc.stride = sizeof(Vertex);
    vertexBufferDesc.debugName = "TriangleVertexBuffer";

    auto vertexBuffer = device->CreateBuffer(vertexBufferDesc);
    std::memcpy(vertexBuffer->Map(), triangleVertices, sizeof(triangleVertices));
    vertexBuffer->Unmap();

    // =========================================================================
    // Constant Buffer
    // =========================================================================
    RVX::RHIBufferDesc cbDesc;
    cbDesc.size = (sizeof(TransformCB) + 255) & ~255u; // 256-byte aligned
    cbDesc.usage = RVX::RHIBufferUsage::Constant;
    cbDesc.memoryType = RVX::RHIMemoryType::Upload;
    cbDesc.debugName = "TransformCB";

    auto constantBuffer = device->CreateBuffer(cbDesc);

    // =========================================================================
    // Shaders
    // =========================================================================
    std::string exeDir = GetExecutableDir();
    RVX::ShaderManager shaderManager(RVX::CreateShaderCompiler());

    RVX::ShaderLoadDesc vsLoad;
    vsLoad.path = exeDir + "Shaders/Triangle.hlsl";
    vsLoad.entryPoint = "VSMain";
    vsLoad.stage = RVX::RHIShaderStage::Vertex;
    vsLoad.backend = backend;

    auto vsResult = shaderManager.LoadFromFile(device.get(), vsLoad);
    if (!vsResult.compileResult.success)
    {
        RVX_CORE_ERROR("Failed to compile vertex shader: {}", vsResult.compileResult.errorMessage);
        return -1;
    }

    RVX::ShaderLoadDesc psLoad = vsLoad;
    psLoad.entryPoint = "PSMain";
    psLoad.stage = RVX::RHIShaderStage::Pixel;

    auto psResult = shaderManager.LoadFromFile(device.get(), psLoad);
    if (!psResult.compileResult.success)
    {
        RVX_CORE_ERROR("Failed to compile pixel shader: {}", psResult.compileResult.errorMessage);
        return -1;
    }

    RVX_CORE_INFO("Compiled shaders successfully");

    // =========================================================================
    // Pipeline Layout
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
    // Graphics Pipeline
    // =========================================================================
    RVX::RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vsResult.shader.Get();
    pipelineDesc.pixelShader = psResult.shader.Get();
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.debugName = "TrianglePipeline";

    pipelineDesc.inputLayout.AddElement("POSITION", RVX::RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("COLOR", RVX::RHIFormat::RGBA32_FLOAT, 0);

    pipelineDesc.rasterizerState = RVX::RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.cullMode = RVX::RHICullMode::None;

    pipelineDesc.depthStencilState.depthTestEnable = false;
    pipelineDesc.depthStencilState.depthWriteEnable = false;

    pipelineDesc.blendState = RVX::RHIBlendState::Default();

    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = swapChain->GetFormat();
    pipelineDesc.primitiveTopology = RVX::RHIPrimitiveTopology::TriangleList;

    auto pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        RVX_CORE_ERROR("Failed to create graphics pipeline");
        return -1;
    }

    // =========================================================================
    // RenderGraph
    // =========================================================================
    RVX::RenderGraph renderGraph;
    renderGraph.SetDevice(device.get());

    // =========================================================================
    // Main Loop
    // =========================================================================
    RVX_CORE_INFO("Triangle sample initialized - entering main loop");
    RVX_CORE_INFO("Controls:");
    RVX_CORE_INFO("  Arrow Keys: Rotate X/Y");
    RVX_CORE_INFO("  Q/E: Rotate Z");
    RVX_CORE_INFO("  Left Mouse Drag: Rotate X/Y");
    RVX_CORE_INFO("  ESC: Exit");

    RVX::uint32 frameCount = 0;
    double lastFPSTime = glfwGetTime();
    double lastFrameTime = glfwGetTime();

    RVX::Vec3 rotation{0.0f, 0.0f, 0.0f};
    float modelRoll = 0.0f;

    std::vector<RVX::RHIResourceState> backBufferStates(
        swapChain->GetBufferCount(), RVX::RHIResourceState::Undefined);

    double lastMouseX = 0, lastMouseY = 0;
    glfwGetCursorPos(window, &lastMouseX, &lastMouseY);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        double now = glfwGetTime();
        float deltaTime = static_cast<float>(now - lastFrameTime);
        lastFrameTime = now;

        // ESC to exit
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Mouse input
        double mouseX, mouseY;
        glfwGetCursorPos(window, &mouseX, &mouseY);
        float mouseDeltaX = static_cast<float>(mouseX - lastMouseX);
        float mouseDeltaY = static_cast<float>(mouseY - lastMouseY);
        lastMouseX = mouseX;
        lastMouseY = mouseY;

        // Mouse drag rotation
        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
        {
            rotation.y += mouseDeltaX * 0.01f;
            rotation.x += mouseDeltaY * 0.01f;
        }

        // Keyboard rotation
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
            rotation.y -= 2.0f * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
            rotation.y += 2.0f * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
            rotation.x -= 2.0f * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
            rotation.x += 2.0f * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
            modelRoll -= 2.0f * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
            modelRoll += 2.0f * deltaTime;

        // Update constant buffer
        TransformCB transformData;
        transformData.worldMatrix = RVX::transpose(
            RVX::MakeRotationXYZ(RVX::Vec3(rotation.x, rotation.y, modelRoll)));

        float pulse = (std::sin(static_cast<float>(now) * 2.0f) + 1.0f) * 0.2f + 0.8f;
        transformData.tintColor[0] = pulse;
        transformData.tintColor[1] = pulse;
        transformData.tintColor[2] = pulse;
        transformData.tintColor[3] = 1.0f;

        constantBuffer->Upload(&transformData, 1);

        // =====================================================================
        // Render Frame using RenderGraph
        // =====================================================================
        device->BeginFrame();

        RVX::uint32 backBufferIndex = swapChain->GetCurrentBackBufferIndex();
        RVX::uint32 frameIndex = device->GetCurrentFrameIndex();

        auto &ctx = *cmdContexts[frameIndex];
        ctx.Begin();

        // Build RenderGraph
        renderGraph.Clear();

        auto *backBuffer = swapChain->GetCurrentBackBuffer();
        auto *backBufferView = swapChain->GetCurrentBackBufferView();

        auto backBufferHandle = renderGraph.ImportTexture(backBuffer, backBufferStates[backBufferIndex]);
        renderGraph.SetExportState(backBufferHandle, RVX::RHIResourceState::Present);

        struct TrianglePassData
        {
            RVX::RGTextureHandle renderTarget;
        };

        // Capture necessary pointers for the lambda
        auto *pPipeline = pipeline.Get();
        auto *pDescSet = descriptorSet.Get();
        auto *pVB = vertexBuffer.Get();
        auto *pSwapChain = swapChain.Get();
        auto *pBackBufferView = backBufferView;

        renderGraph.AddPass<TrianglePassData>(
            "TrianglePass",
            RVX::RenderGraphPassType::Graphics,
            [&](RVX::RenderGraphBuilder &builder, TrianglePassData &data)
            {
                data.renderTarget = builder.Write(backBufferHandle, RVX::RHIResourceState::RenderTarget);
            },
            [=](const TrianglePassData &, RVX::RHICommandContext &cmdCtx)
            {
                RVX::RHIRenderPassDesc renderPass;
                renderPass.AddColorAttachment(pBackBufferView,
                                              RVX::RHILoadOp::Clear, RVX::RHIStoreOp::Store,
                                              {0.1f, 0.1f, 0.2f, 1.0f});

                cmdCtx.BeginRenderPass(renderPass);

                RVX::RHIViewport viewport = {0, 0,
                                             static_cast<float>(pSwapChain->GetWidth()),
                                             static_cast<float>(pSwapChain->GetHeight()),
                                             0.0f, 1.0f};
                cmdCtx.SetViewport(viewport);

                RVX::RHIRect scissor = {0, 0, pSwapChain->GetWidth(), pSwapChain->GetHeight()};
                cmdCtx.SetScissor(scissor);

                cmdCtx.SetPipeline(pPipeline);
                cmdCtx.SetDescriptorSet(0, pDescSet);
                cmdCtx.SetVertexBuffer(0, pVB);
                cmdCtx.Draw(3, 1, 0, 0);

                cmdCtx.EndRenderPass();
            });

        renderGraph.Compile();
        renderGraph.Execute(ctx);

        backBufferStates[backBufferIndex] = RVX::RHIResourceState::Present;

        ctx.End();
        device->SubmitCommandContext(&ctx, nullptr);
        swapChain->Present();
        device->EndFrame();

        frameCount++;

        // FPS counter
        if (now - lastFPSTime >= 1.0)
        {
            RVX_CORE_DEBUG("FPS: {}", frameCount);
            frameCount = 0;
            lastFPSTime = now;
        }

        // Handle resize
        int newWidth, newHeight;
        glfwGetFramebufferSize(window, &newWidth, &newHeight);
        if (newWidth > 0 && newHeight > 0 &&
            (static_cast<RVX::uint32>(newWidth) != swapChain->GetWidth() ||
             static_cast<RVX::uint32>(newHeight) != swapChain->GetHeight()))
        {
            device->WaitIdle();
            renderGraph.Clear();
            swapChain->Resize(static_cast<RVX::uint32>(newWidth), static_cast<RVX::uint32>(newHeight));
            backBufferStates.assign(swapChain->GetBufferCount(), RVX::RHIResourceState::Undefined);
            RVX_CORE_INFO("Resized to {}x{}", newWidth, newHeight);
        }
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
    cmdContexts.clear();
    vertexBuffer = nullptr;
    swapChain = nullptr;
    device.reset();

    glfwDestroyWindow(window);
    glfwTerminate();

    RVX::Log::Shutdown();
    return 0;
}
