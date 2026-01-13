#include "Core/Core.h"
#include "RHI/RHI.h"

#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#endif

#include <fstream>
#include <vector>
#include <string>
#include <cmath>

// =============================================================================
// Math Utilities
// =============================================================================
struct Vec3
{
    float x, y, z;
    Vec3(float x_ = 0, float y_ = 0, float z_ = 0) : x(x_), y(y_), z(z_) {}
};

struct Mat4
{
    float m[16];

    Mat4()
    {
        Identity();
    }

    void Identity()
    {
        for (int i = 0; i < 16; ++i) m[i] = 0.0f;
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    static Mat4 RotationX(float angle)
    {
        Mat4 result;
        float c = std::cos(angle);
        float s = std::sin(angle);
        result.m[5] = c;
        result.m[6] = s;
        result.m[9] = -s;
        result.m[10] = c;
        return result;
    }

    static Mat4 RotationY(float angle)
    {
        Mat4 result;
        float c = std::cos(angle);
        float s = std::sin(angle);
        result.m[0] = c;
        result.m[2] = -s;
        result.m[8] = s;
        result.m[10] = c;
        return result;
    }

    static Mat4 RotationZ(float angle)
    {
        Mat4 result;
        float c = std::cos(angle);
        float s = std::sin(angle);
        result.m[0] = c;
        result.m[1] = s;
        result.m[4] = -s;
        result.m[5] = c;
        return result;
    }

    Mat4 operator*(const Mat4& other) const
    {
        Mat4 result;
        for (int i = 0; i < 4; ++i)
        {
            for (int j = 0; j < 4; ++j)
            {
                result.m[i * 4 + j] = 0;
                for (int k = 0; k < 4; ++k)
                {
                    result.m[i * 4 + j] += m[i * 4 + k] * other.m[k * 4 + j];
                }
            }
        }
        return result;
    }
};

// =============================================================================
// Transform Constant Buffer Data
// =============================================================================
struct TransformCB
{
    Mat4 worldMatrix;
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
// Mouse State
// =============================================================================
struct MouseState
{
    bool isDragging = false;
    double lastX = 0;
    double lastY = 0;
    float rotationX = 0.0f;  // Pitch
    float rotationY = 0.0f;  // Yaw
    float rotationZ = 0.0f;  // Roll (mouse wheel)
    float targetRotX = 0.0f;
    float targetRotY = 0.0f;
};

static MouseState g_mouseState;

// =============================================================================
// GLFW Callbacks
// =============================================================================
void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
    if (button == GLFW_MOUSE_BUTTON_LEFT)
    {
        if (action == GLFW_PRESS)
        {
            g_mouseState.isDragging = true;
            glfwGetCursorPos(window, &g_mouseState.lastX, &g_mouseState.lastY);
        }
        else if (action == GLFW_RELEASE)
        {
            g_mouseState.isDragging = false;
        }
    }
}

void CursorPosCallback(GLFWwindow* window, double xpos, double ypos)
{
    if (g_mouseState.isDragging)
    {
        double deltaX = xpos - g_mouseState.lastX;
        double deltaY = ypos - g_mouseState.lastY;

        // Sensitivity for rotation
        const float sensitivity = 0.01f;

        g_mouseState.targetRotY += static_cast<float>(deltaX) * sensitivity;
        g_mouseState.targetRotX += static_cast<float>(deltaY) * sensitivity;

        g_mouseState.lastX = xpos;
        g_mouseState.lastY = ypos;
    }
}

void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset)
{
    // Scroll for Z-axis rotation
    g_mouseState.rotationZ += static_cast<float>(yoffset) * 0.1f;
}

// =============================================================================
// File Loading
// =============================================================================
#ifdef _WIN32
#include <Windows.h>
#endif

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

std::vector<uint8_t> LoadFile(const std::string& path)
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
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
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

