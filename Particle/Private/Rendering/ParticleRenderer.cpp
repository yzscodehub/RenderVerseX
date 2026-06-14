#include "Particle/Rendering/ParticleRenderer.h"
#include "Core/Log.h"
#include "Particle/GPU/IParticleSimulator.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/Rendering/TrailRenderer.h"
#include "Render/Renderer/ViewData.h"
#include "ShaderCompiler/ShaderCompiler.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace RVX::Particle
{
namespace
{
    std::filesystem::path FindParticleShaderDirectory()
    {
        std::filesystem::path cursor = std::filesystem::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            const std::filesystem::path sourceCandidate = cursor / "Particle" / "Shaders";
            if (std::filesystem::exists(sourceCandidate / "ParticleBillboard.hlsl"))
            {
                return sourceCandidate;
            }

            const std::filesystem::path buildCandidate = cursor / "Shaders" / "Particle";
            if (std::filesystem::exists(buildCandidate / "ParticleBillboard.hlsl"))
            {
                return buildCandidate;
            }

            if (!cursor.has_parent_path() || cursor == cursor.parent_path())
                break;

            cursor = cursor.parent_path();
        }

        return {};
    }

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    RHIBlendState BuildParticleBlendState(ParticleBlendMode blend)
    {
        RHIBlendState state = RHIBlendState::Default();
        switch (blend)
        {
        case ParticleBlendMode::Additive:
            state.renderTargets[0] = RHIRenderTargetBlendState::Additive();
            break;
        case ParticleBlendMode::AlphaBlend:
            state.renderTargets[0] = RHIRenderTargetBlendState::AlphaBlend();
            break;
        default:
            break;
        }
        return state;
    }

    RHIDepthStencilState BuildParticleDepthState(bool reverseZ, RHIFormat depthFormat)
    {
        if (depthFormat == RHIFormat::Unknown)
        {
            return RHIDepthStencilState::Disabled();
        }

        RHIDepthStencilState state = RHIDepthStencilState::ReadOnly();
        state.depthCompareOp = reverseZ ? RHICompareOp::GreaterEqual : RHICompareOp::LessEqual;
        return state;
    }

} // namespace

ParticleRenderer::~ParticleRenderer()
{
    Shutdown();
}

void ParticleRenderer::Initialize(IRHIDevice* device)
{
    ParticleRendererConfig config;
    Initialize(device, config);
}

void ParticleRenderer::Initialize(IRHIDevice* device, const ParticleRendererConfig& config)
{
    if (m_device)
        Shutdown();

    if (!device)
    {
        RVX_CORE_ERROR("ParticleRenderer: Cannot initialize without an RHI device");
        SetUnsupported("No RHI device");
        return;
    }

    m_device = device;
    m_config = config;
    m_renderingSupported = false;
    m_unsupportedReason = "Particle render pipelines are not initialized";
    if (m_config.shaderDirectory.empty())
    {
        const std::filesystem::path shaderDirectory = FindParticleShaderDirectory();
        if (!shaderDirectory.empty())
        {
            m_config.shaderDirectory = shaderDirectory.string();
        }
    }
    if (!ValidateConfig())
    {
        return;
    }

    CreateQuadBuffers();
    if (!m_quadVertexBuffer || !m_quadIndexBuffer)
    {
        SetUnsupported("Particle quad buffers could not be created");
        return;
    }

    // Render constants buffer
    RHIBufferDesc constDesc;
    constDesc.size = sizeof(RenderGPUData);
    constDesc.usage = RHIBufferUsage::Constant;
    constDesc.memoryType = RHIMemoryType::Upload;
    constDesc.debugName = "ParticleRenderConstants";
    m_renderConstantsBuffer = m_device->CreateBuffer(constDesc);
    if (!m_renderConstantsBuffer)
    {
        SetUnsupported("Particle render constants buffer could not be created");
        return;
    }

    // Initialize trail renderer
    m_trailRenderer = std::make_unique<TrailRenderer>();
    m_trailRenderer->Initialize(device, 100000);  // 100k trail vertices

    if (!CreateSharedResources())
    {
        return;
    }

    if (!CreatePipelineIfNeeded(ParticleRenderMode::Billboard, ParticleBlendMode::AlphaBlend, ParticleDepthMode::FixedFunction) ||
        !CreatePipelineIfNeeded(ParticleRenderMode::Billboard, ParticleBlendMode::Additive, ParticleDepthMode::FixedFunction) ||
        !CreatePipelineIfNeeded(ParticleRenderMode::Billboard, ParticleBlendMode::AlphaBlend, ParticleDepthMode::ShaderDepth) ||
        !CreatePipelineIfNeeded(ParticleRenderMode::Billboard, ParticleBlendMode::Additive, ParticleDepthMode::ShaderDepth))
    {
        SetUnsupported("Particle billboard pipelines could not be created");
        return;
    }

    m_renderingSupported = true;
    m_unsupportedReason.clear();
    RVX_CORE_INFO("ParticleRenderer: Initialized");
}

