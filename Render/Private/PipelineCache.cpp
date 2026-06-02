/**
 * @file PipelineCache.cpp
 * @brief PipelineCache implementation
 */

#include "Render/PipelineCache.h"
#include "Core/Log.h"
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
    constexpr uint64 RVX_PIPELINE_HASH_OFFSET_BASIS = 0xcbf29ce484222325ull;
    constexpr uint64 RVX_PIPELINE_HASH_PRIME = 0x100000001b3ull;

    uint64 AlignConstantBufferSize(uint64 size)
    {
        return (size + RVX_CONSTANT_BUFFER_ALIGNMENT - 1) & ~(RVX_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    void HashBytes(uint64& hash, const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= RVX_PIPELINE_HASH_PRIME;
        }
    }

    template<typename T>
    void HashValue(uint64& hash, const T& value)
    {
        HashBytes(hash, &value, sizeof(T));
    }

    void HashString(uint64& hash, const char* value)
    {
        if (!value)
        {
            uint32 zeroLength = 0;
            HashValue(hash, zeroLength);
            return;
        }

        uint32 length = static_cast<uint32>(std::strlen(value));
        HashValue(hash, length);
        HashBytes(hash, value, length);
    }

    void HashFloat(uint64& hash, float value)
    {
        if (value == 0.0f)
        {
            value = 0.0f;
        }

        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        HashValue(hash, bits);
    }

    bool IsRequiredDefaultLitBinding(uint32 set, uint32 binding)
    {
        if ((set == 0 || set == 1) && binding == 0)
        {
            return true;
        }

        return set == 2 && binding <= 6;
    }

    bool BindingTypeMatches(RHIBindingType actual, RHIBindingType expected)
    {
        if (actual == expected)
        {
            return true;
        }

        return expected == RHIBindingType::DynamicUniformBuffer &&
               actual == RHIBindingType::UniformBuffer;
    }
} // namespace

PipelineCache::PipelineCache() = default;

PipelineCache::~PipelineCache()
{
    Shutdown();
}

void PipelineCache::SetConfig(const PipelineCacheConfig& config)
{
    if (m_initialized)
    {
        SetLastError("Cannot change PipelineCache config after initialization");
        return;
    }

    m_config = config;
    m_renderTargetFormat = config.renderTargetFormat;
}

uint64 PipelineCache::GetPipelineStateHashForVariant(MaterialPipelineVariant variant) const
{
    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
            return m_stats.maskedPipelineHash;
        case MaterialPipelineVariant::Transparent:
            return m_stats.transparentPipelineHash;
        case MaterialPipelineVariant::Opaque:
        default:
            return m_stats.opaquePipelineHash;
    }
}

void PipelineCache::SetLastError(std::string message)
{
    m_lastError = std::move(message);
    if (!m_lastError.empty())
    {
        RVX_CORE_ERROR("PipelineCache: {}", m_lastError);
    }
}

bool PipelineCache::Initialize(IRHIDevice* device, const std::string& shaderDir)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("PipelineCache already initialized");
        return true;
    }

    m_lastError.clear();
    m_stats = {};
    m_pipelineCache.clear();

    if (!device)
    {
        SetLastError("Invalid device");
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
        if (m_lastError.empty())
        {
            SetLastError("Failed to compile shaders");
        }
        return false;
    }

    if (!CreatePipelineLayout())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create pipeline layout");
        }
        return false;
    }

    if (!CreateObjectConstantBuffer())
    {
        SetLastError("Failed to create object constant buffer");
        return false;
    }

    if (!CreateViewConstantBuffer())
    {
        SetLastError("Failed to create view constant buffer");
        return false;
    }

    m_objectDescriptorSet = CreateObjectDescriptorSet();
    if (!m_objectDescriptorSet)
    {
        SetLastError("Failed to create object descriptor set");
        return false;
    }

    if (!CreatePipeline())
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create graphics pipeline");
        }
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
    m_pipelineCache.clear();
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
        SetLastError("Shader file not found: " + shaderPath);

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
        SetLastError("Failed to compile vertex shader: " + vsResult.compileResult.errorMessage);
        return false;
    }
    if (!vsResult.shader)
    {
        SetLastError("Failed to create vertex shader");
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
        SetLastError("Failed to compile pixel shader: " + psResult.compileResult.errorMessage);
        return false;
    }
    if (!psResult.shader)
    {
        SetLastError("Failed to create pixel shader");
        return false;
    }
    m_pixelShader = psResult.shader;
    m_psCompileResult = std::make_unique<ShaderCompileResult>(std::move(psResult.compileResult));

    RVX_CORE_DEBUG("PipelineCache: Compiled shaders successfully");
    return true;
}

