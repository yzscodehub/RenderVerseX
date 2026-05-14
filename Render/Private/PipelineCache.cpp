/**
 * @file PipelineCache.cpp
 * @brief PipelineCache implementation
 */

#include "Render/PipelineCache.h"
#include "Core/Log.h"
#include "Render/Renderer/ViewData.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderManager.h"

#include <cstring>
#include <filesystem>

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

    if (!CompileShaders())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to compile shaders");
        return false;
    }

    if (!CreatePipelineLayout())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create pipeline layout");
        return false;
    }

    if (!CreateObjectConstantBuffer())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create object constant buffer");
        return false;
    }

    if (!CreateViewConstantBuffer())
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create view constant buffer");
        return false;
    }

    m_objectDescriptorSet = CreateObjectDescriptorSet();
    if (!m_objectDescriptorSet)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create object descriptor set");
        return false;
    }

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
    m_maskedPipeline.Reset();
    m_transparentPipeline.Reset();
    m_depthOnlyPipeline.Reset();
    m_frameDescriptorSet.Reset();
    m_objectDescriptorSet.Reset();
    m_viewConstantBuffer.Reset();
    m_objectConstantBuffer.Reset();
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

    if (!std::filesystem::exists(shaderPath))
    {
        RVX_CORE_ERROR("PipelineCache: Shader file not found: {}", shaderPath);

        std::filesystem::path absPath = std::filesystem::absolute(shaderPath);
        RVX_CORE_ERROR("  Absolute path tried: {}", absPath.string());
        RVX_CORE_ERROR("  Current working directory: {}", std::filesystem::current_path().string());
        return false;
    }

    RVX_CORE_INFO("  Shader file found!");

    RHIBackendType backend = m_device->GetBackendType();
    RVX_CORE_INFO("  Backend type: {}", static_cast<int>(backend));

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
    RHIDescriptorSetLayoutDesc frameLayoutDesc;
    frameLayoutDesc.debugName = "DefaultFrameSetLayout";
    frameLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Vertex | RHIShaderStage::Pixel);

    RHIDescriptorSetLayoutDesc objectLayoutDesc;
    objectLayoutDesc.debugName = "DefaultObjectSetLayout";
    RHIBindingLayoutEntry objectEntry;
    objectEntry.binding = 0;
    objectEntry.type = RHIBindingType::DynamicUniformBuffer;
    objectEntry.visibility = RHIShaderStage::Vertex;
    objectEntry.isDynamic = true;
    objectLayoutDesc.entries.push_back(objectEntry);

    RHIDescriptorSetLayoutDesc materialLayoutDesc;
    materialLayoutDesc.debugName = "DefaultMaterialSetLayout";
    RHIBindingLayoutEntry materialEntry;
    materialEntry.binding = 0;
    materialEntry.type = RHIBindingType::DynamicUniformBuffer;
    materialEntry.visibility = RHIShaderStage::Pixel;
    materialEntry.isDynamic = true;
    materialLayoutDesc.entries.push_back(materialEntry);
    materialLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    materialLayoutDesc.AddBinding(2, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    materialLayoutDesc.AddBinding(3, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    materialLayoutDesc.AddBinding(4, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    materialLayoutDesc.AddBinding(5, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    materialLayoutDesc.AddBinding(6, RHIBindingType::Sampler, RHIShaderStage::Pixel);

    m_setLayouts.resize(3);
    m_setLayouts[0] = m_device->CreateDescriptorSetLayout(frameLayoutDesc);
    m_setLayouts[1] = m_device->CreateDescriptorSetLayout(objectLayoutDesc);
    m_setLayouts[2] = m_device->CreateDescriptorSetLayout(materialLayoutDesc);

    if (!m_setLayouts[0] || !m_setLayouts[1] || !m_setLayouts[2])
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create descriptor set layouts");
        return false;
    }

    RHIPipelineLayoutDesc layoutDesc;
    layoutDesc.debugName = "DefaultLitPipelineLayout";
    layoutDesc.setLayouts.push_back(m_setLayouts[0].Get());
    layoutDesc.setLayouts.push_back(m_setLayouts[1].Get());
    layoutDesc.setLayouts.push_back(m_setLayouts[2].Get());

    m_pipelineLayout = m_device->CreatePipelineLayout(layoutDesc);
    if (!m_pipelineLayout)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created pipeline layout with {} set layouts", m_setLayouts.size());
    return true;
}

void PipelineCache::BeginFrame()
{
    m_objectConstantCursor = 0;
    m_currentObjectConstantOffset = 0;
}

RHIDescriptorSet* PipelineCache::GetFrameDescriptorSet()
{
    return m_frameDescriptorSet.Get();
}

RHIDescriptorSet* PipelineCache::GetObjectDescriptorSet()
{
    return m_objectDescriptorSet.Get();
}

RHIDescriptorSetLayout* PipelineCache::GetMaterialSetLayout() const
{
    if (m_setLayouts.size() <= 2)
        return nullptr;

    return m_setLayouts[2].Get();
}

std::array<uint32, 1> PipelineCache::GetCurrentObjectDynamicOffset() const
{
    return BuildSingleDynamicOffset(m_currentObjectConstantOffset);
}

RHIPipeline* PipelineCache::GetPipelineForVariant(MaterialPipelineVariant variant) const
{
    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
            return GetMaskedPipeline();
        case MaterialPipelineVariant::Transparent:
            return GetTransparentPipeline();
        case MaterialPipelineVariant::Opaque:
        default:
            return GetOpaquePipeline();
    }
}

bool PipelineCache::CreateViewConstantBuffer()
{
    RHIBufferDesc cbDesc;
    cbDesc.size = AlignConstantBufferSize(sizeof(ViewConstants));
    cbDesc.usage = RHIBufferUsage::Constant;
    cbDesc.memoryType = RHIMemoryType::Upload;
    cbDesc.debugName = "ViewConstantBuffer";

    m_viewConstantBuffer = m_device->CreateBuffer(cbDesc);
    if (!m_viewConstantBuffer)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create view constant buffer");
        return false;
    }

    m_frameDescriptorSet = CreateFrameDescriptorSet();
    if (!m_frameDescriptorSet)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create frame descriptor set");
        return false;
    }

    return true;
}