void ParticleRenderer::Shutdown()
{
    m_pipelineCache.clear();
    m_vertexShader.Reset();
    m_pixelShader.Reset();
    m_descriptorSetLayout.Reset();
    m_pipelineLayout.Reset();
    m_quadVertexBuffer.Reset();
    m_quadIndexBuffer.Reset();
    m_renderConstantsBuffer.Reset();
    m_fallbackTextureView.Reset();
    m_fallbackTexture.Reset();
    m_fallbackDepthTextureView.Reset();
    m_fallbackDepthTexture.Reset();
    m_sampler.Reset();
    m_depthSampler.Reset();
    m_retainedDescriptorSets.clear();
    m_lastDrawStats = {};
    m_trailRenderer.reset();
    m_device = nullptr;
    m_renderingSupported = false;
    m_unsupportedReason = "Particle renderer is shut down";
}

void ParticleRenderer::CreateQuadBuffers()
{
    // Billboard quad vertices (-1 to 1, with UVs)
    struct QuadVertex
    {
        Vec3 position;
        Vec2 uv;
    };

    QuadVertex vertices[4] = {
        { Vec3(-1.0f, -1.0f, 0.0f), Vec2(0.0f, 1.0f) },
        { Vec3( 1.0f, -1.0f, 0.0f), Vec2(1.0f, 1.0f) },
        { Vec3( 1.0f,  1.0f, 0.0f), Vec2(1.0f, 0.0f) },
        { Vec3(-1.0f,  1.0f, 0.0f), Vec2(0.0f, 0.0f) }
    };

    uint16 indices[6] = { 0, 1, 2, 0, 2, 3 };

    // Create vertex buffer
    RHIBufferDesc vbDesc;
    vbDesc.size = sizeof(vertices);
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.memoryType = RHIMemoryType::Upload;
    vbDesc.debugName = "ParticleQuadVB";
    m_quadVertexBuffer = m_device->CreateBuffer(vbDesc);
    if (m_quadVertexBuffer)
    {
        m_quadVertexBuffer->Upload(vertices, 4);
    }
    else
    {
        RVX_CORE_WARN("ParticleRenderer: Failed to create quad vertex buffer");
    }

    // Create index buffer
    RHIBufferDesc ibDesc;
    ibDesc.size = sizeof(indices);
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.memoryType = RHIMemoryType::Upload;
    ibDesc.debugName = "ParticleQuadIB";
    m_quadIndexBuffer = m_device->CreateBuffer(ibDesc);
    if (m_quadIndexBuffer)
    {
        m_quadIndexBuffer->Upload(indices, 6);
    }
    else
    {
        RVX_CORE_WARN("ParticleRenderer: Failed to create quad index buffer");
    }
}

bool ParticleRenderer::ValidateConfig()
{
    if (m_config.colorTargetFormat == RHIFormat::Unknown || IsDepthFormat(m_config.colorTargetFormat))
    {
        SetUnsupported("Particle renderer requires a valid color render target format");
        return false;
    }

    if (m_config.depthStencilFormat != RHIFormat::Unknown && !IsDepthFormat(m_config.depthStencilFormat))
    {
        SetUnsupported("Particle renderer depthStencilFormat must be Unknown or a depth format");
        return false;
    }

    switch (m_config.sampleCount)
    {
    case RHISampleCount::Count1:
    case RHISampleCount::Count2:
    case RHISampleCount::Count4:
    case RHISampleCount::Count8:
    case RHISampleCount::Count16:
        return true;
    default:
        SetUnsupported("Particle renderer sampleCount is invalid");
        return false;
    }
}