bool PipelineCache::CreatePipelineLayout()
{
    std::vector<RHIDescriptorSetLayoutDesc> layoutDescs;
    if (!BuildReflectedDefaultLitLayouts(layoutDescs))
    {
        return false;
    }

    m_setLayouts.resize(3);
    m_setLayouts[0] = m_device->CreateDescriptorSetLayout(layoutDescs[0]);
    m_setLayouts[1] = m_device->CreateDescriptorSetLayout(layoutDescs[1]);
    m_setLayouts[2] = m_device->CreateDescriptorSetLayout(layoutDescs[2]);

    if (!m_setLayouts[0] || !m_setLayouts[1] || !m_setLayouts[2])
    {
        SetLastError("Failed to create descriptor set layouts");
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
        SetLastError("Failed to create pipeline layout");
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created pipeline layout with {} set layouts", m_setLayouts.size());
    return true;
}

bool PipelineCache::BuildReflectedDefaultLitLayouts(std::vector<RHIDescriptorSetLayoutDesc>& outLayouts)
{
    outLayouts.clear();

    if (!m_vsCompileResult || !m_psCompileResult)
    {
        SetLastError("Shader compile results are missing");
        return false;
    }

    if (m_vsCompileResult->reflection.resources.empty() &&
        m_psCompileResult->reflection.resources.empty())
    {
        SetLastError("Shader reflection metadata is missing");
        return false;
    }

    AutoPipelineLayout autoLayout = BuildAutoPipelineLayout({
        {m_vsCompileResult->reflection, RHIShaderStage::Vertex},
        {m_psCompileResult->reflection, RHIShaderStage::Pixel}
    });

    if (autoLayout.setLayouts.size() < 3)
    {
        SetLastError("DefaultLit reflection did not produce the required three descriptor sets");
        return false;
    }

    outLayouts = std::move(autoLayout.setLayouts);
    outLayouts.resize(3);
    outLayouts[0].debugName = "DefaultFrameSetLayout";
    outLayouts[1].debugName = "DefaultObjectSetLayout";
    outLayouts[2].debugName = "DefaultMaterialSetLayout";

    for (uint32 setIndex = 0; setIndex < static_cast<uint32>(outLayouts.size()); ++setIndex)
    {
        auto& entries = outLayouts[setIndex].entries;
        for (auto& entry : entries)
        {
            if ((setIndex == 1 || setIndex == 2) &&
                entry.binding == 0 &&
                entry.type == RHIBindingType::UniformBuffer)
            {
                entry.type = RHIBindingType::DynamicUniformBuffer;
                entry.isDynamic = true;
            }

            if (!IsRequiredDefaultLitBinding(setIndex, entry.binding))
            {
                RVX_CORE_WARN("PipelineCache: DefaultLit reflection includes optional set {} binding {}; leaving it in the layout",
                              setIndex,
                              entry.binding);
            }
        }

        std::sort(entries.begin(), entries.end(),
            [](const RHIBindingLayoutEntry& a, const RHIBindingLayoutEntry& b)
            {
                if (a.binding != b.binding)
                    return a.binding < b.binding;
                return static_cast<uint8>(a.type) < static_cast<uint8>(b.type);
            });
    }

    return ValidateDefaultLitLayouts(outLayouts);
}

bool PipelineCache::ValidateDefaultLitLayouts(const std::vector<RHIDescriptorSetLayoutDesc>& layouts)
{
    if (layouts.size() < 3)
    {
        SetLastError("DefaultLit layout validation requires three descriptor sets");
        return false;
    }

    auto requireBinding = [this, &layouts](uint32 set, uint32 binding, RHIBindingType expectedType) -> bool
    {
        const auto& entries = layouts[set].entries;
        auto it = std::find_if(entries.begin(), entries.end(),
            [binding](const RHIBindingLayoutEntry& entry)
            {
                return entry.binding == binding;
            });

        if (it == entries.end())
        {
            SetLastError("DefaultLit reflection missing required set " +
                         std::to_string(set) + " binding " + std::to_string(binding));
            return false;
        }

        if (!BindingTypeMatches(it->type, expectedType))
        {
            SetLastError("DefaultLit reflection incompatible type at set " +
                         std::to_string(set) + " binding " + std::to_string(binding));
            return false;
        }

        if (expectedType == RHIBindingType::DynamicUniformBuffer && !it->isDynamic)
        {
            SetLastError("DefaultLit dynamic binding is not marked dynamic at set " +
                         std::to_string(set) + " binding " + std::to_string(binding));
            return false;
        }

        return true;
    };

    if (!requireBinding(0, 0, RHIBindingType::UniformBuffer))
        return false;
    if (!requireBinding(1, 0, RHIBindingType::DynamicUniformBuffer))
        return false;
    if (!requireBinding(2, 0, RHIBindingType::DynamicUniformBuffer))
        return false;

    for (uint32 binding = 1; binding <= 5; ++binding)
    {
        if (!requireBinding(2, binding, RHIBindingType::SampledTexture))
            return false;
    }

    return requireBinding(2, 6, RHIBindingType::Sampler);
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
    m_opaquePipeline = GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Opaque,
                                                     "DefaultOpaquePipeline",
                                                     RHIDepthStencilState::Default(),
                                                     RHIBlendState::Default());
    if (!m_opaquePipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create opaque pipeline");
        }
        return false;
    }

    m_maskedPipeline = GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Masked,
                                                     "DefaultMaskedPipeline",
                                                     RHIDepthStencilState::Default(),
                                                     RHIBlendState::Default());
    if (!m_maskedPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create masked pipeline");
        }
        return false;
    }

    RHIBlendState transparentBlend = RHIBlendState::Default();
    transparentBlend.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();

    m_transparentPipeline = GetOrCreateDefaultLitPipeline(MaterialPipelineVariant::Transparent,
                                                          "DefaultTransparentPipeline",
                                                          RHIDepthStencilState::ReadOnly(),
                                                          transparentBlend);
    if (!m_transparentPipeline)
    {
        if (m_lastError.empty())
        {
            SetLastError("Failed to create transparent pipeline");
        }
        return false;
    }

    RVX_CORE_DEBUG("PipelineCache: Created material pipeline variants");
    return true;
}

