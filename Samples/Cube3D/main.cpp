#include "Core/Core.h"
#include "RHI/RHI.h"
#include "Core/MathTypes.h"

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

#include "ShaderCompiler/ShaderManager.h"
#include "ShaderCompiler/ShaderLayout.h"

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
void GenerateCubeVertices(std::vector<Vertex>& vertices, std::vector<RVX::uint16>& indices)
{
    // Helper to add a vertex
    auto addVertex = [&](float px, float py, float pz, float nx, float ny, float nz, 
                         float cr, float cg, float cb, float ca) {
        Vertex v;
        v.position[0] = px; v.position[1] = py; v.position[2] = pz;
        v.normal[0] = nx; v.normal[1] = ny; v.normal[2] = nz;
        v.color[0] = cr; v.color[1] = cg; v.color[2] = cb; v.color[3] = ca;
        vertices.push_back(v);
    };

    // Right face (+X) - Red
    addVertex( 0.5f, -0.5f, -0.5f,  1, 0, 0,  1.0f, 0.3f, 0.3f, 1.0f);
    addVertex( 0.5f,  0.5f, -0.5f,  1, 0, 0,  1.0f, 0.3f, 0.3f, 1.0f);
    addVertex( 0.5f,  0.5f,  0.5f,  1, 0, 0,  1.0f, 0.3f, 0.3f, 1.0f);
    addVertex( 0.5f, -0.5f,  0.5f,  1, 0, 0,  1.0f, 0.3f, 0.3f, 1.0f);

    // Left face (-X) - Green
    addVertex(-0.5f, -0.5f,  0.5f, -1, 0, 0,  0.3f, 1.0f, 0.3f, 1.0f);
    addVertex(-0.5f,  0.5f,  0.5f, -1, 0, 0,  0.3f, 1.0f, 0.3f, 1.0f);
    addVertex(-0.5f,  0.5f, -0.5f, -1, 0, 0,  0.3f, 1.0f, 0.3f, 1.0f);
    addVertex(-0.5f, -0.5f, -0.5f, -1, 0, 0,  0.3f, 1.0f, 0.3f, 1.0f);

    // Top face (+Y) - Blue
    addVertex(-0.5f,  0.5f, -0.5f,  0, 1, 0,  0.3f, 0.3f, 1.0f, 1.0f);
    addVertex(-0.5f,  0.5f,  0.5f,  0, 1, 0,  0.3f, 0.3f, 1.0f, 1.0f);
    addVertex( 0.5f,  0.5f,  0.5f,  0, 1, 0,  0.3f, 0.3f, 1.0f, 1.0f);
    addVertex( 0.5f,  0.5f, -0.5f,  0, 1, 0,  0.3f, 0.3f, 1.0f, 1.0f);

    // Bottom face (-Y) - Yellow
    addVertex(-0.5f, -0.5f,  0.5f,  0,-1, 0,  1.0f, 1.0f, 0.3f, 1.0f);
    addVertex(-0.5f, -0.5f, -0.5f,  0,-1, 0,  1.0f, 1.0f, 0.3f, 1.0f);
    addVertex( 0.5f, -0.5f, -0.5f,  0,-1, 0,  1.0f, 1.0f, 0.3f, 1.0f);
    addVertex( 0.5f, -0.5f,  0.5f,  0,-1, 0,  1.0f, 1.0f, 0.3f, 1.0f);

    // Front face (+Z) - Magenta
    addVertex(-0.5f, -0.5f,  0.5f,  0, 0, 1,  1.0f, 0.3f, 1.0f, 1.0f);
    addVertex( 0.5f, -0.5f,  0.5f,  0, 0, 1,  1.0f, 0.3f, 1.0f, 1.0f);
    addVertex( 0.5f,  0.5f,  0.5f,  0, 0, 1,  1.0f, 0.3f, 1.0f, 1.0f);
    addVertex(-0.5f,  0.5f,  0.5f,  0, 0, 1,  1.0f, 0.3f, 1.0f, 1.0f);

    // Back face (-Z) - Cyan
    addVertex( 0.5f, -0.5f, -0.5f,  0, 0,-1,  0.3f, 1.0f, 1.0f, 1.0f);
    addVertex(-0.5f, -0.5f, -0.5f,  0, 0,-1,  0.3f, 1.0f, 1.0f, 1.0f);
    addVertex(-0.5f,  0.5f, -0.5f,  0, 0,-1,  0.3f, 1.0f, 1.0f, 1.0f);
    addVertex( 0.5f,  0.5f, -0.5f,  0, 0,-1,  0.3f, 1.0f, 1.0f, 1.0f);

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
int main(int argc, char* argv[])
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("RenderVerseX 3D Cube Sample");
    RVX_CORE_INFO("Demonstrates depth buffer, 3D transforms, and basic lighting");

    if (!glfwInit())
    {
        RVX_CORE_CRITICAL("Failed to initialize GLFW");
        return -1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "RenderVerseX - 3D Cube", nullptr, nullptr);
    if (!window)
    {
        RVX_CORE_CRITICAL("Failed to create window");
        glfwTerminate();
        return -1;
    }

    // Select backend (platform-specific default)
#if defined(__APPLE__)
    RVX::RHIBackendType backend = RVX::RHIBackendType::Metal;
#elif defined(_WIN32)
    RVX::RHIBackendType backend = RVX::RHIBackendType::DX12;
#else
    RVX::RHIBackendType backend = RVX::RHIBackendType::Vulkan;
#endif

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

    // Create RHI device
    RVX::RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.applicationName = "Cube3D Sample";

    auto device = RVX::CreateRHIDevice(backend, deviceDesc);
    if (!device)
    {
        RVX_CORE_CRITICAL("Failed to create RHI device");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    RVX_CORE_INFO("Adapter: {}", device->GetCapabilities().adapterName);

    // Create swap chain
    RVX::RHISwapChainDesc swapChainDesc;
#ifdef _WIN32
    swapChainDesc.windowHandle = glfwGetWin32Window(window);
#elif __APPLE__
    swapChainDesc.windowHandle = glfwGetCocoaWindow(window);
#endif
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

    // Create depth buffer
    RVX::RHITextureDesc depthDesc = RVX::RHITextureDesc::DepthStencil(
        swapChain->GetWidth(), swapChain->GetHeight(), RVX::RHIFormat::D24_UNORM_S8_UINT);
    depthDesc.debugName = "DepthBuffer";
    auto depthBuffer = device->CreateTexture(depthDesc);
    
    RVX::RHITextureViewDesc depthViewDesc;
    depthViewDesc.format = RVX::RHIFormat::D24_UNORM_S8_UINT;
    auto depthView = device->CreateTextureView(depthBuffer.Get(), depthViewDesc);
    RVX_CORE_INFO("Created depth buffer");

    // Create command context
    auto cmdContext = device->CreateCommandContext(RVX::RHICommandQueueType::Graphics);

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

    RVX::ShaderLoadDesc vsLoad;
    vsLoad.path = exeDir + "Shaders/Cube3D.hlsl";
    vsLoad.entryPoint = "VSMain";
    vsLoad.stage = RVX::RHIShaderStage::Vertex;
    vsLoad.backend = backend;

    auto vsResult = shaderManager.LoadFromFile(device.get(), vsLoad);
    if (!vsResult.compileResult.success)
    {
        RVX_CORE_CRITICAL("Failed to compile vertex shader: {}", vsResult.compileResult.errorMessage);
        return -1;
    }

    RVX::ShaderLoadDesc psLoad = vsLoad;
    psLoad.entryPoint = "PSMain";
    psLoad.stage = RVX::RHIShaderStage::Pixel;

    auto psResult = shaderManager.LoadFromFile(device.get(), psLoad);
    if (!psResult.compileResult.success)
    {
        RVX_CORE_CRITICAL("Failed to compile pixel shader: {}", psResult.compileResult.errorMessage);
        return -1;
    }

    RVX_CORE_INFO("Compiled shaders successfully");

    // =========================================================================
    // Create Pipeline Layout
    // =========================================================================
    std::vector<RVX::ReflectedShader> reflectedShaders = {
        {vsResult.compileResult.reflection, RVX::RHIShaderStage::Vertex},
        {psResult.compileResult.reflection, RVX::RHIShaderStage::Pixel}
    };

    auto autoLayout = RVX::BuildAutoPipelineLayout(reflectedShaders);

    std::vector<RVX::RHIDescriptorSetLayoutRef> setLayouts(autoLayout.setLayouts.size());
    for (size_t i = 0; i < autoLayout.setLayouts.size(); ++i)
    {
        if (autoLayout.setLayouts[i].entries.empty())
            continue;
        setLayouts[i] = device->CreateDescriptorSetLayout(autoLayout.setLayouts[i]);
    }

    RVX::RHIPipelineLayoutDesc pipelineLayoutDesc = autoLayout.pipelineLayout;
    for (const auto& layout : setLayouts)
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
    pipelineDesc.depthStencilState = RVX::RHIDepthStencilState::Default();  // Enable depth test
    pipelineDesc.blendState = RVX::RHIBlendState::Default();

    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = swapChain->GetFormat();
    pipelineDesc.depthStencilFormat = RVX::RHIFormat::D24_UNORM_S8_UINT;
    pipelineDesc.primitiveTopology = RVX::RHIPrimitiveTopology::TriangleList;

    auto pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    RVX_CORE_INFO("Created graphics pipeline with depth testing");

    // =========================================================================
    // Main Loop
    // =========================================================================
    RVX_CORE_INFO("3D Cube sample initialized - press ESC to exit");
    RVX_CORE_INFO("The cube will rotate automatically");

    RVX::RHIResourceState backBufferState = RVX::RHIResourceState::Undefined;
    RVX::RHIResourceState depthState = RVX::RHIResourceState::Undefined;
    double startTime = glfwGetTime();

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        float time = static_cast<float>(glfwGetTime() - startTime);

        // Update transform
        float aspect = static_cast<float>(swapChain->GetWidth()) / 
                       static_cast<float>(swapChain->GetHeight());
        
        RVX::Mat4 world = RVX::MakeRotationXYZ(RVX::Vec3(time * 0.5f, time * 0.7f, time * 0.3f));
        
        // Create view matrix (camera at z=-3 looking at origin)
        RVX::Vec3 eye(0.0f, 0.0f, -3.0f);
        RVX::Vec3 at(0.0f, 0.0f, 0.0f);
        RVX::Vec3 up(0.0f, 1.0f, 0.0f);
        RVX::Mat4 view = RVX::lookAt(eye, at, up);
        
        RVX::Mat4 proj = RVX::MakePerspective(60.0f * 3.14159f / 180.0f, aspect, 0.1f, 100.0f);
        
        TransformCB cbData;
        cbData.worldViewProj = world * view * proj;
        cbData.world = world;
        // Light direction (normalized)
        cbData.lightDir[0] = 0.577f;
        cbData.lightDir[1] = 0.577f;
        cbData.lightDir[2] = -0.577f;
        cbData.lightDir[3] = 0.0f;
        
        constantBuffer->Upload(&cbData, 1);

        // Begin frame - resets fence and prepares for new frame
        device->BeginFrame();
        
        auto* backBuffer = swapChain->GetCurrentBackBuffer();
        auto* backBufferView = swapChain->GetCurrentBackBufferView();

        cmdContext->Begin();

        // Transition resources
        cmdContext->TextureBarrier({backBuffer, backBufferState, RVX::RHIResourceState::RenderTarget});
        // Only transition depth buffer if not already in DepthWrite state
        if (depthState != RVX::RHIResourceState::DepthWrite)
        {
            cmdContext->TextureBarrier({depthBuffer.Get(), depthState, RVX::RHIResourceState::DepthWrite});
            depthState = RVX::RHIResourceState::DepthWrite;
        }

        // Begin render pass with depth
        RVX::RHIRenderPassDesc renderPass;
        renderPass.AddColorAttachment(backBufferView, RVX::RHILoadOp::Clear, RVX::RHIStoreOp::Store,
                                      {0.1f, 0.1f, 0.15f, 1.0f});
        renderPass.SetDepthStencil(depthView.Get(), 
                                   RVX::RHILoadOp::Clear, RVX::RHIStoreOp::Store,
                                   1.0f, 0);

        cmdContext->BeginRenderPass(renderPass);

        RVX::RHIViewport viewport = {0, 0, 
            static_cast<float>(swapChain->GetWidth()), 
            static_cast<float>(swapChain->GetHeight()), 
            0.0f, 1.0f};
        cmdContext->SetViewport(viewport);

        RVX::RHIRect scissor = {0, 0, swapChain->GetWidth(), swapChain->GetHeight()};
        cmdContext->SetScissor(scissor);

        cmdContext->SetPipeline(pipeline.Get());
        cmdContext->SetDescriptorSet(0, descriptorSet.Get());
        cmdContext->SetVertexBuffer(0, vertexBuffer.Get());
        cmdContext->SetIndexBuffer(indexBuffer.Get(), RVX::RHIFormat::R16_UINT);
        cmdContext->DrawIndexed(static_cast<RVX::uint32>(indices.size()), 1, 0, 0, 0);

        cmdContext->EndRenderPass();

        // Transition to present
        cmdContext->TextureBarrier({backBuffer, RVX::RHIResourceState::RenderTarget, RVX::RHIResourceState::Present});
        backBufferState = RVX::RHIResourceState::Present;
        // depthState stays as DepthWrite (no transition needed)

        cmdContext->End();

        device->SubmitCommandContext(cmdContext.Get(), nullptr);
        swapChain->Present();
        device->EndFrame();  // Advance to next frame index
    }

    // Cleanup
    device->WaitIdle();

    descriptorSet = nullptr;
    pipeline = nullptr;
    pipelineLayout = nullptr;
    setLayouts.clear();
    shaderManager.ClearCache();
    constantBuffer = nullptr;
    indexBuffer = nullptr;
    vertexBuffer = nullptr;
    depthView = nullptr;
    depthBuffer = nullptr;
    cmdContext = nullptr;
    swapChain = nullptr;
    device.reset();

    glfwDestroyWindow(window);
    glfwTerminate();

    RVX::Log::Shutdown();
    return 0;
}
