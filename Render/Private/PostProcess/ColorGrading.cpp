/**
 * @file ColorGrading.cpp
 * @brief Color grading implementation
 */

#include "Render/PostProcess/ColorGrading.h"
#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"

#include <cstring>

namespace RVX
{

namespace
{
    constexpr uint64 RVX_POST_PROCESS_CONSTANT_BUFFER_ALIGNMENT = 256;

    uint64 AlignPostProcessConstantBufferSize(uint64 size)
    {
        return (size + RVX_POST_PROCESS_CONSTANT_BUFFER_ALIGNMENT - 1) &
               ~(RVX_POST_PROCESS_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    struct ColorGradingGPUConstants
    {
        float textureSize[2] = {1.0f, 1.0f};
        float invTextureSize[2] = {1.0f, 1.0f};
        float whiteBalance[2] = {0.0f, 0.0f};
        float exposure = 0.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float hueShift = 0.0f;
        float paddingAfterHue[2] = {0.0f, 0.0f};
        float lift[4] = {1.0f, 1.0f, 1.0f, 0.0f};
        float gamma[4] = {1.0f, 1.0f, 1.0f, 0.0f};
        float gain[4] = {1.0f, 1.0f, 1.0f, 0.0f};
        float redChannel[3] = {1.0f, 0.0f, 0.0f};
        float padding0 = 0.0f;
        float greenChannel[3] = {0.0f, 1.0f, 0.0f};
        float padding1 = 0.0f;
        float blueChannel[3] = {0.0f, 0.0f, 1.0f};
        float padding2 = 0.0f;
        float shadowsTint[3] = {0.5f, 0.5f, 0.5f};
        float splitToningBalance = 0.0f;
        float highlightsTint[3] = {0.5f, 0.5f, 0.5f};
        float brightness = 0.0f;
    };
} // namespace

ColorGradingPass::ColorGradingPass()
{
    m_enabled = false;
    MarkUnsupported("ColorGrading requires fullscreen pipeline resources before it can execute");
}

bool ColorGradingPass::IsEnabled() const
{
    return m_config.mode == ColorGradingMode::LDR && IPostProcessPass::IsEnabled();
}

void ColorGradingPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableColorGrading;
    m_config.mode = ColorGradingMode::LDR;
    m_config.contrast = settings.contrast;
    m_config.saturation = settings.saturation;
    m_config.exposure = 0.0f;
    m_config.hueShift = 0.0f;
    m_config.temperature = 0.0f;
    m_config.tint = 0.0f;
    m_config.brightness = settings.brightness;
    RefreshSupportState();
}

void ColorGradingPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
{
    m_pipelineCache = pipelineCache;
    m_viewCache = viewCache;

    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (device != m_resourceDevice)
    {
        m_retainedDescriptorSets.clear();
        m_constantBuffer.Reset();
        m_sampler.Reset();
        m_resourceDevice = device;
    }

    RefreshSupportState();
}

void ColorGradingPass::SetConfig(const ColorGradingConfig& config)
{
    m_config = config;
    RefreshSupportState();
}

void ColorGradingPass::SetLUT(RHITexture* lut)
{
    m_lutTexture = lut;
    m_config.useLUT = lut != nullptr;
    RefreshSupportState();
}

void ColorGradingPass::SetUseLUT(bool enable)
{
    m_config.useLUT = enable;
    RefreshSupportState();
}

void ColorGradingPass::SetMode(ColorGradingMode mode)
{
    m_config.mode = mode;
    RefreshSupportState();
}

RHITextureRef ColorGradingPass::BakeToLUT(IRHIDevice* device, uint32 size)
{
    (void)device;
    (void)size;
    RVX_CORE_WARN("ColorGrading: LUT baking is not implemented");
    return nullptr;
}

void ColorGradingPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("ColorGrading: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    struct ColorGradingData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        ColorGradingConfig config;
    };