bool PipelineCache::CreateObjectConstantBuffer()
{
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

RHIDescriptorSetRef PipelineCache::CreateFrameDescriptorSet()
{
    if (m_setLayouts.empty() || !m_setLayouts[0] || !m_viewConstantBuffer)
        return {};

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_setLayouts[0].Get();
    descSetDesc.debugName = "DefaultFrameDescriptorSet";
    descSetDesc.BindBuffer(0, m_viewConstantBuffer.Get(), 0, AlignConstantBufferSize(sizeof(ViewConstants)));

    return m_device->CreateDescriptorSet(descSetDesc);
}

RHIDescriptorSetRef PipelineCache::CreateObjectDescriptorSet()
{
    if (m_setLayouts.size() <= 1 || !m_setLayouts[1] || !m_objectConstantBuffer)
        return {};

    RHIDescriptorSetDesc descSetDesc;
    descSetDesc.layout = m_setLayouts[1].Get();
    descSetDesc.debugName = "DefaultObjectDescriptorSet";
    descSetDesc.BindBuffer(0, m_objectConstantBuffer.Get(), 0, m_objectConstantStride);

    return m_device->CreateDescriptorSet(descSetDesc);
}

bool PipelineCache::CreatePipeline()
{
    m_opaquePipeline = CreateDefaultLitPipeline("DefaultOpaquePipeline",
                                                RHIDepthStencilState::Default(),
                                                RHIBlendState::Default());
    if (!m_opaquePipeline)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create opaque pipeline");
        return false;
    }

    m_maskedPipeline = CreateDefaultLitPipeline("DefaultMaskedPipeline",
                                                RHIDepthStencilState::Default(),
                                                RHIBlendState::Default());
    if (!m_maskedPipeline)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create masked pipeline");
        return false;
    }

    RHIBlendState transparentBlend = RHIBlendState::Default();
    transparentBlend.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();

    m_transparentPipeline = CreateDefaultLitPipeline("DefaultTransparentPipeline",
                                                     RHIDepthStencilState::ReadOnly(),
                                                     transparentBlend);
    if (!m_transparentPipeline)
    {
        RVX_CORE_ERROR("PipelineCache: Failed to create transparent pipeline");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created material pipeline variants");
    return true;
}