// =============================================================================
// Shader Compilation (D3DCompile for DX12)
// =============================================================================
#ifdef _WIN32
bool CompileShader(const std::string& source, const char* entryPoint, const char* target,
                   std::vector<uint8_t>& outBytecode, std::string& outError)
{
    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompile(
        source.c_str(),
        source.length(),
        nullptr,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint,
        target,
        compileFlags,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            outError = std::string(static_cast<const char*>(errorBlob->GetBufferPointer()),
                                   errorBlob->GetBufferSize());
            errorBlob->Release();
        }
        if (shaderBlob)
        {
            shaderBlob->Release();
        }
        return false;
    }

    outBytecode.resize(shaderBlob->GetBufferSize());
    memcpy(outBytecode.data(), shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());

    shaderBlob->Release();
    if (errorBlob)
    {
        errorBlob->Release();
    }

    return true;
}
#endif

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[])
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
    GLFWwindow* window = glfwCreateWindow(1280, 720, "RenderVerseX - Interactive Triangle", nullptr, nullptr);
    if (!window)
    {
        RVX_CORE_CRITICAL("Failed to create window");
        glfwTerminate();
        return -1;
    }

    // Set up mouse callbacks
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorPosCallback);
    glfwSetScrollCallback(window, ScrollCallback);

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
    swapChainDesc.format = RVX::RHIFormat::BGRA8_UNORM;
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
    std::vector<RVX::RHICommandContextRef> cmdContexts(swapChain->GetBufferCount());
    for (RVX::uint32 i = 0; i < swapChain->GetBufferCount(); ++i)
    {
        cmdContexts[i] = device->CreateCommandContext(RVX::RHICommandQueueType::Graphics);
    }

    // =========================================================================
    // Create Vertex Buffer - Triangle in NDC
    // =========================================================================
    Vertex triangleVertices[] =
    {
        {{ 0.0f,  0.6f, 0.0f}, {1.0f, 0.2f, 0.3f, 1.0f}},  // Top - Coral
        {{ 0.5f, -0.4f, 0.0f}, {0.2f, 1.0f, 0.4f, 1.0f}},  // Right - Lime
        {{-0.5f, -0.4f, 0.0f}, {0.3f, 0.4f, 1.0f, 1.0f}},  // Left - Sky Blue
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
    cbDesc.size = sizeof(TransformCB);
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

    std::vector<uint8_t> vsBytecode, psBytecode;
    std::string compileError;

#ifdef _WIN32
    // Compile vertex shader
    if (!CompileShader(shaderSource, "VSMain", "vs_5_0", vsBytecode, compileError))
    {
        RVX_CORE_CRITICAL("Failed to compile vertex shader: {}", compileError);
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    RVX_CORE_INFO("Compiled vertex shader ({} bytes)", vsBytecode.size());

    // Compile pixel shader
    if (!CompileShader(shaderSource, "PSMain", "ps_5_0", psBytecode, compileError))
    {
        RVX_CORE_CRITICAL("Failed to compile pixel shader: {}", compileError);
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    RVX_CORE_INFO("Compiled pixel shader ({} bytes)", psBytecode.size());
#endif

    // Create RHI shader objects
    RVX::RHIShaderDesc vsDesc;
    vsDesc.stage = RVX::RHIShaderStage::Vertex;
    vsDesc.bytecode = vsBytecode.data();
    vsDesc.bytecodeSize = vsBytecode.size();
    vsDesc.entryPoint = "VSMain";
    vsDesc.debugName = "TriangleVS";
    auto vertexShader = device->CreateShader(vsDesc);

    RVX::RHIShaderDesc psDesc;
    psDesc.stage = RVX::RHIShaderStage::Pixel;
    psDesc.bytecode = psBytecode.data();
    psDesc.bytecodeSize = psBytecode.size();
    psDesc.entryPoint = "PSMain";
    psDesc.debugName = "TrianglePS";
    auto pixelShader = device->CreateShader(psDesc);

    if (!vertexShader || !pixelShader)
    {
        RVX_CORE_CRITICAL("Failed to create shader objects");
        device.reset();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // =========================================================================
    // Create Descriptor Set Layout and Pipeline Layout
    // =========================================================================
    RVX::RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "TriangleSetLayout";
    setLayoutDesc.AddBinding(0, RVX::RHIBindingType::UniformBuffer, RVX::RHIShaderStage::Vertex);
    auto descriptorSetLayout = device->CreateDescriptorSetLayout(setLayoutDesc);

    RVX::RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.setLayouts.push_back(descriptorSetLayout.Get());
    pipelineLayoutDesc.debugName = "TrianglePipelineLayout";
    auto pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);

    if (!descriptorSetLayout || !pipelineLayout)
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
    descSetDesc.layout = descriptorSetLayout.Get();
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
    pipelineDesc.renderTargetFormats[0] = RVX::RHIFormat::BGRA8_UNORM;
    pipelineDesc.depthStencilFormat = RVX::RHIFormat::Unknown;
    pipelineDesc.primitiveTopology = RVX::RHIPrimitiveTopology::TriangleList;

    auto pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        RVX_CORE_CRITICAL("Failed to create graphics pipeline");
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
    std::vector<RVX::RHIResourceState> backBufferStates(swapChain->GetBufferCount(), RVX::RHIResourceState::Common);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Check for ESC key
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        // Smooth rotation interpolation
        float smoothFactor = 0.15f;
        g_mouseState.rotationX += (g_mouseState.targetRotX - g_mouseState.rotationX) * smoothFactor;
        g_mouseState.rotationY += (g_mouseState.targetRotY - g_mouseState.rotationY) * smoothFactor;

        // Update constant buffer with transform
        TransformCB transformData;
        Mat4 rotX = Mat4::RotationX(g_mouseState.rotationX);
        Mat4 rotY = Mat4::RotationY(g_mouseState.rotationY);
        Mat4 rotZ = Mat4::RotationZ(g_mouseState.rotationZ);
        transformData.worldMatrix = rotZ * rotY * rotX;
        
        // Pulsing tint color based on time
        double currentTime = glfwGetTime();
        float pulse = (std::sin(static_cast<float>(currentTime) * 2.0f) + 1.0f) * 0.2f + 0.8f;
        transformData.tintColor[0] = pulse;
        transformData.tintColor[1] = pulse;
        transformData.tintColor[2] = pulse;
        transformData.tintColor[3] = 1.0f;

        constantBuffer->Upload(&transformData, 1);

        // Begin frame
        device->BeginFrame();

        // Get current back buffer
        RVX::uint32 backBufferIndex = swapChain->GetCurrentBackBufferIndex();
        auto* backBuffer = swapChain->GetCurrentBackBuffer();
        auto* backBufferView = swapChain->GetCurrentBackBufferView();
        auto& cmdContext = cmdContexts[backBufferIndex];

        // Begin command recording
        cmdContext->Begin();

        // Transition to render target
        cmdContext->TextureBarrier(backBuffer, backBufferStates[backBufferIndex], RVX::RHIResourceState::RenderTarget);

        // Begin render pass
        RVX::RHIRenderPassDesc renderPass;
        renderPass.AddColorAttachment(
            backBufferView,
            RVX::RHILoadOp::Clear,
            RVX::RHIStoreOp::Store,
            {0.08f, 0.08f, 0.12f, 1.0f}  // Dark background
        );

        cmdContext->BeginRenderPass(renderPass);

        // Set viewport and scissor
        RVX::RHIViewport viewport;
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = static_cast<float>(swapChain->GetWidth());
        viewport.height = static_cast<float>(swapChain->GetHeight());
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        cmdContext->SetViewport(viewport);

        RVX::RHIRect scissor;
        scissor.x = 0;
        scissor.y = 0;
        scissor.width = swapChain->GetWidth();
        scissor.height = swapChain->GetHeight();
        cmdContext->SetScissor(scissor);

        // Set pipeline and resources
        cmdContext->SetPipeline(pipeline.Get());
        cmdContext->SetDescriptorSet(0, descriptorSet.Get());
        cmdContext->SetVertexBuffer(0, vertexBuffer.Get());

        // Draw the triangle!
        cmdContext->Draw(3, 1, 0, 0);

        cmdContext->EndRenderPass();

        // Transition back to present
        cmdContext->TextureBarrier(backBuffer, RVX::RHIResourceState::RenderTarget, RVX::RHIResourceState::Present);
        backBufferStates[backBufferIndex] = RVX::RHIResourceState::Present;

        cmdContext->End();

        // Submit commands
        device->SubmitCommandContext(cmdContext.Get(), nullptr);

        // Present
        swapChain->Present();

        // End frame
        device->EndFrame();

        frameCount++;

        // FPS counter
        if (currentTime - lastTime >= 1.0)
        {
            RVX_CORE_DEBUG("FPS: {}", frameCount);
            frameCount = 0;
            lastTime = currentTime;
        }

        // Handle resize
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        if (width > 0 && height > 0 &&
            (static_cast<RVX::uint32>(width) != swapChain->GetWidth() ||
             static_cast<RVX::uint32>(height) != swapChain->GetHeight()))
        {
            device->WaitIdle();
            swapChain->Resize(static_cast<RVX::uint32>(width), static_cast<RVX::uint32>(height));
            
            // Reset back buffer states after resize
            backBufferStates.assign(swapChain->GetBufferCount(), RVX::RHIResourceState::Common);
            
            RVX_CORE_INFO("Resized to {}x{}", width, height);
        }
    }

    // Cleanup
    device->WaitIdle();

    descriptorSet = nullptr;
    pipeline = nullptr;
    pipelineLayout = nullptr;
    descriptorSetLayout = nullptr;
    vertexShader = nullptr;
    pixelShader = nullptr;
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