bool ParticleRenderer::DrawParticles(RHICommandContext& ctx,
                                     ParticleSystemInstance* instance,
                                     const ViewData& view,
                                     RHITextureView* sceneDepthView,
                                     ParticleDepthMode depthMode,
                                     bool allowSoftParticles)
{
    m_lastDrawStats = {};
    if (!instance || !instance->HasSystem())
        return false;

    if (!IsRenderingSupported())
    {
        RVX_CORE_WARN("ParticleRenderer: draw skipped: {}", GetUnsupportedReason());
        return false;
    }

    uint32 aliveCount = instance->GetAliveCount();
    if (aliveCount == 0)
        return false;

    auto* system = instance->GetSystem().get();
    auto* simulator = instance->GetSimulator();
    if (!simulator)
    {
        RVX_CORE_WARN("ParticleRenderer: draw skipped: particle simulator is unavailable");
        return false;
    }

    if (!m_renderConstantsBuffer || !m_quadVertexBuffer || !m_quadIndexBuffer)
    {
        RVX_CORE_WARN("ParticleRenderer: draw skipped: render buffers are unavailable");
        return false;
    }

    if (system->renderMode != ParticleRenderMode::Billboard)
    {
        RVX_CORE_WARN("ParticleRenderer: draw skipped: render mode {} is unsupported in RQ32",
                      static_cast<int>(system->renderMode));
        return false;
    }

    if (system->blendMode != ParticleBlendMode::AlphaBlend &&
        system->blendMode != ParticleBlendMode::Additive)
    {
        RVX_CORE_WARN("ParticleRenderer: draw skipped: blend mode {} is unsupported in RQ32",
                      static_cast<int>(system->blendMode));
        return false;
    }

    if (depthMode == ParticleDepthMode::ShaderDepth && !sceneDepthView)
    {
        depthMode = ParticleDepthMode::None;
    }

    RHITextureView* shaderDepthView = depthMode == ParticleDepthMode::ShaderDepth ? sceneDepthView : nullptr;
    SoftParticleConfig softConfig =
        ResolveSoftParticleConfig(*instance, shaderDepthView, depthMode, allowSoftParticles);
    UploadRenderConstants(view, softConfig, m_lastDrawStats.sceneDepthTestEnabled);

    RHIDescriptorSetRef descriptorSet = CreateParticleDescriptorSet(instance, shaderDepthView);
    if (!descriptorSet)
    {
        RVX_CORE_WARN("ParticleRenderer: draw skipped: descriptor set is unavailable");
        return false;
    }

    // Get appropriate pipeline
    RHIPipeline* pipeline = GetBillboardPipeline(system->blendMode, depthMode);

    if (!pipeline)
    {
        RVX_CORE_WARN("ParticleRenderer: draw skipped: pipeline is unavailable");
        return false;
    }

    ctx.SetDescriptorSet(0, descriptorSet.Get());
    ctx.SetPipeline(pipeline);

    // Bind quad geometry
    ctx.SetVertexBuffer(0, m_quadVertexBuffer.Get());
    ctx.SetIndexBuffer(m_quadIndexBuffer.Get(), GetQuadIndexFormat(), 0);

    // Draw instanced
    ctx.DrawIndexed(6, aliveCount, 0, 0, 0);
    m_retainedDescriptorSets.push_back(descriptorSet);
    if (m_retainedDescriptorSets.size() > 256)
    {
        m_retainedDescriptorSets.pop_front();
    }
    return true;
}

