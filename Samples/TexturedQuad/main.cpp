#include "Core/Core.h"
#include "RHI/RHI.h"

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

// =============================================================================
// Vertex Structure
// =============================================================================
struct Vertex
{
    float position[3];
    float texCoord[2];
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

std::string LoadTextFile(const std::string& path)
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

// Generate a simple checkerboard texture
std::vector<uint8_t> GenerateCheckerboardTexture(uint32_t width, uint32_t height, uint32_t tileSize)
{
    std::vector<uint8_t> data(width * height * 4);
    
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            uint32_t tileX = x / tileSize;
            uint32_t tileY = y / tileSize;
            bool isWhite = ((tileX + tileY) % 2) == 0;
            
            uint32_t idx = (y * width + x) * 4;
            if (isWhite)
            {
                data[idx + 0] = 240;  // R
                data[idx + 1] = 240;  // G
                data[idx + 2] = 240;  // B
            }
            else
            {
                data[idx + 0] = 60;   // R
                data[idx + 1] = 60;   // G
                data[idx + 2] = 180;  // B
            }
            data[idx + 3] = 255;  // A
        }
    }
    
    return data;
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[])
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("RenderVerseX Textured Quad Sample");

    if (!glfwInit())
    {
        RVX_CORE_CRITICAL("Failed to initialize GLFW");
        return -1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "RenderVerseX - Textured Quad", nullptr, nullptr);
    if (!window)
    {
        RVX_CORE_CRITICAL("Failed to create window");
        glfwTerminate();
        return -1;
    }

    // Select backend
    RVX::RHIBackendType backend = RVX::RHIBackendType::DX12;
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
    deviceDesc.applicationName = "Textured Quad Sample";

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

    // Create command context
    auto cmdContext = device->CreateCommandContext(RVX::RHICommandQueueType::Graphics);

    // =========================================================================
    // Create Quad Vertex Buffer
    // =========================================================================
    Vertex quadVertices[] =
    {
        // Position              TexCoord
        {{-0.6f,  0.6f, 0.0f}, {0.0f, 0.0f}},  // Top-left
        {{ 0.6f,  0.6f, 0.0f}, {1.0f, 0.0f}},  // Top-right
        {{ 0.6f, -0.6f, 0.0f}, {1.0f, 1.0f}},  // Bottom-right
        {{-0.6f, -0.6f, 0.0f}, {0.0f, 1.0f}},  // Bottom-left
    };

    RVX::RHIBufferDesc vbDesc;
    vbDesc.size = sizeof(quadVertices);
    vbDesc.usage = RVX::RHIBufferUsage::Vertex;
    vbDesc.memoryType = RVX::RHIMemoryType::Upload;
    vbDesc.stride = sizeof(Vertex);
    vbDesc.debugName = "Quad VB";

    auto vertexBuffer = device->CreateBuffer(vbDesc);
    vertexBuffer->Upload(quadVertices, 4);
    RVX_CORE_INFO("Created vertex buffer");

    // =========================================================================
    // Create Index Buffer
    // =========================================================================
    RVX::uint16 indices[] = { 0, 1, 2, 0, 2, 3 };

    RVX::RHIBufferDesc ibDesc;
    ibDesc.size = sizeof(indices);
    ibDesc.usage = RVX::RHIBufferUsage::Index;
    ibDesc.memoryType = RVX::RHIMemoryType::Upload;
    ibDesc.debugName = "Quad IB";

    auto indexBuffer = device->CreateBuffer(ibDesc);
    indexBuffer->Upload(indices, 6);
    RVX_CORE_INFO("Created index buffer");

    // =========================================================================
    // Create Texture
    // =========================================================================
    const RVX::uint32 texWidth = 256;
    const RVX::uint32 texHeight = 256;
    auto textureData = GenerateCheckerboardTexture(texWidth, texHeight, 32);

    RVX::RHITextureDesc texDesc = RVX::RHITextureDesc::Texture2D(
        texWidth, texHeight, RVX::RHIFormat::RGBA8_UNORM,
        RVX::RHITextureUsage::ShaderResource);
    texDesc.debugName = "Checkerboard";

    auto texture = device->CreateTexture(texDesc);
    RVX_CORE_INFO("Created texture");

    // Create staging buffer for texture upload
    RVX::RHIBufferDesc stagingDesc;
    stagingDesc.size = textureData.size();
    stagingDesc.usage = RVX::RHIBufferUsage::None;
    stagingDesc.memoryType = RVX::RHIMemoryType::Upload;
    stagingDesc.debugName = "Texture Staging";

    auto stagingBuffer = device->CreateBuffer(stagingDesc);
    std::memcpy(stagingBuffer->Map(), textureData.data(), textureData.size());
    stagingBuffer->Unmap();

    // Upload texture data
    cmdContext->Begin();
    cmdContext->TextureBarrier({texture.Get(), RVX::RHIResourceState::Undefined, RVX::RHIResourceState::CopyDest});
    
    RVX::RHIBufferTextureCopyDesc copyDesc;
    copyDesc.bufferOffset = 0;
    copyDesc.bufferRowPitch = texWidth * 4;
    copyDesc.textureSubresource = 0;
    copyDesc.textureRegion = {0, 0, texWidth, texHeight};
    cmdContext->CopyBufferToTexture(stagingBuffer.Get(), texture.Get(), copyDesc);
    
    cmdContext->TextureBarrier({texture.Get(), RVX::RHIResourceState::CopyDest, RVX::RHIResourceState::ShaderResource});
    cmdContext->End();
    device->SubmitCommandContext(cmdContext.Get(), nullptr);
    device->WaitIdle();
    RVX_CORE_INFO("Uploaded texture data");

    // Create texture view
    RVX::RHITextureViewDesc viewDesc;
    viewDesc.format = RVX::RHIFormat::RGBA8_UNORM;
    auto textureView = device->CreateTextureView(texture.Get(), viewDesc);

    // Create sampler
    RVX::RHISamplerDesc samplerDesc;
    samplerDesc.magFilter = RVX::RHIFilterMode::Linear;
    samplerDesc.minFilter = RVX::RHIFilterMode::Linear;
    samplerDesc.mipFilter = RVX::RHIFilterMode::Linear;
    samplerDesc.addressU = RVX::RHIAddressMode::Repeat;
    samplerDesc.addressV = RVX::RHIAddressMode::Repeat;
    samplerDesc.addressW = RVX::RHIAddressMode::Repeat;
    samplerDesc.debugName = "LinearSampler";

    auto sampler = device->CreateSampler(samplerDesc);
    RVX_CORE_INFO("Created sampler");

    // =========================================================================
    // Load and Compile Shaders
    // =========================================================================
    std::string exeDir = GetExecutableDir();
    std::string shaderPath = exeDir + "Shaders/TexturedQuad.hlsl";
    RVX_CORE_INFO("Loading shader from: {}", shaderPath);

    RVX::ShaderManager shaderManager(RVX::CreateShaderCompiler());

    RVX::ShaderLoadDesc vsLoad;
    vsLoad.path = shaderPath;
    vsLoad.entryPoint = "VSMain";
    vsLoad.stage = RVX::RHIShaderStage::Vertex;
    vsLoad.backend = backend;

    auto vsResult = shaderManager.LoadFromFile(device.get(), vsLoad);
    if (!vsResult.compileResult.success)
    {
        RVX_CORE_CRITICAL("Failed to compile vertex shader: {}", vsResult.compileResult.errorMessage);
        return -1;
    }
    auto vertexShader = vsResult.shader;

    RVX::ShaderLoadDesc psLoad = vsLoad;
    psLoad.entryPoint = "PSMain";
    psLoad.stage = RVX::RHIShaderStage::Pixel;

    auto psResult = shaderManager.LoadFromFile(device.get(), psLoad);
    if (!psResult.compileResult.success)
    {
        RVX_CORE_CRITICAL("Failed to compile pixel shader: {}", psResult.compileResult.errorMessage);
        return -1;
    }
    auto pixelShader = psResult.shader;

    RVX_CORE_INFO("Compiled shaders successfully");

    // =========================================================================
    // Create Pipeline Layout with Auto Layout
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
    descSetDesc.BindTexture(0, textureView.Get());
    descSetDesc.BindSampler(0, sampler.Get());
    auto descriptorSet = device->CreateDescriptorSet(descSetDesc);

    // =========================================================================
    // Create Graphics Pipeline
    // =========================================================================
    RVX::RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.vertexShader = vertexShader.Get();
    pipelineDesc.pixelShader = pixelShader.Get();
    pipelineDesc.pipelineLayout = pipelineLayout.Get();
    pipelineDesc.debugName = "TexturedQuadPipeline";

    pipelineDesc.inputLayout.AddElement("POSITION", RVX::RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("TEXCOORD", RVX::RHIFormat::RG32_FLOAT, 0);

    pipelineDesc.rasterizerState = RVX::RHIRasterizerState::NoCull();
    pipelineDesc.depthStencilState = RVX::RHIDepthStencilState::Disabled();
    pipelineDesc.blendState = RVX::RHIBlendState::Default();

    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = swapChain->GetFormat();
    pipelineDesc.depthStencilFormat = RVX::RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RVX::RHIPrimitiveTopology::TriangleList;

    auto pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    RVX_CORE_INFO("Created graphics pipeline");

    // =========================================================================
    // Main Loop
    // =========================================================================
    RVX_CORE_INFO("Textured Quad sample initialized - press ESC to exit");

    RVX::RHIResourceState backBufferState = RVX::RHIResourceState::Undefined;

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Begin frame
        auto* backBuffer = swapChain->GetCurrentBackBuffer();
        auto* backBufferView = swapChain->GetCurrentBackBufferView();

        cmdContext->Begin();

        // Transition to render target
        cmdContext->TextureBarrier({backBuffer, backBufferState, RVX::RHIResourceState::RenderTarget});

        // Begin render pass
        RVX::RHIRenderPassDesc renderPass;
        renderPass.AddColorAttachment(backBufferView, RVX::RHILoadOp::Clear, RVX::RHIStoreOp::Store,
                                      {0.1f, 0.1f, 0.15f, 1.0f});

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
        cmdContext->DrawIndexed(6, 1, 0, 0, 0);

        cmdContext->EndRenderPass();

        // Transition to present
        cmdContext->TextureBarrier({backBuffer, RVX::RHIResourceState::RenderTarget, RVX::RHIResourceState::Present});
        backBufferState = RVX::RHIResourceState::Present;

        cmdContext->End();

        device->SubmitCommandContext(cmdContext.Get(), nullptr);
        swapChain->Present();
        device->WaitIdle();
    }

    // Cleanup
    device->WaitIdle();

    descriptorSet = nullptr;
    pipeline = nullptr;
    pipelineLayout = nullptr;
    setLayouts.clear();
    vertexShader = nullptr;
    pixelShader = nullptr;
    shaderManager.ClearCache();
    sampler = nullptr;
    textureView = nullptr;
    texture = nullptr;
    stagingBuffer = nullptr;
    indexBuffer = nullptr;
    vertexBuffer = nullptr;
    cmdContext = nullptr;
    swapChain = nullptr;
    device.reset();

    glfwDestroyWindow(window);
    glfwTerminate();

    RVX::Log::Shutdown();
    return 0;
}