RHIPipelineRef PipelineCache::GetOrCreateDefaultLitPipeline(MaterialPipelineVariant variant,
                                                            const char* debugName,
                                                            const RHIDepthStencilState& depthStencilState,
                                                            const RHIBlendState& blendState)
{
    RHIGraphicsPipelineDesc pipelineDesc = BuildDefaultLitPipelineDesc(debugName, depthStencilState, blendState);
    if (!pipelineDesc.vertexShader)
    {
        SetLastError("Cannot create pipeline without vertex shader");
        return {};
    }
    if (!pipelineDesc.pixelShader)
    {
        SetLastError("Cannot create pipeline without pixel shader");
        return {};
    }
    if (!pipelineDesc.pipelineLayout)
    {
        SetLastError("Cannot create pipeline without pipeline layout");
        return {};
    }
    if (pipelineDesc.numRenderTargets == 0 || pipelineDesc.renderTargetFormats[0] == RHIFormat::Unknown)
    {
        SetLastError("Cannot create pipeline with invalid render target format");
        return {};
    }
    if (pipelineDesc.depthStencilFormat == RHIFormat::Unknown)
    {
        SetLastError("Cannot create pipeline with invalid depth stencil format");
        return {};
    }

    const uint64 stateHash = ComputePipelineStateHash(pipelineDesc, variant);
    StoreVariantHash(variant, stateHash);
    m_stats.lastPipelineStateHash = stateHash;

    auto cached = m_pipelineCache.find(stateHash);
    if (cached != m_pipelineCache.end())
    {
        ++m_stats.pipelineCacheHitCount;
        return cached->second;
    }

    ++m_stats.pipelineCacheMissCount;
    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
    if (!pipeline)
    {
        SetLastError("Backend failed to create pipeline '" + std::string(debugName ? debugName : "") + "'");
        return {};
    }

    ++m_stats.pipelineCreateCount;
    m_pipelineCache[stateHash] = pipeline;
    return pipeline;
}

RHIGraphicsPipelineDesc PipelineCache::BuildDefaultLitPipelineDesc(const char* debugName,
                                                                   const RHIDepthStencilState& depthStencilState,
                                                                   const RHIBlendState& blendState) const
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
    pipelineDesc.depthStencilFormat = m_config.depthStencilFormat;
    pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;

    return pipelineDesc;
}

uint64 PipelineCache::StoreVariantHash(MaterialPipelineVariant variant, uint64 hash)
{
    switch (variant)
    {
        case MaterialPipelineVariant::Masked:
            m_stats.maskedPipelineHash = hash;
            break;
        case MaterialPipelineVariant::Transparent:
            m_stats.transparentPipelineHash = hash;
            break;
        case MaterialPipelineVariant::Opaque:
        default:
            m_stats.opaquePipelineHash = hash;
            break;
    }

    return hash;
}

