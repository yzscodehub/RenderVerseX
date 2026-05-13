/**
 * @file PipelineCache.cpp
 * @brief PipelineCache implementation
 */

#include "Render/PipelineCache.h"
#include "Core/Log.h"
#include "Render/GPUUploadService.h"
#include "Render/Renderer/ViewData.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderLayout.h"
#include "ShaderCompiler/ShaderManager.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <utility>

namespace RVX
{

namespace
{
    constexpr uint64 RVX_CONSTANT_BUFFER_ALIGNMENT = 256;
    constexpr uint64 RVX_MAX_DRAW_CONSTANTS_PER_FRAME = 8192;

    uint64 AlignConstantBufferSize(uint64 size)
    {
        return (size + RVX_CONSTANT_BUFFER_ALIGNMENT - 1) & ~(RVX_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

} // namespace

PipelineCache::PipelineCache() = default;

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

    ShaderManagerConfig shaderConfig;
    shaderConfig.cacheDirectory = std::filesystem::current_path() / "ShaderCache";
    shaderConfig.shaderDirectories.push_back(std::filesystem::path(shaderDir));
    m_shaderManager = std::make_unique<ShaderManager>(shaderConfig);

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
    if (!CreateMaterialConstantBuffer())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create material constant buffer");
        return false;
    }
    // Create default material resources before descriptor set allocation.
    if (!CreateDefaultMaterialResources())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create default material resources");
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
    m_materialDescriptorSets.clear();
    m_viewDescriptorSet.Reset();
    m_defaultSampler.Reset();
    m_defaultWhiteTextureView.Reset();
    m_defaultWhiteTexture.Reset();
    m_viewConstantBuffer.Reset();
    m_objectConstantBuffer.Reset();
    m_materialConstantBuffer.Reset();
    m_pipelineLayout.Reset();
    m_setLayouts.clear();
    m_vertexShader.Reset();
    m_pixelShader.Reset();
    m_vsCompileResult.reset();
    m_psCompileResult.reset();
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
    m_vsCompileResult = std::make_unique<ShaderCompileResult>(std::move(vsResult.compileResult));

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
    m_psCompileResult = std::make_unique<ShaderCompileResult>(std::move(psResult.compileResult));

