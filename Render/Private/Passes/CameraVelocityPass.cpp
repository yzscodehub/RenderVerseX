#include "Render/Passes/CameraVelocityPass.h"

#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
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
        constexpr uint64 RVX_CAMERA_VELOCITY_CONSTANT_BUFFER_ALIGNMENT = 256;

        uint64 AlignCameraVelocityConstantBufferSize(uint64 size)
        {
            return (size + RVX_CAMERA_VELOCITY_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_CAMERA_VELOCITY_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        struct CameraVelocityGPUConstants
        {
            Mat4 inverseViewProjection = Mat4Identity();
            Mat4 previousViewProjection = Mat4Identity();
            Vec4 outputSizeAndInvSize{1.0f, 1.0f, 1.0f, 1.0f};
            Vec4 velocityParams{0.0f, 0.0f, 0.999999f, 0.0f};
        };
    } // namespace

    void CameraVelocityPass::OnAdd(IRHIDevice* device)
    {
        if (m_device != device)
        {
            m_constantBuffer.Reset();
            m_sampler.Reset();
        }
        m_device = device;
    }

    void CameraVelocityPass::OnRemove()
    {
        m_device = nullptr;
        m_pipelineCache = nullptr;
        m_viewCache = nullptr;
        m_depthReadHandle = {};
        m_velocityWriteHandle = {};
        m_depthViewHandle = {};
        m_velocityViewHandle = {};
        m_constantBuffer.Reset();
        m_sampler.Reset();
        m_stats = {};
        m_enabled = false;
    }

    void CameraVelocityPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
    {
        IRHIDevice* newDevice = pipelineCache ? pipelineCache->GetDevice() : nullptr;
        if (newDevice != m_device)
        {
            m_constantBuffer.Reset();
            m_sampler.Reset();
            m_device = newDevice;
        }

        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
    }

    bool CameraVelocityPass::IsSupported() const
    {
        if (!m_enabled)
        {
            m_unsupportedReason = "Camera velocity pass has not been requested";
            return false;
        }

        if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
        {
            m_unsupportedReason = "Camera velocity pass requires an initialized PipelineCache";
            return false;
        }

        if (!m_viewCache || !m_viewCache->IsInitialized())
        {
            m_unsupportedReason = "Camera velocity pass requires an initialized ResourceViewCache";
            return false;
        }

        if (!m_pipelineCache->GetPostProcessSetLayout() ||
            !m_pipelineCache->GetCameraVelocityPipeline(RHIFormat::RG16_FLOAT))
        {
            m_unsupportedReason = "Camera velocity fullscreen pipeline resources are not available";
            return false;
        }

        m_unsupportedReason.clear();
        return true;
    }

    void CameraVelocityPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        m_stats = {};
        m_stats.requested = m_enabled;
        m_stats.supported = IsSupported();
        m_stats.depthAvailable = view.depthTarget.IsValid();
        m_stats.previousViewProjectionAvailable = view.previousViewProjectionValid != 0 && !view.resetTemporalHistory;
        m_stats.velocityTargetAvailable = view.velocityTarget.IsValid();
        m_stats.width = view.viewportWidth;
        m_stats.height = view.viewportHeight;
        m_stats.outputFormat = RHIFormat::RG16_FLOAT;
        m_depthReadHandle = {};
        m_velocityWriteHandle = {};
        m_depthViewHandle = {};
        m_velocityViewHandle = {};

        if (!m_stats.supported ||
            !m_stats.depthAvailable ||
            !m_stats.velocityTargetAvailable ||
            view.viewportWidth == 0 ||
            view.viewportHeight == 0)
        {
            return;
        }

        RGTextureHandle depthHandle = view.depthTarget;
        depthHandle.hasSubresourceRange = true;
        depthHandle.subresourceRange = RHISubresourceRange{0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
        m_depthReadHandle = depthHandle;
        if (const RHITextureDesc* depthDesc =
                builder.GetTextureDesc(m_depthReadHandle))
        {
            RHITextureViewDesc viewDesc;
            viewDesc.format = depthDesc->format;
            viewDesc.dimension = depthDesc->dimension;
            viewDesc.subresourceRange = depthHandle.subresourceRange;
            viewDesc.type = RHITextureViewType::ShaderResource;
            viewDesc.debugName = "CameraVelocityDepthSRV";
            m_depthViewHandle = builder.CreateTextureView(
                m_depthReadHandle, viewDesc);
            m_depthViewHandle = builder.Read(
                m_depthViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Pixel));
        }
        m_velocityWriteHandle = view.velocityTarget;
        if (const RHITextureDesc* velocityDesc =
                builder.GetTextureDesc(m_velocityWriteHandle))
        {
            m_stats.outputFormat = velocityDesc->format;
            RHITextureViewDesc viewDesc;
            viewDesc.format = velocityDesc->format;
            viewDesc.dimension = velocityDesc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::RenderTarget;
            viewDesc.debugName = "CameraVelocityRTV";
            m_velocityViewHandle = builder.CreateTextureView(
                m_velocityWriteHandle, viewDesc);
            m_velocityViewHandle = builder.Write(
                m_velocityViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::RenderTarget,
                    RHIShaderStage::Pixel,
                    RHIDiscardIntent::Discard));
        }
        m_stats.outputDeclared = m_depthViewHandle.IsValid() &&
            m_velocityViewHandle.IsValid();
    }

    void CameraVelocityPass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        (void)ctx;
        (void)view;
        // Typed AddToGraph owns graph resource realization and execution.
    }

    void CameraVelocityPass::Execute(RenderGraphPassContext& context,
                                     const ViewData& view)
    {
        RHICommandContext& ctx = context.Commands();
        if (!m_stats.outputDeclared ||
            !m_pipelineCache ||
            !m_depthReadHandle.IsValid() ||
            !m_velocityWriteHandle.IsValid())
        {
            m_stats.velocityRecorded = false;
            return;
        }

        RHITexture* depthTexture = context.GetTexture(m_depthReadHandle);
        RHITexture* velocityTexture = context.GetTexture(m_velocityWriteHandle);
        if (!depthTexture || !velocityTexture)
        {
            m_stats.velocityRecorded = false;
            return;
        }

        RHITextureView* depthView = context.GetTextureView(m_depthViewHandle);
        RHITextureView* velocityView =
            context.GetTextureView(m_velocityViewHandle);
        if (!depthView || !velocityView)
        {
            RVX_CORE_WARN("CameraVelocityPass: failed to resolve depth SRV or velocity RTV");
            m_stats.velocityRecorded = false;
            return;
        }

        const RHIFormat outputFormat = m_stats.outputFormat;

        RHIPipeline* pipeline = m_pipelineCache->GetCameraVelocityPipeline(outputFormat);
        RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetPostProcessSetLayout();
        IRHIDevice* device = m_pipelineCache->GetDevice();
        if (!pipeline || !setLayout || !device)
        {
            RVX_CORE_WARN("CameraVelocityPass: pipeline resources are unavailable");
            m_stats.velocityRecorded = false;
            return;
        }

        if (!EnsureRuntimeResources() ||
            !UpdateConstants(view, velocityTexture->GetWidth(), velocityTexture->GetHeight()))
        {
            m_stats.velocityRecorded = false;
            return;
        }

        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.layout = setLayout;
        descriptorDesc.debugName = "CameraVelocityDescriptorSet";
        descriptorDesc.BindBuffer(
            0,
            m_constantBuffer.Get(),
            0,
            AlignCameraVelocityConstantBufferSize(sizeof(CameraVelocityGPUConstants)));
        descriptorDesc.BindTexture(1, depthView);
        descriptorDesc.BindSampler(2, m_sampler.Get());
        descriptorDesc.BindTexture(3, depthView);

        RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
        if (!descriptorSet)
        {
            RVX_CORE_WARN("CameraVelocityPass: failed to create descriptor set");
            m_stats.velocityRecorded = false;
            return;
        }

        if (!context.RetainSubmissionResource(descriptorSet) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(m_constantBuffer)) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(m_sampler)))
        {
            RVX_CORE_WARN("CameraVelocityPass: submission ownership rejected descriptor set");
            m_stats.velocityRecorded = false;
            return;
        }

        RHIRenderPassDesc renderPassDesc;
        renderPassDesc.AddColorAttachment(velocityView, RHILoadOp::DontCare, RHIStoreOp::Store);
        renderPassDesc.SetRenderArea(0, 0, velocityTexture->GetWidth(), velocityTexture->GetHeight());

        ctx.BeginRenderPass(renderPassDesc);
        ctx.SetPipeline(pipeline);
        ctx.SetDescriptorSet(0, descriptorSet.Get());

        RHIViewport viewport;
        viewport.width = static_cast<float>(velocityTexture->GetWidth());
        viewport.height = static_cast<float>(velocityTexture->GetHeight());
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        ctx.SetViewport(viewport);

        RHIRect scissor;
        scissor.width = velocityTexture->GetWidth();
        scissor.height = velocityTexture->GetHeight();
        ctx.SetScissor(scissor);

        ctx.Draw(3, 1, 0, 0);
        ctx.EndRenderPass();

        m_stats.velocityRecorded = true;
    }

    bool CameraVelocityPass::EnsureRuntimeResources()
    {
        IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : m_device;
        if (!device)
            return false;

        if (!m_constantBuffer)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = AlignCameraVelocityConstantBufferSize(sizeof(CameraVelocityGPUConstants));
            bufferDesc.usage = RHIBufferUsage::Constant;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = "CameraVelocityConstants";

            m_constantBuffer = device->CreateBuffer(bufferDesc);
            if (!m_constantBuffer)
                return false;
        }

        if (!m_sampler)
        {
            RHISamplerDesc samplerDesc = RHISamplerDesc::PointClamp();
            samplerDesc.debugName = "CameraVelocityPointClampSampler";
            m_sampler = device->CreateSampler(samplerDesc);
            if (!m_sampler)
                return false;
        }

        return true;
    }

    bool CameraVelocityPass::UpdateConstants(const ViewData& view, uint32 width, uint32 height)
    {
        if (!m_constantBuffer)
            return false;

        const float outputWidth = static_cast<float>(std::max(1u, width));
        const float outputHeight = static_cast<float>(std::max(1u, height));
        const bool previousValid = view.previousViewProjectionValid != 0 && !view.resetTemporalHistory;

        CameraVelocityGPUConstants constants;
        constants.inverseViewProjection = view.inverseViewMatrix * view.inverseProjectionMatrix;
        constants.previousViewProjection = previousValid ? view.previousViewProjectionMatrix : view.viewProjectionMatrix;
        constants.outputSizeAndInvSize = Vec4(outputWidth, outputHeight, 1.0f / outputWidth, 1.0f / outputHeight);
        constants.velocityParams = Vec4(previousValid ? 1.0f : 0.0f, 0.0f, 0.999999f, 0.0f);

        void* mapped = m_constantBuffer->Map();
        if (!mapped)
            return false;

        std::memcpy(mapped, &constants, sizeof(constants));
        if (!m_constantBuffer->CommitMappedWrite())
            return false;
        m_stats.constantsUploaded = true;
        return true;
    }

} // namespace RVX
