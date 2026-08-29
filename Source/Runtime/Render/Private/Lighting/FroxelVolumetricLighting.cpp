#include "Render/Lighting/FroxelVolumetricLighting.h"

#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace RVX
{
    namespace
    {
        constexpr uint64 RVX_FROXEL_CONSTANT_BUFFER_ALIGNMENT = 256;
        constexpr uint32 RVX_FROXEL_INJECTION_GROUP_SIZE = 4;
        constexpr uint32 RVX_FROXEL_INTEGRATION_GROUP_SIZE = 4;
        constexpr uint32 RVX_FROXEL_COMPOSITE_GROUP_SIZE = 8;

        enum class FroxelStage : uint8
        {
            Injection = 0,
            Integration,
            Temporal,
            Composite,
            Count,
        };

        struct FroxelVolumetricGPUConstants
        {
            Mat4 inverseViewProjection{1.0f};
            float cameraPositionNearPlane[4] = {0.0f, 0.0f, 0.0f, 0.1f};
            float cameraForwardFarPlane[4] = {0.0f, 0.0f, -1.0f, 128.0f};
            float outputSizeInverseOutputSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            float gridSizeTilePixels[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            float scattering[4] = {0.0f, 0.0f, 1.0f, 0.0f};
            float heightFogTemporal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float shadowFlags[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        };

        static_assert(sizeof(FroxelVolumetricGPUConstants) == 176);

        [[nodiscard]] uint64 AlignConstantBufferSize(uint64 size) noexcept
        {
            return (size + RVX_FROXEL_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_FROXEL_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        [[nodiscard]] uint32 DivideRoundUp(uint32 value, uint32 divisor) noexcept
        {
            return divisor != 0 ? (value + divisor - 1) / divisor : 0;
        }

        [[nodiscard]] bool IsCanonicalHDRScene(const RHITextureDesc& desc,
                                                const RenderOutputExtent& extent) noexcept
        {
            return desc.dimension == RHITextureDimension::Texture2D &&
                   desc.width == extent.width && desc.height == extent.height &&
                   desc.depth == 1 && desc.arraySize == 1 && desc.mipLevels == 1 &&
                   desc.sampleCount == RHISampleCount::Count1 &&
                   desc.format == RHIFormat::RGBA16_FLOAT &&
                   HasFlag(desc.usage, RHITextureUsage::ShaderResource);
        }

        [[nodiscard]] bool IsCanonicalDeviceDepth(const RHITextureDesc& desc,
                                                   const RenderOutputExtent& extent) noexcept
        {
            return desc.dimension == RHITextureDimension::Texture2D &&
                   desc.width == extent.width && desc.height == extent.height &&
                   desc.depth == 1 && desc.arraySize == 1 && desc.mipLevels == 1 &&
                   desc.sampleCount == RHISampleCount::Count1 &&
                   IsDepthFormat(desc.format) &&
                   HasFlag(desc.usage, RHITextureUsage::ShaderResource);
        }

        [[nodiscard]] bool IsCanonicalShadowTexture(const RHITextureDesc& desc) noexcept
        {
            return desc.dimension == RHITextureDimension::Texture2D &&
                   desc.width != 0 && desc.height != 0 && desc.depth == 1 &&
                   desc.arraySize != 0 && desc.mipLevels == 1 &&
                   desc.sampleCount == RHISampleCount::Count1 &&
                   HasFlag(desc.usage, RHITextureUsage::ShaderResource);
        }

        [[nodiscard]] RGTextureViewHandle DeclareComputeSRV(
            RenderGraphBuilder& builder,
            RGTextureHandle texture,
            RHITextureAspect aspect,
            const char* debugName)
        {
            const RHITextureDesc* const desc = builder.GetTextureDesc(texture);
            if (!texture.IsValid() || desc == nullptr)
            {
                return {};
            }
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.subresourceRange.aspect = aspect;
            viewDesc.type = RHITextureViewType::ShaderResource;
            viewDesc.debugName = debugName;
            return builder.Read(
                builder.CreateTextureView(texture, viewDesc),
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Compute));
        }

        [[nodiscard]] RGTextureViewHandle DeclareComputeUAV(
            RenderGraphBuilder& builder,
            RGTextureHandle texture,
            const char* debugName)
        {
            const RHITextureDesc* const desc = builder.GetTextureDesc(texture);
            if (!texture.IsValid() || desc == nullptr)
            {
                return {};
            }
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::UnorderedAccess;
            viewDesc.debugName = debugName;
            return builder.Write(
                builder.CreateTextureView(texture, viewDesc),
                MakeRGAccessDesc(RHIResourceState::UnorderedAccess,
                                 RHIShaderStage::Compute,
                                 RHIDiscardIntent::Discard));
        }

        [[nodiscard]] FroxelVolumetricGPUConstants BuildConstants(
            const ViewData& view,
            const VolumetricLightingFramePlan& plan,
            bool temporalHistoryUsed,
            bool reverseZ) noexcept
        {
            const FroxelGridMetadata& grid = plan.GetFroxelGrid();
            const RenderVolumetricLightingSettings& settings = plan.GetSettings();
            FroxelVolumetricGPUConstants constants;
            constants.inverseViewProjection = view.inverseJitteredViewProjectionMatrix;
            constants.cameraPositionNearPlane[0] = view.cameraPosition.x;
            constants.cameraPositionNearPlane[1] = view.cameraPosition.y;
            constants.cameraPositionNearPlane[2] = view.cameraPosition.z;
            constants.cameraPositionNearPlane[3] = std::max(view.nearPlane, 0.001f);
            constants.cameraForwardFarPlane[0] = view.cameraForward.x;
            constants.cameraForwardFarPlane[1] = view.cameraForward.y;
            constants.cameraForwardFarPlane[2] = view.cameraForward.z;
            constants.cameraForwardFarPlane[3] =
                std::max(settings.maxDistanceMeters, constants.cameraPositionNearPlane[3] + 0.001f);
            constants.outputSizeInverseOutputSize[0] =
                static_cast<float32>(grid.outputExtent.width);
            constants.outputSizeInverseOutputSize[1] =
                static_cast<float32>(grid.outputExtent.height);
            constants.outputSizeInverseOutputSize[2] =
                1.0f / constants.outputSizeInverseOutputSize[0];
            constants.outputSizeInverseOutputSize[3] =
                1.0f / constants.outputSizeInverseOutputSize[1];
            constants.gridSizeTilePixels[0] = static_cast<float32>(grid.froxelWidth);
            constants.gridSizeTilePixels[1] = static_cast<float32>(grid.froxelHeight);
            constants.gridSizeTilePixels[2] = static_cast<float32>(grid.depthSliceCount);
            constants.gridSizeTilePixels[3] = static_cast<float32>(grid.tileSizePixels);
            constants.scattering[0] = settings.density;
            constants.scattering[1] = settings.anisotropy;
            constants.scattering[2] = settings.maxDistanceMeters;
            constants.scattering[3] = settings.temporalBlend;
            constants.heightFogTemporal[0] = settings.heightFogBaseMeters;
            constants.heightFogTemporal[1] = settings.heightFogFalloffPerMeter;
            constants.heightFogTemporal[2] = static_cast<float32>(
                plan.GetFrameIdentity().frameSequence & 0xffffffffull);
            constants.heightFogTemporal[3] = temporalHistoryUsed ? 1.0f : 0.0f;
            constants.shadowFlags[0] =
                plan.GetInputReadiness().directionalShadowRequired ? 1.0f : 0.0f;
            constants.shadowFlags[1] =
                plan.GetInputReadiness().localShadowRequired ? 1.0f : 0.0f;
            constants.shadowFlags[2] = reverseZ ? 1.0f : 0.0f;
            constants.shadowFlags[3] = static_cast<float32>(
                static_cast<uint32>(plan.GetHistoryResetReasons()));
            return constants;
        }

        [[nodiscard]] bool CreateAndUploadConstants(
            IRHIDevice& device,
            const FroxelVolumetricGPUConstants& constants,
            RHIBufferRef& outBuffer)
        {
            RHIBufferDesc desc;
            desc.size = AlignConstantBufferSize(sizeof(constants));
            desc.usage = RHIBufferUsage::Constant;
            desc.memoryType = RHIMemoryType::Upload;
            desc.debugName = "FroxelVolumetricConstants";
            outBuffer = device.CreateBuffer(desc);
            if (!outBuffer)
            {
                return false;
            }
            void* const mapped = outBuffer->Map();
            if (mapped == nullptr)
            {
                outBuffer.Reset();
                return false;
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            outBuffer->Unmap();
            return true;
        }
    } // namespace

    struct FroxelVolumetricLightingExecutionState
    {
        std::atomic<uint32> completedPassCount{0};
        std::array<std::atomic_bool, static_cast<size_t>(FroxelStage::Count)>
            stageRecorded{};
        std::atomic_bool failed{false};
        RenderViewHistoryIdentity candidateIdentity{};
        uint64 planIdentity = 0;
    };

    namespace
    {
        struct FroxelStageGraphData
        {
            FroxelStage stage = FroxelStage::Injection;
            RGTextureHandle deviceDepth{};
            RGTextureHandle directionalShadow{};
            RGTextureHandle localShadow{};
            RGTextureHandle froxelInput{};
            RGTextureHandle froxelHistory{};
            RGTextureHandle froxelOutput{};
            RGTextureHandle hdrInput{};
            RGTextureHandle hdrOutput{};
            RGBufferHandle lightConstants{};
            RGBufferHandle pointLights{};
            RGBufferHandle spotLights{};
            RGBufferHandle clusterConstants{};
            RGBufferHandle clusterData{};
            RGBufferHandle clusterLightIndices{};
            RGTextureViewHandle deviceDepthView{};
            RGTextureViewHandle directionalShadowView{};
            RGTextureViewHandle localShadowView{};
            RGTextureViewHandle froxelInputView{};
            RGTextureViewHandle froxelHistoryView{};
            RGTextureViewHandle froxelOutputView{};
            RGTextureViewHandle hdrInputView{};
            RGTextureViewHandle hdrOutputView{};
            FrozenFrameLightResources frozenLightResources{};
            RHITextureRef froxelInputOwner;
            RHITextureRef froxelHistoryOwner;
            RHITextureRef froxelOutputOwner;
            RHITextureRef neutralShadowOwner;
            RHISamplerRef sampler;
            RHIPipelineRef pipeline;
            RHIDescriptorSetLayoutRef setLayout;
            IRHIDevice* device = nullptr;
            FroxelVolumetricGPUConstants constants{};
            std::shared_ptr<FroxelVolumetricLightingExecutionState> executionState{};
            bool inputsDeclared = false;
        };

        [[nodiscard]] bool AreExactFrozenBuffers(
            const FroxelStageGraphData& data,
            RenderGraphPassContext& context) noexcept
        {
            return context.GetBuffer(data.lightConstants) ==
                       data.frozenLightResources.lightConstantsBuffer.Get() &&
                   context.GetBuffer(data.pointLights) ==
                       data.frozenLightResources.pointLightsBuffer.Get() &&
                   context.GetBuffer(data.spotLights) ==
                       data.frozenLightResources.spotLightsBuffer.Get() &&
                   context.GetBuffer(data.clusterConstants) ==
                       data.frozenLightResources.clusterConstantsBuffer.Get() &&
                   context.GetBuffer(data.clusterData) ==
                       data.frozenLightResources.clusterBuffer.Get() &&
                   context.GetBuffer(data.clusterLightIndices) ==
                       data.frozenLightResources.clusterLightIndexBuffer.Get();
        }

        void MarkStageFailed(const FroxelStageGraphData& data,
                             RenderGraphPassContext& context,
                             const char* diagnostic)
        {
            if (data.executionState)
            {
                data.executionState->failed.store(true, std::memory_order_relaxed);
            }
            context.MarkExecutionFailed(diagnostic);
        }

        void RecordFroxelStage(const FroxelStageGraphData& data,
                               RenderGraphPassContext& context)
        {
            if (!data.inputsDeclared || !data.executionState || !data.device ||
                !data.pipeline || !data.setLayout || !data.sampler ||
                !AreExactFrozenBuffers(data, context))
            {
                MarkStageFailed(data, context,
                                "Froxel volumetric stage lost sealed graph dependencies");
                return;
            }

            RHITextureView* const depth = context.GetTextureView(data.deviceDepthView);
            RHITextureView* const directionalShadow =
                context.GetTextureView(data.directionalShadowView);
            RHITextureView* const localShadow = context.GetTextureView(data.localShadowView);
            RHITextureView* const froxelInput = context.GetTextureView(data.froxelInputView);
            RHITextureView* const froxelHistory = context.GetTextureView(data.froxelHistoryView);
            RHITextureView* const froxelOutput = context.GetTextureView(data.froxelOutputView);
            RHITextureView* const hdrInput = context.GetTextureView(data.hdrInputView);
            RHITextureView* const hdrOutput = context.GetTextureView(data.hdrOutputView);
            if (depth == nullptr || directionalShadow == nullptr ||
                localShadow == nullptr || froxelInput == nullptr ||
                froxelHistory == nullptr || froxelOutput == nullptr ||
                hdrInput == nullptr || hdrOutput == nullptr ||
                !data.froxelHistoryOwner || !data.neutralShadowOwner)
            {
                MarkStageFailed(data, context,
                                "Froxel volumetric stage could not resolve required RHI views");
                return;
            }

            RHIBufferRef constants;
            if (!CreateAndUploadConstants(*data.device, data.constants, constants))
            {
                MarkStageFailed(data, context,
                                "Froxel volumetric stage could not upload constants");
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = data.setLayout.Get();
            descriptorDesc.debugName = "FroxelVolumetricLightingDescriptorSet";
            descriptorDesc.BindBuffer(0, constants.Get(), 0,
                                      AlignConstantBufferSize(sizeof(data.constants)));
            descriptorDesc.BindTexture(1, depth);
            descriptorDesc.BindTexture(2, directionalShadow);
            descriptorDesc.BindTexture(3, localShadow);
            descriptorDesc.BindBuffer(4,
                                      data.frozenLightResources.lightConstantsBuffer.Get());
            descriptorDesc.BindBuffer(5,
                                      data.frozenLightResources.pointLightsBuffer.Get());
            descriptorDesc.BindBuffer(6,
                                      data.frozenLightResources.spotLightsBuffer.Get());
            descriptorDesc.BindBuffer(7,
                                      data.frozenLightResources.clusterConstantsBuffer.Get());
            descriptorDesc.BindBuffer(8,
                                      data.frozenLightResources.clusterBuffer.Get());
            descriptorDesc.BindBuffer(9,
                                      data.frozenLightResources.clusterLightIndexBuffer.Get());
            descriptorDesc.BindTexture(10, froxelInput);
            descriptorDesc.BindTexture(11, froxelHistory);
            descriptorDesc.BindTexture(12, froxelOutput);
            descriptorDesc.BindTexture(13, hdrOutput);
            descriptorDesc.BindTexture(14, hdrInput);
            descriptorDesc.BindSampler(15, data.sampler.Get());
            RHIDescriptorSetRef descriptorSet =
                data.device->CreateDescriptorSet(descriptorDesc);
            const auto retainOptional = [&context](const Ref<RefCounted>& resource)
            {
                return !resource || context.RetainSubmissionResource(resource);
            };
            if (!descriptorSet || !descriptorSet->IsReadyForBinding(data.setLayout.Get()) ||
                !context.RetainSubmissionResource(Ref<RefCounted>(descriptorSet)) ||
                !context.RetainSubmissionResource(Ref<RefCounted>(constants)) ||
                !context.RetainSubmissionResource(Ref<RefCounted>(data.pipeline)) ||
                !context.RetainSubmissionResource(Ref<RefCounted>(data.setLayout)) ||
                !context.RetainSubmissionResource(Ref<RefCounted>(data.sampler)) ||
                !retainOptional(Ref<RefCounted>(data.froxelInputOwner)) ||
                !context.RetainSubmissionResource(Ref<RefCounted>(data.froxelHistoryOwner)) ||
                !retainOptional(Ref<RefCounted>(data.froxelOutputOwner)) ||
                !context.RetainSubmissionResource(Ref<RefCounted>(data.neutralShadowOwner)) ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(data.frozenLightResources.lightConstantsBuffer)) ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(data.frozenLightResources.pointLightsBuffer)) ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(data.frozenLightResources.spotLightsBuffer)) ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(data.frozenLightResources.clusterConstantsBuffer)) ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(data.frozenLightResources.clusterBuffer)) ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(data.frozenLightResources.clusterLightIndexBuffer)))
            {
                MarkStageFailed(data, context,
                                "Froxel volumetric stage could not retain recording resources");
                return;
            }

            uint32 groupsX = 0;
            uint32 groupsY = 0;
            uint32 groupsZ = 0;
            const uint32 froxelWidth = static_cast<uint32>(data.constants.gridSizeTilePixels[0]);
            const uint32 froxelHeight = static_cast<uint32>(data.constants.gridSizeTilePixels[1]);
            const uint32 froxelDepth = static_cast<uint32>(data.constants.gridSizeTilePixels[2]);
            if (data.stage == FroxelStage::Integration)
            {
                groupsX = DivideRoundUp(froxelWidth, RVX_FROXEL_INTEGRATION_GROUP_SIZE);
                groupsY = DivideRoundUp(froxelHeight, RVX_FROXEL_INTEGRATION_GROUP_SIZE);
                groupsZ = 1;
            }
            else if (data.stage == FroxelStage::Composite)
            {
                groupsX = DivideRoundUp(
                    static_cast<uint32>(data.constants.outputSizeInverseOutputSize[0]),
                    RVX_FROXEL_COMPOSITE_GROUP_SIZE);
                groupsY = DivideRoundUp(
                    static_cast<uint32>(data.constants.outputSizeInverseOutputSize[1]),
                    RVX_FROXEL_COMPOSITE_GROUP_SIZE);
                groupsZ = 1;
            }
            else
            {
                groupsX = DivideRoundUp(froxelWidth, RVX_FROXEL_INJECTION_GROUP_SIZE);
                groupsY = DivideRoundUp(froxelHeight, RVX_FROXEL_INJECTION_GROUP_SIZE);
                groupsZ = DivideRoundUp(froxelDepth, RVX_FROXEL_INJECTION_GROUP_SIZE);
            }
            if (groupsX == 0 || groupsY == 0 || groupsZ == 0)
            {
                MarkStageFailed(data, context,
                                "Froxel volumetric stage computed an empty dispatch");
                return;
            }

            RHICommandContext& commands = context.Commands();
            commands.SetPipeline(data.pipeline.Get());
            commands.SetDescriptorSet(0, descriptorSet.Get());
            commands.Dispatch(groupsX, groupsY, groupsZ);
            data.executionState->stageRecorded[static_cast<size_t>(data.stage)].store(
                true, std::memory_order_relaxed);
            data.executionState->completedPassCount.fetch_add(1,
                                                               std::memory_order_relaxed);
        }
    } // namespace

    FroxelVolumetricLighting::~FroxelVolumetricLighting()
    {
        Shutdown();
    }

    void FroxelVolumetricLighting::SetPipelineCache(PipelineCache* pipelineCache) noexcept
    {
        const IRHIDevice* const nextDevice =
            pipelineCache != nullptr ? pipelineCache->GetDevice() : nullptr;
        if (m_device != nextDevice)
        {
            ResetHistory();
            m_historyTextures[0].Reset();
            m_historyTextures[1].Reset();
            m_neutralShadowTexture.Reset();
            m_linearClampSampler.Reset();
            m_historyWidth = 0;
            m_historyHeight = 0;
            m_historyDepth = 0;
            m_device = const_cast<IRHIDevice*>(nextDevice);
        }
        m_pipelineCache = pipelineCache;
    }

    bool FroxelVolumetricLighting::EnsurePipelineResources()
    {
        if (m_pipelineCache == nullptr || !m_pipelineCache->IsInitialized() ||
            m_pipelineCache->GetDevice() == nullptr)
        {
            RecordFailure("Froxel volumetric lighting requires an initialized PipelineCache");
            return false;
        }
        m_device = m_pipelineCache->GetDevice();
        if (!m_device->GetCapabilities().supportsComputePipeline)
        {
            RecordFailure("Froxel volumetric lighting requires native compute-pipeline support");
            return false;
        }
        if (m_pipelineCache->GetFroxelVolumetricInjectionPipeline() == nullptr ||
            m_pipelineCache->GetFroxelVolumetricIntegrationPipeline() == nullptr ||
            m_pipelineCache->GetFroxelVolumetricTemporalPipeline() == nullptr ||
            m_pipelineCache->GetFroxelVolumetricCompositePipeline() == nullptr ||
            m_pipelineCache->GetFroxelVolumetricSetLayout() == nullptr)
        {
            RecordFailure(m_pipelineCache->GetLastError().empty()
                              ? "Froxel volumetric cooked compute programs are unavailable"
                              : m_pipelineCache->GetLastError().c_str());
            return false;
        }
        if (!m_linearClampSampler)
        {
            RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
            samplerDesc.debugName = "FroxelVolumetricLinearClamp";
            m_linearClampSampler = m_device->CreateSampler(samplerDesc);
            if (!m_linearClampSampler)
            {
                RecordFailure("Froxel volumetric lighting could not create its 3D sampler");
                return false;
            }
        }
        if (!m_neutralShadowTexture)
        {
            RHITextureDesc shadowDesc = RHITextureDesc::Texture2D(
                1, 1, RHIFormat::R32_FLOAT, RHITextureUsage::ShaderResource);
            // Keep an array-shaped resource so disabled branches satisfy the
            // Texture2DArray descriptor type without sampling a 2D alias.
            shadowDesc.arraySize = 2;
            shadowDesc.debugName = "FroxelVolumetricNeutralShadowArray";
            m_neutralShadowTexture = m_device->CreateTexture(shadowDesc);
            if (!m_neutralShadowTexture)
            {
                RecordFailure("Froxel volumetric lighting could not create neutral shadow fallback");
                return false;
            }
        }
        m_unsupportedReason.clear();
        return true;
    }

    bool FroxelVolumetricLighting::EnsureHistoryResources(
        const FroxelGridMetadata& grid)
    {
        if (m_device == nullptr || !grid.IsValid())
        {
            return false;
        }
        const bool matches = m_historyTextures[0] && m_historyTextures[1] &&
            m_historyWidth == grid.froxelWidth &&
            m_historyHeight == grid.froxelHeight &&
            m_historyDepth == grid.depthSliceCount;
        if (matches)
        {
            return true;
        }

        ResetHistory();
        m_historyTextures[0].Reset();
        m_historyTextures[1].Reset();
        RHITextureDesc desc = RHITextureDesc::Texture2D(
            grid.froxelWidth, grid.froxelHeight, RHIFormat::RGBA16_FLOAT,
            RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess);
        desc.dimension = RHITextureDimension::Texture3D;
        desc.depth = grid.depthSliceCount;
        desc.arraySize = 1;
        desc.mipLevels = 1;
        desc.debugName = "FroxelVolumetricHistory0";
        m_historyTextures[0] = m_device->CreateTexture(desc);
        desc.debugName = "FroxelVolumetricHistory1";
        m_historyTextures[1] = m_device->CreateTexture(desc);
        if (!m_historyTextures[0] || !m_historyTextures[1])
        {
            m_historyTextures[0].Reset();
            m_historyTextures[1].Reset();
            return false;
        }
        m_historyWidth = grid.froxelWidth;
        m_historyHeight = grid.froxelHeight;
        m_historyDepth = grid.depthSliceCount;
        m_historyResourceStatesInitialized = false;
        return true;
    }

    bool FroxelVolumetricLighting::IsCandidateIdentityCompatible(
        const VolumetricLightingFramePlan& plan,
        const RenderViewHistoryIdentity& candidate) const noexcept
    {
        return candidate.IsValid() && candidate.previousFrame == plan.GetFrameIdentity() &&
               candidate.view == plan.GetViewIdentity() &&
               candidate.extent == plan.GetFroxelGrid().outputExtent;
    }

    bool FroxelVolumetricLighting::IsCommittedHistoryCompatible(
        const VolumetricLightingFramePlan& plan,
        const FroxelVolumetricLightingInputs& inputs) const noexcept
    {
        if (!plan.UsesTemporalHistory())
        {
            return !inputs.historyAvailable;
        }
        return inputs.historyAvailable && m_historyValid &&
               m_committedHistoryIdentity == plan.GetHistoryIdentity() &&
               plan.GetHistoryResetReasons() == RenderHistoryResetReason::None;
    }

    void FroxelVolumetricLighting::RecordFailure(const char* diagnostic) noexcept
    {
        m_unsupportedReason = diagnostic != nullptr ? diagnostic : "Unknown froxel volumetric failure";
        m_lastStats.failureDiagnostic = m_unsupportedReason;
    }

    FroxelVolumetricLightingOutput FroxelVolumetricLighting::AddToGraph(
        RenderFeatureGraphBuilder& featureBuilder,
        const ViewData& view,
        const VolumetricLightingFramePlan& plan,
        const FroxelVolumetricLightingInputs& inputs)
    {
        m_lastStats = {};
        m_lastStats.requested = plan.IsValid() && !plan.IsIdentityOutput();
        m_lastStats.identityRoute = plan.IsIdentityOutput();
        m_lastStats.volumetricPlanIdentity = plan.GetIdentityHash();
        m_lastStats.lightingPlanIdentity = plan.GetLightingPlanIdentity();
        m_lastStats.historyCandidate = inputs.historyCandidate;

        FroxelVolumetricLightingOutput output;
        output.sourceSceneColor = inputs.sourceSceneColor;
        output.resolvedSceneColor = inputs.sourceSceneColor;
        output.froxelGrid = plan.GetFroxelGrid();
        output.historyCandidate = inputs.historyCandidate;
        output.volumetricPlanIdentity = plan.GetIdentityHash();
        output.lightingPlanIdentity = plan.GetLightingPlanIdentity();
        output.identityRoute = plan.IsIdentityOutput();

        // An exact identity plan is intentionally handled before all pipeline,
        // fallback, history, or graph-resource allocation.  The caller may
        // publish only the original HDR surface for this route.
        if (!plan.IsValid() || plan.IsIdentityOutput())
        {
            if (!plan.IsValid() || !featureBuilder.IsValid() ||
                !inputs.sourceSceneColor.IsValid() ||
                !IsCandidateIdentityCompatible(plan, inputs.historyCandidate))
            {
                RecordFailure("Froxel volumetric identity route received invalid frame or candidate identity");
            }
            return output;
        }

        if (!featureBuilder.IsValid() || m_hasPendingHistory ||
            m_pendingExecutionState || !inputs.HasCompleteLightingResources() ||
            inputs.lightingPlanIdentity != plan.GetLightingPlanIdentity() ||
            !IsCandidateIdentityCompatible(plan, inputs.historyCandidate) ||
            !IsCommittedHistoryCompatible(plan, inputs))
        {
            RecordFailure("Froxel volumetric lighting rejected stale, incomplete, or overlapping frame inputs");
            return output;
        }
        const VolumetricLightingInputReadiness& readiness = plan.GetInputReadiness();
        if (inputs.directionalShadowEnabled != readiness.directionalShadowRequired ||
            inputs.localShadowEnabled != readiness.localShadowRequired)
        {
            RecordFailure("Froxel volumetric shadow inputs disagree with the sealed frame plan");
            return output;
        }
        if (!EnsurePipelineResources() || !EnsureHistoryResources(plan.GetFroxelGrid()))
        {
            if (m_unsupportedReason.empty())
            {
                RecordFailure("Froxel volumetric lighting could not allocate its persistent Texture3D history");
            }
            return output;
        }

        const RenderGraph& graph = featureBuilder.GetGraph();
        const RHITextureDesc* const sourceDesc =
            graph.GetTextureDesc(inputs.sourceSceneColor);
        const RHITextureDesc* const depthDesc = graph.GetTextureDesc(inputs.deviceDepth);
        if (sourceDesc == nullptr || depthDesc == nullptr ||
            !IsCanonicalHDRScene(*sourceDesc, plan.GetFroxelGrid().outputExtent) ||
            !IsCanonicalDeviceDepth(*depthDesc, plan.GetFroxelGrid().outputExtent))
        {
            RecordFailure("Froxel volumetric lighting requires matching RGBA16F HDR and single-sample device-depth inputs");
            return output;
        }
        if (inputs.directionalShadowEnabled &&
            (!graph.GetTextureDesc(inputs.directionalShadowMap) ||
             !IsCanonicalShadowTexture(*graph.GetTextureDesc(inputs.directionalShadowMap))))
        {
            RecordFailure("Froxel volumetric directional shadow input has an invalid graph texture contract");
            return output;
        }
        if (inputs.localShadowEnabled &&
            (!graph.GetTextureDesc(inputs.localShadowAtlas) ||
             !IsCanonicalShadowTexture(*graph.GetTextureDesc(inputs.localShadowAtlas))))
        {
            RecordFailure("Froxel volumetric local shadow input has an invalid graph texture contract");
            return output;
        }

        RHITextureDesc froxelDesc = RHITextureDesc::Texture2D(
            plan.GetFroxelGrid().froxelWidth, plan.GetFroxelGrid().froxelHeight,
            RHIFormat::RGBA16_FLOAT,
            RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess);
        froxelDesc.dimension = RHITextureDimension::Texture3D;
        froxelDesc.depth = plan.GetFroxelGrid().depthSliceCount;
        froxelDesc.arraySize = 1;
        froxelDesc.mipLevels = 1;
        froxelDesc.debugName = "FroxelVolumetricInjected";
        const RGTextureHandle injected = featureBuilder.CreateTexture(froxelDesc);
        froxelDesc.debugName = "FroxelVolumetricIntegrated";
        const RGTextureHandle integrated = featureBuilder.CreateTexture(froxelDesc);
        RHITextureDesc resolvedDesc = *sourceDesc;
        resolvedDesc.usage = RHITextureUsage::ShaderResource |
                             RHITextureUsage::UnorderedAccess;
        resolvedDesc.debugName = "FroxelVolumetricOpaqueComposite";
        const RGTextureHandle resolved = featureBuilder.CreateTexture(resolvedDesc);
        const uint32 readIndex = m_committedHistoryIndex;
        const uint32 writeIndex = 1u - readIndex;
        const RHIResourceState historyInitialState = m_historyResourceStatesInitialized
            ? RHIResourceState::ShaderResource : RHIResourceState::Undefined;
        const RGTextureHandle historyRead = featureBuilder.ImportTexture(
            RHITextureRef(m_historyTextures[readIndex]), historyInitialState);
        const RGTextureHandle historyWrite = featureBuilder.ImportTexture(
            RHITextureRef(m_historyTextures[writeIndex]), historyInitialState);
        const RGTextureHandle neutralShadow = featureBuilder.ImportTexture(
            RHITextureRef(m_neutralShadowTexture), RHIResourceState::Undefined);
        if (!injected.IsValid() || !integrated.IsValid() || !resolved.IsValid() ||
            !historyRead.IsValid() || !historyWrite.IsValid() ||
            !neutralShadow.IsValid())
        {
            RecordFailure("Froxel volumetric lighting could not declare required graph resources");
            return output;
        }

        const bool temporalHistoryUsed = plan.UsesTemporalHistory();
        const FroxelVolumetricGPUConstants constants = BuildConstants(
            view, plan, temporalHistoryUsed, m_pipelineCache->IsReverseZ());
        const RHIPipelineRef injectionPipeline(
            m_pipelineCache->GetFroxelVolumetricInjectionPipeline());
        const RHIPipelineRef integrationPipeline(
            m_pipelineCache->GetFroxelVolumetricIntegrationPipeline());
        const RHIPipelineRef temporalPipeline(
            m_pipelineCache->GetFroxelVolumetricTemporalPipeline());
        const RHIPipelineRef compositePipeline(
            m_pipelineCache->GetFroxelVolumetricCompositePipeline());
        const RHIDescriptorSetLayoutRef setLayout(
            m_pipelineCache->GetFroxelVolumetricSetLayout());
        if (!injectionPipeline || !integrationPipeline || !temporalPipeline ||
            !compositePipeline || !setLayout)
        {
            RecordFailure("Froxel volumetric lighting lost native pipeline ownership before graph registration");
            return output;
        }

        const std::shared_ptr<FroxelVolumetricLightingExecutionState> executionState =
            std::make_shared<FroxelVolumetricLightingExecutionState>();
        executionState->candidateIdentity = inputs.historyCandidate;
        executionState->planIdentity = plan.GetIdentityHash();
        const RGTextureHandle directionalShadow = inputs.directionalShadowEnabled
            ? inputs.directionalShadowMap : neutralShadow;
        const RGTextureHandle localShadow = inputs.localShadowEnabled
            ? inputs.localShadowAtlas : neutralShadow;

        // Seal the input value before registering callbacks.  The graph may
        // execute after this call returns, so no setup callback may borrow the
        // caller's frame-input object.
        const FroxelVolumetricLightingInputs sealedInputs = inputs;
        const auto addStage = [&featureBuilder, sealedInputs, executionState, constants,
                               directionalShadow, localShadow, historyRead, historyWrite,
                               injected, integrated, resolved, source = sealedInputs.sourceSceneColor,
                               device = m_device, sampler = m_linearClampSampler,
                               neutralShadowOwner = m_neutralShadowTexture,
                               readOwner = m_historyTextures[readIndex],
                               writeOwner = m_historyTextures[writeIndex], setLayout](
                                  FroxelStage stage,
                                  const char* name,
                                  const RHIPipelineRef& pipeline,
                                  RGTextureHandle froxelInput,
                                  RGTextureHandle froxelOutput,
                                  RHITextureRef froxelInputOwner,
                                  RHITextureRef froxelOutputOwner,
                                  bool exportOutput) -> bool
        {
            return featureBuilder.AddPass<FroxelStageGraphData>(
                name, RenderGraphPassType::Compute,
                [=](RenderGraphBuilder& builder, FroxelStageGraphData& data)
                {
                    data.stage = stage;
                    data.deviceDepth = sealedInputs.deviceDepth;
                    data.directionalShadow = directionalShadow;
                    data.localShadow = localShadow;
                    data.froxelInput = froxelInput;
                    data.froxelHistory = historyRead;
                    data.froxelOutput = froxelOutput;
                    data.hdrInput = source;
                    data.hdrOutput = resolved;
                    data.lightConstants = sealedInputs.lightConstants;
                    data.pointLights = sealedInputs.pointLights;
                    data.spotLights = sealedInputs.spotLights;
                    data.clusterConstants = sealedInputs.clusterConstants;
                    data.clusterData = sealedInputs.clusterData;
                    data.clusterLightIndices = sealedInputs.clusterLightIndices;
                    data.frozenLightResources = sealedInputs.frozenLightResources;
                    data.froxelInputOwner = froxelInputOwner;
                    data.froxelHistoryOwner = readOwner;
                    data.froxelOutputOwner = froxelOutputOwner;
                    data.neutralShadowOwner = neutralShadowOwner;
                    data.sampler = sampler;
                    data.pipeline = pipeline;
                    data.setLayout = setLayout;
                    data.device = device;
                    data.constants = constants;
                    data.executionState = executionState;
                    data.deviceDepthView = DeclareComputeSRV(
                        builder, data.deviceDepth, RHITextureAspect::Depth,
                        "FroxelVolumetricDepthSRV");
                    const RHITextureDesc* const directionalDesc =
                        builder.GetTextureDesc(data.directionalShadow);
                    const RHITextureDesc* const localDesc =
                        builder.GetTextureDesc(data.localShadow);
                    data.directionalShadowView = DeclareComputeSRV(
                        builder, data.directionalShadow,
                        directionalDesc != nullptr && IsDepthFormat(directionalDesc->format)
                            ? RHITextureAspect::Depth : RHITextureAspect::Color,
                        "FroxelVolumetricDirectionalShadowSRV");
                    data.localShadowView = DeclareComputeSRV(
                        builder, data.localShadow,
                        localDesc != nullptr && IsDepthFormat(localDesc->format)
                            ? RHITextureAspect::Depth : RHITextureAspect::Color,
                        "FroxelVolumetricLocalShadowSRV");
                    data.froxelInputView = DeclareComputeSRV(
                        builder, data.froxelInput, RHITextureAspect::Color,
                        "FroxelVolumetricInputSRV");
                    data.froxelHistoryView = DeclareComputeSRV(
                        builder, data.froxelHistory, RHITextureAspect::Color,
                        "FroxelVolumetricHistorySRV");
                    data.froxelOutputView = DeclareComputeUAV(
                        builder, data.froxelOutput, "FroxelVolumetricOutputUAV");
                    data.hdrInputView = DeclareComputeSRV(
                        builder, data.hdrInput, RHITextureAspect::Color,
                        "FroxelVolumetricHDRInputSRV");
                    data.hdrOutputView = DeclareComputeUAV(
                        builder, data.hdrOutput, "FroxelVolumetricHDROutputUAV");
                    data.lightConstants = builder.Read(
                        data.lightConstants, MakeRGAccessDesc(
                            RHIResourceState::ConstantBuffer, RHIShaderStage::Compute));
                    data.pointLights = builder.Read(
                        data.pointLights, MakeRGAccessDesc(
                            RHIResourceState::ShaderResource, RHIShaderStage::Compute));
                    data.spotLights = builder.Read(
                        data.spotLights, MakeRGAccessDesc(
                            RHIResourceState::ShaderResource, RHIShaderStage::Compute));
                    data.clusterConstants = builder.Read(
                        data.clusterConstants, MakeRGAccessDesc(
                            RHIResourceState::ConstantBuffer, RHIShaderStage::Compute));
                    data.clusterData = builder.Read(
                        data.clusterData, MakeRGAccessDesc(
                            RHIResourceState::ShaderResource, RHIShaderStage::Compute));
                    data.clusterLightIndices = builder.Read(
                        data.clusterLightIndices, MakeRGAccessDesc(
                            RHIResourceState::ShaderResource, RHIShaderStage::Compute));
                    if (exportOutput)
                    {
                        builder.SetExportState(data.froxelOutput,
                                               RHIResourceState::ShaderResource);
                    }
                    if (stage == FroxelStage::Composite)
                    {
                        builder.SetExportState(data.hdrOutput,
                                               RHIResourceState::ShaderResource);
                    }
                    data.inputsDeclared = data.deviceDepthView.IsValid() &&
                        data.directionalShadowView.IsValid() &&
                        data.localShadowView.IsValid() && data.froxelInputView.IsValid() &&
                        data.froxelHistoryView.IsValid() && data.froxelOutputView.IsValid() &&
                        data.hdrInputView.IsValid() && data.hdrOutputView.IsValid() &&
                        data.lightConstants.IsValid() && data.pointLights.IsValid() &&
                        data.spotLights.IsValid() && data.clusterConstants.IsValid() &&
                        data.clusterData.IsValid() && data.clusterLightIndices.IsValid();
                },
                [](const FroxelStageGraphData& data, RenderGraphPassContext& context)
                {
                    RecordFroxelStage(data, context);
                });
        };

        const bool registered =
            addStage(FroxelStage::Injection, "FroxelVolumetricInjection",
                     injectionPipeline, historyRead, injected,
                     RHITextureRef(m_historyTextures[readIndex]), {}, false) &&
            addStage(FroxelStage::Integration, "FroxelVolumetricZIntegration",
                     integrationPipeline, injected, integrated, {}, {}, false) &&
            addStage(FroxelStage::Temporal, "FroxelVolumetricTemporalResolve",
                     temporalPipeline, integrated, historyWrite, {},
                     RHITextureRef(m_historyTextures[writeIndex]), true) &&
            addStage(FroxelStage::Composite, "FroxelVolumetricOpaqueComposite",
                     compositePipeline, historyWrite, injected,
                     RHITextureRef(m_historyTextures[writeIndex]), {}, false);
        if (!registered)
        {
            RecordFailure("Froxel volumetric lighting could not register all required graph passes");
            return output;
        }

        m_pendingExecutionState = executionState;
        m_pendingHistoryIndex = writeIndex;
        m_pendingHistoryIdentity = inputs.historyCandidate;
        m_pendingVolumetricPlanIdentity = plan.GetIdentityHash();
        m_hasPendingHistory = false;
        m_pendingHistorySubmissionAccepted = false;
        output.resolvedSceneColor = resolved;
        output.integratedScatteringTransmittance = historyWrite;
        output.identityRoute = false;
        output.temporalHistoryUsed = temporalHistoryUsed;
        m_lastStats.pipelineReady = true;
        m_lastStats.inputsReady = true;
        m_lastStats.identityRoute = false;
        m_lastStats.temporalHistorySampled = temporalHistoryUsed;
        return output;
    }

    void FroxelVolumetricLighting::FinalizeGraphExecution(bool executionAccepted) noexcept
    {
        const std::shared_ptr<FroxelVolumetricLightingExecutionState> state =
            std::move(m_pendingExecutionState);
        if (!state)
        {
            return;
        }
        const bool allStagesRecorded =
            state->completedPassCount.load(std::memory_order_relaxed) ==
                static_cast<uint32>(FroxelStage::Count) &&
            std::all_of(state->stageRecorded.begin(), state->stageRecorded.end(),
                        [](const std::atomic_bool& recorded)
                        {
                            return recorded.load(std::memory_order_relaxed);
                        });
        m_lastStats.injectionRecorded = state->stageRecorded[
            static_cast<size_t>(FroxelStage::Injection)].load(std::memory_order_relaxed);
        m_lastStats.integrationRecorded = state->stageRecorded[
            static_cast<size_t>(FroxelStage::Integration)].load(std::memory_order_relaxed);
        m_lastStats.temporalResolveRecorded = state->stageRecorded[
            static_cast<size_t>(FroxelStage::Temporal)].load(std::memory_order_relaxed);
        m_lastStats.opaqueCompositeRecorded = state->stageRecorded[
            static_cast<size_t>(FroxelStage::Composite)].load(std::memory_order_relaxed);
        m_lastStats.injectionDispatchCount = m_lastStats.injectionRecorded ? 1u : 0u;
        m_lastStats.integrationDispatchCount = m_lastStats.integrationRecorded ? 1u : 0u;
        m_lastStats.temporalDispatchCount = m_lastStats.temporalResolveRecorded ? 1u : 0u;
        m_lastStats.compositeDispatchCount = m_lastStats.opaqueCompositeRecorded ? 1u : 0u;
        if (!executionAccepted || state->failed.load(std::memory_order_relaxed) ||
            !allStagesRecorded)
        {
            m_hasPendingHistory = false;
            m_pendingHistorySubmissionAccepted = false;
            m_pendingHistoryIdentity = {};
            m_pendingVolumetricPlanIdentity = 0;
            m_lastStats.failureDiagnostic =
                "Froxel volumetric graph execution did not record all four required GPU stages";
            return;
        }
        m_hasPendingHistory = true;
        m_pendingHistorySubmissionAccepted = false;
        m_lastStats.historyCandidatePrepared = true;
    }

    bool FroxelVolumetricLighting::CanCommitHistoryAfterPresent(
        const RenderViewHistoryIdentity& candidateIdentity,
        uint64 volumetricPlanIdentity) const noexcept
    {
        return m_hasPendingHistory && m_pendingHistorySubmissionAccepted &&
               candidateIdentity.IsValid() &&
               candidateIdentity == m_pendingHistoryIdentity &&
               volumetricPlanIdentity != 0 &&
               volumetricPlanIdentity == m_pendingVolumetricPlanIdentity;
    }

    bool FroxelVolumetricLighting::NotifyHistorySubmission(
        const RenderViewHistoryIdentity& candidateIdentity,
        uint64 volumetricPlanIdentity) noexcept
    {
        if (!m_hasPendingHistory || !candidateIdentity.IsValid() ||
            candidateIdentity != m_pendingHistoryIdentity ||
            volumetricPlanIdentity == 0 ||
            volumetricPlanIdentity != m_pendingVolumetricPlanIdentity)
        {
            return false;
        }
        m_historyResourceStatesInitialized = true;
        m_pendingHistorySubmissionAccepted = true;
        m_lastStats.historySubmissionAccepted = true;
        return true;
    }

    bool FroxelVolumetricLighting::CommitHistoryAfterPresent(
        const RenderViewHistoryIdentity& candidateIdentity,
        uint64 volumetricPlanIdentity) noexcept
    {
        if (!CanCommitHistoryAfterPresent(candidateIdentity, volumetricPlanIdentity))
        {
            return false;
        }
        m_committedHistoryIndex = m_pendingHistoryIndex;
        m_committedHistoryIdentity = m_pendingHistoryIdentity;
        m_committedVolumetricPlanIdentity = m_pendingVolumetricPlanIdentity;
        m_historyValid = true;
        m_hasPendingHistory = false;
        m_pendingHistorySubmissionAccepted = false;
        m_pendingHistoryIdentity = {};
        m_pendingVolumetricPlanIdentity = 0;
        m_lastStats.historyCommitted = true;
        return true;
    }

    bool FroxelVolumetricLighting::AbandonPendingHistory(
        const RenderViewHistoryIdentity& candidateIdentity,
        uint64 volumetricPlanIdentity) noexcept
    {
        if (!m_hasPendingHistory || candidateIdentity != m_pendingHistoryIdentity ||
            volumetricPlanIdentity == 0 ||
            volumetricPlanIdentity != m_pendingVolumetricPlanIdentity)
        {
            return false;
        }
        m_hasPendingHistory = false;
        m_pendingHistorySubmissionAccepted = false;
        m_pendingHistoryIdentity = {};
        m_pendingVolumetricPlanIdentity = 0;
        m_lastStats.historyAbandoned = true;
        return true;
    }

    void FroxelVolumetricLighting::ResetHistory() noexcept
    {
        m_historyValid = false;
        m_historyResourceStatesInitialized = false;
        m_committedHistoryIndex = 0;
        m_pendingHistoryIndex = 0;
        m_hasPendingHistory = false;
        m_pendingHistorySubmissionAccepted = false;
        m_committedHistoryIdentity = {};
        m_pendingHistoryIdentity = {};
        m_committedVolumetricPlanIdentity = 0;
        m_pendingVolumetricPlanIdentity = 0;
        m_pendingExecutionState.reset();
    }

    void FroxelVolumetricLighting::Shutdown() noexcept
    {
        ResetHistory();
        m_historyTextures[0].Reset();
        m_historyTextures[1].Reset();
        m_neutralShadowTexture.Reset();
        m_linearClampSampler.Reset();
        m_historyWidth = 0;
        m_historyHeight = 0;
        m_historyDepth = 0;
        m_device = nullptr;
        m_pipelineCache = nullptr;
        m_unsupportedReason.clear();
    }
} // namespace RVX
