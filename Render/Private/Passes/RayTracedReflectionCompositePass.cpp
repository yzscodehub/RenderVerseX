#include "Render/Passes/RayTracedReflectionCompositePass.h"

#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHIRenderPass.h"

#include <cstring>

namespace RVX
{
    namespace
    {
        constexpr uint64 RVX_RAY_TRACED_REFLECTION_COMPOSITE_CONSTANT_BUFFER_ALIGNMENT = 256;

        uint64 AlignRayTracedReflectionCompositeConstantBufferSize(uint64 size)
        {
            return (size + RVX_RAY_TRACED_REFLECTION_COMPOSITE_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_RAY_TRACED_REFLECTION_COMPOSITE_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        struct RayTracedReflectionCompositeGPUConstants
        {
            float intensityScale = 1.0f;
            float padding[3] = {0.0f, 0.0f, 0.0f};
        };
    } // namespace

    void RayTracedReflectionCompositePass::OnAdd(IRHIDevice* device)
    {
        m_device = device;
    }

    void RayTracedReflectionCompositePass::OnRemove()
    {
        m_device = nullptr;
        m_pipelineCache = nullptr;
        m_viewCache = nullptr;
        m_reflectionPass = nullptr;
        m_denoisePass = nullptr;
        m_reflectionReadHandle = {};
        m_colorTargetHandle = {};
        m_constantBuffer.Reset();
        m_sampler.Reset();
        m_retainedDescriptorSets.clear();
        m_stats = {};
        m_enabled = false;
    }

    void RayTracedReflectionCompositePass::SetResources(PipelineCache* pipelineCache,
                                                        ResourceViewCache* viewCache)
    {
        IRHIDevice* newDevice = pipelineCache ? pipelineCache->GetDevice() : nullptr;
        if (newDevice != m_device)
        {
            m_constantBuffer.Reset();
            m_sampler.Reset();
            m_retainedDescriptorSets.clear();
            m_device = newDevice;
        }

        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
    }

    bool RayTracedReflectionCompositePass::IsSupported() const
    {
        if (!m_enabled)
        {
            m_unsupportedReason = "Ray traced reflection composite has not been requested";
            return false;
        }

        if (!m_reflectionPass)
        {
            m_unsupportedReason = "Ray traced reflection composite requires a reflection source pass";
            return false;
        }

        if (!m_reflectionPass->IsEnabled())
        {
            m_unsupportedReason = "Ray traced reflection source pass is not enabled";
            return false;
        }

        if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
        {
            m_unsupportedReason = "Ray traced reflection composite requires an initialized PipelineCache";
            return false;
        }

        if (!m_viewCache || !m_viewCache->IsInitialized())
        {
            m_unsupportedReason = "Ray traced reflection composite requires an initialized ResourceViewCache";
            return false;
        }

        if (!m_pipelineCache->GetPostProcessSetLayout() ||
            !m_pipelineCache->GetRayTracedReflectionCompositePipeline())
        {
            m_unsupportedReason = "Ray traced reflection composite pipeline resources are not available";
            return false;
        }

        m_unsupportedReason.clear();
        return true;
    }

    void RayTracedReflectionCompositePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        m_stats = {};
        m_stats.requested = m_enabled;
        m_stats.supported = IsSupported();
        m_stats.sourcePassEnabled = m_reflectionPass && m_reflectionPass->IsEnabled();
        m_stats.denoiseRequested = m_denoisePass && m_denoisePass->IsRequestedEnabled();
        m_stats.width = view.viewportWidth;
        m_stats.height = view.viewportHeight;
        m_reflectionReadHandle = {};
        m_colorTargetHandle = {};

        if (!m_stats.supported ||
            !m_stats.sourcePassEnabled ||
            !view.colorTarget.IsValid() ||
            !view.renderGraph)
        {
            return;
        }

        RGTextureHandle reflectionHandle;
        if (m_denoisePass && m_denoisePass->IsEnabled())
        {
            const RGTextureHandle denoisedHandle = m_denoisePass->GetDenoisedReflectionHandle();
            m_stats.denoisedSourceAvailable = denoisedHandle.IsValid();
            if (m_stats.denoisedSourceAvailable)
            {
                reflectionHandle = denoisedHandle;
                m_stats.denoisedSourceUsed = true;
                m_stats.source = RayTracedReflectionCompositeSource::DenoisedReflection;
            }
        }

        if (!reflectionHandle.IsValid())
        {
            reflectionHandle = m_reflectionPass->GetReflectionHandle();
            m_stats.rawSourceUsed = reflectionHandle.IsValid();
            m_stats.denoiseFallbackToRaw = m_stats.rawSourceUsed && m_stats.denoiseRequested;
            if (m_stats.rawSourceUsed)
            {
                m_stats.source = RayTracedReflectionCompositeSource::RawReflection;
            }
        }

        m_stats.reflectionHandleAvailable = reflectionHandle.IsValid();
        if (!m_stats.reflectionHandleAvailable)
            return;

        m_reflectionReadHandle = builder.Read(reflectionHandle, RHIShaderStage::Pixel);
        builder.Read(view.colorTarget, RHIShaderStage::Pixel);
        m_colorTargetHandle = builder.Write(view.colorTarget, RHIResourceState::RenderTarget);
        m_stats.outputDeclared = true;
    }