bool ParticleRenderer::DrawParticlesIndirect(RHICommandContext& ctx,
                                     ParticleSystemInstance* instance,
                                     const ViewData& view,
                                     RHITextureView* sceneDepthView,
                                     ParticleDepthMode depthMode,
                                     bool allowSoftParticles)
{
    m_lastDrawStats = {};
    if (!instance || !instance->HasSystem())
        return false;

    if (!IsRenderingSupported())
    {
        RVX_CORE_WARN("ParticleRenderer: indirect draw skipped: {}", GetUnsupportedReason());
        return false;
    }

    if (instance->GetAliveCount() == 0)
        return false;

    auto* system = instance->GetSystem().get();
    auto* simulator = instance->GetSimulator();
    if (!simulator)
    {
        RVX_CORE_WARN("ParticleRenderer: indirect draw skipped: particle simulator is unavailable");
        return false;
    }

    if (!m_renderConstantsBuffer || !m_quadVertexBuffer || !m_quadIndexBuffer)
    {
        RVX_CORE_WARN("ParticleRenderer: indirect draw skipped: render buffers are unavailable");
        return false;
    }

    if (system->renderMode != ParticleRenderMode::Billboard)
    {
        RVX_CORE_WARN("ParticleRenderer: indirect draw skipped: render mode {} is unsupported in RQ32",
                      static_cast<int>(system->renderMode));
        return false;
    }

    if (system->blendMode != ParticleBlendMode::AlphaBlend &&
        system->blendMode != ParticleBlendMode::Additive)
    {
        RVX_CORE_WARN("ParticleRenderer: indirect draw skipped: blend mode {} is unsupported in RQ32",
                      static_cast<int>(system->blendMode));
        return false;
    }

    if (depthMode == ParticleDepthMode::ShaderDepth && !sceneDepthView)
    {
        depthMode = ParticleDepthMode::None;
    }

    RHITextureView* shaderDepthView = depthMode == ParticleDepthMode::ShaderDepth ? sceneDepthView : nullptr;
    SoftParticleConfig softConfig =
        ResolveSoftParticleConfig(*instance, shaderDepthView, depthMode, allowSoftParticles);
    UploadRenderConstants(view, softConfig, m_lastDrawStats.sceneDepthTestEnabled);

    RHIDescriptorSetRef descriptorSet = CreateParticleDescriptorSet(instance, shaderDepthView);
    if (!descriptorSet)
    {
        RVX_CORE_WARN("ParticleRenderer: indirect draw skipped: descriptor set is unavailable");
        return false;
    }

    // Get pipeline (same as DrawParticles)
    RHIPipeline* pipeline = GetBillboardPipeline(system->blendMode, depthMode);
    if (!pipeline)
    {
        RVX_CORE_WARN("ParticleRenderer: indirect draw skipped: pipeline is unavailable");
        return false;
    }

    RHIBuffer* indirectDrawBuffer = simulator->GetIndirectDrawBuffer();
    if (!indirectDrawBuffer)
    {
        RVX_CORE_WARN("ParticleRenderer: indirect draw skipped: indirect draw buffer is unavailable");
        return false;
    }

    ctx.SetDescriptorSet(0, descriptorSet.Get());
    ctx.SetPipeline(pipeline);
    ctx.SetVertexBuffer(0, m_quadVertexBuffer.Get());
    ctx.SetIndexBuffer(m_quadIndexBuffer.Get(), GetQuadIndexFormat(), 0);

    // Indirect draw (1 draw call, stride = 0 for single draw)
    ctx.DrawIndexedIndirect(indirectDrawBuffer, 0, 1, 0);
    m_retainedDescriptorSets.push_back(descriptorSet);
    if (m_retainedDescriptorSets.size() > 256)
    {
        m_retainedDescriptorSets.pop_front();
    }
    return true;
}