    RVX_CORE_DEBUG("PipelineCache: Compiled shaders successfully");
    return true;
}

bool PipelineCache::CreatePipelineLayout()
{
    if (!m_vsCompileResult || !m_psCompileResult)
    {
        RVX_CORE_ERROR("PipelineCache: Missing shader reflection data");
        return false;
    }

    // Use shader reflection to auto-generate layout
    std::vector<ReflectedShader> reflectedShaders = {
        {m_vsCompileResult->reflection, RHIShaderStage::Vertex},
        {m_psCompileResult->reflection, RHIShaderStage::Pixel}
    };

    AutoPipelineLayout autoLayout = BuildAutoPipelineLayout(reflectedShaders);
    for (auto& setLayout : autoLayout.setLayouts)
    {
        std::sort(setLayout.entries.begin(), setLayout.entries.end(),
                  [](const RHIBindingLayoutEntry& lhs, const RHIBindingLayoutEntry& rhs) {
                      return lhs.binding < rhs.binding;
                  });

        for (auto& entry : setLayout.entries)
        {
            if (entry.type == RHIBindingType::UniformBuffer &&
                (entry.binding == 1 || entry.binding == 4))
            {
                entry.type = RHIBindingType::DynamicUniformBuffer;
                entry.isDynamic = true;
            }
        }
    }

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

void PipelineCache::BeginFrame()
{
    m_objectConstantCursor = 0;
    m_materialConstantCursor = 0;
    m_currentObjectConstantOffset = 0;
    m_currentMaterialConstantOffset = 0;
}

RHIDescriptorSet* PipelineCache::GetViewDescriptorSet()
{
    return GetMaterialDescriptorSet(nullptr);
}

RHIDescriptorSet* PipelineCache::GetMaterialDescriptorSet(RHITextureView* baseColorView)
{
    return GetMaterialDescriptorSetInternal(baseColorView);
}

RHIDescriptorSet* PipelineCache::GetMaterialDescriptorSet(RHITextureView* baseColorView, uint64 textureViewGeneration)
{
    if (m_materialDescriptorCacheGeneration != textureViewGeneration)
    {
        m_materialDescriptorCacheGeneration = textureViewGeneration;
        ClearMaterialDescriptorSets();
    }

    return GetMaterialDescriptorSetInternal(baseColorView);
}

RHIDescriptorSet* PipelineCache::GetMaterialDescriptorSetInternal(RHITextureView* baseColorView)
{
    if (!m_device || m_setLayouts.empty() || !m_setLayouts[0])
        return m_viewDescriptorSet.Get();

    RHITextureView* resolvedView = baseColorView ? baseColorView : m_defaultWhiteTextureView.Get();
    if (!resolvedView)
        return m_viewDescriptorSet.Get();

    MaterialDescriptorKey key;
    key.textureView = resolvedView;

    auto it = m_materialDescriptorSets.find(key);
    if (it != m_materialDescriptorSets.end())
        return it->second.Get();

    RHIDescriptorSetRef descriptorSet = CreateMaterialDescriptorSet(resolvedView);
    if (!descriptorSet)
        return m_viewDescriptorSet.Get();

    RHIDescriptorSet* result = descriptorSet.Get();
    m_materialDescriptorSets.emplace(key, std::move(descriptorSet));
    return result;
}

std::array<uint32, 2> PipelineCache::GetCurrentConstantDynamicOffsets() const
{
    return BuildConstantDynamicOffsets(m_currentObjectConstantOffset, m_currentMaterialConstantOffset);
}

void PipelineCache::ClearMaterialDescriptorSets()
{
    m_materialDescriptorSets.clear();
}

bool PipelineCache::CreateDefaultMaterialResources()
{
    RHITextureDesc textureDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::RGBA8_UNORM);
    textureDesc.debugName = "DefaultWhiteTexture";

    const uint32 whitePixel = 0xFFFFFFFFu;

    GPUUploadTextureDesc uploadDesc;
    uploadDesc.textureDesc = textureDesc;
    uploadDesc.dataSize = sizeof(whitePixel);

    GPUUploadService uploadService;
    uploadService.Initialize(m_device);
    GPUUploadTextureResult uploadResult = uploadService.UploadTextureDataWithResult(uploadDesc, &whitePixel);
    if (!uploadResult)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to upload default white texture");
        return false;
    }

    m_defaultWhiteTexture = uploadResult.resource;
    uploadService.FlushAndWaitForUploads();
    if (uploadResult.isPending && !uploadService.IsUploadComplete(uploadResult.uploadId))
    {
        RVX_CORE_ERROR("PipelineCache: Default white texture upload did not complete");
        return false;
    }
    uploadService.ForgetCompletedUpload(uploadResult.uploadId);

    RHITextureViewDesc viewDesc;
    viewDesc.debugName = "DefaultWhiteTextureView";
    m_defaultWhiteTextureView = m_device->CreateTextureView(m_defaultWhiteTexture.Get(), viewDesc);
    if (!m_defaultWhiteTextureView)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create default white texture view");
        return false;
    }

    RHISamplerDesc samplerDesc = RHISamplerDesc::LinearWrap();
    samplerDesc.debugName = "DefaultMaterialSampler";
    m_defaultSampler = m_device->CreateSampler(samplerDesc);
    if (!m_defaultSampler)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create default material sampler");
        return false;
    }

    auto commandContext = m_device->CreateCommandContext(RHICommandQueueType::Graphics);
    if (!commandContext)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create command context for default material resources");
        return false;
    }

    commandContext->Begin();
    commandContext->TextureBarrier(m_defaultWhiteTexture.Get(), RHIResourceState::Common, RHIResourceState::ShaderResource);
    commandContext->End();
    m_device->SubmitCommandContext(commandContext.Get());
    m_device->WaitIdle();

    return true;
}

