#include "Render/Passes/RayTracedReflectionDenoisePass.h"

#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <cstring>

namespace RVX
{
    namespace
    {
        constexpr uint64 RVX_RAY_TRACED_REFLECTION_DENOISE_CONSTANT_BUFFER_ALIGNMENT = 256;

        uint64 AlignRayTracedReflectionDenoiseConstantBufferSize(uint64 size)
        {
            return (size + RVX_RAY_TRACED_REFLECTION_DENOISE_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_RAY_TRACED_REFLECTION_DENOISE_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        struct RayTracedReflectionDenoiseGPUConstants
        {
            float outputSizeAndInvSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            float sceneDepthSizeAndInvSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            float denoiseParams[4] = {1.0f, 0.01f, 0.85f, 1.0f};
            float denoiseQualityParams[4] = {1.0f, 4.0f, 0.0f, 0.0f};
        };
    } // namespace

    void RayTracedReflectionDenoisePass::OnAdd(IRHIDevice* device)
    {
        m_device = device;
    }

    void RayTracedReflectionDenoisePass::OnRemove()
    {
        m_device = nullptr;
        m_pipelineCache = nullptr;
        m_viewCache = nullptr;
        m_reflectionPass = nullptr;
        m_reflectionReadHandle = {};
        m_depthReadHandle = {};
        m_normalGuideReadHandle = {};
        m_denoisedReflectionHandle = {};
        m_reflectionViewHandle = {};
        m_depthViewHandle = {};
        m_normalGuideViewHandle = {};
        m_outputViewHandle = {};
        m_constantBuffer.Reset();
        m_stats = {};
        m_enabled = false;
    }

    void RayTracedReflectionDenoisePass::SetResources(PipelineCache* pipelineCache,
                                                      ResourceViewCache* viewCache)
    {
        IRHIDevice* newDevice = pipelineCache ? pipelineCache->GetDevice() : nullptr;
        if (newDevice != m_device)
        {
            m_constantBuffer.Reset();
            m_device = newDevice;
        }

        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
    }

    bool RayTracedReflectionDenoisePass::IsSupported() const
    {
        if (!m_enabled)
        {
            m_unsupportedReason = "Ray traced reflection denoise has not been requested";
            return false;
        }

        if (!m_reflectionPass)
        {
            m_unsupportedReason = "Ray traced reflection denoise requires a reflection source pass";
            return false;
        }

        if (!m_reflectionPass->IsEnabled())
        {
            m_unsupportedReason = "Ray traced reflection source pass is not enabled";
            return false;
        }

        if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
        {
            m_unsupportedReason = "Ray traced reflection denoise requires an initialized PipelineCache";
            return false;
        }

        if (!m_viewCache || !m_viewCache->IsInitialized())
        {
            m_unsupportedReason = "Ray traced reflection denoise requires an initialized ResourceViewCache";
            return false;
        }

        if (!m_pipelineCache->GetRayTracedReflectionDenoiseSetLayout() ||
            !m_pipelineCache->GetRayTracedReflectionDenoisePipeline())
        {
            m_unsupportedReason = "Ray traced reflection denoise pipeline resources are not available";
            return false;
        }

        m_unsupportedReason.clear();
        return true;
    }

    void RayTracedReflectionDenoisePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        m_stats = {};
        m_stats.requested = m_enabled;
        m_stats.supported = IsSupported();
        m_stats.sourcePassEnabled = m_reflectionPass && m_reflectionPass->IsEnabled();
        m_stats.depthAvailable = view.depthTarget.IsValid();
        m_stats.radius = std::min<uint32>(m_config.radius, 3u);
        m_stats.depthSigma = std::max(m_config.depthSigma, 1.0e-5f);
        m_stats.normalThreshold = std::clamp(m_config.normalThreshold, 0.0f, 1.0f);
        m_stats.confidencePower = std::max(m_config.confidencePower, 0.01f);
        m_stats.centerWeight = std::max(m_config.centerWeight, 0.0f);
        m_stats.lowConfidenceDepthScale = std::max(m_config.lowConfidenceDepthScale, 1.0f);
        m_stats.kernelTapCount = m_stats.radius == 0u ? 0u : (m_stats.radius * 2u + 1u) * (m_stats.radius * 2u + 1u);
        m_stats.width = view.viewportWidth;
        m_stats.height = view.viewportHeight;
        m_stats.dispatchPixelCount = static_cast<uint64>(m_stats.width) * static_cast<uint64>(m_stats.height);
        m_stats.estimatedTapCount = m_stats.dispatchPixelCount * static_cast<uint64>(m_stats.kernelTapCount);
        m_reflectionReadHandle = {};
        m_depthReadHandle = {};
        m_normalGuideReadHandle = {};
        m_denoisedReflectionHandle = {};
        m_reflectionViewHandle = {};
        m_depthViewHandle = {};
        m_normalGuideViewHandle = {};
        m_outputViewHandle = {};
        m_outputFormat = RHIFormat::Unknown;

        if (!m_stats.supported ||
            !m_stats.sourcePassEnabled ||
            !m_stats.depthAvailable ||
            view.viewportWidth == 0 ||
            view.viewportHeight == 0)
        {
            return;
        }

        const RGTextureHandle reflectionHandle = m_reflectionPass->GetReflectionHandle();
        m_stats.reflectionHandleAvailable = reflectionHandle.IsValid();
        if (!m_stats.reflectionHandleAvailable)
            return;

        const RGTextureHandle normalGuideHandle = m_reflectionPass->GetCurrentNormalHistoryHandle();
        m_stats.normalGuideAvailable = normalGuideHandle.IsValid();
        if (!m_stats.normalGuideAvailable)
            return;

        RHITextureDesc outputDesc =
            RHITextureDesc::RenderTarget(view.viewportWidth, view.viewportHeight, RHIFormat::RGBA16_FLOAT);
        if (const RHITextureDesc* reflectionDesc =
                builder.GetTextureDesc(reflectionHandle))
        {
            outputDesc = *reflectionDesc;
            outputDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
        }
        m_stats.width = outputDesc.width;
        m_stats.height = outputDesc.height;
        m_stats.dispatchPixelCount = static_cast<uint64>(m_stats.width) * static_cast<uint64>(m_stats.height);
        m_stats.estimatedTapCount = m_stats.dispatchPixelCount * static_cast<uint64>(m_stats.kernelTapCount);
        outputDesc.debugName = "RayTracedReflectionDenoised";

        RGTextureHandle depthHandle = view.depthTarget;
        depthHandle.hasSubresourceRange = true;
        depthHandle.subresourceRange = RHISubresourceRange{0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};

        const auto createReadView = [&builder](
                                        RGTextureHandle texture,
                                        const char* debugName)
        {
            const RHITextureDesc* desc = builder.GetTextureDesc(texture);
            if (!desc)
                return RGTextureViewHandle{};
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = texture.hasSubresourceRange
                ? texture.subresourceRange : RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::ShaderResource;
            viewDesc.debugName = debugName;
            RGTextureViewHandle viewHandle = builder.CreateTextureView(
                texture, viewDesc);
            return builder.Read(
                viewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Pixel));
        };
        m_reflectionReadHandle = reflectionHandle;
        m_reflectionViewHandle = createReadView(
            m_reflectionReadHandle, "RayTracedReflectionDenoiseInputSRV");
        m_depthReadHandle = depthHandle;
        m_depthViewHandle = createReadView(
            m_depthReadHandle, "RayTracedReflectionDenoiseDepthSRV");
        m_normalGuideReadHandle = normalGuideHandle;
        m_normalGuideViewHandle = createReadView(
            m_normalGuideReadHandle, "RayTracedReflectionDenoiseNormalSRV");
        m_denoisedReflectionHandle = builder.CreateTexture(outputDesc);
        m_outputFormat = outputDesc.format;
        RHITextureViewDesc outputViewDesc;
        outputViewDesc.format = outputDesc.format;
        outputViewDesc.dimension = outputDesc.dimension;
        outputViewDesc.subresourceRange = RHISubresourceRange::All();
        outputViewDesc.type = RHITextureViewType::RenderTarget;
        outputViewDesc.debugName = "RayTracedReflectionDenoiseRTV";
        m_outputViewHandle = builder.CreateTextureView(
            m_denoisedReflectionHandle, outputViewDesc);
        m_outputViewHandle = builder.Write(
            m_outputViewHandle,
            MakeRGAccessDesc(
                RHIResourceState::RenderTarget,
                RHIShaderStage::Pixel,
                RHIDiscardIntent::Discard));
        m_stats.outputDeclared = m_reflectionViewHandle.IsValid() &&
            m_depthViewHandle.IsValid() &&
            m_normalGuideViewHandle.IsValid() &&
            m_outputViewHandle.IsValid();
    }