void ParticleRenderer::UploadRenderConstants(const ViewData& view,
                                             const SoftParticleConfig& softConfig,
                                             bool sceneDepthTestEnabled)
{
    RenderGPUData data;
    data.viewMatrix = view.viewMatrix;
    data.projMatrix = view.projectionMatrix;
    data.viewProjMatrix = view.viewProjectionMatrix;
    data.cameraPosition = Vec4(view.cameraPosition, 1.0f);
    data.cameraRight = Vec4(GetRightFromMatrix(view.inverseViewMatrix), 0.0f);
    data.cameraUp = Vec4(GetUpFromMatrix(view.inverseViewMatrix), 0.0f);
    data.cameraForward = Vec4(view.cameraForward, 0.0f);
    data.screenSize = Vec2(static_cast<float>(std::max(1u, view.viewportWidth)),
                           static_cast<float>(std::max(1u, view.viewportHeight)));
    data.invScreenSize = Vec2(1.0f / data.screenSize.x, 1.0f / data.screenSize.y);
    data.softParticleFadeDistance = softConfig.fadeDistance;
    data.softParticleContrast = softConfig.contrastPower;
    data.softParticleEnabled = softConfig.enabled ? 1 : 0;
    data.sceneDepthTestEnabled = sceneDepthTestEnabled ? 1 : 0;
    data.nearPlane = view.nearPlane;
    data.farPlane = view.farPlane;
    data.reverseZ = m_config.reverseZ ? 1 : 0;
    data.pad = 0;

    m_renderConstantsBuffer->Upload(&data, 1);
}

SoftParticleConfig ParticleRenderer::ResolveSoftParticleConfig(const ParticleSystemInstance& instance,
                                                               RHITextureView* sceneDepthView,
                                                               ParticleDepthMode depthMode,
                                                               bool allowSoftParticles)
{
    SoftParticleConfig softConfig;
    const auto system = instance.GetSystem();
    if (system)
    {
        softConfig = system->softParticleConfig;
    }

    const bool usingRealSceneDepth =
        depthMode == ParticleDepthMode::ShaderDepth && sceneDepthView != nullptr;
    m_lastDrawStats.depthMode = usingRealSceneDepth ? ParticleDepthMode::ShaderDepth : depthMode;
    m_lastDrawStats.usedRealSceneDepth = usingRealSceneDepth;
    m_lastDrawStats.sceneDepthTestEnabled = usingRealSceneDepth;

    if (!usingRealSceneDepth)
    {
        softConfig.enabled = false;
        m_lastDrawStats.softParticlesEnabled = false;
        m_lastDrawStats.softParticleFallbackReason =
            "Scene depth SRV unavailable; soft particles disabled";
        return softConfig;
    }

    if (!allowSoftParticles)
    {
        softConfig.enabled = false;
        m_lastDrawStats.softParticlesEnabled = false;
        m_lastDrawStats.softParticleFallbackReason =
            "Soft particles disabled by ParticlePass configuration";
        return softConfig;
    }

    if (!softConfig.enabled)
    {
        m_lastDrawStats.softParticlesEnabled = false;
        m_lastDrawStats.softParticleFallbackReason =
            "Soft particles disabled by ParticleSystem configuration";
        return softConfig;
    }

    m_lastDrawStats.softParticlesEnabled = true;
    m_lastDrawStats.softParticleFallbackReason.clear();
    return softConfig;
}

bool ParticleRenderer::CreateSharedResources()
{
    if (!CreateDescriptorLayout())
    {
        return false;
    }
    if (!CreateFallbackTextureResources())
    {
        return false;
    }
    if (!CreateShaders())
    {
        return false;
    }
    return true;
}

