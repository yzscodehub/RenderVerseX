/**
 * @file ToneMapping.cpp
 * @brief ToneMappingPass implementation
 */

#include "Render/PostProcess/ToneMapping.h"
#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"

#include <algorithm>
#include <cmath>
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

    struct ToneMappingGPUConstants
    {
        float gamma = 2.2f;
        float whitePoint = 11.2f;
        uint32 operatorType = 0;
        uint32 outputColorSpace = static_cast<uint32>(ToneMappingOutputColorSpace::SRGB);
        float textureSize[2] = {1.0f, 1.0f};
        float invTextureSize[2] = {1.0f, 1.0f};
        float padding[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    static_assert(sizeof(ToneMappingGPUConstants) == 48,
                  "ToneMappingGPUConstants must match ToneMapping.hlsl cbuffer packing");

    [[nodiscard]] RHIBufferRef CreateImmutableToneMappingConstants(
        IRHIDevice& device,
        const ToneMappingGPUConstants& constants)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignPostProcessConstantBufferSize(
            sizeof(ToneMappingGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "ToneMappingPassConstants";
        RHIBufferRef buffer = device.CreateBuffer(bufferDesc);
        if (!buffer)
        {
            return {};
        }
        void* const mapped = buffer->Map();
        if (mapped == nullptr)
        {
            return {};
        }
        std::memcpy(mapped, &constants, sizeof(constants));
        buffer->Unmap();
        return buffer;
    }
} // namespace

ToneMappingPass::ToneMappingPass()
{
    m_enabled = true;
    MarkUnsupported("ToneMapping shader and fullscreen pipeline are not implemented");
}

void ToneMappingPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableToneMapping;
    m_gamma = settings.gamma;
    m_operator = settings.toneMappingOperator;
    m_outputColorSpace = settings.toneMappingOutputColorSpace;
}

void ToneMappingPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
{
    m_pipelineCache = pipelineCache;
    m_viewCache = viewCache;

    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (device != m_resourceDevice)
    {
        m_sampler.Reset();
        m_resourceDevice = device;
    }

    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        MarkUnsupported("ToneMapping requires an initialized PipelineCache");
        return;
    }

    if (!m_viewCache || !m_viewCache->IsInitialized())
    {
        MarkUnsupported("ToneMapping requires an initialized ResourceViewCache");
        return;
    }

    if (!m_pipelineCache->GetToneMappingPipeline() ||
        !m_pipelineCache->GetExposurePostProcessLayout() ||
        !m_pipelineCache->GetExposurePostProcessSetLayout())
    {
        MarkUnsupported("ToneMapping fullscreen pipeline resources are not available");
        return;
    }

    if (!EnsureRuntimeResources())
    {
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

void ToneMappingPass::SetPixelProbeRequest(
    const RenderFrameCaptureRequest& request,
    uint64 frameSequence,
    uint64 requiredSceneRevision,
    uint64 runtimeSurfaceGeneration)
{
    m_pixelProbe = {};
    if (!request.pixelProbeEnabled)
    {
        return;
    }

    m_pixelProbe.armed = true;
    RenderFramePixelProbeResult& result = m_pixelProbe.result;
    result.requestId = request.requestId;
    result.frameSequence = frameSequence;
    result.requiredSceneRevision = requiredSceneRevision;
    result.runtimeSurfaceGeneration = runtimeSurfaceGeneration;
    result.x = request.pixelProbeX;
    result.y = request.pixelProbeY;
    result.preToneFormat = RHIFormat::RGBA16_FLOAT;

    if (request.kind != RenderFrameCaptureKind::Color || request.requestId == 0 ||
        request.width == 0 || request.height == 0)
    {
        result.code = RenderFramePixelProbeResultCode::InvalidRequest;
        result.message = "Pixel probe requires a complete color capture request";
    }
    else if (request.pixelProbeX >= request.width ||
             request.pixelProbeY >= request.height)
    {
        result.code = RenderFramePixelProbeResultCode::OutOfBounds;
        result.message = "Pixel probe coordinates are outside the capture extent";
    }
    else if (frameSequence == 0 || requiredSceneRevision == 0 ||
             runtimeSurfaceGeneration == 0)
    {
        result.code = RenderFramePixelProbeResultCode::ForeignFrame;
        result.message = "Pixel probe lacks an exact scene, runtime, or frame identity";
    }
}

bool ToneMappingPass::CompletePixelProbe(
    uint64 requestId,
    uint64 frameSequence,
    RenderFramePixelProbeResult& outResult)
{
    outResult = m_pixelProbe.result;
    if (!m_pixelProbe.armed)
    {
        return false;
    }
    if (requestId != m_pixelProbe.result.requestId ||
        frameSequence != m_pixelProbe.result.frameSequence)
    {
        outResult.code = RenderFramePixelProbeResultCode::ForeignFrame;
        outResult.message = "Pixel probe completion did not match its carrying frame";
        return true;
    }
    if (outResult.code != RenderFramePixelProbeResultCode::None)
    {
        m_pixelProbe = {};
        return true;
    }
    if (!m_pixelProbe.recorded || !m_pixelProbe.buffer)
    {
        outResult.code = RenderFramePixelProbeResultCode::NotRecorded;
        outResult.message = "ToneMapping input probe was not recorded for the carrying frame";
        m_pixelProbe = {};
        return true;
    }
    if (m_pixelProbe.buffer->GetSize() <
        sizeof(outResult.preToneRGBA16FloatBits))
    {
        outResult.code = RenderFramePixelProbeResultCode::NotRecorded;
        outResult.message = "ToneMapping input probe readback is smaller than RGBA16_FLOAT";
        m_pixelProbe = {};
        return true;
    }

    const void* mapped = m_pixelProbe.buffer->Map();
    if (mapped == nullptr)
    {
        outResult.code = RenderFramePixelProbeResultCode::MapFailed;
        outResult.message = "ToneMapping input probe readback mapping failed";
        m_pixelProbe = {};
        return true;
    }
    std::memcpy(outResult.preToneRGBA16FloatBits.data(),
                mapped,
                sizeof(outResult.preToneRGBA16FloatBits));
    m_pixelProbe.buffer->Unmap();
    outResult.code = RenderFramePixelProbeResultCode::Completed;
    outResult.message.clear();
    m_pixelProbe = {};
    return true;
}

void ToneMappingPass::AddToGraph(
    RenderGraph& graph,
    RGTextureHandle input,
    RGTextureHandle output)
{
    static_cast<void>(graph);
    static_cast<void>(input);
    static_cast<void>(output);
    m_lastExposureStateContentHash = 0;
    MarkUnsupported("ToneMapping requires the candidate exposure-state graph input");
}

void ToneMappingPass::AddToGraph(
    RenderGraph& graph,
    const PostProcessFrameInputs& frameInputs,
    RGTextureHandle output)
{
    if (!frameInputs.HasExposureState() ||
        frameInputs.exposureStateContentHash == 0)
    {
        m_lastExposureStateContentHash = 0;
        MarkUnsupported("ToneMapping requires a non-zero candidate exposure-state identity");
        return;
    }
    AddToGraphWithExposure(graph,
                           frameInputs.sceneColor,
                           frameInputs.exposureState,
                           frameInputs.exposureStateContentHash,
                           output);
}

void ToneMappingPass::AddToGraphWithExposure(RenderGraph& graph,
                                              RGTextureHandle input,
                                              RGBufferHandle exposureState,
                                              uint64 exposureStateContentHash,
                                              RGTextureHandle output)
{
    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("ToneMapping: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    if (m_pixelProbe.armed &&
        m_pixelProbe.result.code == RenderFramePixelProbeResultCode::None)
    {
        const RHITextureDesc* inputDesc = graph.GetTextureDesc(input);
        IRHIDevice* const device = m_pipelineCache
                                       ? m_pipelineCache->GetDevice()
                                       : nullptr;
        if (inputDesc == nullptr || inputDesc->format != RHIFormat::RGBA16_FLOAT)
        {
            m_pixelProbe.result.code =
                RenderFramePixelProbeResultCode::UnsupportedFormat;
            m_pixelProbe.result.message =
                "ToneMapping input probe requires an RGBA16_FLOAT source";
        }
        else if (m_pixelProbe.result.x >= inputDesc->width ||
                 m_pixelProbe.result.y >= inputDesc->height)
        {
            m_pixelProbe.result.code =
                RenderFramePixelProbeResultCode::OutOfBounds;
            m_pixelProbe.result.message =
                "Pixel probe coordinates are outside the ToneMapping input";
        }
        else if (device == nullptr)
        {
            m_pixelProbe.result.code =
                RenderFramePixelProbeResultCode::ResourceCreationFailed;
            m_pixelProbe.result.message =
                "ToneMapping input probe has no RHI device";
        }
        else
        {
            const uint32 alignment =
                device->GetBackendType() == RHIBackendType::DX12 ||
                        device->GetBackendType() == RHIBackendType::Metal
                    ? 256U
                    : 1U;
            m_pixelProbe.rowPitch =
                (static_cast<uint32>(sizeof(uint16) * 4U) + alignment - 1U) &
                ~(alignment - 1U);

            RHIBufferDesc bufferDesc;
            bufferDesc.size = m_pixelProbe.rowPitch;
            bufferDesc.usage = RHIBufferUsage::CopyDst;
            bufferDesc.memoryType = RHIMemoryType::Readback;
            bufferDesc.debugName = "ToneMappingPixelProbeReadback";
            m_pixelProbe.buffer = device->CreateBuffer(bufferDesc);
            if (!m_pixelProbe.buffer)
            {
                m_pixelProbe.result.code =
                    RenderFramePixelProbeResultCode::ResourceCreationFailed;
                m_pixelProbe.result.message =
                    "ToneMapping input probe readback allocation failed";
            }
            else
            {
                struct PixelProbeData
                {
                    RGTextureHandle input;
                    RGBufferHandle readback;
                    uint32 x = 0;
                    uint32 y = 0;
                    uint32 rowPitch = 0;
                };

                const uint32 sourceY =
                    device->GetBackendType() == RHIBackendType::OpenGL
                        ? inputDesc->height - 1U - m_pixelProbe.result.y
                        : m_pixelProbe.result.y;
                const RHIBufferRef readback = m_pixelProbe.buffer;
                const uint64 requestId = m_pixelProbe.result.requestId;
                const uint64 frameSequence = m_pixelProbe.result.frameSequence;
                graph.AddPass<PixelProbeData>(
                    "ToneMapping.PixelProbe",
                    RenderGraphPassType::Copy,
                    [input, readback, requestId, frameSequence, sourceY, this](
                        RenderGraphBuilder& builder,
                        PixelProbeData& data)
                    {
                        data.input = builder.Read(
                            input,
                            RHIResourceState::CopySource,
                            RHIShaderStage::None);
                        data.readback = builder.ImportBuffer(
                            readback,
                            MakeRHIBufferAccessSnapshot(
                                RHIResourceState::CopyDest,
                                RHIShaderStage::None,
                                GPUQueueDomain::Graphics,
                                RHIContentValidity::Invalid));
                        data.readback = builder.Write(
                            data.readback,
                            RHIResourceState::CopyDest);
                        data.x = m_pixelProbe.result.x;
                        data.y = sourceY;
                        data.rowPitch = m_pixelProbe.rowPitch;
                        if (m_pixelProbe.result.requestId != requestId ||
                            m_pixelProbe.result.frameSequence != frameSequence)
                        {
                            data.input = {};
                            data.readback = {};
                        }
                    },
                    [this, requestId, frameSequence](
                        const PixelProbeData& data,
                        RenderGraphPassContext& context)
                    {
                        if (!data.input.IsValid() || !data.readback.IsValid() ||
                            m_pixelProbe.result.requestId != requestId ||
                            m_pixelProbe.result.frameSequence != frameSequence)
                        {
                            return;
                        }
                        RHITexture* const source = context.GetTexture(data.input);
                        RHIBuffer* const readback = context.GetBuffer(data.readback);
                        if (source == nullptr || readback == nullptr)
                        {
                            return;
                        }
                        RHIBufferTextureCopyDesc copyDesc;
                        copyDesc.bufferRowPitch = data.rowPitch;
                        copyDesc.textureRegion = {
                            static_cast<int32>(data.x),
                            static_cast<int32>(data.y),
                            1,
                            1};
                        context.Commands().CopyTextureToBuffer(source, readback, copyDesc);
                        m_pixelProbe.recorded = true;
                    });
            }
        }
    }

    struct ToneMappingData
    {
        RGTextureHandle input;
        RGBufferHandle exposureState;
        RGTextureHandle output;
        RGTextureViewHandle inputView;
        RGTextureViewHandle outputView;
        RHIFormat outputFormat = RHIFormat::Unknown;
        ToneMappingOperator op;
        ToneMappingOutputColorSpace outputColorSpace;
        uint64 exposureStateContentHash = 0;
        float gamma;
        float whitePoint;
    };

    graph.AddPass<ToneMappingData>(
        "ToneMapping",
        RenderGraphPassType::Graphics,
        [this, input, exposureState, exposureStateContentHash, output](
            RenderGraphBuilder& builder, ToneMappingData& data)
        {
            data.input = input;
            data.exposureState = builder.Read(
                exposureState,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.output = output;
            const RHITextureDesc* inputDesc = builder.GetTextureDesc(input);
            const RHITextureDesc* outputDesc = builder.GetTextureDesc(output);
            if (inputDesc)
            {
                RHITextureViewDesc viewDesc;
                viewDesc.format = inputDesc->format;
                viewDesc.dimension = inputDesc->dimension;
                viewDesc.subresourceRange = RHISubresourceRange::All();
                if (IsDepthFormat(viewDesc.format))
                    viewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
                viewDesc.type = RHITextureViewType::ShaderResource;
                viewDesc.debugName = "ToneMappingInputSRV";
                data.inputView = builder.Read(
                    builder.CreateTextureView(input, viewDesc),
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
            }
            if (outputDesc)
            {
                data.outputFormat = outputDesc->format;
                RHITextureViewDesc viewDesc;
                viewDesc.format = outputDesc->format;
                viewDesc.dimension = outputDesc->dimension;
                viewDesc.subresourceRange = RHISubresourceRange::All();
                viewDesc.type = RHITextureViewType::RenderTarget;
                viewDesc.debugName = "ToneMappingOutputRTV";
                data.outputView = builder.Write(
                    builder.CreateTextureView(output, viewDesc),
                    MakeRGAccessDesc(
                        RHIResourceState::RenderTarget,
                        RHIShaderStage::Pixel,
                        RHIDiscardIntent::Discard));
            }
            data.op = m_operator;
            data.outputColorSpace = m_outputColorSpace;
            data.exposureStateContentHash = exposureStateContentHash;
            data.gamma = m_gamma;
            data.whitePoint = m_whitePoint;
        },
        [this](const ToneMappingData& data, RenderGraphPassContext& context)
        {
            if (!m_pipelineCache)
            {
                RVX_CORE_WARN("ToneMapping: missing resources during execution");
                context.MarkExecutionFailed(
                    "ToneMapping requires an initialized PipelineCache");
                return;
            }

            RHIPipeline* pipeline = m_pipelineCache->GetToneMappingPipeline(data.outputFormat);
            RHIDescriptorSetLayout* setLayout =
                m_pipelineCache->GetExposurePostProcessSetLayout();
            IRHIDevice* device = m_pipelineCache->GetDevice();
            if (!pipeline || !setLayout || !device)
            {
                RVX_CORE_WARN("ToneMapping: fullscreen pipeline resources are unavailable");
                context.MarkExecutionFailed(
                    "ToneMapping fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* inputTexture = context.GetTexture(data.input);
            RHITexture* outputTexture = context.GetTexture(data.output);
            RHIBuffer* exposureState = context.GetBuffer(data.exposureState);
            if (!inputTexture || !outputTexture || !exposureState ||
                data.exposureStateContentHash == 0)
            {
                RVX_CORE_WARN("ToneMapping: input or output texture is unavailable");
                context.MarkExecutionFailed(
                    "ToneMapping input or output texture is unavailable");
                return;
            }

            RHITextureView* inputView = context.GetTextureView(data.inputView);
            RHITextureView* outputView = context.GetTextureView(data.outputView);
            if (!inputView || !outputView)
            {
                RVX_CORE_WARN("ToneMapping: failed to resolve input SRV or output RTV");
                context.MarkExecutionFailed(
                    "ToneMapping failed to resolve input SRV or output RTV");
                return;
            }

            if (!EnsureRuntimeResources())
            {
                context.MarkExecutionFailed(
                    "ToneMapping runtime resources or constants are unavailable");
                return;
            }

            ToneMappingGPUConstants constants;
            constants.gamma = data.gamma;
            constants.whitePoint = data.whitePoint;
            constants.operatorType = static_cast<uint32>(data.op);
            constants.outputColorSpace = static_cast<uint32>(data.outputColorSpace);
            constants.textureSize[0] = static_cast<float>(inputTexture->GetWidth());
            constants.textureSize[1] = static_cast<float>(inputTexture->GetHeight());
            constants.invTextureSize[0] = constants.textureSize[0] > 0.0f
                ? 1.0f / constants.textureSize[0] : 1.0f;
            constants.invTextureSize[1] = constants.textureSize[1] > 0.0f
                ? 1.0f / constants.textureSize[1] : 1.0f;
            RHIBufferRef constantBuffer = CreateImmutableToneMappingConstants(*device, constants);
            if (!constantBuffer ||
                !context.RetainSubmissionResource(Ref<RefCounted>(constantBuffer)))
            {
                context.MarkExecutionFailed(
                    "ToneMapping immutable constants are unavailable");
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "ToneMappingDescriptorSet";
            descriptorDesc.BindBuffer(0,
                                      constantBuffer.Get(),
                                      0,
                                      AlignPostProcessConstantBufferSize(sizeof(ToneMappingGPUConstants)));
            descriptorDesc.BindTexture(1, inputView);
            descriptorDesc.BindSampler(2, m_sampler.Get());
            descriptorDesc.BindTexture(3, inputView);
            descriptorDesc.BindBuffer(4, exposureState);

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("ToneMapping: failed to create descriptor set");
                context.MarkExecutionFailed(
                    "ToneMapping failed to create descriptor set");
                return;
            }
            if (!context.RetainSubmissionResource(
                    Ref<RefCounted>(descriptorSet)))
            {
                RVX_CORE_WARN("ToneMapping: submission ownership rejected descriptor set");
                context.MarkExecutionFailed(
                    "ToneMapping submission ownership rejected descriptor set");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(outputView, RHILoadOp::DontCare, RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(0, 0, outputTexture->GetWidth(), outputTexture->GetHeight());

            RHICommandContext& ctx = context.Commands();
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
            m_lastExposureStateContentHash = data.exposureStateContentHash;
        });
}

bool ToneMappingPass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        MarkUnsupported("ToneMapping requires an RHI device");
        return false;
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "ToneMappingLinearClampSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            MarkUnsupported("ToneMapping sampler creation failed");
            return false;
        }
    }

    return true;
}

} // namespace RVX
