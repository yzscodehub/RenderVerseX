/**
 * @file BasicRHI Sample
 * @brief Basic rendering demo using the low-level RHI contract
 *
 * This sample demonstrates:
 * - Direct RHI device and swapchain creation
 * - Explicit resource barriers at the RHI boundary
 * - Camera controls with mouse/keyboard input
 */

#include "Core/Core.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include "RHI_BackendFactory/RHIBackendFactory.h"
#include "Samples/SampleCLI.h"
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

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

void AppendVertex(std::vector<Vertex>& vertices,
                  float px,
                  float py,
                  float pz,
                  float cr,
                  float cg,
                  float cb)
{
    vertices.push_back({{px, py, pz}, {cr, cg, cb, 1.0f}});
}

void AppendBasicTriangle(std::vector<Vertex>& vertices)
{
    AppendVertex(vertices, -0.72f, 0.45f, 0.0f, 1.0f, 0.15f, 0.1f);
    AppendVertex(vertices, -0.42f, -0.28f, 0.0f, 0.1f, 0.85f, 0.3f);
    AppendVertex(vertices, -1.02f, -0.28f, 0.0f, 0.15f, 0.35f, 1.0f);
}

void AppendBasicQuad(std::vector<Vertex>& vertices)
{
    AppendVertex(vertices, -0.28f, 0.34f, 0.0f, 0.92f, 0.92f, 0.92f);
    AppendVertex(vertices, 0.28f, 0.34f, 0.0f, 0.16f, 0.18f, 0.64f);
    AppendVertex(vertices, 0.28f, -0.34f, 0.0f, 0.92f, 0.92f, 0.92f);

    AppendVertex(vertices, -0.28f, 0.34f, 0.0f, 0.92f, 0.92f, 0.92f);
    AppendVertex(vertices, 0.28f, -0.34f, 0.0f, 0.92f, 0.92f, 0.92f);
    AppendVertex(vertices, -0.28f, -0.34f, 0.0f, 0.16f, 0.18f, 0.64f);
}

void AppendCubeFace(std::vector<Vertex>& vertices,
                    const Vertex& a,
                    const Vertex& b,
                    const Vertex& c,
                    const Vertex& d)
{
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
    vertices.push_back(a);
    vertices.push_back(c);
    vertices.push_back(d);
}