uint64 PipelineCache::ComputeShaderHash(const ShaderCompileResult* result) const
{
    uint64 hash = RVX_PIPELINE_HASH_OFFSET_BASIS;
    if (!result)
    {
        return hash;
    }

    HashValue(hash, result->sourceInfo.combinedHash);
    HashValue(hash, result->permutationHash);

    const uint64 bytecodeSize = static_cast<uint64>(result->bytecode.size());
    HashValue(hash, bytecodeSize);
    if (!result->bytecode.empty())
    {
        HashBytes(hash, result->bytecode.data(), result->bytecode.size());
    }

    HashString(hash, result->glslSource.c_str());
    HashString(hash, result->mslSource.c_str());
    HashString(hash, result->mslEntryPoint.c_str());
    HashValue(hash, result->glslVersion);

    return hash;
}

uint64 PipelineCache::ComputePipelineStateHash(const RHIGraphicsPipelineDesc& desc,
                                               MaterialPipelineVariant variant) const
{
    uint64 hash = RVX_PIPELINE_HASH_OFFSET_BASIS;

    const RHIBackendType backend = m_device ? m_device->GetBackendType() : RHIBackendType::None;
    HashValue(hash, backend);
    HashValue(hash, variant);
    HashValue(hash, ComputeShaderHash(m_vsCompileResult.get()));
    HashValue(hash, ComputeShaderHash(m_psCompileResult.get()));

    HashValue(hash, desc.tessellationControlPoints);
    HashValue(hash, desc.primitiveTopology);
    HashValue(hash, desc.numRenderTargets);
    for (uint32 i = 0; i < RVX_MAX_RENDER_TARGETS; ++i)
    {
        HashValue(hash, desc.renderTargetFormats[i]);
    }
    HashValue(hash, desc.depthStencilFormat);
    HashValue(hash, desc.sampleCount);

    HashValue(hash, desc.rasterizerState.fillMode);
    HashValue(hash, desc.rasterizerState.cullMode);
    HashValue(hash, desc.rasterizerState.frontFace);
    HashFloat(hash, desc.rasterizerState.depthBias);
    HashFloat(hash, desc.rasterizerState.depthBiasClamp);
    HashFloat(hash, desc.rasterizerState.slopeScaledDepthBias);
    HashValue(hash, desc.rasterizerState.depthClipEnable);
    HashValue(hash, desc.rasterizerState.multisampleEnable);
    HashValue(hash, desc.rasterizerState.antialiasedLineEnable);
    HashValue(hash, desc.rasterizerState.conservativeRasterEnable);

    HashValue(hash, desc.depthStencilState.depthTestEnable);
    HashValue(hash, desc.depthStencilState.depthWriteEnable);
    HashValue(hash, desc.depthStencilState.depthCompareOp);
    HashValue(hash, desc.depthStencilState.stencilTestEnable);
    HashValue(hash, desc.depthStencilState.stencilReadMask);
    HashValue(hash, desc.depthStencilState.stencilWriteMask);
    HashValue(hash, desc.depthStencilState.frontFace.failOp);
    HashValue(hash, desc.depthStencilState.frontFace.depthFailOp);
    HashValue(hash, desc.depthStencilState.frontFace.passOp);
    HashValue(hash, desc.depthStencilState.frontFace.compareOp);
    HashValue(hash, desc.depthStencilState.backFace.failOp);
    HashValue(hash, desc.depthStencilState.backFace.depthFailOp);
    HashValue(hash, desc.depthStencilState.backFace.passOp);
    HashValue(hash, desc.depthStencilState.backFace.compareOp);

    HashValue(hash, desc.blendState.alphaToCoverageEnable);
    HashValue(hash, desc.blendState.independentBlendEnable);
    for (const auto& target : desc.blendState.renderTargets)
    {
        HashValue(hash, target.blendEnable);
        HashValue(hash, target.srcColorBlend);
        HashValue(hash, target.dstColorBlend);
        HashValue(hash, target.colorBlendOp);
        HashValue(hash, target.srcAlphaBlend);
        HashValue(hash, target.dstAlphaBlend);
        HashValue(hash, target.alphaBlendOp);
        HashValue(hash, target.colorWriteMask);
    }

    const uint32 inputElementCount = static_cast<uint32>(desc.inputLayout.elements.size());
    HashValue(hash, inputElementCount);
    for (const auto& element : desc.inputLayout.elements)
    {
        HashString(hash, element.semanticName);
        HashValue(hash, element.semanticIndex);
        HashValue(hash, element.format);
        HashValue(hash, element.inputSlot);
        HashValue(hash, element.alignedByteOffset);
        HashValue(hash, element.perInstance);
        HashValue(hash, element.instanceDataStepRate);
    }

    return hash;
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
