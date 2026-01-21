/**
 * @file PipelineCache.cpp
 * @brief PipelineCache implementation
 */

#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include "Core/Log.h"
#include <filesystem>

namespace RVX
{

PipelineCache::~PipelineCache()
{
    Shutdown();
}

bool PipelineCache::Initialize(IRHIDevice* device, const std::string& shaderDir)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("PipelineCache already initialized");
        return true;
    }

    if (!device)
    {
        RVX_CORE_ERROR("PipelineCache: Invalid device");
        return false;
    }

    m_device = device;
    m_shaderDir = shaderDir;

    // Create shader manager with compiler
    m_shaderManager = std::make_unique<ShaderManager>(CreateShaderCompiler());

    // Compile shaders
    if (!CompileShaders())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to compile shaders");
        return false;
    }

    // Create pipeline layout using shader reflection
    if (!CreatePipelineLayout())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create pipeline layout");
        return false;
    }

    // Create object constant buffer FIRST (needed for descriptor set creation)
    if (!CreateObjectConstantBuffer())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create object constant buffer");
        return false;
    }
    // Create view constant buffer and descriptor set (binds both buffers)
    if (!CreateViewConstantBuffer())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create view constant buffer");
        return false;
    }

    // Create graphics pipeline
    if (!CreatePipeline())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create graphics pipeline");
        return false;
    }

    m_initialized = true;
    RVX_CORE_DEBUG("PipelineCache initialized");
    return true;
}

void PipelineCache::Shutdown()
{
    if (!m_initialized)
        return;

    m_opaquePipeline.Reset();
    m_viewDescriptorSet.Reset();
    m_viewConstantBuffer.Reset();
    m_objectConstantBuffer.Reset();
    m_pipelineLayout.Reset();
    m_setLayouts.clear();
    m_vertexShader.Reset();
    m_pixelShader.Reset();
    m_shaderManager.reset();
    m_device = nullptr;
    m_initialized = false;

    RVX_CORE_DEBUG("PipelineCache shutdown");
}

bool PipelineCache::CompileShaders()
{
    std::string shaderPath = m_shaderDir + "/DefaultLit.hlsl";

    RVX_CORE_INFO("PipelineCache: Compiling shaders...");
    RVX_CORE_INFO("  Shader directory: {}", m_shaderDir);
    RVX_CORE_INFO("  Shader path: {}", shaderPath);

    // Check if shader file exists
    if (!std::filesystem::exists(shaderPath))
    {
        RVX_CORE_ERROR("PipelineCache: Shader file not found: {}", shaderPath);
        
        // Try absolute path
        std::filesystem::path absPath = std::filesystem::absolute(shaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    RVX_CORE_INFO("  Shader file found!");

    RHIBackendType backend = m_device->GetBackendType();
    RVX_CORE_INFO("  Backend type: {}", static_cast<int>(backend));

    // Compile vertex shader
    ShaderLoadDesc vsDesc;
    vsDesc.path = shaderPath;
    vsDesc.entryPoint = "VSMain";
    vsDesc.stage = RHIShaderStage::Vertex;
    vsDesc.backend = backend;
    vsDesc.enableDebugInfo = true;

    auto vsResult = m_shaderManager->LoadFromFile(m_device, vsDesc);
    if (!vsResult.compileResult.success)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to compile vertex shader: {}", 
                       vsResult.compileResult.errorMessage);
        return false;
    }
    m_vertexShader = vsResult.shader;
    m_vsCompileResult = vsResult.compileResult;

    // Compile pixel shader
    ShaderLoadDesc psDesc = vsDesc;
    psDesc.entryPoint = "PSMain";
    psDesc.stage = RHIShaderStage::Pixel;

    auto psResult = m_shaderManager->LoadFromFile(m_device, psDesc);
    if (!psResult.compileResult.success)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to compile pixel shader: {}", 
                       psResult.compileResult.errorMessage);
        return false;
    }
    m_pixelShader = psResult.shader;
    m_psCompileResult = psResult.compileResult;

    RVX_CORE_DEBUG("PipelineCache: Compiled shaders successfully");
    return true;
}

bool PipelineCache::CreatePipelineLayout()
{
    // Use shader reflection to auto-generate layout
    std::vector<ReflectedShader> reflectedShaders = {
        {m_vsCompileResult.reflection, RHIShaderStage::Vertex},
        {m_psCompileResult.reflection, RHIShaderStage::Pixel}
    };

    AutoPipelineLayout autoLayout = BuildAutoPipelineLayout(reflectedShaders);

    // Create descriptor set layouts from reflection
    m_setLayouts.resize(autoLayout.setLayouts.size());
    for (size_t i = 0; i < autoLayout.setLayouts.size(); ++i)
    {
        if (!autoLayout.setLayouts[i].entries.empty())
        {
            m_setLayouts[i] = m_device->CreateDescriptorSetLayout(autoLayout.setLayouts[i]);
            if (!m_setLayouts[i])
            {
                RVX_CORE_ERROR("PipelineCache: Failed to create descriptor set layout {}", i);
                return false;
            }
        }
    }

    // Add set layouts to pipeline layout descriptor
    RHIPipelineLayoutDesc layoutDesc = autoLayout.pipelineLayout;
    for (const auto& layout : m_setLayouts)
    {
        if (layout)
        {
            layoutDesc.setLayouts.push_back(layout.Get());
        }
    }

    // Create pipeline layout
    m_pipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_pipelineLayout)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created pipeline layout with {} set layouts", 
                   m_setLayouts.size());
    return true;
}