RHIPipelineRef PipelineCache::CreateDefaultLitPipeline(const char* debugName,
                                                       const RHIDepthStencilState& depthStencilState,
                                                       const RHIBlendState& blendState)
{
    RHIGraphicsPipelineDesc pipelineDesc;

    pipelineDesc.vertexShader = m_vertexShader.Get();
    pipelineDesc.pixelShader = m_pixelShader.Get();
    pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
    pipelineDesc.debugName = debugName;

    pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RGB32_FLOAT, 0);
    pipelineDesc.inputLayout.AddElement("NORMAL", RHIFormat::RGB32_FLOAT, 1);
    pipelineDesc.inputLayout.AddElement("TEXCOORD", RHIFormat::RG32_FLOAT, 2);
    pipelineDesc.inputLayout.AddElement("TANGENT", RHIFormat::RGBA32_FLOAT, 3);

    pipelineDesc.rasterizerState = RHIRasterizerState::Default();
    pipelineDesc.rasterizerState.frontFace = RHIFrontFace::Clockwise;
    pipelineDesc.rasterizerState.cullMode = RHICullMode::None;

    pipelineDesc.depthStencilState = depthStencilState;
    pipelineDesc.blendState = blendState;

    pipelineDesc.numRenderTargets = 1;
    pipelineDesc.renderTargetFormats[0] = m_renderTargetFormat;
    pipelineDesc.depthStencilFormat = RHIFormat::D24_UNORM_S8_UINT;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return m_device->CreateGraphicsPipeline(pipelineDesc);
}

uint64 PipelineCache::AllocateObjectConstantSlot()
{
    if (m_objectConstantStride == 0)
        m_objectConstantStride = AlignConstantBufferSize(sizeof(ObjectConstants));

    if (m_objectConstantCursor >= RVX_MAX_DRAW_CONSTANTS_PER_FRAME)
    {
        RVX_VERIFY(false,
                   "PipelineCache: Object constant buffer exhausted for this frame (max {} draws). "
                   "Reusing the final slot to avoid wrapping over earlier draw constants.",
                   RVX_MAX_DRAW_CONSTANTS_PER_FRAME);
        const uint64 offset = (RVX_MAX_DRAW_CONSTANTS_PER_FRAME - 1) * m_objectConstantStride;
        m_currentObjectConstantOffset = offset;
        return offset;
    }

    const uint64 offset = m_objectConstantCursor * m_objectConstantStride;
    ++m_objectConstantCursor;
    m_currentObjectConstantOffset = offset;
    return offset;
}

void PipelineCache::UpdateViewConstants(const ViewData& view)
{
    if (!m_viewConstantBuffer)
        return;

    ViewConstants constants;
    constants.viewProjection = view.viewProjectionMatrix;

    if (m_device && m_device->GetBackendType() == RHIBackendType::Vulkan)
    {
        constants.viewProjection[1] = -constants.viewProjection[1];
    }

    constants.cameraPosition = view.cameraPosition;
    constants.time = view.time;
    constants.lightDirection = Vec3(0.5f, -0.8f, 0.3f);
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
    constants.world = worldMatrix;

    const uint64 offset = AllocateObjectConstantSlot();
    void* mapped = m_objectConstantBuffer->Map();
    if (mapped)
    {
        std::memcpy(static_cast<uint8*>(mapped) + offset, &constants, sizeof(ObjectConstants));
        m_objectConstantBuffer->Unmap();
    }
}

} // namespace RVX