    void RayTracedReflectionCompositePass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        if (!m_stats.outputDeclared ||
            !view.renderGraph ||
            !m_pipelineCache ||
            !m_viewCache ||
            !m_reflectionReadHandle.IsValid() ||
            !m_colorTargetHandle.IsValid())
        {
            m_stats.compositeRecorded = false;
            return;
        }

        RHITexture* reflectionTexture = view.renderGraph->GetTexture(m_reflectionReadHandle);
        RHITexture* colorTarget = view.renderGraph->GetTexture(m_colorTargetHandle);
        if (!reflectionTexture || !colorTarget)
        {
            m_stats.compositeRecorded = false;
            return;
        }

        RHITextureView* reflectionView = m_viewCache->GetDefaultSRV(reflectionTexture);
        RHITextureView* colorTargetView = m_viewCache->GetDefaultRTV(colorTarget);
        if (!reflectionView || !colorTargetView)
        {
            RVX_CORE_WARN("RayTracedReflectionCompositePass: failed to resolve reflection SRV or color RTV");
            m_stats.compositeRecorded = false;
            return;
        }

        RHIFormat outputFormat = RHIFormat::Unknown;
        if (const RHITextureDesc* outputDesc = view.renderGraph->GetTextureDesc(m_colorTargetHandle))
        {
            outputFormat = outputDesc->format;
        }

        RHIPipeline* pipeline = m_pipelineCache->GetRayTracedReflectionCompositePipeline(outputFormat);
        RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetPostProcessSetLayout();
        IRHIDevice* device = m_pipelineCache->GetDevice();
        if (!pipeline || !setLayout || !device)
        {
            RVX_CORE_WARN("RayTracedReflectionCompositePass: pipeline resources are unavailable");
            m_stats.compositeRecorded = false;
            return;
        }

        if (!EnsureRuntimeResources() || !UpdateConstants())
        {
            m_stats.compositeRecorded = false;
            return;
        }

        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.layout = setLayout;
        descriptorDesc.debugName = "RayTracedReflectionCompositeDescriptorSet";
        descriptorDesc.BindBuffer(
            0,
            m_constantBuffer.Get(),
            0,
            AlignRayTracedReflectionCompositeConstantBufferSize(
                sizeof(RayTracedReflectionCompositeGPUConstants)));
        descriptorDesc.BindTexture(1, reflectionView);
        descriptorDesc.BindSampler(2, m_sampler.Get());

        RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
        if (!descriptorSet)
        {
            RVX_CORE_WARN("RayTracedReflectionCompositePass: failed to create descriptor set");
            m_stats.compositeRecorded = false;
            return;
        }

        m_retainedDescriptorSets.push_back(descriptorSet);
        while (m_retainedDescriptorSets.size() > RVX_MAX_FRAME_COUNT + 1)
        {
            m_retainedDescriptorSets.pop_front();
        }

        RHIRenderPassDesc renderPassDesc;
        renderPassDesc.AddColorAttachment(colorTargetView, RHILoadOp::Load, RHIStoreOp::Store);
        renderPassDesc.SetRenderArea(0, 0, colorTarget->GetWidth(), colorTarget->GetHeight());

        ctx.BeginRenderPass(renderPassDesc);
        ctx.SetPipeline(pipeline);
        ctx.SetDescriptorSet(0, descriptorSet.Get());

        RHIViewport viewport;
        viewport.width = static_cast<float>(colorTarget->GetWidth());
        viewport.height = static_cast<float>(colorTarget->GetHeight());
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        ctx.SetViewport(viewport);

        RHIRect scissor;
        scissor.width = colorTarget->GetWidth();
        scissor.height = colorTarget->GetHeight();
        ctx.SetScissor(scissor);

        ctx.Draw(3, 1, 0, 0);
        ctx.EndRenderPass();

        m_stats.compositeRecorded = true;
    }

    bool RayTracedReflectionCompositePass::EnsureRuntimeResources()
    {
        IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : m_device;
        if (!device)
            return false;

        if (!m_constantBuffer)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = AlignRayTracedReflectionCompositeConstantBufferSize(
                sizeof(RayTracedReflectionCompositeGPUConstants));
            bufferDesc.usage = RHIBufferUsage::Constant;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = "RayTracedReflectionCompositeConstants";

            m_constantBuffer = device->CreateBuffer(bufferDesc);
            if (!m_constantBuffer)
                return false;
        }

        if (!m_sampler)
        {
            RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
            samplerDesc.debugName = "RayTracedReflectionCompositeLinearClampSampler";
            m_sampler = device->CreateSampler(samplerDesc);
            if (!m_sampler)
                return false;
        }

        return true;
    }

    bool RayTracedReflectionCompositePass::UpdateConstants()
    {
        if (!m_constantBuffer)
            return false;

        RayTracedReflectionCompositeGPUConstants constants;
        constants.intensityScale = 1.0f;

        void* mapped = m_constantBuffer->Map();
        if (!mapped)
            return false;

        std::memcpy(mapped, &constants, sizeof(constants));
        m_constantBuffer->Unmap();
        m_stats.constantsUploaded = true;
        return true;
    }

} // namespace RVX