RHIDescriptorSetRef PipelineCache::CreateMaterialDescriptorSet(RHITextureView* baseColorView)
{
    if (m_setLayouts.empty() || !m_setLayouts[0] || !m_viewConstantBuffer || !m_objectConstantBuffer ||
        !m_materialConstantBuffer ||
        !baseColorView || !m_defaultSampler)
    {
        return {};
    }

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_setLayouts[0].Get();
    descSetDesc.BindBuffer(0, m_viewConstantBuffer.Get(), 0, AlignConstantBufferSize(sizeof(ViewConstants)));
    descSetDesc.BindBuffer(1, m_objectConstantBuffer.Get(), 0, m_objectConstantStride);
    descSetDesc.BindBuffer(4, m_materialConstantBuffer.Get(), 0, m_materialConstantStride);
    descSetDesc.BindTexture(2, baseColorView);
    descSetDesc.BindSampler(3, m_defaultSampler.Get());

    return m_device->CreateDescriptorSet(descSetDesc);
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
        m_viewDescriptorSet = CreateMaterialDescriptorSet(m_defaultWhiteTextureView.Get());
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
    m_objectConstantStride = AlignConstantBufferSize(sizeof(ObjectConstants));

    RHIBufferDesc cbDesc;
    cbDesc.size = m_objectConstantStride * RVX_MAX_DRAW_CONSTANTS_PER_FRAME;
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

bool PipelineCache::CreateMaterialConstantBuffer()
{
    m_materialConstantStride = AlignConstantBufferSize(sizeof(MaterialConstants));

    RHIBufferDesc cbDesc;
    cbDesc.size = m_materialConstantStride * RVX_MAX_DRAW_CONSTANTS_PER_FRAME;
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.memoryType = RHIMemoryType::Upload;
    cbDesc.debugName = "MaterialConstantBuffer";

    m_materialConstantBuffer = m_device->CreateBuffer(cbDesc);
    if (!m_materialConstantBuffer)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create material constant buffer");
        return false;
    }

    UpdateMaterialConstants(Vec4(1.0f, 1.0f, 1.0f, 1.0f));
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

uint64 PipelineCache::AllocateObjectConstantSlot()
{
    if (m_objectConstantStride == 0)
        m_objectConstantStride = AlignConstantBufferSize(sizeof(ObjectConstants));

    const uint64 offset = m_objectConstantCursor * m_objectConstantStride;
    m_objectConstantCursor = (m_objectConstantCursor + 1) % RVX_MAX_DRAW_CONSTANTS_PER_FRAME;
    m_currentObjectConstantOffset = offset;
    return offset;
}

uint64 PipelineCache::AllocateMaterialConstantSlot()
{
    if (m_materialConstantStride == 0)
        m_materialConstantStride = AlignConstantBufferSize(sizeof(MaterialConstants));

    const uint64 offset = m_materialConstantCursor * m_materialConstantStride;
    m_materialConstantCursor = (m_materialConstantCursor + 1) % RVX_MAX_DRAW_CONSTANTS_PER_FRAME;
    m_currentMaterialConstantOffset = offset;
    return offset;
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

    const uint64 offset = AllocateObjectConstantSlot();
    void* mapped = m_objectConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(static_cast<uint8*>(mapped) + offset, &constants, sizeof(ObjectConstants));
        m_objectConstantBuffer->Unmap();
    }
}

void PipelineCache::UpdateMaterialConstants(const Vec4& baseColorFactor)
{
    if (!m_materialConstantBuffer)
        return;

    MaterialConstants constants;
    constants.baseColorFactor = baseColorFactor;

    const uint64 offset = AllocateMaterialConstantSlot();
    void* mapped = m_materialConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(static_cast<uint8*>(mapped) + offset, &constants, sizeof(MaterialConstants));
        m_materialConstantBuffer->Unmap();
    }
}

} // namespace RVX