bool ParticleRenderer::CreateShaders()
{
    const auto createShaderFromBytecode = [this](RHIShaderStage stage,
                                                 const std::vector<uint8>& bytecode,
                                                 const char* entryPoint,
                                                 const char* debugName) -> RHIShaderRef
    {
        if (bytecode.empty())
            return {};

        RHIShaderDesc desc;
        desc.stage = stage;
        desc.bytecode = bytecode.data();
        desc.bytecodeSize = bytecode.size();
        desc.entryPoint = entryPoint;
        desc.debugName = debugName;
        return m_device->CreateShader(desc);
    };

    m_vertexShader = createShaderFromBytecode(RHIShaderStage::Vertex,
                                              m_config.vertexShaderBytecode,
                                              "VSMain",
                                              "ParticleBillboardVS");
    m_pixelShader = createShaderFromBytecode(RHIShaderStage::Pixel,
                                             m_config.pixelShaderBytecode,
                                             "PSMain",
                                             "ParticleBillboardPS");
    if (m_vertexShader && m_pixelShader)
    {
        return true;
    }

    if (m_config.shaderDirectory.empty())
    {
        SetUnsupported("Particle shader directory is not configured");
        return false;
    }

    const std::filesystem::path shaderPath =
        std::filesystem::path(m_config.shaderDirectory) / "ParticleBillboard.hlsl";
    const std::string sourceCode = ReadTextFile(shaderPath);
    if (sourceCode.empty())
    {
        SetUnsupported("Particle billboard shader source is unavailable");
        return false;
    }

    std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
    if (!compiler)
    {
        SetUnsupported("Particle shader compiler is unavailable");
        return false;
    }

    const auto compileShader = [&](RHIShaderStage stage,
                                   const char* entryPoint,
                                   const char* debugName) -> RHIShaderRef
    {
        ShaderCompileOptions options;
        options.stage = stage;
        options.entryPoint = entryPoint;
        options.sourceCode = sourceCode.c_str();
        const std::string pathString = shaderPath.string();
        options.sourcePath = pathString.c_str();
        options.targetBackend = m_device->GetBackendType();
        options.enableDebugInfo = false;
        options.enableOptimization = true;

        ShaderCompileResult result = compiler->Compile(options);
        if (!result.success || result.bytecode.empty())
        {
            SetUnsupported("Particle shader compile failed: " + result.errorMessage);
            return {};
        }

        RHIShaderDesc desc;
        desc.stage = stage;
        desc.bytecode = result.bytecode.data();
        desc.bytecodeSize = result.bytecode.size();
        desc.entryPoint = entryPoint;
        desc.debugName = debugName;
        return m_device->CreateShader(desc);
    };

    m_vertexShader = compileShader(RHIShaderStage::Vertex, "VSMain", "ParticleBillboardVS");
    m_pixelShader = compileShader(RHIShaderStage::Pixel, "PSMain", "ParticleBillboardPS");
    if (!m_vertexShader || !m_pixelShader)
    {
        if (m_unsupportedReason.empty())
        {
            SetUnsupported("Particle shaders could not be created");
        }
        return false;
    }

    return true;
}

bool ParticleRenderer::CreateDescriptorLayout()
{
    RHIDescriptorSetLayoutDesc layoutDesc;
    layoutDesc.debugName = "ParticleBillboardSetLayout";
    layoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Vertex | RHIShaderStage::Pixel);
    layoutDesc.AddBinding(1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);
    layoutDesc.AddBinding(2, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Vertex);
    layoutDesc.AddBinding(3, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    layoutDesc.AddBinding(4, RHIBindingType::Sampler, RHIShaderStage::Pixel);
    layoutDesc.AddBinding(5, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
    layoutDesc.AddBinding(6, RHIBindingType::Sampler, RHIShaderStage::Pixel);

    m_descriptorSetLayout = m_device->CreateDescriptorSetLayout(layoutDesc);
    if (!m_descriptorSetLayout)
    {
        SetUnsupported("Particle descriptor set layout could not be created");
        return false;
    }

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.debugName = "ParticleBillboardPipelineLayout";
    pipelineLayoutDesc.setLayouts.push_back(m_descriptorSetLayout.Get());
    m_pipelineLayout = m_device->CreatePipelineLayout(pipelineLayoutDesc);
    if (!m_pipelineLayout)
    {
        SetUnsupported("Particle pipeline layout could not be created");
        return false;
    }

    return true;
}

bool ParticleRenderer::CreateFallbackTextureResources()
{
    RHITextureDesc textureDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::RGBA8_UNORM,
                                                          RHITextureUsage::ShaderResource);
    textureDesc.debugName = "ParticleFallbackWhiteTexture";
    m_fallbackTexture = m_device->CreateTexture(textureDesc);
    if (!m_fallbackTexture)
    {
        SetUnsupported("Particle fallback texture could not be created");
        return false;
    }

    RHITextureViewDesc viewDesc;
    viewDesc.format = m_fallbackTexture->GetFormat();
    viewDesc.dimension = m_fallbackTexture->GetDimension();
    viewDesc.type = RHITextureViewType::ShaderResource;
    viewDesc.debugName = "ParticleFallbackWhiteTextureSRV";
    m_fallbackTextureView = m_device->CreateTextureView(m_fallbackTexture.Get(), viewDesc);
    if (!m_fallbackTextureView)
    {
        SetUnsupported("Particle fallback texture view could not be created");
        return false;
    }

    RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
    samplerDesc.debugName = "ParticleLinearClampSampler";
    m_sampler = m_device->CreateSampler(samplerDesc);
    if (!m_sampler)
    {
        SetUnsupported("Particle sampler could not be created");
        return false;
    }

    RHITextureDesc depthDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::R32_FLOAT,
                                                        RHITextureUsage::ShaderResource);
    depthDesc.debugName = "ParticleFallbackDepthTexture";
    m_fallbackDepthTexture = m_device->CreateTexture(depthDesc);
    if (!m_fallbackDepthTexture)
    {
        SetUnsupported("Particle fallback depth texture could not be created");
        return false;
    }

    RHITextureViewDesc depthViewDesc;
    depthViewDesc.format = m_fallbackDepthTexture->GetFormat();
    depthViewDesc.dimension = m_fallbackDepthTexture->GetDimension();
    depthViewDesc.type = RHITextureViewType::ShaderResource;
    depthViewDesc.debugName = "ParticleFallbackDepthTextureSRV";
    m_fallbackDepthTextureView = m_device->CreateTextureView(m_fallbackDepthTexture.Get(), depthViewDesc);
    if (!m_fallbackDepthTextureView)
    {
        SetUnsupported("Particle fallback depth texture view could not be created");
        return false;
    }

    RHISamplerDesc depthSamplerDesc = RHISamplerDesc::PointClamp();
    depthSamplerDesc.debugName = "ParticleDepthPointClampSampler";
    m_depthSampler = m_device->CreateSampler(depthSamplerDesc);
    if (!m_depthSampler)
    {
        SetUnsupported("Particle depth sampler could not be created");
        return false;
    }

    return true;
}