void AppendBasicCube(std::vector<Vertex>& vertices)
{
    const float cx = 0.72f;
    const float s = 0.27f;
    const Vertex p000 = {{cx - s, -s, -s}, {0.90f, 0.20f, 0.20f, 1.0f}};
    const Vertex p001 = {{cx - s, -s, s}, {0.20f, 0.80f, 0.35f, 1.0f}};
    const Vertex p010 = {{cx - s, s, -s}, {0.20f, 0.35f, 0.90f, 1.0f}};
    const Vertex p011 = {{cx - s, s, s}, {0.90f, 0.80f, 0.25f, 1.0f}};
    const Vertex p100 = {{cx + s, -s, -s}, {0.90f, 0.35f, 0.85f, 1.0f}};
    const Vertex p101 = {{cx + s, -s, s}, {0.25f, 0.85f, 0.90f, 1.0f}};
    const Vertex p110 = {{cx + s, s, -s}, {0.75f, 0.55f, 0.25f, 1.0f}};
    const Vertex p111 = {{cx + s, s, s}, {0.85f, 0.85f, 0.85f, 1.0f}};

    AppendCubeFace(vertices, p100, p110, p111, p101);
    AppendCubeFace(vertices, p000, p001, p011, p010);
    AppendCubeFace(vertices, p010, p011, p111, p110);
    AppendCubeFace(vertices, p000, p100, p101, p001);
    AppendCubeFace(vertices, p001, p101, p111, p011);
    AppendCubeFace(vertices, p000, p010, p110, p100);
}

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
    RVX_CORE_INFO("BasicRHI Sample - explicit RHI demo");

    RVX::SampleCLIOptions options;
    options.backend = RVX::SelectBestBackend();

    std::string parseError;
    if (!RVX::ParseSampleCLI(argc, argv, options, &parseError))
    {
        if (!parseError.empty())
        {
            RVX_CORE_ERROR("{}", parseError);
        }
        RVX::PrintSampleCLIUsage(std::cerr, "BasicRHI");
        RVX::Log::Shutdown();
        return 2;
    }

    if (options.showHelp)
    {
        RVX::PrintSampleCLIUsage(std::cout, "BasicRHI");
        RVX::Log::Shutdown();
        return 0;
    }

    RVX::RHIBackendType backend =
        options.backend == RVX::RHIBackendType::Auto ? RVX::SelectBestBackend() : options.backend;
    RVX::RHIBackendType reportBackend = backend;
    const RVX::uint32 frameLimit = options.frames;

    const auto writeReport =
        [&](bool pass, RVX::uint32 frameCount, std::vector<std::string> fallbackReasons = {})
    {
        if (options.reportPath.empty())
        {
            return;
        }

        RVX::SampleReport report;
        report.sampleName = "BasicRHI";
        report.backend = reportBackend;
        report.frameCount = frameCount;
        report.width = options.width;
        report.height = options.height;
        report.quality = options.quality;
        report.diagnostics = options.diagnostics;
        report.enabledFeatures = {
            "RHI",
            "ExplicitResourceBarriers",
            "BasicTriangle",
            "BasicQuad",
            "BasicCube",
        };
        if (options.diagnostics)
        {
            report.enabledFeatures.push_back("DiagnosticsReport");
        }
        if (!options.screenshotPath.empty())
        {
            report.screenshotPath = options.screenshotPath;
            report.unsupportedFeatures.push_back("ScreenshotCapture");
            fallbackReasons.push_back("BasicRHI does not capture screenshots yet");
        }
        if (options.quality != "default")
        {
            report.unsupportedFeatures.push_back("QualityProfile");
            fallbackReasons.push_back("BasicRHI uses a fixed quality profile");
        }
        report.fallbackReasons = fallbackReasons;
        report.pass = pass;

        std::string reportError;
        if (!RVX::WriteSampleReportJson(report, options.reportPath, &reportError))
        {
            RVX_CORE_ERROR("Failed to write BasicRHI report: {}", reportError);
        }
    };

    // =========================================================================
    // GLFW Window Setup
    // =========================================================================
    if (!glfwInit())
    {
        RVX_CORE_ERROR("Failed to initialize GLFW");
        writeReport(false, 0, {"Failed to initialize GLFW"});
        RVX::Log::Shutdown();
        return -1;
    }

    glfwWindowHint(GLFW_CLIENT_API,
                   backend == RVX::RHIBackendType::OpenGL
                       ? GLFW_OPENGL_API
                       : GLFW_NO_API);
    GLFWwindow *window = glfwCreateWindow(
        static_cast<int>(options.width),
        static_cast<int>(options.height),
        "BasicRHI - RenderVerseX",
        nullptr,
        nullptr);
    if (!window)
    {
        RVX_CORE_ERROR("Failed to create GLFW window");
        writeReport(false, 0, {"Failed to create GLFW window"});
        glfwTerminate();
        RVX::Log::Shutdown();
        return -1;
    }

    RVX_CORE_INFO("Using backend: {}", RVX::ToString(backend));

    RVX::NativeSurfaceDesc surface;
#ifdef _WIN32
    surface.platform = RVX::NativeSurfacePlatform::Win32;
    surface.nativeWindow =
        reinterpret_cast<uintptr_t>(glfwGetWin32Window(window));
#elif defined(__APPLE__)
    surface.platform = RVX::NativeSurfacePlatform::Cocoa;
    surface.nativeWindow =
        reinterpret_cast<uintptr_t>(glfwGetCocoaWindow(window));
#else
    surface.platform = RVX::NativeSurfacePlatform::GLFW;