bool PipelineCache::CreateViewConstantBuffer()
{
    // Create constant buffer for view data
    RHIBufferDesc cbDesc;
    cbDesc.size = (sizeof(ViewConstants) + 255) & ~255u;  // 256-byte aligned
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.memoryType = RHIMemoryType::Upload;
    cbDesc.debugName = "ViewConstantBuffer";

    m_viewConstantBuffer = m_device->CreateBuffer(cbDesc);
    if (!m_viewConstantBuffer)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create view constant buffer");
        return false;
    }

    // Create descriptor set for view/object constants
    if (!m_setLayouts.empty() && m_setLayouts[0])
    {
        RHIDescriptorSetDesc descSetDesc;
        descSetDesc.layout = m_setLayouts[0].Get();
        descSetDesc.BindBuffer(0, m_viewConstantBuffer.Get());
        descSetDesc.BindBuffer(1, m_objectConstantBuffer.Get());
        
        m_viewDescriptorSet = m_device->CreateDescriptorSet(descSetDesc);
        if (!m_viewDescriptorSet)
        {
            RVX_CORE_ERROR("PipelineCache: Failed to create view descriptor set");
            return false;
        }
    }

    return true;
}

bool PipelineCache::CreateObjectConstantBuffer()
{
    // Create constant buffer for object data
    RHIBufferDesc cbDesc;
    cbDesc.size = (sizeof(ObjectConstants) + 255) & ~255u;  // 256-byte aligned
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.memoryType = RHIMemoryType::Upload;
    cbDesc.debugName = "ObjectConstantBuffer";

    m_objectConstantBuffer = m_device->CreateBuffer(cbDesc);
    if (!m_objectConstantBuffer)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create object constant buffer");
        return false;
    }

    return true;
}

bool PipelineCache::CreatePipeline()
{
    RHIGraphicsPipelineDesc pipelineDesc;
    
    // Shaders
    pipelineDesc.vertexShader = m_vertexShader.Get();
    pipelineDesc.pixelShader = m_pixelShader.Get();
    pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
    pipelineDesc.debugName = "DefaultOpaquePipeline";

    // Input layout - separate vertex buffer slots
    // Slot 0: Position (float3)
    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RGB32_FLOAT, 0);
    // Slot 1: Normal (float3)
    pipelineDesc.inputLayout.AddElement("NORMAL", RHIFormat::RGB32_FLOAT, 1);
    // Slot 2: TexCoord (float2)
    pipelineDesc.inputLayout.AddElement("TEXCOORD", RHIFormat::RG32_FLOAT, 2);

    // Rasterizer state
    // Note: We flip Y in the projection matrix for Vulkan, which reverses triangle winding.
    // So we need to use Clockwise front face instead of the default CounterClockwise.
    // Also disable backface culling for now to debug depth issues with various models.
    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.frontFace = RHIFrontFace::Clockwise;
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;  // Disable culling for debugging
    
    // Depth stencil state
    pipelineDesc.depthStencilState = RHIDepthStencilState::Default();
    
    // Blend state
    pipelineDesc.blendState = RHIBlendState::Default();

    // Render target formats
    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = m_renderTargetFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::D24_UNORM_S8_UINT;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    m_opaquePipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!m_opaquePipeline)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create opaque pipeline");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created opaque pipeline");
    return true;
}

void PipelineCache::UpdateViewConstants(const ViewData& view)
{
    if (!m_viewConstantBuffer)
        return;

    ViewConstants constants;
    // GLM and HLSL cbuffer both use column-major storage by default.
    // Shader uses mul(M, v) for correct column-vector multiplication.
    constants.viewProjection = view.viewProjectionMatrix;
    
    // Vulkan has Y-axis pointing down in clip space (opposite of DX/OpenGL).
    // Apply Y-flip to the view-projection matrix for Vulkan backend.
    if (m_device && m_device->GetBackendType() == RHIBackendType::Vulkan)
    {
        // Flip Y by negating the second row of the matrix
        constants.viewProjection[1] = -constants.viewProjection[1];
    }
    
    constants.cameraPosition = view.cameraPosition;
    constants.time = view.time;
    constants.lightDirection = Vec3(0.5f, -0.8f, 0.3f);  // Default light direction
    constants.padding = 0.0f;

    void* mapped = m_viewConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(mapped, &constants, sizeof(ViewConstants));
        m_viewConstantBuffer->Unmap();
    }
}

void PipelineCache::UpdateObjectConstants(const Mat4& worldMatrix)
{
    if (!m_objectConstantBuffer)
        return;

    ObjectConstants constants;
    // GLM and HLSL cbuffer both use column-major storage.
    // Shader uses mul(M, v) for correct column-vector multiplication.
    constants.world = worldMatrix;

    void* mapped = m_objectConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(mapped, &constants, sizeof(ObjectConstants));
        m_objectConstantBuffer->Unmap();
    }
}

} // namespace RVX