    graph.AddPass<ColorGradingData>(
        "ColorGrading",
        RenderGraphPassType::Graphics,
        [this, input, output](RenderGraphBuilder& builder, ColorGradingData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
            data.config = m_config;
        },
        [this, &graph](const ColorGradingData& data, RHICommandContext& ctx)
        {
            if (!m_pipelineCache || !m_viewCache)
            {
                RVX_CORE_WARN("ColorGrading: missing resources during execution");
                return;
            }

            RHIFormat outputFormat = RHIFormat::Unknown;
            if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(data.output))
            {
                outputFormat = outputDesc->format;
            }

            RHIPipeline* pipeline = m_pipelineCache->GetColorGradingPipeline(outputFormat);
            RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetPostProcessSetLayout();
            IRHIDevice* device = m_pipelineCache->GetDevice();
            if (!pipeline || !setLayout || !device)
            {
                RVX_CORE_WARN("ColorGrading: fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* inputTexture = graph.GetTexture(data.input);
            RHITexture* outputTexture = graph.GetTexture(data.output);
            if (!inputTexture || !outputTexture)
            {
                RVX_CORE_WARN("ColorGrading: input or output texture is unavailable");
                return;
            }

            RHITextureView* inputView = m_viewCache->GetDefaultSRV(inputTexture);
            RHITextureView* outputView = m_viewCache->GetDefaultRTV(outputTexture);
            if (!inputView || !outputView)
            {
                RVX_CORE_WARN("ColorGrading: failed to resolve input SRV or output RTV");
                return;
            }

            if (!EnsureRuntimeResources() ||
                !UpdateConstants(inputTexture->GetWidth(), inputTexture->GetHeight(), data.config))
            {
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "ColorGradingDescriptorSet";
            descriptorDesc.BindBuffer(0,
                                      m_constantBuffer.Get(),
                                      0,
                                      AlignPostProcessConstantBufferSize(sizeof(ColorGradingGPUConstants)));
            descriptorDesc.BindTexture(1, inputView);
            descriptorDesc.BindSampler(2, m_sampler.Get());

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("ColorGrading: failed to create descriptor set");
                return;
            }
            m_retainedDescriptorSets.push_back(descriptorSet);
            while (m_retainedDescriptorSets.size() > RVX_MAX_FRAME_COUNT + 1)
            {
                m_retainedDescriptorSets.pop_front();
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(outputView, RHILoadOp::DontCare, RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(0, 0, outputTexture->GetWidth(), outputTexture->GetHeight());

            ctx.BeginRenderPass(renderPassDesc);
            ctx.SetPipeline(pipeline);
            ctx.SetDescriptorSet(0, descriptorSet.Get());

            RHIViewport viewport;
            viewport.width = static_cast<float>(outputTexture->GetWidth());
            viewport.height = static_cast<float>(outputTexture->GetHeight());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            ctx.SetViewport(viewport);

            RHIRect scissor;
            scissor.width = outputTexture->GetWidth();
            scissor.height = outputTexture->GetHeight();
            ctx.SetScissor(scissor);

            ctx.Draw(3, 1, 0, 0);
            ctx.EndRenderPass();
        });
}

bool ColorGradingPass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        MarkUnsupported("ColorGrading requires an RHI device");
        return false;
    }

    if (!m_constantBuffer)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignPostProcessConstantBufferSize(sizeof(ColorGradingGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "ColorGradingConstants";

        m_constantBuffer = device->CreateBuffer(bufferDesc);
        if (!m_constantBuffer)
        {
            MarkUnsupported("ColorGrading constant buffer creation failed");
            return false;
        }
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "ColorGradingLinearClampSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            MarkUnsupported("ColorGrading sampler creation failed");
            return false;
        }
    }

    return true;
}

bool ColorGradingPass::UpdateConstants(uint32 width, uint32 height, const ColorGradingConfig& config)
{
    if (!m_constantBuffer)
    {
        return false;
    }

    ColorGradingGPUConstants constants;
    constants.textureSize[0] = static_cast<float>(width);
    constants.textureSize[1] = static_cast<float>(height);
    constants.invTextureSize[0] = width > 0 ? 1.0f / static_cast<float>(width) : 1.0f;
    constants.invTextureSize[1] = height > 0 ? 1.0f / static_cast<float>(height) : 1.0f;
    constants.whiteBalance[0] = config.temperature;
    constants.whiteBalance[1] = config.tint;
    constants.exposure = config.exposure;
    constants.contrast = config.contrast;
    constants.saturation = config.saturation;
    constants.hueShift = config.hueShift;
    constants.lift[0] = config.lift.x;
    constants.lift[1] = config.lift.y;
    constants.lift[2] = config.lift.z;
    constants.lift[3] = config.lift.w;
    constants.gamma[0] = config.gamma.x;
    constants.gamma[1] = config.gamma.y;
    constants.gamma[2] = config.gamma.z;
    constants.gamma[3] = config.gamma.w;
    constants.gain[0] = config.gain.x;
    constants.gain[1] = config.gain.y;
    constants.gain[2] = config.gain.z;
    constants.gain[3] = config.gain.w;
    constants.redChannel[0] = config.redChannel.x;
    constants.redChannel[1] = config.redChannel.y;
    constants.redChannel[2] = config.redChannel.z;
    constants.greenChannel[0] = config.greenChannel.x;
    constants.greenChannel[1] = config.greenChannel.y;
    constants.greenChannel[2] = config.greenChannel.z;
    constants.blueChannel[0] = config.blueChannel.x;
    constants.blueChannel[1] = config.blueChannel.y;
    constants.blueChannel[2] = config.blueChannel.z;
    constants.shadowsTint[0] = config.shadowsTint.x;
    constants.shadowsTint[1] = config.shadowsTint.y;
    constants.shadowsTint[2] = config.shadowsTint.z;
    constants.splitToningBalance = config.splitToningBalance;
    constants.highlightsTint[0] = config.highlightsTint.x;
    constants.highlightsTint[1] = config.highlightsTint.y;
    constants.highlightsTint[2] = config.highlightsTint.z;
    constants.brightness = config.brightness;

    void* mapped = m_constantBuffer->Map();
    if (!mapped)
    {
        RVX_CORE_WARN("ColorGrading: failed to map constants");
        return false;
    }

    std::memcpy(mapped, &constants, sizeof(constants));
    m_constantBuffer->Unmap();
    return true;
}

void ColorGradingPass::RefreshSupportState()
{
    if (m_config.mode == ColorGradingMode::None)
    {
        m_supported = true;
        m_unsupportedReason.clear();
        return;
    }

    if (m_config.mode == ColorGradingMode::HDR)
    {
        MarkUnsupported("ColorGrading HDR mode is not implemented");
        return;
    }

    if (m_config.useLUT || m_lutTexture)
    {
        MarkUnsupported("ColorGrading LUT path is not implemented");
        return;
    }

    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        MarkUnsupported("ColorGrading requires an initialized PipelineCache");
        return;
    }

    if (!m_viewCache || !m_viewCache->IsInitialized())
    {
        MarkUnsupported("ColorGrading requires an initialized ResourceViewCache");
        return;
    }

    if (!m_pipelineCache->GetColorGradingPipeline() ||
        !m_pipelineCache->GetPostProcessLayout() ||
        !m_pipelineCache->GetPostProcessSetLayout())
    {
        MarkUnsupported("ColorGrading fullscreen pipeline resources are not available");
        return;
    }

    if (!EnsureRuntimeResources())
    {
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

} // namespace RVX