RHIDescriptorSetRef ParticleRenderer::CreateParticleDescriptorSet(ParticleSystemInstance* instance,
                                                                  RHITextureView* sceneDepthView)
{
    if (!m_descriptorSetLayout || !m_renderConstantsBuffer || !m_fallbackTextureView || !m_sampler ||
        !m_fallbackDepthTextureView || !m_depthSampler || !instance)
        return {};

    IParticleSimulator* simulator = instance->GetSimulator();
    if (!simulator || !simulator->GetParticleBuffer() || !simulator->GetAliveIndexBuffer())
        return {};

    RHITextureView* depthView = sceneDepthView ? sceneDepthView : m_fallbackDepthTextureView.Get();

    RHIDescriptorSetDesc desc;
    desc.debugName = "ParticleBillboardDescriptorSet";
    desc.SetLayout(m_descriptorSetLayout.Get())
        .BindBuffer(0, m_renderConstantsBuffer.Get())
        .BindBuffer(1, simulator->GetParticleBuffer())
        .BindBuffer(2, simulator->GetAliveIndexBuffer())
        .BindTexture(3, m_fallbackTextureView.Get())
        .BindSampler(4, m_sampler.Get())
        .BindTexture(5, depthView)
        .BindSampler(6, m_depthSampler.Get());

    return m_device->CreateDescriptorSet(desc);
}

uint32 ParticleRenderer::MakePipelineKey(ParticleRenderMode mode,
                                         ParticleBlendMode blend,
                                         ParticleDepthMode depthMode)
{
    const uint32 depthKey = depthMode == ParticleDepthMode::FixedFunction ? 0u : 1u;
    return (static_cast<uint32>(mode) << 16) |
           (static_cast<uint32>(blend) << 8) |
           depthKey;
}

RHIPipeline* ParticleRenderer::GetBillboardPipeline(ParticleBlendMode blend, ParticleDepthMode depthMode)
{
    return CreatePipelineIfNeeded(ParticleRenderMode::Billboard, blend, depthMode);
}

RHIPipeline* ParticleRenderer::GetStretchedBillboardPipeline(ParticleBlendMode blend, ParticleDepthMode depthMode)
{
    return CreatePipelineIfNeeded(ParticleRenderMode::StretchedBillboard, blend, depthMode);
}