#endif
    surface.backendWindow = reinterpret_cast<uintptr_t>(window);
    surface.width = options.width;
    surface.height = options.height;
    surface.preferredFormat = RVX::RHIFormat::RGBA8_UNORM;
    surface.vsync = true;
    surface.generation = 1;

    if (!surface.IsValidFor(backend))
    {
        RVX_CORE_ERROR("BasicRHI has no HAL-owned native surface for {}",
                       RVX::ToString(backend));
        writeReport(false, 0, {"HAL-owned native surface unavailable"});
        glfwDestroyWindow(window);
        glfwTerminate();
        RVX::Log::Shutdown();
        return -1;
    }

    // =========================================================================
    // RHI Device Creation
    // =========================================================================
    RVX::RHIDeviceDesc deviceDesc;
    deviceDesc.initialSurface = surface;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.applicationName = "BasicRHI Sample";

    auto device = RVX::CreateRHIDevice(backend, deviceDesc);
    if (!device)
    {
        RVX_CORE_ERROR("Failed to create RHI device");
        writeReport(false, 0, {"Failed to create RHI device"});
        glfwDestroyWindow(window);
        glfwTerminate();
        RVX::Log::Shutdown();
        return -1;
    }
    reportBackend = device->GetBackendType();

    RVX_CORE_INFO("Adapter: {}", device->GetCapabilities().adapterName);

    // =========================================================================
    // Swap Chain Creation
    // =========================================================================
    RVX::RHISwapChainDesc swapChainDesc;
    swapChainDesc.surface = surface;
    swapChainDesc.bufferCount = 3;

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
            writeReport(false, 0, {"Failed to create command context"});
            return -1;
        }
    }

    // =========================================================================
    // Vertex Buffer
    // =========================================================================
    std::vector<Vertex> basicVertices;
    basicVertices.reserve(45);
    AppendBasicTriangle(basicVertices);
    AppendBasicQuad(basicVertices);
    AppendBasicCube(basicVertices);
    const RVX::uint32 drawVertexCount = static_cast<RVX::uint32>(basicVertices.size());

    RVX::RHIBufferDesc vertexBufferDesc;
    vertexBufferDesc.size = sizeof(Vertex) * basicVertices.size();
    vertexBufferDesc.usage = RVX::RHIBufferUsage::Vertex;
    vertexBufferDesc.memoryType = RVX::RHIMemoryType::Upload;
    vertexBufferDesc.stride = sizeof(Vertex);
    vertexBufferDesc.debugName = "BasicRHIGeometryVertexBuffer";

    auto vertexBuffer = device->CreateBuffer(vertexBufferDesc);
    std::memcpy(vertexBuffer->Map(), basicVertices.data(), vertexBufferDesc.size);
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
        writeReport(false, 0, {"Failed to compile vertex shader"});
        return -1;
    }

    RVX::ShaderLoadDesc psLoad = vsLoad;
    psLoad.entryPoint = "PSMain";
    psLoad.stage = RVX::RHIShaderStage::Pixel;

    auto psResult = shaderManager.LoadFromFile(device.get(), psLoad);
    if (!psResult.compileResult.success)
    {
        RVX_CORE_ERROR("Failed to compile pixel shader: {}", psResult.compileResult.errorMessage);
        writeReport(false, 0, {"Failed to compile pixel shader"});
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
    pipelineDesc.debugName = "BasicRHITrianglePipeline";

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
        writeReport(false, 0, {"Failed to create graphics pipeline"});
        return -1;
    }

    // =========================================================================
    // Main Loop
    // =========================================================================
    RVX_CORE_INFO("BasicRHI initialized - entering main loop");
    RVX_CORE_INFO("Cases consolidated here: triangle, quad, and cube fundamentals");
    RVX_CORE_INFO("Controls:");
    RVX_CORE_INFO("  Arrow Keys: Rotate X/Y");
    RVX_CORE_INFO("  Q/E: Rotate Z");
    RVX_CORE_INFO("  Left Mouse Drag: Rotate X/Y");
    RVX_CORE_INFO("  ESC: Exit");

    RVX::uint32 frameCount = 0;
    RVX::uint32 renderedFrames = 0;
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
        // Render Frame using the explicit low-level RHI contract
        // =====================================================================
        device->BeginFrame();

        RVX::uint32 backBufferIndex = swapChain->GetCurrentBackBufferIndex();
        RVX::uint32 frameIndex = device->GetCurrentFrameIndex();

        auto &ctx = *cmdContexts[frameIndex];
        ctx.Begin();

        auto *backBuffer = swapChain->GetCurrentBackBuffer();
        auto *backBufferView = swapChain->GetCurrentBackBufferView();

        ctx.TextureBarrier(
            backBuffer,
            backBufferStates[backBufferIndex],
            RVX::RHIResourceState::RenderTarget);

        RVX::RHIRenderPassDesc renderPass;
        renderPass.AddColorAttachment(
            backBufferView,
            RVX::RHILoadOp::Clear,
            RVX::RHIStoreOp::Store,
            {0.1f, 0.1f, 0.2f, 1.0f});
        ctx.BeginRenderPass(renderPass);

        RVX::RHIViewport viewport = {
            0,
            0,
            static_cast<float>(swapChain->GetWidth()),
            static_cast<float>(swapChain->GetHeight()),
            0.0f,
            1.0f};
        ctx.SetViewport(viewport);

        RVX::RHIRect scissor = {
            0,
            0,
            swapChain->GetWidth(),
            swapChain->GetHeight()};
        ctx.SetScissor(scissor);

        ctx.SetPipeline(pipeline.Get());
        ctx.SetDescriptorSet(0, descriptorSet.Get());
        ctx.SetVertexBuffer(0, vertexBuffer.Get());
        ctx.Draw(drawVertexCount, 1, 0, 0);

        ctx.EndRenderPass();
        ctx.TextureBarrier(
            backBuffer,
            RVX::RHIResourceState::RenderTarget,
            RVX::RHIResourceState::Present);

        backBufferStates[backBufferIndex] = RVX::RHIResourceState::Present;

        ctx.End();
        device->SubmitCommandContext(&ctx, nullptr);
        swapChain->Present();
        device->EndFrame();

        frameCount++;
        renderedFrames++;

        if (frameLimit > 0 && renderedFrames >= frameLimit)
        {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

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

    writeReport(true, renderedFrames);

    RVX::Log::Shutdown();
    return 0;
}