    void RayTracedReflectionDenoisePass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        (void)ctx;
        (void)view;
        // Typed AddToGraph owns graph resource realization and execution.
    }

    void RayTracedReflectionDenoisePass::Execute(
        RenderGraphPassContext& context,
        const ViewData& view)
    {
        RHICommandContext& ctx = context.Commands();
        if (!m_stats.outputDeclared ||
            !m_pipelineCache ||
            !m_reflectionReadHandle.IsValid() ||
            !m_depthReadHandle.IsValid() ||
            !m_normalGuideReadHandle.IsValid() ||
            !m_denoisedReflectionHandle.IsValid())
        {
            m_stats.denoiseRecorded = false;
            return;
        }

        RHITexture* reflectionTexture = context.GetTexture(m_reflectionReadHandle);
        RHITexture* depthTexture = context.GetTexture(m_depthReadHandle);
        RHITexture* normalGuideTexture = context.GetTexture(m_normalGuideReadHandle);
        RHITexture* outputTexture = context.GetTexture(m_denoisedReflectionHandle);
        if (!reflectionTexture || !depthTexture || !normalGuideTexture || !outputTexture)
        {
            m_stats.denoiseRecorded = false;
            return;
        }

        RHITextureView* reflectionView =
            context.GetTextureView(m_reflectionViewHandle);
        RHITextureView* depthView = context.GetTextureView(m_depthViewHandle);
        RHITextureView* normalGuideView =
            context.GetTextureView(m_normalGuideViewHandle);
        RHITextureView* outputView = context.GetTextureView(m_outputViewHandle);
        if (!reflectionView || !depthView || !normalGuideView || !outputView)
        {
            RVX_CORE_WARN(
                "RayTracedReflectionDenoisePass: failed to resolve reflection SRV, depth SRV, normal SRV, or output RTV");
            m_stats.denoiseRecorded = false;
            return;
        }

        RHIPipeline* pipeline =
            m_pipelineCache->GetRayTracedReflectionDenoisePipeline(
                m_outputFormat);
        RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetRayTracedReflectionDenoiseSetLayout();
        IRHIDevice* device = m_pipelineCache->GetDevice();
        if (!pipeline || !setLayout || !device)
        {
            RVX_CORE_WARN("RayTracedReflectionDenoisePass: pipeline resources are unavailable");
            m_stats.denoiseRecorded = false;
            return;
        }

        if (!EnsureRuntimeResources() || !UpdateConstants(view, outputTexture->GetWidth(), outputTexture->GetHeight()))
        {
            m_stats.denoiseRecorded = false;
            return;
        }

        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.layout = setLayout;
        descriptorDesc.debugName = "RayTracedReflectionDenoiseDescriptorSet";
        descriptorDesc.BindBuffer(
            0,
            m_constantBuffer.Get(),
            0,
            AlignRayTracedReflectionDenoiseConstantBufferSize(
                sizeof(RayTracedReflectionDenoiseGPUConstants)));
        descriptorDesc.BindTexture(1, reflectionView);
        descriptorDesc.BindTexture(2, depthView);
        descriptorDesc.BindTexture(3, normalGuideView);

        RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
        if (!descriptorSet)
        {
            RVX_CORE_WARN("RayTracedReflectionDenoisePass: failed to create descriptor set");
            m_stats.denoiseRecorded = false;
            return;
        }

        if (!context.RetainSubmissionResource(descriptorSet) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(m_constantBuffer)))
        {
            RVX_CORE_WARN("RayTracedReflectionDenoisePass: submission ownership rejected descriptor set");
            m_stats.denoiseRecorded = false;
            return;
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

        m_stats.denoiseRecorded = true;
    }

    bool RayTracedReflectionDenoisePass::EnsureRuntimeResources()
    {
        IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : m_device;
        if (!device)
            return false;

        if (!m_constantBuffer)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = AlignRayTracedReflectionDenoiseConstantBufferSize(
                sizeof(RayTracedReflectionDenoiseGPUConstants));
            bufferDesc.usage = RHIBufferUsage::Constant;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = "RayTracedReflectionDenoiseConstants";

            m_constantBuffer = device->CreateBuffer(bufferDesc);
            if (!m_constantBuffer)
                return false;
        }

        return true;
    }

    bool RayTracedReflectionDenoisePass::UpdateConstants(const ViewData& view,
                                                         uint32 outputWidth,
                                                         uint32 outputHeight)
    {
        if (!m_constantBuffer)
            return false;

        RayTracedReflectionDenoiseGPUConstants constants;
        constants.outputSizeAndInvSize[0] = static_cast<float>(std::max<uint32>(outputWidth, 1));
        constants.outputSizeAndInvSize[1] = static_cast<float>(std::max<uint32>(outputHeight, 1));
        constants.outputSizeAndInvSize[2] = 1.0f / constants.outputSizeAndInvSize[0];
        constants.outputSizeAndInvSize[3] = 1.0f / constants.outputSizeAndInvSize[1];
        constants.sceneDepthSizeAndInvSize[0] = static_cast<float>(std::max<uint32>(view.viewportWidth, 1));
        constants.sceneDepthSizeAndInvSize[1] = static_cast<float>(std::max<uint32>(view.viewportHeight, 1));
        constants.sceneDepthSizeAndInvSize[2] = 1.0f / constants.sceneDepthSizeAndInvSize[0];
        constants.sceneDepthSizeAndInvSize[3] = 1.0f / constants.sceneDepthSizeAndInvSize[1];
        constants.denoiseParams[0] = static_cast<float>(std::min<uint32>(m_config.radius, 3u));
        constants.denoiseParams[1] = std::max(m_config.depthSigma, 1.0e-5f);
        constants.denoiseParams[2] = std::clamp(m_config.normalThreshold, 0.0f, 1.0f);
        constants.denoiseParams[3] = std::max(m_config.confidencePower, 0.01f);
        constants.denoiseQualityParams[0] = std::max(m_config.centerWeight, 0.0f);
        constants.denoiseQualityParams[1] = std::max(m_config.lowConfidenceDepthScale, 1.0f);

        void* mapped = m_constantBuffer->Map();
        if (!mapped)
            return false;

        std::memcpy(mapped, &constants, sizeof(constants));
        m_constantBuffer->Unmap();
        m_stats.constantsUploaded = true;
        return true;
    }

} // namespace RVX