RHIPipeline* ParticleRenderer::GetMeshPipeline(ParticleBlendMode blend)
{
    return CreatePipelineIfNeeded(ParticleRenderMode::Mesh, blend, ParticleDepthMode::FixedFunction);
}

RHIPipeline* ParticleRenderer::GetTrailPipeline(ParticleBlendMode blend, ParticleDepthMode depthMode)
{
    return CreatePipelineIfNeeded(ParticleRenderMode::Trail, blend, depthMode);
}

RHIPipeline* ParticleRenderer::CreatePipelineIfNeeded(ParticleRenderMode mode,
                                                      ParticleBlendMode blend,
                                                      ParticleDepthMode depthMode)
{
    if (mode != ParticleRenderMode::Billboard)
    {
        RVX_CORE_WARN("ParticleRenderer: Unsupported pipeline request mode={}", static_cast<int>(mode));
        return nullptr;
    }

    if (blend != ParticleBlendMode::AlphaBlend && blend != ParticleBlendMode::Additive)
    {
        RVX_CORE_WARN("ParticleRenderer: Unsupported billboard blend mode={}", static_cast<int>(blend));
        return nullptr;
    }

    const bool fixedFunctionDepth = depthMode == ParticleDepthMode::FixedFunction;
    uint32 key = MakePipelineKey(mode, blend, depthMode);
    auto it = m_pipelineCache.find(key);
    if (it != m_pipelineCache.end())
        return it->second.Get();

    if (!m_vertexShader || !m_pixelShader || !m_pipelineLayout)
    {
        RVX_CORE_WARN("ParticleRenderer: Pipeline dependencies are unavailable for mode={}, blend={}",
                      static_cast<int>(mode), static_cast<int>(blend));
        return nullptr;
    }

    RHIGraphicsPipelineDesc desc;
    desc.vertexShader = m_vertexShader.Get();
    desc.pixelShader = m_pixelShader.Get();
    desc.pipelineLayout = m_pipelineLayout.Get();
    desc.rasterizerState = RHIRasterizerState::NoCull();
    desc.depthStencilState = fixedFunctionDepth
                                 ? BuildParticleDepthState(m_config.reverseZ, m_config.depthStencilFormat)
                                 : RHIDepthStencilState::Disabled();
    desc.blendState = BuildParticleBlendState(blend);
    desc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
    desc.numRenderTargets = 1;
    desc.renderTargetFormats[0] = m_config.colorTargetFormat;
    desc.depthStencilFormat = fixedFunctionDepth ? m_config.depthStencilFormat : RHIFormat::Unknown;
    desc.sampleCount = m_config.sampleCount;

    const std::string debugName = std::string("ParticleBillboard") +
                                  (blend == ParticleBlendMode::Additive ? "Additive" : "AlphaBlend") +
                                  (fixedFunctionDepth ? "FixedDepth" : "ShaderDepth") +
                                  "Pipeline";
    desc.debugName = debugName.c_str();

    RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(desc);
    if (!pipeline)
    {
        RVX_CORE_WARN("ParticleRenderer: Failed to create {}", debugName);
        return nullptr;
    }

    m_pipelineCache[key] = pipeline;
    return pipeline.Get();
}

const char* ParticleRenderer::GetShaderNameForMode(ParticleRenderMode mode) const
{
    switch (mode)
    {
        case ParticleRenderMode::Billboard:
            return "ParticleBillboard";
        case ParticleRenderMode::StretchedBillboard:
            return "ParticleStretchedBillboard";
        case ParticleRenderMode::Mesh:
            return "ParticleMesh";
        case ParticleRenderMode::Trail:
            return "ParticleTrail";
        default:
            return "ParticleBillboard";
    }
}

RHIBuffer* ParticleRenderer::GetQuadVertexBuffer()
{
    return m_quadVertexBuffer.Get();
}

RHIBuffer* ParticleRenderer::GetQuadIndexBuffer()
{
    return m_quadIndexBuffer.Get();
}

void ParticleRenderer::SetUnsupported(const std::string& reason)
{
    m_renderingSupported = false;
    m_unsupportedReason = reason;
    RVX_CORE_WARN("ParticleRenderer: {}", m_unsupportedReason);
}

} // namespace RVX::Particle
