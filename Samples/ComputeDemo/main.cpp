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
// Constants
// =============================================================================
constexpr RVX::uint32 TEXTURE_SIZE = 512;
constexpr RVX::uint32 GROUP_SIZE = 8;

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
// Main - Compute Shader Demo
// =============================================================================
int main(int argc, char* argv[])
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("RenderVerseX Compute Shader Demo");
    RVX_CORE_INFO("Demonstrates GPU compute for procedural texture generation");

    if (!glfwInit())
    {
        RVX_CORE_CRITICAL("Failed to initialize GLFW");
        return -1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "RenderVerseX - Compute Demo", nullptr, nullptr);
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
        if (arg == "--dx12" || arg == "-d12")
            backend = RVX::RHIBackendType::DX12;
        else if (arg == "--vulkan" || arg == "-vk")
            backend = RVX::RHIBackendType::Vulkan;
    }

    // DX11 doesn't support UAV textures in the same way, skip
    if (backend == RVX::RHIBackendType::DX11)
    {
        RVX_CORE_WARN("DX11 not fully supported for this demo, using DX12");
        backend = RVX::RHIBackendType::DX12;
    }

    RVX_CORE_INFO("Using backend: {}", RVX::ToString(backend));

    // Create RHI device
    RVX::RHIDeviceDesc deviceDesc;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.applicationName = "Compute Demo";

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

    // Create command contexts
    auto graphicsContext = device->CreateCommandContext(RVX::RHICommandQueueType::Graphics);
    auto computeContext = device->CreateCommandContext(RVX::RHICommandQueueType::Compute);

    // =========================================================================
    // Create UAV Texture for Compute Output
    // =========================================================================
    RVX::RHITextureDesc uavTexDesc;
    uavTexDesc.width = TEXTURE_SIZE;
    uavTexDesc.height = TEXTURE_SIZE;
    uavTexDesc.format = RVX::RHIFormat::RGBA8_UNORM;
    uavTexDesc.usage = RVX::RHITextureUsage::UnorderedAccess | RVX::RHITextureUsage::ShaderResource;
    uavTexDesc.debugName = "ComputeOutput";

    auto computeTexture = device->CreateTexture(uavTexDesc);
    RVX_CORE_INFO("Created UAV texture {}x{}", TEXTURE_SIZE, TEXTURE_SIZE);

    // Create views
    RVX::RHITextureViewDesc srvViewDesc;
    srvViewDesc.format = RVX::RHIFormat::RGBA8_UNORM;
    auto textureSRV = device->CreateTextureView(computeTexture.Get(), srvViewDesc);

    RVX::RHITextureViewDesc uavViewDesc;
    uavViewDesc.format = RVX::RHIFormat::RGBA8_UNORM;
    auto textureUAV = device->CreateTextureView(computeTexture.Get(), uavViewDesc);

    // Create sampler
    RVX::RHISamplerDesc samplerDesc;
    samplerDesc.magFilter = RVX::RHIFilterMode::Linear;
    samplerDesc.minFilter = RVX::RHIFilterMode::Linear;
    samplerDesc.addressU = RVX::RHIAddressMode::ClampToEdge;
    samplerDesc.addressV = RVX::RHIAddressMode::ClampToEdge;
    auto sampler = device->CreateSampler(samplerDesc);

    // =========================================================================
    // Create Constant Buffer for Compute Parameters
    // =========================================================================
    struct ComputeParams
    {
        float time;
        float scale;
        RVX::uint32 width;
        RVX::uint32 height;
    };

    RVX::RHIBufferDesc cbDesc;
    cbDesc.size = (sizeof(ComputeParams) + 255) & ~255u;
    cbDesc.usage = RVX::RHIBufferUsage::Constant;
    cbDesc.memoryType = RVX::RHIMemoryType::Upload;
    cbDesc.debugName = "ComputeParams";

    auto paramsBuffer = device->CreateBuffer(cbDesc);

    // =========================================================================
    // Load and Compile Shaders
    // =========================================================================
    std::string exeDir = GetExecutableDir();
    RVX::ShaderManager shaderManager(RVX::CreateShaderCompiler());

    // Compute shader
    RVX::ShaderLoadDesc csLoad;
    csLoad.path = exeDir + "Shaders/ProceduralTexture.hlsl";
    csLoad.entryPoint = "CSMain";
    csLoad.stage = RVX::RHIShaderStage::Compute;
    csLoad.backend = backend;

    auto csResult = shaderManager.LoadFromFile(device.get(), csLoad);
    if (!csResult.compileResult.success)
    {
        RVX_CORE_CRITICAL("Failed to compile compute shader: {}", csResult.compileResult.errorMessage);
        return -1;
    }
    auto computeShader = csResult.shader;
    RVX_CORE_INFO("Compiled compute shader");

    // Fullscreen quad shaders
    RVX::ShaderLoadDesc vsLoad;
    vsLoad.path = exeDir + "Shaders/FullscreenQuad.hlsl";
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

    RVX_CORE_INFO("Compiled graphics shaders");

    // =========================================================================
    // Create Compute Pipeline
    // =========================================================================
    std::vector<RVX::ReflectedShader> computeReflected = {
        {csResult.compileResult.reflection, RVX::RHIShaderStage::Compute}
    };
    auto computeAutoLayout = RVX::BuildAutoPipelineLayout(computeReflected);

    std::vector<RVX::RHIDescriptorSetLayoutRef> computeSetLayouts(computeAutoLayout.setLayouts.size());
    for (size_t i = 0; i < computeAutoLayout.setLayouts.size(); ++i)
    {
        if (computeAutoLayout.setLayouts[i].entries.empty())
            continue;
        computeSetLayouts[i] = device->CreateDescriptorSetLayout(computeAutoLayout.setLayouts[i]);
    }

    RVX::RHIPipelineLayoutDesc computePipelineLayoutDesc = computeAutoLayout.pipelineLayout;
    for (const auto& layout : computeSetLayouts)
    {
        computePipelineLayoutDesc.setLayouts.push_back(layout.Get());
    }
    auto computePipelineLayout = device->CreatePipelineLayout(computePipelineLayoutDesc);

    RVX::RHIComputePipelineDesc computePipelineDesc;
    computePipelineDesc.computeShader = computeShader.Get();
    computePipelineDesc.pipelineLayout = computePipelineLayout.Get();
    computePipelineDesc.debugName = "ProceduralTexturePipeline";

    auto computePipeline = device->CreateComputePipeline(computePipelineDesc);
    RVX_CORE_INFO("Created compute pipeline");

    // Create compute descriptor set
    RVX::RHIDescriptorSetDesc computeDescSetDesc;
    computeDescSetDesc.layout = computeSetLayouts.empty() ? nullptr : computeSetLayouts[0].Get();
    computeDescSetDesc.BindBuffer(0, paramsBuffer.Get());
    computeDescSetDesc.BindTexture(1, textureUAV.Get());  // UAV binding
    auto computeDescSet = device->CreateDescriptorSet(computeDescSetDesc);

    // =========================================================================
    // Create Graphics Pipeline
    // =========================================================================
    std::vector<RVX::ReflectedShader> graphicsReflected = {
        {vsResult.compileResult.reflection, RVX::RHIShaderStage::Vertex},
        {psResult.compileResult.reflection, RVX::RHIShaderStage::Pixel}
    };
    auto graphicsAutoLayout = RVX::BuildAutoPipelineLayout(graphicsReflected);

    std::vector<RVX::RHIDescriptorSetLayoutRef> graphicsSetLayouts(graphicsAutoLayout.setLayouts.size());
    for (size_t i = 0; i < graphicsAutoLayout.setLayouts.size(); ++i)
    {
        if (graphicsAutoLayout.setLayouts[i].entries.empty())
            continue;
        graphicsSetLayouts[i] = device->CreateDescriptorSetLayout(graphicsAutoLayout.setLayouts[i]);
    }

    RVX::RHIPipelineLayoutDesc graphicsPipelineLayoutDesc = graphicsAutoLayout.pipelineLayout;
    for (const auto& layout : graphicsSetLayouts)
    {
        graphicsPipelineLayoutDesc.setLayouts.push_back(layout.Get());
    }
    auto graphicsPipelineLayout = device->CreatePipelineLayout(graphicsPipelineLayoutDesc);

    RVX::RHIGraphicsPipelineDesc graphicsPipelineDesc;
    graphicsPipelineDesc.vertexShader = vsResult.shader.Get();
    graphicsPipelineDesc.pixelShader = psResult.shader.Get();
    graphicsPipelineDesc.pipelineLayout = graphicsPipelineLayout.Get();
    graphicsPipelineDesc.rasterizerState = RVX::RHIRasterizerState::NoCull();
    graphicsPipelineDesc.depthStencilState = RVX::RHIDepthStencilState::Disabled();
    graphicsPipelineDesc.blendState = RVX::RHIBlendState::Default();
    graphicsPipelineDesc.numRenderTargets = 1;
    graphicsPipelineDesc.renderTargetFormats[0] = swapChain->GetFormat();
    graphicsPipelineDesc.primitiveTopology = RVX::RHIPrimitiveTopology::TriangleList;
    graphicsPipelineDesc.debugName = "FullscreenQuadPipeline";

    auto graphicsPipeline = device->CreateGraphicsPipeline(graphicsPipelineDesc);
    RVX_CORE_INFO("Created graphics pipeline");

    // Create graphics descriptor set
    RVX::RHIDescriptorSetDesc graphicsDescSetDesc;
    graphicsDescSetDesc.layout = graphicsSetLayouts.empty() ? nullptr : graphicsSetLayouts[0].Get();
    graphicsDescSetDesc.BindTexture(0, textureSRV.Get());
    graphicsDescSetDesc.BindSampler(0, sampler.Get());
    auto graphicsDescSet = device->CreateDescriptorSet(graphicsDescSetDesc);

    // =========================================================================
    // Main Loop
    // =========================================================================
    RVX_CORE_INFO("Compute Demo initialized - press ESC to exit");
    RVX_CORE_INFO("The texture is generated procedurally by a compute shader each frame");

    RVX::RHIResourceState textureState = RVX::RHIResourceState::Undefined;
    RVX::RHIResourceState backBufferState = RVX::RHIResourceState::Undefined;
    double startTime = glfwGetTime();

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        float time = static_cast<float>(glfwGetTime() - startTime);

        // Update compute parameters
        ComputeParams params;
        params.time = time;
        params.scale = 4.0f;
        params.width = TEXTURE_SIZE;
        params.height = TEXTURE_SIZE;
        paramsBuffer->Upload(&params, 1);

        // Begin frame
        auto* backBuffer = swapChain->GetCurrentBackBuffer();
        auto* backBufferView = swapChain->GetCurrentBackBufferView();

        // Compute pass
        computeContext->Begin();
        
        // Transition texture to UAV
        computeContext->TextureBarrier({computeTexture.Get(), textureState, RVX::RHIResourceState::UnorderedAccess});
        
        computeContext->SetPipeline(computePipeline.Get());
        computeContext->SetDescriptorSet(0, computeDescSet.Get());
        
        RVX::uint32 groupsX = (TEXTURE_SIZE + GROUP_SIZE - 1) / GROUP_SIZE;
        RVX::uint32 groupsY = (TEXTURE_SIZE + GROUP_SIZE - 1) / GROUP_SIZE;
        computeContext->Dispatch(groupsX, groupsY, 1);
        
        // Transition texture to SRV
        computeContext->TextureBarrier({computeTexture.Get(), RVX::RHIResourceState::UnorderedAccess, RVX::RHIResourceState::ShaderResource});
        textureState = RVX::RHIResourceState::ShaderResource;
        
        computeContext->End();
        device->SubmitCommandContext(computeContext.Get(), nullptr);

        // Graphics pass
        graphicsContext->Begin();
        
        graphicsContext->TextureBarrier({backBuffer, backBufferState, RVX::RHIResourceState::RenderTarget});

        RVX::RHIRenderPassDesc renderPass;
        renderPass.AddColorAttachment(backBufferView, RVX::RHILoadOp::Clear, RVX::RHIStoreOp::Store,
                                      {0.0f, 0.0f, 0.0f, 1.0f});

        graphicsContext->BeginRenderPass(renderPass);

        RVX::RHIViewport viewport = {0, 0, 
            static_cast<float>(swapChain->GetWidth()), 
            static_cast<float>(swapChain->GetHeight()), 
            0.0f, 1.0f};
        graphicsContext->SetViewport(viewport);

        RVX::RHIRect scissor = {0, 0, swapChain->GetWidth(), swapChain->GetHeight()};
        graphicsContext->SetScissor(scissor);

        graphicsContext->SetPipeline(graphicsPipeline.Get());
        graphicsContext->SetDescriptorSet(0, graphicsDescSet.Get());
        graphicsContext->Draw(3, 1, 0, 0);  // Fullscreen triangle

        graphicsContext->EndRenderPass();

        graphicsContext->TextureBarrier({backBuffer, RVX::RHIResourceState::RenderTarget, RVX::RHIResourceState::Present});
        backBufferState = RVX::RHIResourceState::Present;

        graphicsContext->End();

        device->SubmitCommandContext(graphicsContext.Get(), nullptr);
        swapChain->Present();
        device->WaitIdle();
    }

    // Cleanup
    device->WaitIdle();

    graphicsDescSet = nullptr;
    computeDescSet = nullptr;
    graphicsPipeline = nullptr;
    computePipeline = nullptr;
    graphicsPipelineLayout = nullptr;
    computePipelineLayout = nullptr;
    graphicsSetLayouts.clear();
    computeSetLayouts.clear();
    shaderManager.ClearCache();
    paramsBuffer = nullptr;
    sampler = nullptr;
    textureUAV = nullptr;
    textureSRV = nullptr;
    computeTexture = nullptr;
    computeContext = nullptr;
    graphicsContext = nullptr;
    swapChain = nullptr;
    device.reset();

    glfwDestroyWindow(window);
    glfwTerminate();

    RVX::Log::Shutdown();
    return 0;
}
