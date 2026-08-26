#include "Render/Passes/ParticleFeaturePass.h"

#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "Render/Particles/ParticleBillboardDrawPlan.h"
#include "ShaderContracts/ShaderProgramProvider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace RVX
{
    namespace
    {
        constexpr uint32 RVX_PARTICLE_FRAME_SLOT_COUNT = RVX_MAX_FRAME_COUNT;
        constexpr uint32 RVX_PARTICLE_CONSTANT_BUFFER_SIZE = 256u;
        static_assert(RVX_PARTICLE_FRAME_SLOT_COUNT == 3u);

        struct alignas(16) ParticleBillboardGPUInstance
        {
            Vec4 positionAndRotation{};
            Vec4 color{};
            Vec4 sizeAgeAndLifetime{};
            uint32 blendMode = 0;
            uint32 flags = 0;
            uint32 stableParticleId = 0;
            uint32 padding = 0;
        };
        static_assert(sizeof(ParticleBillboardGPUInstance) == 64u);

        struct alignas(16) ParticleBillboardFrameConstants
        {
            Mat4 viewProjection = Mat4Identity();
            Vec4 cameraRight{};
            Vec4 cameraUp{};
        };
        static_assert(sizeof(ParticleBillboardFrameConstants) <=
                      RVX_PARTICLE_CONSTANT_BUFFER_SIZE);

        [[nodiscard]] Vec3 NormalizeOr(const Vec3& value,
                                       const Vec3& fallback) noexcept
        {
            const float valueLength = length(value);
            return std::isfinite(valueLength) && valueLength > 0.0001f
                ? value / valueLength
                : fallback;
        }

        [[nodiscard]] uint32 BlendPipelineIndex(
            ParticleRenderSnapshotBlendMode blendMode) noexcept
        {
            switch (blendMode)
            {
                case ParticleRenderSnapshotBlendMode::AlphaBlend: return 0;
                case ParticleRenderSnapshotBlendMode::Premultiplied: return 1;
                case ParticleRenderSnapshotBlendMode::Additive: return 2;
                default: return RVX_INVALID_INDEX;
            }
        }

        [[nodiscard]] RHIRenderTargetBlendState MakeBlendState(
            ParticleRenderSnapshotBlendMode blendMode)
        {
            switch (blendMode)
            {
                case ParticleRenderSnapshotBlendMode::AlphaBlend:
                    return RHIRenderTargetBlendState::AlphaBlend();
                case ParticleRenderSnapshotBlendMode::Premultiplied:
                    return RHIRenderTargetBlendState::PremultipliedAlpha();
                case ParticleRenderSnapshotBlendMode::Additive:
                    return RHIRenderTargetBlendState::Additive();
                case ParticleRenderSnapshotBlendMode::Multiply:
                default:
                    return RHIRenderTargetBlendState::Opaque();
            }
        }

        [[nodiscard]] uint32 GrowCapacity(uint32 required) noexcept
        {
            uint32 capacity = 1;
            while (capacity < required && capacity <=
                   std::numeric_limits<uint32>::max() / 2u)
            {
                capacity *= 2u;
            }
            return capacity >= required ? capacity : 0;
        }

        [[nodiscard]] bool Retain(RenderGraphBuilder& builder,
                                  RefCounted* resource)
        {
            return resource == nullptr ||
                   builder.RetainSubmissionResource(Ref<RefCounted>(resource));
        }
    } // namespace

    /** @brief Render-owned persistent resources for CPU billboard drawing. */
    class ParticleBillboardRenderer final
    {
    public:
        struct Submission
        {
            std::shared_ptr<const ParticleBillboardDrawPlan> plan;
            RHIBufferRef constantsBuffer;
            RHIBufferRef instanceBuffer;
            RHIDescriptorSetRef descriptorSet;
            std::array<RHIPipelineRef, 3> pipelines{};
            uint32 frameSlot = RVX_INVALID_INDEX;
            uint64 uploadBytes = 0;
            uint64 resourceCreationCount = 0;
            uint64 pipelineCreationCount = 0;
            std::string diagnostic;

            [[nodiscard]] bool IsReady() const noexcept
            {
                return plan != nullptr && plan->valid && constantsBuffer && instanceBuffer &&
                       descriptorSet && frameSlot != RVX_INVALID_INDEX;
            }
        };

        void SetShaderProgramProvider(IShaderProgramProvider* provider) noexcept
        {
            if (m_shaderProgramProvider != provider)
            {
                ResetPipelineResources();
                m_shaderProgramProvider = provider;
            }
        }

        void SetReverseZ(bool reverseZ) noexcept
        {
            if (m_reverseZ != reverseZ)
            {
                ResetPipelineResources();
                m_reverseZ = reverseZ;
            }
        }

        void OnAdd(IRHIDevice* device) noexcept
        {
            if (m_device != device)
            {
                ResetAllResources();
                m_device = device;
            }
        }

        void OnRemove() noexcept
        {
            ResetAllResources();
            m_device = nullptr;
        }

        [[nodiscard]] Submission Prepare(
            std::shared_ptr<const ParticleBillboardDrawPlan> plan,
            const ViewData& view)
        {
            Submission submission;
            submission.plan = std::move(plan);
            submission.resourceCreationCount = m_resourceCreationCount;
            submission.pipelineCreationCount = m_pipelineCreationCount;
            if (submission.plan == nullptr || !submission.plan->valid)
            {
                submission.diagnostic = submission.plan != nullptr
                    ? submission.plan->diagnostic
                    : "Particle billboard draw plan is unavailable";
                return submission;
            }
            if (submission.plan->items.empty())
            {
                return submission;
            }
            if (m_device == nullptr)
            {
                submission.diagnostic = "Particle billboard renderer has no RHI device";
                return submission;
            }
            if (!EnsurePipelineResources())
            {
                submission.diagnostic = m_unavailableReason;
                return submission;
            }
            const uint32 frameSlot = m_device->GetCurrentFrameIndex();
            if (frameSlot >= m_frameSlots.size())
            {
                submission.diagnostic =
                    "Particle billboard renderer received an invalid frame slot";
                return submission;
            }
            FrameSlot& slot = m_frameSlots[frameSlot];
            if (!EnsureSlot(slot, submission.plan->GetInstanceCount()))
            {
                submission.diagnostic = m_unavailableReason;
                return submission;
            }

            ParticleBillboardFrameConstants constants;
            constants.viewProjection = view.jitteredViewProjectionMatrix;
            constants.cameraRight = Vec4(
                NormalizeOr(GetRightFromMatrix(view.inverseViewMatrix),
                            Vec3(1.0f, 0.0f, 0.0f)),
                0.0f);
            constants.cameraUp = Vec4(
                NormalizeOr(GetUpFromMatrix(view.inverseViewMatrix),
                            Vec3(0.0f, 1.0f, 0.0f)),
                0.0f);
            if (!Upload(slot.constantsBuffer.Get(), &constants, sizeof(constants)))
            {
                submission.diagnostic =
                    "Particle billboard constants upload failed to publish";
                return submission;
            }

            slot.stagingInstances.resize(submission.plan->items.size());
            for (size_t index = 0; index < submission.plan->items.size(); ++index)
            {
                const ParticleBillboardDrawItem& item =
                    submission.plan->items[index];
                slot.stagingInstances[index] = {
                    .positionAndRotation = Vec4(
                        item.particle.position, item.particle.rotation),
                    .color = item.particle.color,
                    .sizeAgeAndLifetime = Vec4(
                        item.particle.size.x,
                        item.particle.size.y,
                        item.particle.age,
                        item.particle.lifetime),
                    .blendMode = BlendPipelineIndex(item.blendMode),
                    .flags = item.particle.flags,
                    .stableParticleId = item.particle.stableParticleId,
                };
            }
            const uint64 instanceBytes =
                static_cast<uint64>(slot.stagingInstances.size()) *
                sizeof(ParticleBillboardGPUInstance);
            if (!Upload(slot.instanceBuffer.Get(), slot.stagingInstances.data(),
                        instanceBytes))
            {
                submission.diagnostic =
                    "Particle billboard instance upload failed to publish";
                return submission;
            }

            submission.constantsBuffer = slot.constantsBuffer;
            submission.instanceBuffer = slot.instanceBuffer;
            submission.descriptorSet = slot.descriptorSet;
            submission.pipelines = m_pipelines;
            submission.frameSlot = frameSlot;
            submission.uploadBytes = sizeof(constants) + instanceBytes;
            submission.resourceCreationCount = m_resourceCreationCount;
            submission.pipelineCreationCount = m_pipelineCreationCount;
            return submission;
        }

    private:
        struct FrameSlot
        {
            RHIBufferRef constantsBuffer;
            RHIBufferRef instanceBuffer;
            RHIDescriptorSetRef descriptorSet;
            std::vector<ParticleBillboardGPUInstance> stagingInstances;
            uint32 instanceCapacity = 0;
        };

        void ResetPipelineResources() noexcept
        {
            m_vertexShader.Reset();
            m_pixelShader.Reset();
            m_descriptorSetLayout.Reset();
            m_pipelineLayout.Reset();
            for (RHIPipelineRef& pipeline : m_pipelines)
            {
                pipeline.Reset();
            }
            for (FrameSlot& slot : m_frameSlots)
            {
                slot.descriptorSet.Reset();
            }
            m_unavailableReason.clear();
        }

        void ResetAllResources() noexcept
        {
            ResetPipelineResources();
            for (FrameSlot& slot : m_frameSlots)
            {
                slot.constantsBuffer.Reset();
                slot.instanceBuffer.Reset();
                slot.instanceCapacity = 0;
            }
        }

        [[nodiscard]] bool EnsurePipelineResources()
        {
            if (m_descriptorSetLayout && m_pipelineLayout && m_vertexShader &&
                m_pixelShader && std::all_of(m_pipelines.begin(), m_pipelines.end(),
                    [](const RHIPipelineRef& pipeline)
                    {
                        return static_cast<bool>(pipeline);
                    }))
            {
                return true;
            }
            if (m_device == nullptr)
            {
                m_unavailableReason = "Particle billboard renderer has no RHI device";
                return false;
            }
            if (m_shaderProgramProvider == nullptr)
            {
                m_unavailableReason =
                    "Particle billboard renderer requires an injected cooked shader program provider";
                return false;
            }

            ResetPipelineResources();
            RHIDescriptorSetLayoutDesc setLayoutDesc;
            setLayoutDesc.debugName = "ParticleBillboard.DescriptorSetLayout";
            setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer,
                                     RHIShaderStage::Vertex);
            setLayoutDesc.AddBinding(1, RHIBindingType::ShaderResourceBuffer,
                                     RHIShaderStage::Vertex);
            m_descriptorSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
            if (!m_descriptorSetLayout)
            {
                m_unavailableReason =
                    "Particle billboard descriptor set layout creation failed";
                return false;
            }
            ++m_resourceCreationCount;

            RHIPipelineLayoutDesc pipelineLayoutDesc;
            pipelineLayoutDesc.debugName = "ParticleBillboard.PipelineLayout";
            pipelineLayoutDesc.setLayouts = {m_descriptorSetLayout.Get()};
            m_pipelineLayout = m_device->CreatePipelineLayout(pipelineLayoutDesc);
            if (!m_pipelineLayout)
            {
                m_unavailableReason = "Particle billboard pipeline layout creation failed";
                return false;
            }
            ++m_resourceCreationCount;

            const auto loadProgram = [this](RHIShaderStage stage, const char* entryPoint)
            {
                ShaderProgramRequest request;
                request.logicalAssetId = "Particle/ParticleBillboard.hlsl";
                request.stage = stage;
                request.entryPoint = entryPoint;
                request.backend = m_device->GetBackendType();
                return m_shaderProgramProvider->Load(*m_device, request);
            };
            ShaderProgramResult vertex = loadProgram(RHIShaderStage::Vertex, "VSMain");
            ShaderProgramResult pixel = loadProgram(RHIShaderStage::Pixel, "PSMain");
            if (!vertex.Succeeded() || !pixel.Succeeded())
            {
                m_unavailableReason = "Particle billboard cooked shader program load failed: " +
                    (!vertex.Succeeded() ? vertex.diagnostic : pixel.diagnostic);
                return false;
            }
            m_vertexShader = vertex.shader;
            m_pixelShader = pixel.shader;

            const std::array<ParticleRenderSnapshotBlendMode, 3> blendModes = {
                ParticleRenderSnapshotBlendMode::AlphaBlend,
                ParticleRenderSnapshotBlendMode::Premultiplied,
                ParticleRenderSnapshotBlendMode::Additive};
            for (uint32 index = 0; index < blendModes.size(); ++index)
            {
                RHIGraphicsPipelineDesc pipelineDesc;
                pipelineDesc.vertexShader = m_vertexShader.Get();
                pipelineDesc.pixelShader = m_pixelShader.Get();
                pipelineDesc.rasterizerState = RHIRasterizerState::NoCull();
                pipelineDesc.depthStencilState = RHIDepthStencilState::ReadOnly();
                pipelineDesc.depthStencilState.depthCompareOp = m_reverseZ
                    ? RHICompareOp::GreaterEqual : RHICompareOp::LessEqual;
                pipelineDesc.blendState.renderTargets[0] =
                    MakeBlendState(blendModes[index]);
                pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
                pipelineDesc.numRenderTargets = 1;
                pipelineDesc.renderTargetFormats[0] = RHIFormat::RGBA16_FLOAT;
                pipelineDesc.depthStencilFormat = RHIFormat::D32_FLOAT;
                pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
                switch (blendModes[index])
                {
                    case ParticleRenderSnapshotBlendMode::AlphaBlend:
                        pipelineDesc.debugName = "ParticleBillboard.AlphaPipeline";
                        break;
                    case ParticleRenderSnapshotBlendMode::Premultiplied:
                        pipelineDesc.debugName = "ParticleBillboard.PremultipliedPipeline";
                        break;
                    case ParticleRenderSnapshotBlendMode::Additive:
                        pipelineDesc.debugName = "ParticleBillboard.AdditivePipeline";
                        break;
                    default: break;
                }
                m_pipelines[index] = m_device->CreateGraphicsPipeline(pipelineDesc);
                if (!m_pipelines[index])
                {
                    m_unavailableReason =
                        "Particle billboard graphics pipeline creation failed";
                    return false;
                }
                ++m_pipelineCreationCount;
            }
            return true;
        }

        [[nodiscard]] bool EnsureSlot(FrameSlot& slot, uint32 requiredInstances)
        {
            if (m_device == nullptr || requiredInstances == 0)
            {
                m_unavailableReason = "Particle billboard frame slot request is invalid";
                return false;
            }
            if (!slot.constantsBuffer)
            {
                RHIBufferDesc constantsDesc;
                constantsDesc.size = RVX_PARTICLE_CONSTANT_BUFFER_SIZE;
                constantsDesc.usage = RHIBufferUsage::Constant;
                constantsDesc.memoryType = RHIMemoryType::Upload;
                constantsDesc.debugName = "ParticleBillboard.FrameConstants";
                slot.constantsBuffer = m_device->CreateBuffer(constantsDesc);
                if (!slot.constantsBuffer)
                {
                    m_unavailableReason =
                        "Particle billboard constants buffer creation failed";
                    return false;
                }
                ++m_resourceCreationCount;
            }
            if (!slot.instanceBuffer || slot.instanceCapacity < requiredInstances)
            {
                const uint32 capacity = GrowCapacity(requiredInstances);
                if (capacity == 0 ||
                    capacity > std::numeric_limits<uint64>::max() /
                        sizeof(ParticleBillboardGPUInstance))
                {
                    m_unavailableReason =
                        "Particle billboard instance capacity overflow";
                    return false;
                }
                RHIBufferDesc instanceDesc;
                instanceDesc.size = static_cast<uint64>(capacity) *
                    sizeof(ParticleBillboardGPUInstance);
                instanceDesc.usage = RHIBufferUsage::Structured |
                    RHIBufferUsage::ShaderResource;
                instanceDesc.memoryType = RHIMemoryType::Upload;
                instanceDesc.stride = sizeof(ParticleBillboardGPUInstance);
                instanceDesc.debugName = "ParticleBillboard.InstanceBuffer";
                RHIBufferRef replacement = m_device->CreateBuffer(instanceDesc);
                if (!replacement)
                {
                    m_unavailableReason =
                        "Particle billboard instance buffer creation failed";
                    return false;
                }
                slot.instanceBuffer = std::move(replacement);
                slot.instanceCapacity = capacity;
                slot.descriptorSet.Reset();
                ++m_resourceCreationCount;
            }
            if (!slot.descriptorSet)
            {
                RHIDescriptorSetDesc descriptorDesc;
                descriptorDesc.debugName = "ParticleBillboard.FrameSlotDescriptorSet";
                descriptorDesc.SetLayout(m_descriptorSetLayout.Get())
                    .BindBuffer(0, slot.constantsBuffer.Get(), 0,
                                RVX_PARTICLE_CONSTANT_BUFFER_SIZE)
                    .BindBuffer(1, slot.instanceBuffer.Get());
                slot.descriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
                if (!slot.descriptorSet ||
                    !slot.descriptorSet->IsReadyForBinding(
                        m_descriptorSetLayout.Get()))
                {
                    slot.descriptorSet.Reset();
                    m_unavailableReason =
                        "Particle billboard frame-slot descriptor set creation failed";
                    return false;
                }
                ++m_resourceCreationCount;
            }
            return true;
        }

        [[nodiscard]] bool Upload(RHIBuffer* buffer,
                                  const void* data,
                                  uint64 byteCount) const
        {
            if (buffer == nullptr || data == nullptr || byteCount == 0 ||
                byteCount > buffer->GetSize())
            {
                return false;
            }
            RHIMappedWriteAccess access = buffer->MapWriteRange(0, byteCount);
            if (!access.IsValid())
            {
                return false;
            }
            std::memcpy(access.GetData(), data, static_cast<size_t>(byteCount));
            return buffer->CommitMappedWriteRange(std::move(access)).IsPublished();
        }

        IRHIDevice* m_device = nullptr;
        IShaderProgramProvider* m_shaderProgramProvider = nullptr;
        bool m_reverseZ = false;
        std::array<FrameSlot, RVX_PARTICLE_FRAME_SLOT_COUNT> m_frameSlots{};
        RHIDescriptorSetLayoutRef m_descriptorSetLayout;
        RHIPipelineLayoutRef m_pipelineLayout;
        RHIShaderRef m_vertexShader;
        RHIShaderRef m_pixelShader;
        std::array<RHIPipelineRef, 3> m_pipelines{};
        uint64 m_resourceCreationCount = 0;
        uint64 m_pipelineCreationCount = 0;
        std::string m_unavailableReason;
    };

    namespace
    {
        struct ParticleGraphPassData
        {
            ParticleBillboardRenderer::Submission submission{};
            RenderPassExecutionData execution{};
            ParticleFeaturePassStats initialStats{};
            RGBufferHandle constantsHandle{};
            RGBufferHandle instanceHandle{};
            RGTextureHandle colorHandle{};
            RGTextureHandle depthHandle{};
            RGTextureViewHandle colorView{};
            RGTextureViewHandle depthView{};
            bool contextValid = false;
            ParticleFeaturePass* owner = nullptr;
        };

        [[nodiscard]] bool PrepareGraphResources(
            ParticleGraphPassData& data,
            RenderGraphBuilder& builder)
        {
            if (!data.contextValid || !data.submission.IsReady() ||
                !data.execution.view.colorTarget.IsValid() ||
                !data.execution.view.depthTarget.IsValid() ||
                !HasCurrentGraphProvenance(data.execution.view.colorTarget,
                                           data.execution.identity))
            {
                return false;
            }
            const ViewData& view = data.execution.view;
            const RHITextureDesc* const colorDesc =
                builder.GetTextureDesc(view.colorTarget);
            if (colorDesc == nullptr || colorDesc->format != RHIFormat::RGBA16_FLOAT)
            {
                return false;
            }
            if (!Retain(builder, data.submission.constantsBuffer.Get()) ||
                !Retain(builder, data.submission.instanceBuffer.Get()) ||
                !Retain(builder, data.submission.descriptorSet.Get()))
            {
                return false;
            }
            for (const RHIPipelineRef& pipeline : data.submission.pipelines)
            {
                if (!pipeline || !Retain(builder, pipeline.Get()))
                {
                    return false;
                }
            }
            data.constantsHandle = builder.ImportBuffer(
                data.submission.constantsBuffer,
                MakeRHIBufferAccessSnapshot(RHIResourceState::ConstantBuffer,
                                            RHIShaderStage::Vertex));
            data.instanceHandle = builder.ImportBuffer(
                data.submission.instanceBuffer,
                MakeRHIBufferAccessSnapshot(RHIResourceState::ShaderResource,
                                            RHIShaderStage::Vertex));
            data.constantsHandle = builder.Read(
                data.constantsHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                      RHIShaderStage::Vertex));
            data.instanceHandle = builder.Read(
                data.instanceHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Vertex));
            if (!data.constantsHandle.IsValid() || !data.instanceHandle.IsValid())
            {
                return false;
            }

            data.colorHandle = view.colorTarget;
            RHITextureViewDesc colorViewDesc;
            colorViewDesc.format = colorDesc->format;
            colorViewDesc.dimension = colorDesc->dimension;
            colorViewDesc.subresourceRange = RHISubresourceRange::All();
            colorViewDesc.type = RHITextureViewType::RenderTarget;
            colorViewDesc.debugName = "ParticleBillboard.ColorRTV";
            data.colorView = builder.CreateTextureView(data.colorHandle, colorViewDesc);
            data.colorView = builder.ReadWrite(
                data.colorView,
                MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                 RHIShaderStage::Pixel));
            if (!data.colorView.IsValid())
            {
                return false;
            }

            if (!HasCurrentGraphProvenance(view.depthTarget,
                                           data.execution.identity))
            {
                return false;
            }
            const RHITextureDesc* const depthDesc =
                builder.GetTextureDesc(view.depthTarget);
            if (depthDesc == nullptr || depthDesc->format != RHIFormat::D32_FLOAT)
            {
                return false;
            }
            data.depthHandle = view.depthTarget;
            RHITextureViewDesc depthViewDesc;
            depthViewDesc.format = depthDesc->format;
            depthViewDesc.dimension = depthDesc->dimension;
            depthViewDesc.subresourceRange = RHISubresourceRange::All();
            depthViewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
            depthViewDesc.type = RHITextureViewType::DepthStencil;
            depthViewDesc.debugName = "ParticleBillboard.DepthDSV";
            data.depthView = builder.CreateTextureView(
                data.depthHandle, depthViewDesc);
            data.depthView = builder.Read(
                data.depthView,
                MakeRGAccessDesc(RHIResourceState::DepthRead,
                                 RHIShaderStage::Vertex | RHIShaderStage::Pixel));
            if (!data.depthView.IsValid())
            {
                return false;
            }
            return true;
        }

        void ExecuteGraphPass(const ParticleGraphPassData& data,
                              RenderGraphPassContext& context)
        {
            ParticleFeaturePassStats stats = data.initialStats;
            stats.requested = data.submission.plan != nullptr &&
                !data.submission.plan->items.empty();
            stats.supported = data.submission.IsReady();
            stats.enabled = stats.requested && stats.supported;
            stats.graphPassScheduled = true;
            stats.uploadPublished = data.submission.IsReady();
            stats.instanceCount = data.submission.plan != nullptr
                ? data.submission.plan->GetInstanceCount() : 0;
            stats.uploadBytes = data.submission.uploadBytes;
            stats.frameSlot = data.submission.frameSlot;
            stats.resourceCreationCount = data.submission.resourceCreationCount;
            stats.pipelineCreationCount = data.submission.pipelineCreationCount;

            const auto fail = [&data, &context, &stats](const char* reason)
            {
                ++stats.fallbackCount;
                stats.supported = false;
                stats.enabled = false;
                stats.unsupportedReason = reason;
                if (data.owner != nullptr)
                {
                    data.owner->PublishExecutionStats(stats);
                }
                context.MarkExecutionFailed(reason);
            };

            if (!data.contextValid || !data.submission.IsReady() ||
                !data.constantsHandle.IsValid() || !data.instanceHandle.IsValid() ||
                !data.colorHandle.IsValid() || !data.colorView.IsValid() ||
                !data.depthHandle.IsValid() || !data.depthView.IsValid())
            {
                fail("Particle billboard graph recording prerequisites are unavailable");
                return;
            }
            RHITexture* const colorTexture = context.GetTexture(data.colorHandle);
            RHITextureView* const colorView = context.GetTextureView(data.colorView);
            RHITextureView* const depthView = context.GetTextureView(data.depthView);
            if (colorTexture == nullptr || colorView == nullptr ||
                depthView == nullptr)
            {
                fail("Particle billboard graph resources could not be realized");
                return;
            }
            if (context.GetBuffer(data.constantsHandle) !=
                    data.submission.constantsBuffer.Get() ||
                context.GetBuffer(data.instanceHandle) !=
                    data.submission.instanceBuffer.Get())
            {
                fail("Particle billboard graph buffer identity mismatch");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                colorView, RHILoadOp::Load, RHIStoreOp::Store);
            renderPassDesc.SetDepthStencil(
                depthView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
            renderPassDesc.depthStencilAttachment.readOnly = true;
            renderPassDesc.SetRenderArea(
                0, 0, colorTexture->GetWidth(), colorTexture->GetHeight());

            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetViewport(data.execution.view.GetRHIViewport());
            commands.SetScissor(data.execution.view.GetRHIScissor());
            for (const ParticleBillboardDrawBatch& batch :
                 data.submission.plan->batches)
            {
                const uint32 pipelineIndex = BlendPipelineIndex(batch.blendMode);
                if (pipelineIndex == RVX_INVALID_INDEX ||
                    pipelineIndex >= data.submission.pipelines.size() ||
                    !data.submission.pipelines[pipelineIndex])
                {
                    commands.EndRenderPass();
                    fail("Particle billboard batch has no legal graphics pipeline");
                    return;
                }
                commands.SetPipeline(data.submission.pipelines[pipelineIndex].Get());
                commands.SetDescriptorSet(0, data.submission.descriptorSet.Get());
                commands.Draw(6, batch.itemCount, 0, batch.firstItem);
                ++stats.drawCallCount;
            }
            commands.EndRenderPass();
            stats.drawSubmitted = stats.drawCallCount != 0;
            if (data.owner != nullptr)
            {
                data.owner->PublishExecutionStats(stats);
            }
        }
    } // namespace

    ParticleFeaturePass::ParticleFeaturePass()
        : m_renderer(std::make_unique<ParticleBillboardRenderer>())
    {
    }

    ParticleFeaturePass::~ParticleFeaturePass() = default;

    void ParticleFeaturePass::SetSnapshot(const ParticleRenderSnapshot* snapshot)
    {
        m_snapshot = snapshot;
        RebuildStats();
    }

    void ParticleFeaturePass::SetShaderProgramProvider(
        IShaderProgramProvider* shaderProgramProvider)
    {
        m_renderer->SetShaderProgramProvider(shaderProgramProvider);
        RebuildStats();
    }

    void ParticleFeaturePass::SetReverseZ(bool reverseZ)
    {
        m_renderer->SetReverseZ(reverseZ);
        RebuildStats();
    }

    void ParticleFeaturePass::PublishExecutionStats(
        const ParticleFeaturePassStats& stats)
    {
        m_stats = stats;
    }

    void ParticleFeaturePass::OnAdd(IRHIDevice* device)
    {
        m_renderer->OnAdd(device);
        RebuildStats();
    }

    void ParticleFeaturePass::OnRemove()
    {
        m_renderer->OnRemove();
        RebuildStats();
    }

    void ParticleFeaturePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        (void)builder;
        (void)view;
        RebuildStats();
    }

    void ParticleFeaturePass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        (void)ctx;
        (void)view;
        // Particle billboards are graph-only. A direct execution would skip the
        // required HDR/depth declarations, so it remains a fail-closed no-work.
        RebuildStats();
        if (m_stats.requested)
        {
            ++m_stats.fallbackCount;
            m_stats.drawSubmitted = false;
            m_stats.unsupportedReason =
                "Particle billboard execution requires RenderGraph resource declarations";
        }
    }

    void ParticleFeaturePass::Execute(RenderGraphPassContext& context,
                                      const ViewData& view)
    {
        (void)view;
        context.MarkExecutionFailed(
            "ParticleFeaturePass must record through its graph-aware AddToGraph override");
    }

    void ParticleFeaturePass::AddToGraph(RenderGraph& graph,
                                         const RenderPassRecordContext& context)
    {
        RebuildStats();
        if (!m_stats.requested)
        {
            return;
        }
        if (!m_snapshot)
        {
            ++m_stats.fallbackCount;
            m_stats.unsupportedReason = "Particle billboard snapshot is unavailable";
            return;
        }

        const RenderPassExecutionData execution = MakeRenderPassExecutionData(context);
        const bool contextValid = context.MatchesTargetGraph(graph) &&
            execution.MatchesTargetGraph(graph) && execution.IsFrameIdentityValid();
        if (!contextValid || !execution.view.colorTarget.IsValid() ||
            !execution.view.depthTarget.IsValid())
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason =
                "Particle billboard rendering requires current HDR color and depth targets";
            return;
        }
        const auto plan = std::make_shared<ParticleBillboardDrawPlan>(
            BuildParticleBillboardDrawPlan(*m_snapshot, execution.view));
        if (!plan->valid)
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason = plan->diagnostic;
            return;
        }
        if (plan->items.empty())
        {
            m_stats.supported = true;
            m_stats.enabled = false;
            return;
        }

        ParticleBillboardRenderer::Submission submission =
            m_renderer->Prepare(plan, execution.view);
        if (!submission.IsReady())
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason = submission.diagnostic.empty()
                ? "Particle billboard renderer resources are unavailable"
                : submission.diagnostic;
            return;
        }
        m_stats.supported = true;
        m_stats.enabled = true;
        m_stats.graphPassScheduled = true;
        m_stats.uploadPublished = true;
        m_stats.instanceCount = plan->GetInstanceCount();
        m_stats.uploadBytes = submission.uploadBytes;
        m_stats.frameSlot = submission.frameSlot;
        m_stats.resourceCreationCount = submission.resourceCreationCount;
        m_stats.pipelineCreationCount = submission.pipelineCreationCount;

        const ParticleFeaturePassStats recordStats = m_stats;
        graph.AddPass<ParticleGraphPassData>(
            GetName(), GetPassType(),
            [submission = std::move(submission), execution, contextValid, recordStats, this](
                RenderGraphBuilder& builder, ParticleGraphPassData& data)
            {
                data.submission = submission;
                data.execution = execution;
                data.initialStats = recordStats;
                data.contextValid = contextValid;
                data.owner = this;
                if (!PrepareGraphResources(data, builder))
                {
                    data.contextValid = false;
                }
            },
            [](const ParticleGraphPassData& data, RenderGraphPassContext& graphContext)
            {
                ExecuteGraphPass(data, graphContext);
            });
    }

    void ParticleFeaturePass::RebuildStats()
    {
        ParticleFeaturePassStats stats;
        if (!m_snapshot || m_snapshot->items.empty())
        {
            m_stats = std::move(stats);
            return;
        }

        stats.requested = true;
        stats.itemCount = static_cast<uint32>(m_snapshot->items.size());
        stats.totalAliveParticles = m_snapshot->metadata.totalAliveParticles;
        if (m_snapshot->metadata.status != ParticleRenderSnapshotStatus::Complete ||
            !m_snapshot->metadata.complete ||
            m_snapshot->metadata.itemCount != m_snapshot->items.size())
        {
            stats.unsupportedReason =
                "Particle billboard draw plan requires a complete particle snapshot";
            m_stats = std::move(stats);
            return;
        }

        uint64 aggregateAliveParticles = 0;
        for (const ParticleRenderSnapshotItem& item : m_snapshot->items)
        {
            if (item.aliveParticleCount > item.maxParticleCount)
            {
                stats.unsupportedReason =
                    "Particle snapshot item alive count exceeds its declared capacity";
                m_stats = std::move(stats);
                return;
            }
            aggregateAliveParticles += item.aliveParticleCount;
            if (aggregateAliveParticles > std::numeric_limits<uint32>::max())
            {
                stats.unsupportedReason =
                    "Particle snapshot aggregate alive count exceeds uint32 diagnostics";
                m_stats = std::move(stats);
                return;
            }
            if (item.payloadStatus == ParticleRenderSnapshotPayloadStatus::MetadataOnly)
            {
                ++stats.metadataOnlyItemCount;
            }
            if (item.payloadStatus == ParticleRenderSnapshotPayloadStatus::CpuPacketReady)
            {
                ++stats.cpuPacketReadyItemCount;
            }
            if (item.sortingSupported)
            {
                ++stats.sortingSupportedItemCount;
            }
        }
        if (aggregateAliveParticles != m_snapshot->metadata.totalAliveParticles)
        {
            stats.unsupportedReason =
                "Particle snapshot aggregate alive count does not match item payloads";
            m_stats = std::move(stats);
            return;
        }

        bool candidateReady = true;
        bool hasVisibleParticles = false;
        std::set<std::tuple<uint64, uint32, uint32>> channelSourceIds;
        std::set<std::tuple<uint64, uint64>> systemInstanceIds;
        for (const ParticleRenderSnapshotItem& item : m_snapshot->items)
        {
            if (item.header.Validate(
                    ParticleRenderFeatureChannelDescriptor::ChannelId,
                    ParticleRenderFeatureChannelDescriptor::SchemaVersion) !=
                RenderFeatureChannelValidationCode::Valid)
            {
                candidateReady = false;
                stats.unsupportedReason =
                    "Particle billboard draw plan requires a valid particle channel envelope";
                break;
            }
            if (!channelSourceIds.insert(std::make_tuple(
                    item.header.source.sceneRuntimeId,
                    item.header.source.entitySlot,
                    item.header.source.entityGeneration)).second)
            {
                candidateReady = false;
                stats.unsupportedReason =
                    "Particle snapshot contains duplicate particle channel source identities";
                break;
            }
            if (!systemInstanceIds.insert(
                    std::make_tuple(item.systemId, item.instanceId)).second)
            {
                candidateReady = false;
                stats.unsupportedReason =
                    "Particle snapshot contains duplicate system-instance identities";
                break;
            }
            if (item.payloadStatus ==
                ParticleRenderSnapshotPayloadStatus::MetadataOnly)
            {
                if (!item.visible && item.HasNoCpuPacket() &&
                    item.aliveParticleCount == 0)
                {
                    continue;
                }
                candidateReady = false;
                // Deliberately retained as the legacy negative-path diagnostic:
                // metadata-only data never claims that a billboard was rendered.
                stats.unsupportedReason =
                    "Particle snapshots contain metadata-only items; Render-owned particle draw implementation is not connected";
                break;
            }
            if (item.simulationBackend !=
                    ParticleRenderSnapshotSimulationBackend::CPU ||
                !item.HasValidCpuPacket() ||
                item.renderMode != ParticleRenderSnapshotMode::Billboard ||
                item.softParticlesEnabled ||
                (item.blendMode != ParticleRenderSnapshotBlendMode::AlphaBlend &&
                 item.blendMode != ParticleRenderSnapshotBlendMode::Premultiplied &&
                 item.blendMode != ParticleRenderSnapshotBlendMode::Additive))
            {
                candidateReady = false;
                stats.unsupportedReason =
                    "Particle billboard rendering received an unsupported particle payload";
                break;
            }
            const std::vector<ParticleRenderParticleData>& particles =
                item.GetParticles();
            if (std::any_of(particles.begin(), particles.end(),
                            [](const ParticleRenderParticleData& particle)
                            {
                                return particle.size.x <= 0.0f ||
                                    particle.size.y <= 0.0f;
                            }))
            {
                candidateReady = false;
                stats.unsupportedReason =
                    "Particle CPU packet contains an invalid billboard size";
                break;
            }
            hasVisibleParticles = hasVisibleParticles ||
                (item.visible && item.aliveParticleCount != 0);
        }
        if (candidateReady)
        {
            // This preflight intentionally performs only linear validation.
            // View sorting and immutable plan construction occur once during
            // AddToGraph, avoiding a second O(N log N) sort before recording.
            stats.supported = true;
            stats.enabled = hasVisibleParticles;
        }
        m_stats = std::move(stats);
    }
} // namespace RVX
