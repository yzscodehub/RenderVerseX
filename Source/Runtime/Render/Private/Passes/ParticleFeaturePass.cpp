#include "Render/Passes/ParticleFeaturePass.h"

#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/Particles/ParticleBillboardDrawPlan.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderClipConvention.h"
#include "Resources/RenderResourceRegistry.h"
#include "ShaderContracts/ShaderProgramProvider.h"

#include <algorithm>
#include <array>
#include <atomic>
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
        constexpr uint32 RVX_PARTICLE_BATCH_RECEIPT_CAPACITY = 256u;
        constexpr uint32 RVX_PARTICLE_FALLBACK_TEXTURE_ROW_PITCH = 256u;
        constexpr uint32 RVX_PARTICLE_EXPOSURE_STATE_BINDING = 6u;
        static_assert(RVX_PARTICLE_FRAME_SLOT_COUNT == 3u);

        struct alignas(16) ParticleBillboardGPUInstance
        {
            Vec4 positionAndRotation{};
            Vec4 color{};
            Vec4 sizeAgeAndLifetime{};
            Vec4 atlasUVOffsetScale{0.0f, 0.0f, 1.0f, 1.0f};
            Vec4 softDepthDistortionReactive{};
            uint32 blendMode = 0;
            uint32 flags = 0;
            uint32 flipbookFrameIndex = 0;
            uint32 packedAtlasTiles = 0;
        };
        static_assert(sizeof(ParticleBillboardGPUInstance) == 96u);

        struct alignas(16) ParticleBillboardFrameConstants
        {
            Mat4 viewProjection = Mat4Identity();
            Mat4 inverseProjection = Mat4Identity();
            Vec4 cameraRight{};
            Vec4 cameraUp{};
            Vec4 inverseExtentAndReverseZ{};
            Vec4 viewportOriginAndTargetExtent{};
        };
        static_assert(sizeof(ParticleBillboardFrameConstants) == 192u,
                      "Particle billboard C++ constants must match the 12-register HLSL cbuffer");
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

        [[nodiscard]] bool IsFiniteMatrix(const Mat4& matrix) noexcept
        {
            for (uint32 column = 0; column < 4u; ++column)
            {
                for (uint32 row = 0; row < 4u; ++row)
                {
                    if (!std::isfinite(matrix[column][row]))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] bool TryGetFiniteJitteredInverseProjection(
            const ViewData& view,
            Mat4& outInverseProjection) noexcept
        {
            if (!IsFiniteMatrix(view.jitteredProjectionMatrix))
            {
                return false;
            }

            outInverseProjection = inverse(view.jitteredProjectionMatrix);
            return IsFiniteMatrix(outInverseProjection);
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

        [[nodiscard]] bool IsSameBufferHandle(
            const RGBufferHandle& lhs, const RGBufferHandle& rhs) noexcept
        {
            return lhs.index == rhs.index && lhs.hasRange == rhs.hasRange &&
                lhs.rangeOffset == rhs.rangeOffset && lhs.rangeSize == rhs.rangeSize &&
                lhs.graphIdentity == rhs.graphIdentity &&
                lhs.recordingGeneration == rhs.recordingGeneration;
        }

        [[nodiscard]] SceneTextures::ExposureStateMetadata
        MakeExposureStateMetadata(const ViewData& view) noexcept
        {
            return {
                view.exposureStateSchemaVersion,
                view.exposureStateStride,
                view.exposureStateMode,
                view.exposureStateConfigurationHash};
        }

        [[nodiscard]] const char* DescribeParticleExposureRecordFailure(
            const ParticleVfxRecordInput& input,
            const RenderPassRecordIdentity& expected) noexcept
        {
            if (input.IsEmpty())
            {
                return "Particle VFX exposure record is absent";
            }
            if (input.identity != expected)
            {
                return "Particle VFX exposure record identity does not match the current graph";
            }
            if (!input.exposureState.IsValid())
            {
                return "Particle VFX committed exposure-state handle is absent";
            }
            if (!HasCurrentGraphProvenance(input.exposureState, expected))
            {
                return "Particle VFX committed exposure-state handle is stale";
            }
            if (input.exposureStateContentHash == 0)
            {
                return "Particle VFX committed exposure-state hash is absent";
            }
            return nullptr;
        }

        /**
         * A descriptor may legally be bound even when the particle does not
         * sample it on a particular shader path.  Explicit APIs still require
         * that its image has a valid sampled layout.  Initialize the shared
         * fallback once, rather than relying on an undefined allocation or a
         * later render-graph transition that cannot see this raw descriptor.
         */
        [[nodiscard]] bool InitializeFallbackTextureForSampling(
            IRHIDevice& device,
            RHITexture& texture)
        {
            RHICommandContextRef uploadContext = device.CreateCommandContext(
                RHICommandQueueType::Graphics);
            if (!uploadContext)
            {
                // An explicit backend must not silently bind an
                // uninitialized fallback texture.
                return !device.GetCapabilities().supportsExplicitResourceBarriers;
            }

            std::array<uint8, RVX_PARTICLE_FALLBACK_TEXTURE_ROW_PITCH> whitePixel{};
            whitePixel[0] = 255u;
            whitePixel[1] = 255u;
            whitePixel[2] = 255u;
            whitePixel[3] = 255u;

            RHIStagingBufferDesc stagingDesc;
            stagingDesc.size = whitePixel.size();
            stagingDesc.debugName = "ParticleBillboard.FallbackTextureUpload";
            RHIStagingBufferRef staging = device.CreateStagingBuffer(stagingDesc);
            if (!staging)
            {
                return false;
            }
            void* const mapped = staging->Map(0, whitePixel.size());
            if (mapped == nullptr)
            {
                return false;
            }
            std::memcpy(mapped, whitePixel.data(), whitePixel.size());
            staging->Unmap();

            uploadContext->Begin();
            // This first-use discard transition is portable: Vulkan textures
            // begin Undefined while DX12 projects that state to Common.
            uploadContext->TextureBarrier(
                &texture,
                RHIResourceState::Undefined,
                RHIResourceState::CopyDest);
            RHIBufferTextureCopyDesc copyDesc;
            copyDesc.bufferRowPitch = RVX_PARTICLE_FALLBACK_TEXTURE_ROW_PITCH;
            copyDesc.bufferImageHeight = 1u;
            copyDesc.textureRegion = {0u, 0u, 1u, 1u};
            uploadContext->CopyBufferToTexture(staging->GetBuffer(), &texture, copyDesc);
            uploadContext->TextureBarrier(
                &texture,
                RHIResourceState::CopyDest,
                RHIResourceState::ShaderResource);
            uploadContext->End();
            device.SubmitCommandContext(uploadContext.Get());
            device.WaitIdle();
            return device.QueryRuntimeStatus() == RHIDeviceRuntimeStatus::Ready;
        }

        [[nodiscard]] bool ValidateParticleGraphTargets(
            const RenderGraph& graph,
            const ViewData& view,
            const ParticleVfxRecordInput& particleVfx,
            std::string& outDiagnostic)
        {
            if (view.viewportX != 0 || view.viewportY != 0 ||
                view.viewportWidth == 0 || view.viewportHeight == 0)
            {
                outDiagnostic =
                    "Particle VFX supports only zero-origin, non-empty render viewports";
                return false;
            }

            const RHITextureDesc* const colorDesc =
                graph.GetTextureDesc(view.colorTarget);
            const RHITextureDesc* const depthDesc =
                graph.GetTextureDesc(view.depthTarget);
            const RHITextureDesc* const reactiveMaskDesc =
                graph.GetTextureDesc(view.reactiveMaskTarget);
            const RHIBufferDesc* const exposureStateDesc =
                graph.GetBufferDesc(particleVfx.exposureState);
            const auto matchesViewExtent = [&view](const RHITextureDesc* desc)
            {
                return desc != nullptr &&
                    desc->width == view.viewportWidth &&
                    desc->height == view.viewportHeight &&
                    desc->sampleCount == RHISampleCount::Count1;
            };
            if (!matchesViewExtent(colorDesc) ||
                colorDesc->format != RHIFormat::RGBA16_FLOAT ||
                !matchesViewExtent(depthDesc) ||
                depthDesc->format != RHIFormat::D32_FLOAT ||
                !matchesViewExtent(reactiveMaskDesc) ||
                reactiveMaskDesc->format != RHIFormat::R8_UNORM)
            {
                outDiagnostic =
                    "Particle VFX requires matching single-sample RGBA16F, D32 and R8 view targets";
                return false;
            }
            if (!IsSameBufferHandle(
                    particleVfx.exposureState, view.exposureState) ||
                particleVfx.exposureStateContentHash == 0 ||
                particleVfx.exposureStateContentHash !=
                    view.exposureStateContentHash ||
                !MakeExposureStateMetadata(view).IsValid() ||
                exposureStateDesc == nullptr ||
                exposureStateDesc->size != SceneTextures::ExposureStateStride ||
                exposureStateDesc->stride != SceneTextures::ExposureStateStride ||
                !HasFlag(exposureStateDesc->usage, RHIBufferUsage::Structured) ||
                !HasFlag(exposureStateDesc->usage,
                         RHIBufferUsage::ShaderResource))
            {
                outDiagnostic =
                    "Particle VFX requires the exact committed pre-exposure state for this HDR frame";
                return false;
            }

            if (particleVfx.softDepthSnapshot.texture.IsValid())
            {
                const RHITextureDesc* const softDepthDesc =
                    graph.GetTextureDesc(particleVfx.softDepthSnapshot.texture);
                if (!matchesViewExtent(softDepthDesc) ||
                    softDepthDesc->format != RHIFormat::D32_FLOAT ||
                    !HasFlag(softDepthDesc->usage,
                             RHITextureUsage::ShaderResource))
                {
                    outDiagnostic =
                        "Particle soft-depth snapshot must be a sampled D32 view target";
                    return false;
                }
            }
            else if (particleVfx.softDepthRequired)
            {
                outDiagnostic =
                    "Particle required soft depth has no graph-owned snapshot";
                return false;
            }

            if (!particleVfx.distortionRequested)
            {
                return true;
            }

            const RHITextureDesc* const distortionDesc =
                graph.GetTextureDesc(particleVfx.distortionVectors);
            const RHITextureDesc* const compositeDesc =
                graph.GetTextureDesc(particleVfx.compositedSceneColor);
            if (!matchesViewExtent(distortionDesc) ||
                distortionDesc->format != RHIFormat::RG16_FLOAT ||
                !matchesViewExtent(compositeDesc) ||
                compositeDesc->format != RHIFormat::RGBA16_FLOAT ||
                particleVfx.compositedSceneColor.index == view.colorTarget.index)
            {
                outDiagnostic =
                    "Particle distortion requires distinct matching RG16F and HDR outputs";
                return false;
            }
            return true;
        }
    } // namespace

    /** @brief Render-owned persistent resources for CPU billboard drawing. */
    class ParticleBillboardRenderer final
    {
    public:
        struct Submission
        {
            std::shared_ptr<const ParticleBillboardDrawPlan> plan;
            IRHIDevice* device = nullptr;
            RHIBufferRef constantsBuffer;
            RHIBufferRef instanceBuffer;
            RHIDescriptorSetLayoutRef descriptorSetLayout;
            RHISamplerRef linearClampSampler;
            RHITextureRef fallbackTexture;
            RHITextureViewRef fallbackTextureView;
            std::array<RHIPipelineRef, 3> pipelines{};
            RHIPipelineRef weightedBlendedPipeline;
            RHIPipelineRef distortionPipeline;
            RHIPipelineRef compositePipeline;
            uint32 frameSlot = RVX_INVALID_INDEX;
            uint64 uploadBytes = 0;
            uint64 resourceCreationCount = 0;
            uint64 pipelineCreationCount = 0;
            std::string diagnostic;

            [[nodiscard]] bool IsReady() const noexcept
            {
                return plan != nullptr && plan->valid && device != nullptr && constantsBuffer && instanceBuffer &&
                       descriptorSetLayout && linearClampSampler && fallbackTexture &&
                       fallbackTextureView && distortionPipeline && compositePipeline &&
                       frameSlot != RVX_INVALID_INDEX;
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

        void OnAdd(IRHIDevice* device)
        {
            if (m_device != device)
            {
                ResetAllResources();
                m_device = device;
            }

            // Pipeline creation includes the one-time upload and transition
            // of the descriptor fallback texture.  It must complete while
            // the pass is attached, before a swap-chain image can be
            // acquired for an engine frame.  Submitting that upload from
            // AddToGraph would consume the frame's acquire semaphore outside
            // the RenderGraph queue plan and make the plan's terminal present
            // synchronization invalid on Vulkan.
            if (m_device != nullptr && m_shaderProgramProvider != nullptr)
            {
                static_cast<void>(EnsurePipelineResources());
            }
        }

        void OnRemove() noexcept
        {
            ResetAllResources();
            m_device = nullptr;
        }

        /**
         * The feature planner must be able to prove that the exact weighted
         * particle path can be materialized before it seals a WBOIT route.
         * Do not involve a RenderGraph here: this only reaches the persistent
         * shader/pipeline cache used by Prepare().
         */
        [[nodiscard]] bool PreflightWeightedBlendedPipeline(
            std::string& outDiagnostic)
        {
            outDiagnostic.clear();
            if (EnsureWeightedBlendedPipeline())
            {
                return true;
            }

            outDiagnostic = m_unavailableReason.empty()
                ? "Particle weighted-blended graphics pipeline is unavailable"
                : m_unavailableReason;
            return false;
        }

        [[nodiscard]] Submission Prepare(
            std::shared_ptr<const ParticleBillboardDrawPlan> plan,
            const ViewData& view,
            bool softDepthAvailable,
            const std::vector<bool>& spriteTextureAvailableByBatch,
            bool weightedBlendedRequired = false)
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
            if (weightedBlendedRequired &&
                !EnsureWeightedBlendedPipeline())
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
            // Particle billboards are feature-owned draws, but their raster
            // clip convention must exactly match PipelineCache scene draws.
            constants.viewProjection = ApplyBackendClipConvention(
                view.jitteredViewProjectionMatrix,
                m_device->GetBackendType());
            // The raster position and depth reconstruction must use the same
            // jittered projection. Mixing a jittered VP with the canonical
            // inverse projection produces a frame-varying soft-depth seam.
            if (!TryGetFiniteJitteredInverseProjection(
                    view, constants.inverseProjection))
            {
                submission.diagnostic =
                    "Particle billboard requires a finite inverse jittered projection";
                return submission;
            }
            constants.cameraRight = Vec4(
                NormalizeOr(GetRightFromMatrix(view.inverseViewMatrix),
                            Vec3(1.0f, 0.0f, 0.0f)),
                0.0f);
            constants.cameraUp = Vec4(
                NormalizeOr(GetUpFromMatrix(view.inverseViewMatrix),
                            Vec3(0.0f, 1.0f, 0.0f)),
                0.0f);
            const RHIViewport viewport = view.GetRHIViewport();
            if (viewport.x != 0.0f || viewport.y != 0.0f ||
                viewport.width <= 0.0f || viewport.height <= 0.0f)
            {
                submission.diagnostic =
                    "Particle billboard requires a zero-origin viewport with a valid extent";
                return submission;
            }
            constants.inverseExtentAndReverseZ = Vec4(
                1.0f / viewport.width,
                1.0f / viewport.height,
                m_reverseZ ? 1.0f : 0.0f,
                0.0f);
            constants.viewportOriginAndTargetExtent = Vec4(
                viewport.x, viewport.y, viewport.width, viewport.height);
            if (!Upload(slot.constantsBuffer.Get(), &constants, sizeof(constants)))
            {
                submission.diagnostic =
                    "Particle billboard constants upload failed to publish";
                return submission;
            }

            slot.stagingInstances.resize(submission.plan->items.size());
            uint32 batchIndex = 0;
            for (size_t index = 0; index < submission.plan->items.size(); ++index)
            {
                while (batchIndex + 1 < submission.plan->batches.size() &&
                       index >= static_cast<size_t>(
                           submission.plan->batches[batchIndex].firstItem) +
                           submission.plan->batches[batchIndex].itemCount)
                {
                    ++batchIndex;
                }
                const ParticleBillboardDrawItem& item =
                    submission.plan->items[index];
                const bool spriteTextureAvailable =
                    batchIndex < spriteTextureAvailableByBatch.size() &&
                    spriteTextureAvailableByBatch[batchIndex];
                slot.stagingInstances[index] = {
                    .positionAndRotation = Vec4(
                        item.particle.position, item.particle.rotation),
                    .color = item.particle.color,
                    .sizeAgeAndLifetime = Vec4(
                        item.particle.size.x,
                        item.particle.size.y,
                        item.particle.age,
                        item.particle.lifetime),
                    .atlasUVOffsetScale = Vec4(
                        item.effectDescriptor.flipbook.uvOffset.x,
                        item.effectDescriptor.flipbook.uvOffset.y,
                        item.effectDescriptor.flipbook.uvScale.x,
                        item.effectDescriptor.flipbook.uvScale.y),
                    .softDepthDistortionReactive = Vec4(
                        item.effectDescriptor.softDepthFadeMeters,
                        item.effectDescriptor.softDepthContrast,
                        item.effectDescriptor.distortionStrengthPixels,
                        item.effectDescriptor.reactiveContribution),
                    .blendMode = BlendPipelineIndex(item.blendMode),
                    .flags = item.particle.flags |
                        (item.effectDescriptor.flipbook.IsBound() &&
                         spriteTextureAvailable
                             ? 0x1u
                             : 0u) |
                        (item.effectDescriptor.softDepthMode !=
                             ParticleRenderSoftDepthMode::Disabled &&
                         softDepthAvailable
                             ? 0x2u
                             : 0u),
                    .flipbookFrameIndex = item.particle.flipbookFrameIndex,
                    .packedAtlasTiles =
                        (item.effectDescriptor.flipbook.tiles.x & 0xffffu) |
                        ((item.effectDescriptor.flipbook.tiles.y & 0xffffu) << 16u),
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
            submission.device = m_device;
            submission.instanceBuffer = slot.instanceBuffer;
            submission.descriptorSetLayout = m_descriptorSetLayout;
            submission.linearClampSampler = m_linearClampSampler;
            submission.fallbackTexture = m_fallbackTexture;
            submission.fallbackTextureView = m_fallbackTextureView;
            submission.pipelines = m_pipelines;
            submission.weightedBlendedPipeline = m_weightedBlendedPipeline;
            submission.distortionPipeline = m_distortionPipeline;
            submission.compositePipeline = m_compositePipeline;
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
            std::vector<ParticleBillboardGPUInstance> stagingInstances;
            uint32 instanceCapacity = 0;
        };

        void ResetPipelineResources() noexcept
        {
            m_vertexShader.Reset();
            m_pixelShader.Reset();
            m_distortionPixelShader.Reset();
            m_compositeVertexShader.Reset();
            m_compositePixelShader.Reset();
            m_weightedBlendedPixelShader.Reset();
            m_linearClampSampler.Reset();
            m_fallbackTextureView.Reset();
            m_fallbackTexture.Reset();
            m_descriptorSetLayout.Reset();
            m_pipelineLayout.Reset();
            for (RHIPipelineRef& pipeline : m_pipelines)
            {
                pipeline.Reset();
            }
            m_distortionPipeline.Reset();
            m_compositePipeline.Reset();
            m_weightedBlendedPipeline.Reset();
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
                m_pixelShader && m_distortionPixelShader && m_compositeVertexShader &&
                m_compositePixelShader && m_linearClampSampler && m_fallbackTexture &&
                m_fallbackTextureView && m_distortionPipeline && m_compositePipeline &&
                std::all_of(m_pipelines.begin(), m_pipelines.end(),
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
            // The shared frame constants also provide inverse extent and
            // inverse projection to the soft-particle and distortion pixel
            // programs.  Advertising vertex-only visibility trips the RHI
            // reflection preflight on every native backend.
            setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer,
                                     RHIShaderStage::Vertex | RHIShaderStage::Pixel);
            setLayoutDesc.AddBinding(1, RHIBindingType::ShaderResourceBuffer,
                                     RHIShaderStage::Vertex);
            setLayoutDesc.AddBinding(2, RHIBindingType::SampledTexture,
                                     RHIShaderStage::Pixel);
            setLayoutDesc.AddBinding(3, RHIBindingType::SampledTexture,
                                     RHIShaderStage::Pixel);
            setLayoutDesc.AddBinding(4, RHIBindingType::SampledTexture,
                                     RHIShaderStage::Pixel);
            setLayoutDesc.AddBinding(5, RHIBindingType::Sampler,
                                     RHIShaderStage::Pixel);
            setLayoutDesc.AddBinding(RVX_PARTICLE_EXPOSURE_STATE_BINDING,
                                     RHIBindingType::ShaderResourceBuffer,
                                     RHIShaderStage::Pixel);
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
                request.defines = {{
                    "RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y",
                    request.backend == RHIBackendType::Vulkan ||
                            request.backend == RHIBackendType::OpenGL
                        ? "1"
                        : "0"}};
                return m_shaderProgramProvider->Load(*m_device, request);
            };
            ShaderProgramResult vertex = loadProgram(RHIShaderStage::Vertex, "VSMain");
            ShaderProgramResult pixel = loadProgram(RHIShaderStage::Pixel, "PSMain");
            ShaderProgramResult distortionPixel = loadProgram(
                RHIShaderStage::Pixel, "PSDistortionMain");
            ShaderProgramResult compositeVertex = loadProgram(
                RHIShaderStage::Vertex, "DistortionCompositeVS");
            ShaderProgramResult compositePixel = loadProgram(
                RHIShaderStage::Pixel, "DistortionCompositePS");
            if (!vertex.Succeeded() || !pixel.Succeeded() ||
                !distortionPixel.Succeeded() || !compositeVertex.Succeeded() ||
                !compositePixel.Succeeded())
            {
                m_unavailableReason = "Particle billboard cooked shader program load failed: " +
                    (!vertex.Succeeded() ? vertex.diagnostic :
                     !pixel.Succeeded() ? pixel.diagnostic :
                     !distortionPixel.Succeeded() ? distortionPixel.diagnostic :
                     !compositeVertex.Succeeded() ? compositeVertex.diagnostic :
                     compositePixel.diagnostic);
                return false;
            }
            m_vertexShader = vertex.shader;
            m_pixelShader = pixel.shader;
            m_distortionPixelShader = distortionPixel.shader;
            m_compositeVertexShader = compositeVertex.shader;
            m_compositePixelShader = compositePixel.shader;

            RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
            samplerDesc.debugName = "ParticleBillboard.LinearClampSampler";
            m_linearClampSampler = m_device->CreateSampler(samplerDesc);
            if (!m_linearClampSampler)
            {
                m_unavailableReason = "Particle billboard sampler creation failed";
                return false;
            }
            ++m_resourceCreationCount;

            RHITextureDesc fallbackDesc = RHITextureDesc::Texture2D(
                1, 1, RHIFormat::RGBA8_UNORM,
                RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
            fallbackDesc.debugName = "ParticleBillboard.FallbackTexture";
            m_fallbackTexture = m_device->CreateTexture(fallbackDesc);
            if (!m_fallbackTexture)
            {
                m_unavailableReason = "Particle billboard fallback texture creation failed";
                return false;
            }
            if (!InitializeFallbackTextureForSampling(*m_device, *m_fallbackTexture))
            {
                m_unavailableReason =
                    "Particle billboard fallback texture initialization failed";
                return false;
            }
            RHITextureViewDesc fallbackViewDesc;
            fallbackViewDesc.format = fallbackDesc.format;
            fallbackViewDesc.dimension = RHITextureDimension::Texture2D;
            fallbackViewDesc.type = RHITextureViewType::ShaderResource;
            fallbackViewDesc.debugName = "ParticleBillboard.FallbackTextureSRV";
            m_fallbackTextureView = m_device->CreateTextureView(
                m_fallbackTexture.Get(), fallbackViewDesc);
            if (!m_fallbackTextureView)
            {
                m_unavailableReason =
                    "Particle billboard fallback texture-view creation failed";
                return false;
            }
            ++m_resourceCreationCount;

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
                pipelineDesc.blendState.independentBlendEnable = true;
                pipelineDesc.blendState.renderTargets[1] =
                    RHIRenderTargetBlendState::Additive();
                pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
                pipelineDesc.numRenderTargets = 2;
                pipelineDesc.renderTargetFormats[0] = RHIFormat::RGBA16_FLOAT;
                pipelineDesc.renderTargetFormats[1] = RHIFormat::R8_UNORM;
                pipelineDesc.depthStencilFormat = RHIFormat::D32_FLOAT;
                pipelineDesc.sampleCount = RHISampleCount::Count1;
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

            RHIGraphicsPipelineDesc distortionPipelineDesc;
            distortionPipelineDesc.vertexShader = m_vertexShader.Get();
            distortionPipelineDesc.pixelShader = m_distortionPixelShader.Get();
            distortionPipelineDesc.rasterizerState = RHIRasterizerState::NoCull();
            distortionPipelineDesc.depthStencilState = RHIDepthStencilState::ReadOnly();
            distortionPipelineDesc.depthStencilState.depthCompareOp = m_reverseZ
                ? RHICompareOp::GreaterEqual : RHICompareOp::LessEqual;
            distortionPipelineDesc.blendState.renderTargets[0] =
                RHIRenderTargetBlendState::Additive();
            distortionPipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
            distortionPipelineDesc.numRenderTargets = 1;
            distortionPipelineDesc.renderTargetFormats[0] = RHIFormat::RG16_FLOAT;
            distortionPipelineDesc.depthStencilFormat = RHIFormat::D32_FLOAT;
            distortionPipelineDesc.sampleCount = RHISampleCount::Count1;
            distortionPipelineDesc.pipelineLayout = m_pipelineLayout.Get();
            distortionPipelineDesc.debugName = "ParticleBillboard.DistortionPipeline";
            m_distortionPipeline = m_device->CreateGraphicsPipeline(
                distortionPipelineDesc);
            if (!m_distortionPipeline)
            {
                m_unavailableReason =
                    "Particle billboard distortion graphics pipeline creation failed";
                return false;
            }
            ++m_pipelineCreationCount;

            RHIGraphicsPipelineDesc compositePipelineDesc;
            compositePipelineDesc.vertexShader = m_compositeVertexShader.Get();
            compositePipelineDesc.pixelShader = m_compositePixelShader.Get();
            compositePipelineDesc.rasterizerState = RHIRasterizerState::NoCull();
            compositePipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
            compositePipelineDesc.blendState.renderTargets[0] =
                RHIRenderTargetBlendState::Opaque();
            compositePipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
            compositePipelineDesc.numRenderTargets = 1;
            compositePipelineDesc.renderTargetFormats[0] = RHIFormat::RGBA16_FLOAT;
            compositePipelineDesc.depthStencilFormat = RHIFormat::Unknown;
            compositePipelineDesc.sampleCount = RHISampleCount::Count1;
            compositePipelineDesc.pipelineLayout = m_pipelineLayout.Get();
            compositePipelineDesc.debugName = "ParticleBillboard.DistortionCompositePipeline";
            m_compositePipeline = m_device->CreateGraphicsPipeline(
                compositePipelineDesc);
            if (!m_compositePipeline)
            {
                m_unavailableReason =
                    "Particle billboard distortion composite pipeline creation failed";
                return false;
            }
            ++m_pipelineCreationCount;
            return true;
        }

        [[nodiscard]] bool EnsureWeightedBlendedPipeline()
        {
            if (m_weightedBlendedPipeline)
            {
                return true;
            }
            if (!EnsurePipelineResources() || m_device == nullptr ||
                m_shaderProgramProvider == nullptr)
            {
                return false;
            }

            ShaderProgramRequest request;
            request.logicalAssetId = "Particle/ParticleBillboard.hlsl";
            request.stage = RHIShaderStage::Pixel;
            request.entryPoint = "PSWeightedBlendedOIT";
            request.backend = m_device->GetBackendType();
            request.defines = {{
                "RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y",
                request.backend == RHIBackendType::Vulkan ||
                        request.backend == RHIBackendType::OpenGL
                    ? "1"
                    : "0"}};
            ShaderProgramResult pixel = m_shaderProgramProvider->Load(
                *m_device, request);
            if (!pixel.Succeeded())
            {
                m_unavailableReason =
                    "Particle weighted-blended cooked shader program load failed: " +
                    pixel.diagnostic;
                return false;
            }

            RHIGraphicsPipelineDesc pipelineDesc;
            pipelineDesc.vertexShader = m_vertexShader.Get();
            pipelineDesc.pixelShader = pixel.shader.Get();
            pipelineDesc.rasterizerState = RHIRasterizerState::NoCull();
            pipelineDesc.depthStencilState = RHIDepthStencilState::ReadOnly();
            pipelineDesc.depthStencilState.depthCompareOp = m_reverseZ
                ? RHICompareOp::GreaterEqual : RHICompareOp::LessEqual;
            pipelineDesc.blendState =
                PipelineCache::BuildWeightedBlendedOITAccumulationBlendState();
            pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
            pipelineDesc.numRenderTargets = 2;
            pipelineDesc.renderTargetFormats[0] =
                PipelineCache::GetWeightedBlendedOITAccumulationFormat();
            pipelineDesc.renderTargetFormats[1] =
                PipelineCache::GetWeightedBlendedOITRevealageFormat();
            pipelineDesc.depthStencilFormat = RHIFormat::D32_FLOAT;
            pipelineDesc.sampleCount = RHISampleCount::Count1;
            pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
            pipelineDesc.debugName =
                "ParticleBillboard.WeightedBlendedOITAccumulationPipeline";
            RHIPipelineRef pipeline = m_device->CreateGraphicsPipeline(
                pipelineDesc);
            if (!pipeline)
            {
                m_unavailableReason =
                    "Particle weighted-blended graphics pipeline creation failed";
                return false;
            }
            m_weightedBlendedPixelShader = std::move(pixel.shader);
            m_weightedBlendedPipeline = std::move(pipeline);
            ++m_pipelineCreationCount;
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
        RHIShaderRef m_distortionPixelShader;
        RHIShaderRef m_compositeVertexShader;
        RHIShaderRef m_compositePixelShader;
        RHIShaderRef m_weightedBlendedPixelShader;
        RHISamplerRef m_linearClampSampler;
        RHITextureRef m_fallbackTexture;
        RHITextureViewRef m_fallbackTextureView;
        std::array<RHIPipelineRef, 3> m_pipelines{};
        RHIPipelineRef m_distortionPipeline;
        RHIPipelineRef m_compositePipeline;
        RHIPipelineRef m_weightedBlendedPipeline;
        uint64 m_resourceCreationCount = 0;
        uint64 m_pipelineCreationCount = 0;
        std::string m_unavailableReason;
    };

    struct ParticleTextureBinding
    {
        RenderResourceHandle resource{};
        RHITextureRef texture;
        RHITextureAccessSnapshot accessSnapshot{};
        bool required = false;
        RGTextureHandle graphTexture{};
        RGTextureViewHandle graphView{};
    };

    enum class ParticleFeatureGraphStage : uint8
    {
        LegacyDirect = 0,
        WeightedBlended,
        Direct,
        Distortion,
        DistortionComposite,
    };

    struct ParticleGraphPassData;

    /** Translation-unit definition of the public opaque prepared handle. */
    class ParticleFeaturePreparedRecording final
    {
    public:
        std::shared_ptr<const TranslucencyFramePlan> translucencyPlan;
        ParticleBillboardRenderer::Submission submission{};
        RenderPassExecutionData execution{};
        ParticleFeaturePassStats initialStats{};
        std::vector<ParticleTextureBinding> textureBindings;
        ParticleVfxRecordInput particleVfx{};
        PipelineCache* pipelineCache = nullptr;
        RenderPassRecordResults* results = nullptr;
        ParticleFeaturePass* owner = nullptr;
        const ParticleRenderSnapshot* snapshot = nullptr;
        uint64 snapshotSequence = 0;
        std::vector<uint32> weightedBatchIndices;
        std::vector<uint32> directBatchIndices;
        RGTextureHandle weightedAccumulation{};
        RGTextureHandle weightedRevealage{};
        std::shared_ptr<ParticleGraphPassData> graphResources;
        bool contextValid = false;
        bool weightedRecorded = false;
        bool directRecorded = false;
        bool commonResourcesPrepared = false;
    };

    namespace
    {

        [[nodiscard]] bool ResolveParticleTextureBindings(
            const ParticleBillboardDrawPlan& plan,
            const RenderResourceRegistry* registry,
            std::vector<ParticleTextureBinding>& outBindings,
            ParticleFeaturePassStats& inOutStats,
            std::string& outDiagnostic)
        {
            outBindings.clear();
            outBindings.reserve(plan.batches.size());
            for (const ParticleBillboardDrawBatch& batch : plan.batches)
            {
                if (batch.firstItem >= plan.items.size())
                {
                    outDiagnostic = "Particle billboard batch points outside its immutable draw plan";
                    return false;
                }
                const ParticleRenderFlipbookDescriptor& flipbook =
                    plan.items[batch.firstItem].effectDescriptor.flipbook;
                ParticleTextureBinding binding;
                binding.resource = flipbook.texture;
                binding.required = flipbook.required;
                if (!flipbook.IsBound())
                {
                    outBindings.push_back(std::move(binding));
                    continue;
                }

                const bool exactIdentity = registry != nullptr &&
                    registry->GetExactKind(flipbook.texture) ==
                        RenderResourceKind::Texture &&
                    registry->GetExactAssetId(flipbook.texture) ==
                        flipbook.textureAssetId &&
                    registry->GetContentRevision(flipbook.texture) ==
                        flipbook.committedContentRevision;
                const RenderTextureResourceData* const texture = exactIdentity
                    ? registry->ResolveTexture(flipbook.texture)
                    : nullptr;
                if (texture == nullptr || !texture->texture)
                {
                    if (flipbook.required)
                    {
                        outDiagnostic =
                            "Particle required flipbook does not resolve to the exact committed texture generation";
                        return false;
                    }
                    ++inOutStats.fallbackCount;
                    outBindings.push_back(std::move(binding));
                    continue;
                }
                binding.texture = texture->texture;
                binding.accessSnapshot = texture->accessSnapshot;
                inOutStats.flipbookValidated = true;
                outBindings.push_back(std::move(binding));
            }
            return true;
        }

    } // namespace

    struct ParticleGraphPassData
        {
            ParticleBillboardRenderer::Submission submission{};
            RenderPassExecutionData execution{};
            ParticleFeaturePassStats initialStats{};
            RGBufferHandle constantsHandle{};
            RGBufferHandle instanceHandle{};
            RGTextureHandle colorHandle{};
            RGTextureHandle depthHandle{};
            RGTextureHandle reactiveMaskHandle{};
            RGTextureHandle softDepthHandle{};
            RGTextureHandle froxelVolumetricLightingHandle{};
            RGBufferHandle exposureStateReadHandle{};
            RGTextureViewHandle colorView{};
            RGTextureViewHandle depthView{};
            RGTextureViewHandle reactiveMaskView{};
            RGTextureViewHandle softDepthView{};
            RGTextureViewHandle froxelVolumetricLightingView{};
            RHITextureViewRef neutralFroxelVolumetricLightingView{};
            uint64 exposureStateContentHash = 0;
            RGTextureViewHandle distortionView{};
            RGTextureViewHandle compositeSourceView{};
            RGTextureViewHandle compositeTargetView{};
            RGTextureHandle accumulationHandle{};
            RGTextureHandle revealageHandle{};
            RGTextureViewHandle accumulationView{};
            RGTextureViewHandle revealageView{};
            std::vector<ParticleTextureBinding> textureBindings;
            std::vector<uint32> batchIndices;
            ParticleVfxRecordInput particleVfx{};
            PipelineCache* pipelineCache = nullptr;
            RenderPassRecordResults* results = nullptr;
            bool contextValid = false;
            ParticleFeatureGraphStage stage =
                ParticleFeatureGraphStage::LegacyDirect;
            ParticleFeaturePass* owner = nullptr;
        };

    namespace
    {

        [[nodiscard]] bool IsExactFroxelVolumetricTexture(
            const RHITextureDesc* textureDesc,
            const VolumetricLightingRecordInput& volumetricLighting) noexcept
        {
            const FroxelGridMetadata& grid = volumetricLighting.froxelGrid;
            return textureDesc != nullptr && !volumetricLighting.identityRoute &&
                grid.tileSizePixels != 0 && grid.froxelWidth != 0 &&
                grid.froxelHeight != 0 && grid.depthSliceCount != 0 &&
                grid.froxelCount == static_cast<uint64>(grid.froxelWidth) *
                    grid.froxelHeight * grid.depthSliceCount &&
                textureDesc->dimension == RHITextureDimension::Texture3D &&
                textureDesc->format == RHIFormat::RGBA16_FLOAT &&
                textureDesc->width == grid.froxelWidth &&
                textureDesc->height == grid.froxelHeight &&
                textureDesc->depth == grid.depthSliceCount &&
                textureDesc->mipLevels == 1 && textureDesc->arraySize == 1 &&
                textureDesc->sampleCount == RHISampleCount::Count1 &&
                HasFlag(textureDesc->usage, RHITextureUsage::ShaderResource);
        }

        [[nodiscard]] bool IsCompatibleFroxelVolumetricLighting(
            const VolumetricLightingRecordInput& volumetricLighting,
            const RenderPassRecordIdentity& identity) noexcept
        {
            const FroxelGridMetadata& grid = volumetricLighting.froxelGrid;
            const bool gridValid = grid.tileSizePixels != 0 &&
                grid.froxelWidth != 0 && grid.froxelHeight != 0 &&
                grid.depthSliceCount != 0 &&
                grid.froxelCount == static_cast<uint64>(grid.froxelWidth) *
                    grid.froxelHeight * grid.depthSliceCount;
            if (volumetricLighting.identity != identity ||
                volumetricLighting.planIdentity == 0 || !gridValid)
            {
                return false;
            }
            return volumetricLighting.identityRoute
                ? !volumetricLighting.integratedScatteringTransmittance.IsValid()
                : HasCurrentGraphProvenance(
                      volumetricLighting.integratedScatteringTransmittance,
                      identity);
        }

        [[nodiscard]] bool PrepareGraphResources(
            ParticleGraphPassData& data,
            RenderGraphBuilder& builder,
            bool prepareDirectTargets)
        {
            if (!data.contextValid || !data.submission.IsReady() ||
                data.pipelineCache == nullptr ||
                !data.particleVfx.IsCompatibleWith(
                    data.execution.identity,
                    RenderOutputExtent{data.execution.view.viewportWidth,
                                       data.execution.view.viewportHeight}) ||
                !IsSameBufferHandle(data.particleVfx.exposureState,
                                    data.execution.view.exposureState) ||
                data.particleVfx.exposureStateContentHash !=
                    data.execution.view.exposureStateContentHash ||
                !MakeExposureStateMetadata(data.execution.view).IsValid() ||
                data.execution.view.exposureStateContentHash == 0 ||
                !IsCompatibleFroxelVolumetricLighting(
                    data.execution.volumetricLighting, data.execution.identity) ||
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
            const RHIBufferDesc* const exposureStateDesc =
                builder.GetBufferDesc(data.particleVfx.exposureState);
            if (view.viewportX != 0 || view.viewportY != 0 ||
                view.viewportWidth == 0 || view.viewportHeight == 0 ||
                colorDesc == nullptr || colorDesc->format != RHIFormat::RGBA16_FLOAT ||
                colorDesc->width != view.viewportWidth ||
                colorDesc->height != view.viewportHeight ||
                exposureStateDesc == nullptr ||
                exposureStateDesc->size != SceneTextures::ExposureStateStride ||
                exposureStateDesc->stride != SceneTextures::ExposureStateStride ||
                !HasFlag(exposureStateDesc->usage, RHIBufferUsage::Structured) ||
                !HasFlag(exposureStateDesc->usage,
                         RHIBufferUsage::ShaderResource))
            {
                return false;
            }
            const VolumetricLightingRecordInput& volumetricLighting =
                data.execution.volumetricLighting;
            const RHITextureDesc* const froxelVolumetricLightingDesc =
                volumetricLighting.identityRoute ? nullptr :
                builder.GetTextureDesc(
                    volumetricLighting.integratedScatteringTransmittance);
            if (!volumetricLighting.identityRoute &&
                !IsExactFroxelVolumetricTexture(
                    froxelVolumetricLightingDesc, volumetricLighting))
            {
                return false;
            }
            if (!Retain(builder, data.submission.constantsBuffer.Get()) ||
                !Retain(builder, data.submission.instanceBuffer.Get()) ||
                !Retain(builder, data.submission.descriptorSetLayout.Get()) ||
                !Retain(builder, data.submission.linearClampSampler.Get()) ||
                !Retain(builder, data.submission.fallbackTexture.Get()) ||
                !Retain(builder, data.submission.fallbackTextureView.Get()) ||
                !Retain(builder, data.submission.distortionPipeline.Get()) ||
                !Retain(builder, data.submission.compositePipeline.Get()) ||
                (volumetricLighting.identityRoute &&
                 !Retain(builder,
                         data.pipelineCache->GetNeutralVolumetricLightingView().Get())) ||
                (data.submission.weightedBlendedPipeline &&
                 !Retain(builder,
                         data.submission.weightedBlendedPipeline.Get())))
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
                                            RHIShaderStage::Vertex),
                {"ParticleFeatureConstants",
                 RGImportedResourceClass::Persistent});
            data.instanceHandle = builder.ImportBuffer(
                data.submission.instanceBuffer,
                MakeRHIBufferAccessSnapshot(RHIResourceState::ShaderResource,
                                            RHIShaderStage::Vertex),
                {"ParticleFeatureInstances",
                 RGImportedResourceClass::Persistent});
            data.constantsHandle = builder.Read(
                data.constantsHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                      RHIShaderStage::Vertex));
            data.instanceHandle = builder.Read(
                data.instanceHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Vertex));
            data.exposureStateReadHandle = builder.Read(
                data.particleVfx.exposureState,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Pixel));
            data.exposureStateContentHash =
                data.particleVfx.exposureStateContentHash;
            if (!data.constantsHandle.IsValid() || !data.instanceHandle.IsValid() ||
                !data.exposureStateReadHandle.IsValid() ||
                data.exposureStateContentHash == 0)
            {
                return false;
            }
            if (volumetricLighting.identityRoute)
            {
                data.neutralFroxelVolumetricLightingView =
                    data.pipelineCache->GetNeutralVolumetricLightingView();
                if (!data.neutralFroxelVolumetricLightingView)
                {
                    return false;
                }
            }
            else
            {
                data.froxelVolumetricLightingHandle = builder.Read(
                    volumetricLighting.integratedScatteringTransmittance,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                RHITextureViewDesc froxelViewDesc;
                froxelViewDesc.format = RHIFormat::RGBA16_FLOAT;
                froxelViewDesc.dimension = RHITextureDimension::Texture3D;
                froxelViewDesc.type = RHITextureViewType::ShaderResource;
                froxelViewDesc.debugName =
                    "ParticleBillboard.FroxelVolumetricLightingSRV";
                data.froxelVolumetricLightingView = builder.CreateTextureView(
                    data.froxelVolumetricLightingHandle, froxelViewDesc);
                data.froxelVolumetricLightingView = builder.Read(
                    data.froxelVolumetricLightingView,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                if (!data.froxelVolumetricLightingHandle.IsValid() ||
                    !data.froxelVolumetricLightingView.IsValid())
                {
                    return false;
                }
            }

            for (ParticleTextureBinding& binding : data.textureBindings)
            {
                if (!binding.texture)
                {
                    continue;
                }
                binding.graphTexture = builder.ImportTexture(
                    binding.texture, binding.accessSnapshot);
                if (!binding.graphTexture.IsValid())
                {
                    return false;
                }
                binding.graphTexture = builder.Read(
                    binding.graphTexture,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                RHITextureViewDesc spriteViewDesc;
                spriteViewDesc.dimension = RHITextureDimension::Texture2D;
                spriteViewDesc.type = RHITextureViewType::ShaderResource;
                spriteViewDesc.debugName = "ParticleBillboard.SpriteSRV";
                binding.graphView = builder.CreateTextureView(
                    binding.graphTexture, spriteViewDesc);
                binding.graphView = builder.Read(
                    binding.graphView,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                if (!binding.graphView.IsValid())
                {
                    return false;
                }
                builder.SetExportState(
                    binding.graphTexture, RHIResourceState::ShaderResource);
                if (data.results != nullptr)
                {
                    const auto existing = std::find_if(
                        data.results->externalTextureAccesses.begin(),
                        data.results->externalTextureAccesses.end(),
                        [&binding](const RenderGraphExternalTextureAccess& access)
                        {
                            return access.resource == binding.resource;
                        });
                    if (existing == data.results->externalTextureAccesses.end())
                    {
                        data.results->externalTextureAccesses.push_back(
                            {binding.resource, binding.graphTexture});
                    }
                }
            }

            if (prepareDirectTargets)
            {
                data.colorHandle = view.colorTarget;
                RHITextureViewDesc colorViewDesc;
                colorViewDesc.format = colorDesc->format;
                colorViewDesc.dimension = colorDesc->dimension;
                colorViewDesc.subresourceRange = RHISubresourceRange::All();
                colorViewDesc.type = RHITextureViewType::RenderTarget;
                colorViewDesc.debugName = "ParticleBillboard.ColorRTV";
                data.colorView = builder.CreateTextureView(
                    data.colorHandle, colorViewDesc);
                data.colorView = builder.ReadWrite(
                    data.colorView,
                    MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                     RHIShaderStage::Pixel));
                if (!data.colorView.IsValid())
                {
                    return false;
                }

                if (!view.reactiveMaskTarget.IsValid() ||
                    !HasCurrentGraphProvenance(view.reactiveMaskTarget,
                                               data.execution.identity))
                {
                    return false;
                }
                const RHITextureDesc* const reactiveMaskDesc =
                    builder.GetTextureDesc(view.reactiveMaskTarget);
                if (reactiveMaskDesc == nullptr ||
                    reactiveMaskDesc->format != RHIFormat::R8_UNORM ||
                    reactiveMaskDesc->width != colorDesc->width ||
                    reactiveMaskDesc->height != colorDesc->height)
                {
                    return false;
                }
                data.reactiveMaskHandle = view.reactiveMaskTarget;
                RHITextureViewDesc reactiveMaskViewDesc;
                reactiveMaskViewDesc.format = reactiveMaskDesc->format;
                reactiveMaskViewDesc.dimension = reactiveMaskDesc->dimension;
                reactiveMaskViewDesc.type = RHITextureViewType::RenderTarget;
                reactiveMaskViewDesc.debugName =
                    "ParticleBillboard.ReactiveMaskRTV";
                data.reactiveMaskView = builder.CreateTextureView(
                    data.reactiveMaskHandle, reactiveMaskViewDesc);
                data.reactiveMaskView = builder.ReadWrite(
                    data.reactiveMaskView,
                    MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                     RHIShaderStage::Pixel));
                if (!data.reactiveMaskView.IsValid())
                {
                    return false;
                }
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

            if (data.particleVfx.softDepthRequested &&
                data.particleVfx.softDepthSnapshot.texture.IsValid())
            {
                const RHITextureDesc* const softDepthDesc = builder.GetTextureDesc(
                    data.particleVfx.softDepthSnapshot.texture);
                if (softDepthDesc == nullptr ||
                    softDepthDesc->format != RHIFormat::D32_FLOAT ||
                    softDepthDesc->width != colorDesc->width ||
                    softDepthDesc->height != colorDesc->height)
                {
                    return false;
                }
                data.softDepthHandle = builder.Read(
                    data.particleVfx.softDepthSnapshot.texture,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                RHITextureViewDesc softDepthViewDesc;
                softDepthViewDesc.format = softDepthDesc->format;
                softDepthViewDesc.dimension = softDepthDesc->dimension;
                softDepthViewDesc.subresourceRange.aspect =
                    RHITextureAspect::Depth;
                softDepthViewDesc.type = RHITextureViewType::ShaderResource;
                softDepthViewDesc.debugName = "ParticleBillboard.SoftDepthSRV";
                data.softDepthView = builder.CreateTextureView(
                    data.softDepthHandle, softDepthViewDesc);
                data.softDepthView = builder.Read(
                    data.softDepthView,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                if (!data.softDepthHandle.IsValid() || !data.softDepthView.IsValid())
                {
                    return false;
                }
            }
            if (data.particleVfx.softDepthRequired && !data.softDepthView.IsValid())
            {
                return false;
            }
            return true;
        }

        [[nodiscard]] bool PrepareDirectGraphTargets(
            ParticleGraphPassData& data,
            RenderGraphBuilder& builder,
            const ParticleDirectRecordTargets& targets)
        {
            if (!data.contextValid || !targets.hdrSceneColor.IsValid() ||
                !targets.reactiveMask.IsValid() ||
                !HasCurrentGraphProvenance(
                    targets.hdrSceneColor, data.execution.identity) ||
                !HasCurrentGraphProvenance(
                    targets.reactiveMask, data.execution.identity))
            {
                return false;
            }
            const RHITextureDesc* const colorDesc =
                builder.GetTextureDesc(targets.hdrSceneColor);
            const RHITextureDesc* const reactiveMaskDesc =
                builder.GetTextureDesc(targets.reactiveMask);
            if (colorDesc == nullptr || reactiveMaskDesc == nullptr ||
                colorDesc->format != RHIFormat::RGBA16_FLOAT ||
                reactiveMaskDesc->format != RHIFormat::R8_UNORM ||
                colorDesc->width != data.execution.view.viewportWidth ||
                colorDesc->height != data.execution.view.viewportHeight ||
                reactiveMaskDesc->width != colorDesc->width ||
                reactiveMaskDesc->height != colorDesc->height ||
                colorDesc->sampleCount != RHISampleCount::Count1 ||
                reactiveMaskDesc->sampleCount != RHISampleCount::Count1)
            {
                return false;
            }

            data.colorHandle = targets.hdrSceneColor;
            RHITextureViewDesc colorViewDesc;
            colorViewDesc.format = RHIFormat::RGBA16_FLOAT;
            colorViewDesc.dimension = colorDesc->dimension;
            colorViewDesc.type = RHITextureViewType::RenderTarget;
            colorViewDesc.debugName = "ParticleBillboard.DirectColorRTV";
            data.colorView = builder.CreateTextureView(
                data.colorHandle, colorViewDesc);
            data.colorView = builder.ReadWrite(
                data.colorView,
                MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                 RHIShaderStage::Pixel));

            data.reactiveMaskHandle = targets.reactiveMask;
            RHITextureViewDesc reactiveViewDesc;
            reactiveViewDesc.format = RHIFormat::R8_UNORM;
            reactiveViewDesc.dimension = reactiveMaskDesc->dimension;
            reactiveViewDesc.type = RHITextureViewType::RenderTarget;
            reactiveViewDesc.debugName =
                "ParticleBillboard.DirectReactiveMaskRTV";
            data.reactiveMaskView = builder.CreateTextureView(
                data.reactiveMaskHandle, reactiveViewDesc);
            data.reactiveMaskView = builder.ReadWrite(
                data.reactiveMaskView,
                MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                 RHIShaderStage::Pixel));
            return data.colorView.IsValid() &&
                   data.reactiveMaskView.IsValid();
        }

        [[nodiscard]] bool PrepareWeightedBlendedGraphTargets(
            ParticleGraphPassData& data,
            RenderGraphBuilder& builder,
            const ParticleWeightedBlendedRecordTargets& targets)
        {
            if (!data.contextValid ||
                !data.submission.weightedBlendedPipeline ||
                !targets.accumulation.IsValid() ||
                !targets.revealage.IsValid() ||
                targets.accumulation.index == targets.revealage.index ||
                targets.accumulation.index ==
                    data.execution.view.colorTarget.index ||
                targets.accumulation.index ==
                    data.execution.view.reactiveMaskTarget.index ||
                targets.accumulation.index ==
                    data.execution.view.depthTarget.index ||
                targets.revealage.index ==
                    data.execution.view.colorTarget.index ||
                targets.revealage.index ==
                    data.execution.view.reactiveMaskTarget.index ||
                targets.revealage.index ==
                    data.execution.view.depthTarget.index ||
                !HasCurrentGraphProvenance(
                    targets.accumulation, data.execution.identity) ||
                !HasCurrentGraphProvenance(
                    targets.revealage, data.execution.identity))
            {
                return false;
            }
            const RHITextureDesc* const accumulationDesc =
                builder.GetTextureDesc(targets.accumulation);
            const RHITextureDesc* const revealageDesc =
                builder.GetTextureDesc(targets.revealage);
            if (accumulationDesc == nullptr || revealageDesc == nullptr ||
                accumulationDesc->format !=
                    PipelineCache::GetWeightedBlendedOITAccumulationFormat() ||
                revealageDesc->format !=
                    PipelineCache::GetWeightedBlendedOITRevealageFormat() ||
                accumulationDesc->width != data.execution.view.viewportWidth ||
                accumulationDesc->height != data.execution.view.viewportHeight ||
                revealageDesc->width != accumulationDesc->width ||
                revealageDesc->height != accumulationDesc->height ||
                accumulationDesc->sampleCount != RHISampleCount::Count1 ||
                revealageDesc->sampleCount != RHISampleCount::Count1)
            {
                return false;
            }

            data.accumulationHandle = targets.accumulation;
            RHITextureViewDesc accumulationViewDesc;
            accumulationViewDesc.format = accumulationDesc->format;
            accumulationViewDesc.dimension = accumulationDesc->dimension;
            accumulationViewDesc.type = RHITextureViewType::RenderTarget;
            accumulationViewDesc.debugName =
                "ParticleBillboard.WeightedAccumulationRTV";
            data.accumulationView = builder.CreateTextureView(
                data.accumulationHandle, accumulationViewDesc);
            data.accumulationView = builder.ReadWrite(
                data.accumulationView,
                MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                 RHIShaderStage::Pixel));

            data.revealageHandle = targets.revealage;
            RHITextureViewDesc revealageViewDesc;
            revealageViewDesc.format = revealageDesc->format;
            revealageViewDesc.dimension = revealageDesc->dimension;
            revealageViewDesc.type = RHITextureViewType::RenderTarget;
            revealageViewDesc.debugName =
                "ParticleBillboard.WeightedRevealageRTV";
            data.revealageView = builder.CreateTextureView(
                data.revealageHandle, revealageViewDesc);
            data.revealageView = builder.ReadWrite(
                data.revealageView,
                MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                 RHIShaderStage::Pixel));
            return data.accumulationView.IsValid() &&
                   data.revealageView.IsValid();
        }

        [[nodiscard]] bool PrepareDistortionGraphResources(
            ParticleGraphPassData& data,
            RenderGraphBuilder& builder)
        {
            if (!data.contextValid || !data.submission.IsReady() ||
                !data.particleVfx.distortionRequested ||
                !data.particleVfx.distortionVectors.IsValid() ||
                !data.exposureStateReadHandle.IsValid() ||
                data.exposureStateContentHash == 0 ||
                !data.depthView.IsValid())
            {
                return false;
            }
            data.constantsHandle = builder.Read(
                data.constantsHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                      RHIShaderStage::Vertex));
            data.instanceHandle = builder.Read(
                data.instanceHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Vertex));
            data.exposureStateReadHandle = builder.Read(
                data.exposureStateReadHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Pixel));
            data.depthView = builder.Read(
                data.depthView,
                MakeRGAccessDesc(RHIResourceState::DepthRead,
                                 RHIShaderStage::Vertex | RHIShaderStage::Pixel));
            for (ParticleTextureBinding& binding : data.textureBindings)
            {
                if (binding.graphView.IsValid())
                {
                    binding.graphView = builder.Read(
                        binding.graphView,
                        MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                         RHIShaderStage::Pixel));
                }
            }
            if (data.softDepthView.IsValid())
            {
                data.softDepthView = builder.Read(
                    data.softDepthView,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
            }
            RHITextureViewDesc distortionViewDesc;
            distortionViewDesc.format = RHIFormat::RG16_FLOAT;
            distortionViewDesc.dimension = RHITextureDimension::Texture2D;
            distortionViewDesc.type = RHITextureViewType::RenderTarget;
            distortionViewDesc.debugName = "ParticleBillboard.DistortionRTV";
            data.distortionView = builder.CreateTextureView(
                data.particleVfx.distortionVectors, distortionViewDesc);
            data.distortionView = builder.Write(
                data.distortionView,
                MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                 RHIShaderStage::Pixel));
            return data.constantsHandle.IsValid() && data.instanceHandle.IsValid() &&
                data.exposureStateReadHandle.IsValid() &&
                data.exposureStateContentHash != 0 && data.distortionView.IsValid();
        }

        [[nodiscard]] bool PrepareReusedCommonGraphResources(
            ParticleGraphPassData& data,
            RenderGraphBuilder& builder)
        {
            if (!data.contextValid || !data.submission.IsReady() ||
                !data.constantsHandle.IsValid() ||
                !data.instanceHandle.IsValid() ||
                !data.exposureStateReadHandle.IsValid() ||
                data.exposureStateContentHash == 0 ||
                !data.depthView.IsValid())
            {
                return false;
            }
            data.constantsHandle = builder.Read(
                data.constantsHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                      RHIShaderStage::Vertex));
            data.instanceHandle = builder.Read(
                data.instanceHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Vertex));
            data.exposureStateReadHandle = builder.Read(
                data.exposureStateReadHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Pixel));
            data.depthView = builder.Read(
                data.depthView,
                MakeRGAccessDesc(RHIResourceState::DepthRead,
                                 RHIShaderStage::Vertex |
                                     RHIShaderStage::Pixel));
            for (ParticleTextureBinding& binding : data.textureBindings)
            {
                if (binding.graphView.IsValid())
                {
                    binding.graphView = builder.Read(
                        binding.graphView,
                        MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                         RHIShaderStage::Pixel));
                }
            }
            if (data.softDepthView.IsValid())
            {
                data.softDepthView = builder.Read(
                    data.softDepthView,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
            }
            if (data.execution.volumetricLighting.identityRoute)
            {
                if (!data.neutralFroxelVolumetricLightingView ||
                    !Retain(builder,
                            data.neutralFroxelVolumetricLightingView.Get()))
                {
                    return false;
                }
            }
            else
            {
                data.froxelVolumetricLightingView = builder.Read(
                    data.froxelVolumetricLightingView,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                if (!data.froxelVolumetricLightingHandle.IsValid() ||
                    !data.froxelVolumetricLightingView.IsValid())
                {
                    return false;
                }
            }
            return data.constantsHandle.IsValid() &&
                   data.instanceHandle.IsValid() &&
                   data.exposureStateReadHandle.IsValid() &&
                   data.exposureStateContentHash != 0 &&
                   data.depthView.IsValid() &&
                   (data.execution.volumetricLighting.identityRoute
                       ? static_cast<bool>(
                             data.neutralFroxelVolumetricLightingView)
                       : data.froxelVolumetricLightingHandle.IsValid() &&
                           data.froxelVolumetricLightingView.IsValid()) &&
                   (!data.particleVfx.softDepthRequired ||
                    data.softDepthView.IsValid());
        }

        [[nodiscard]] bool PrepareDistortionCompositeGraphResources(
            ParticleGraphPassData& data,
            RenderGraphBuilder& builder)
        {
            if (!data.contextValid || !data.submission.IsReady() ||
                !data.particleVfx.distortionRequested ||
                !data.exposureStateReadHandle.IsValid() ||
                data.exposureStateContentHash == 0 ||
                !data.colorHandle.IsValid() ||
                !data.particleVfx.distortionVectors.IsValid() ||
                !data.particleVfx.compositedSceneColor.IsValid())
            {
                return false;
            }
            data.constantsHandle = builder.Read(
                data.constantsHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                      RHIShaderStage::Vertex));
            data.instanceHandle = builder.Read(
                data.instanceHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Vertex));
            data.exposureStateReadHandle = builder.Read(
                data.exposureStateReadHandle,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Pixel));

            data.colorHandle = builder.Read(
                data.colorHandle,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            RHITextureViewDesc sourceViewDesc;
            sourceViewDesc.format = RHIFormat::RGBA16_FLOAT;
            sourceViewDesc.dimension = RHITextureDimension::Texture2D;
            sourceViewDesc.type = RHITextureViewType::ShaderResource;
            sourceViewDesc.debugName = "ParticleDistortionComposite.SourceSRV";
            data.compositeSourceView = builder.CreateTextureView(
                data.colorHandle, sourceViewDesc);
            data.compositeSourceView = builder.Read(
                data.compositeSourceView,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));

            RGTextureHandle distortionHandle = builder.Read(
                data.particleVfx.distortionVectors,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            RHITextureViewDesc distortionViewDesc;
            distortionViewDesc.format = RHIFormat::RG16_FLOAT;
            distortionViewDesc.dimension = RHITextureDimension::Texture2D;
            distortionViewDesc.type = RHITextureViewType::ShaderResource;
            distortionViewDesc.debugName = "ParticleDistortionComposite.VectorSRV";
            data.distortionView = builder.CreateTextureView(
                distortionHandle, distortionViewDesc);
            data.distortionView = builder.Read(
                data.distortionView,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));

            RHITextureViewDesc targetViewDesc;
            targetViewDesc.format = RHIFormat::RGBA16_FLOAT;
            targetViewDesc.dimension = RHITextureDimension::Texture2D;
            targetViewDesc.type = RHITextureViewType::RenderTarget;
            targetViewDesc.debugName = "ParticleDistortionComposite.TargetRTV";
            data.compositeTargetView = builder.CreateTextureView(
                data.particleVfx.compositedSceneColor, targetViewDesc);
            data.compositeTargetView = builder.Write(
                data.compositeTargetView,
                MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                 RHIShaderStage::Pixel));
            return data.constantsHandle.IsValid() && data.instanceHandle.IsValid() &&
                data.exposureStateReadHandle.IsValid() &&
                data.exposureStateContentHash != 0 &&
                data.compositeSourceView.IsValid() && data.distortionView.IsValid() &&
                data.compositeTargetView.IsValid();
        }

        [[nodiscard]] RHIDescriptorSetRef CreateParticleDescriptorSet(
            const ParticleGraphPassData& data,
            RenderGraphPassContext& context,
            uint32 batchIndex,
            bool composite)
        {
            if (data.submission.descriptorSetLayout == nullptr ||
                data.submission.linearClampSampler == nullptr ||
                data.submission.fallbackTextureView == nullptr ||
                data.submission.constantsBuffer == nullptr ||
                data.submission.instanceBuffer == nullptr)
            {
                return {};
            }

            RHITextureView* spriteView = data.submission.fallbackTextureView.Get();
            if (!composite && batchIndex < data.textureBindings.size() &&
                data.textureBindings[batchIndex].graphView.IsValid())
            {
                spriteView = context.GetTextureView(
                    data.textureBindings[batchIndex].graphView);
            }
            else if (composite)
            {
                spriteView = context.GetTextureView(data.compositeSourceView);
            }
            RHITextureView* depthOrDistortionView =
                data.submission.fallbackTextureView.Get();
            if (composite)
            {
                depthOrDistortionView = context.GetTextureView(
                    data.distortionView);
            }
            else if (data.softDepthView.IsValid())
            {
                depthOrDistortionView = context.GetTextureView(data.softDepthView);
            }
            RHITextureView* froxelVolumetricLighting =
                data.execution.volumetricLighting.identityRoute
                    ? data.neutralFroxelVolumetricLightingView.Get()
                    : context.GetTextureView(data.froxelVolumetricLightingView);
            if (spriteView == nullptr || depthOrDistortionView == nullptr ||
                froxelVolumetricLighting == nullptr)
            {
                return {};
            }
            RHIBuffer* const exposureState = context.GetBuffer(
                data.exposureStateReadHandle);
            if (exposureState == nullptr ||
                exposureState->GetSize() != SceneTextures::ExposureStateStride ||
                exposureState->GetStride() != SceneTextures::ExposureStateStride ||
                !HasFlag(exposureState->GetUsage(), RHIBufferUsage::Structured) ||
                !HasFlag(exposureState->GetUsage(),
                         RHIBufferUsage::ShaderResource))
            {
                return {};
            }
            if (!data.execution.volumetricLighting.identityRoute)
            {
                RHITexture* const froxelTexture = context.GetTexture(
                    data.froxelVolumetricLightingHandle);
                const FroxelGridMetadata& grid =
                    data.execution.volumetricLighting.froxelGrid;
                if (froxelTexture == nullptr ||
                    froxelVolumetricLighting->GetTexture() != froxelTexture ||
                    froxelVolumetricLighting->GetFormat() != RHIFormat::RGBA16_FLOAT ||
                    froxelTexture->GetDimension() !=
                        RHITextureDimension::Texture3D ||
                    froxelTexture->GetFormat() != RHIFormat::RGBA16_FLOAT ||
                    froxelTexture->GetWidth() != grid.froxelWidth ||
                    froxelTexture->GetHeight() != grid.froxelHeight ||
                    froxelTexture->GetDepth() != grid.depthSliceCount ||
                    froxelTexture->GetMipLevels() != 1 ||
                    froxelTexture->GetArraySize() != 1 ||
                    froxelTexture->GetSampleCount() != RHISampleCount::Count1)
                {
                    return {};
                }
            }

            IRHIDevice* const device = data.submission.device;
            if (device == nullptr)
            {
                return {};
            }
            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.debugName = composite
                ? "ParticleDistortionCompositeDescriptorSet"
                : "ParticleBillboardDescriptorSet";
            descriptorDesc.SetLayout(data.submission.descriptorSetLayout.Get())
                .BindBuffer(0, data.submission.constantsBuffer.Get(), 0,
                            RVX_PARTICLE_CONSTANT_BUFFER_SIZE)
                .BindBuffer(1, data.submission.instanceBuffer.Get())
                .BindTexture(2, spriteView)
                .BindTexture(3, depthOrDistortionView)
                .BindTexture(4, froxelVolumetricLighting)
                .BindSampler(5, data.submission.linearClampSampler.Get())
                .BindBuffer(RVX_PARTICLE_EXPOSURE_STATE_BINDING, exposureState);
            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet || !descriptorSet->IsReadyForBinding(
                    data.submission.descriptorSetLayout.Get()) ||
                !context.RetainSubmissionResource(descriptorSet) ||
                !context.RetainSubmissionResource(Ref<RefCounted>(exposureState)))
            {
                return {};
            }
            return descriptorSet;
        }

        void ExecuteGraphPass(const ParticleGraphPassData& data,
                              RenderGraphPassContext& context)
        {
            ParticleFeaturePassStats stats = data.initialStats;
            // AddToGraph may finish registering the optional distortion chain
            // after this pass's immutable execution data is captured. Preserve
            // that completed registration receipt instead of overwriting it
            // when the base particle pass publishes its execution result.
            if (data.owner != nullptr)
            {
                const ParticleFeaturePassStats& currentStats =
                    data.owner->GetStats();
                stats.distortionPassScheduled =
                    currentStats.distortionPassScheduled;
                stats.distortionPassExecuted =
                    currentStats.distortionPassExecuted;
                stats.distortionCompositeExecuted =
                    currentStats.distortionCompositeExecuted;
                stats.weightedDrawCallCount =
                    currentStats.weightedDrawCallCount;
                stats.directDrawCallCount =
                    currentStats.directDrawCallCount;
                stats.weightedGraphPassScheduled =
                    currentStats.weightedGraphPassScheduled;
                stats.directGraphPassScheduled =
                    currentStats.directGraphPassScheduled;
                stats.drawCallCount = currentStats.drawCallCount;
                stats.drawSubmitted = currentStats.drawSubmitted;
            }
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
                !data.exposureStateReadHandle.IsValid() ||
                data.exposureStateContentHash == 0 ||
                !data.colorHandle.IsValid() || !data.colorView.IsValid() ||
                !data.reactiveMaskHandle.IsValid() || !data.reactiveMaskView.IsValid() ||
                !data.depthHandle.IsValid() || !data.depthView.IsValid() ||
                !IsCompatibleFroxelVolumetricLighting(
                    data.execution.volumetricLighting, data.execution.identity) ||
                 (data.execution.volumetricLighting.identityRoute
                     ? !data.neutralFroxelVolumetricLightingView
                     : !data.froxelVolumetricLightingHandle.IsValid() ||
                         !data.froxelVolumetricLightingView.IsValid()) ||
                 (data.particleVfx.softDepthRequired &&
                  (!data.softDepthHandle.IsValid() || !data.softDepthView.IsValid())) ||
                 (data.particleVfx.softDepthRequested &&
                  data.particleVfx.softDepthSnapshot.texture.IsValid() &&
                  (data.particleVfx.softDepthSnapshot.receipt == nullptr ||
                   !data.particleVfx.softDepthSnapshot.receipt->copied.load(
                       std::memory_order_acquire))))
            {
                fail("Particle billboard graph recording prerequisites are unavailable");
                return;
            }
            RHITexture* const colorTexture = context.GetTexture(data.colorHandle);
            RHITextureView* const colorView = context.GetTextureView(data.colorView);
            RHITextureView* const reactiveMaskView = context.GetTextureView(
                data.reactiveMaskView);
            RHITextureView* const depthView = context.GetTextureView(data.depthView);
            if (colorTexture == nullptr || colorView == nullptr ||
                reactiveMaskView == nullptr || depthView == nullptr)
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
            RHIBuffer* const exposureState = context.GetBuffer(
                data.exposureStateReadHandle);
            if (exposureState == nullptr ||
                exposureState->GetSize() != SceneTextures::ExposureStateStride ||
                exposureState->GetStride() != SceneTextures::ExposureStateStride ||
                !HasFlag(exposureState->GetUsage(), RHIBufferUsage::Structured) ||
                !HasFlag(exposureState->GetUsage(),
                         RHIBufferUsage::ShaderResource))
            {
                fail("Particle billboard exact committed exposure state is unavailable");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                colorView, RHILoadOp::Load, RHIStoreOp::Store);
            renderPassDesc.AddColorAttachment(
                reactiveMaskView, RHILoadOp::Load, RHIStoreOp::Store);
            renderPassDesc.SetDepthStencil(
                depthView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
            renderPassDesc.depthStencilAttachment.readOnly = true;
            renderPassDesc.SetRenderArea(
                0, 0, colorTexture->GetWidth(), colorTexture->GetHeight());

            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetViewport(data.execution.view.GetRHIViewport());
            commands.SetScissor(data.execution.view.GetRHIScissor());
            const bool useAllBatches =
                data.stage == ParticleFeatureGraphStage::LegacyDirect;
            const uint32 selectedBatchCount = useAllBatches
                ? static_cast<uint32>(data.submission.plan->batches.size())
                : static_cast<uint32>(data.batchIndices.size());
            uint32 stageDrawCallCount = 0;
            for (uint32 selectedIndex = 0;
                 selectedIndex < selectedBatchCount;
                 ++selectedIndex)
            {
                const uint32 batchIndex = useAllBatches
                    ? selectedIndex : data.batchIndices[selectedIndex];
                if (batchIndex >= data.submission.plan->batches.size())
                {
                    commands.EndRenderPass();
                    fail("Particle direct stage references a batch outside its immutable plan");
                    return;
                }
                const ParticleBillboardDrawBatch& batch =
                    data.submission.plan->batches[batchIndex];
                const uint32 pipelineIndex = BlendPipelineIndex(batch.blendMode);
                if (pipelineIndex == RVX_INVALID_INDEX ||
                    pipelineIndex >= data.submission.pipelines.size() ||
                    !data.submission.pipelines[pipelineIndex])
                {
                    commands.EndRenderPass();
                    fail("Particle billboard batch has no legal graphics pipeline");
                    return;
                }
                RHIDescriptorSetRef descriptorSet = CreateParticleDescriptorSet(
                    data, context, batchIndex, false);
                if (!descriptorSet)
                {
                    commands.EndRenderPass();
                    fail("Particle billboard batch descriptor bindings are unavailable");
                    return;
                }
                commands.SetPipeline(data.submission.pipelines[pipelineIndex].Get());
                commands.SetDescriptorSet(0, descriptorSet.Get());
                commands.Draw(6, batch.itemCount, 0, batch.firstItem);
                ++stageDrawCallCount;
            }
            commands.EndRenderPass();
            if (data.stage == ParticleFeatureGraphStage::Direct)
            {
                stats.directDrawCallCount = stageDrawCallCount;
                stats.drawCallCount = stats.weightedDrawCallCount +
                    stats.directDrawCallCount;
            }
            else
            {
                stats.drawCallCount = stageDrawCallCount;
            }
            stats.drawSubmitted = stats.drawSubmitted ||
                stageDrawCallCount != 0;
            stats.reactiveMaskWriteEnabled =
                stats.reactiveMaskEnabled && stageDrawCallCount != 0;
            stats.reactiveMaskWriteDrawCount = stageDrawCallCount;
            stats.exposureStateReady = true;
            stats.exposureStateReadDeclared = true;
            stats.exposureStateDescriptorBound = true;
            stats.exposureStateContentHash = data.exposureStateContentHash;
            if (data.owner != nullptr)
            {
                data.owner->PublishExecutionStats(stats);
            }
        }

        void ExecuteWeightedBlendedGraphPass(
            const ParticleGraphPassData& data,
            RenderGraphPassContext& context)
        {
            ParticleFeaturePassStats stats = data.initialStats;
            if (data.owner != nullptr)
            {
                const ParticleFeaturePassStats& currentStats =
                    data.owner->GetStats();
                stats.directDrawCallCount = currentStats.directDrawCallCount;
                stats.weightedGraphPassScheduled =
                    currentStats.weightedGraphPassScheduled;
                stats.directGraphPassScheduled =
                    currentStats.directGraphPassScheduled;
                stats.distortionPassScheduled =
                    currentStats.distortionPassScheduled;
                stats.distortionPassExecuted =
                    currentStats.distortionPassExecuted;
                stats.distortionCompositeExecuted =
                    currentStats.distortionCompositeExecuted;
                stats.reactiveMaskWriteEnabled =
                    currentStats.reactiveMaskWriteEnabled;
                stats.reactiveMaskWriteDrawCount =
                    currentStats.reactiveMaskWriteDrawCount;
            }
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
                !data.submission.weightedBlendedPipeline ||
                !data.constantsHandle.IsValid() ||
                !data.instanceHandle.IsValid() ||
                !data.exposureStateReadHandle.IsValid() ||
                data.exposureStateContentHash == 0 ||
                !data.accumulationHandle.IsValid() ||
                !data.revealageHandle.IsValid() ||
                !data.accumulationView.IsValid() ||
                !data.revealageView.IsValid() ||
                !data.depthHandle.IsValid() || !data.depthView.IsValid() ||
                !IsCompatibleFroxelVolumetricLighting(
                    data.execution.volumetricLighting, data.execution.identity) ||
                 (data.execution.volumetricLighting.identityRoute
                     ? !data.neutralFroxelVolumetricLightingView
                     : !data.froxelVolumetricLightingHandle.IsValid() ||
                         !data.froxelVolumetricLightingView.IsValid()) ||
                 (data.particleVfx.softDepthRequired &&
                  (!data.softDepthHandle.IsValid() ||
                   !data.softDepthView.IsValid())) ||
                 (data.particleVfx.softDepthRequested &&
                  data.particleVfx.softDepthSnapshot.texture.IsValid() &&
                  (data.particleVfx.softDepthSnapshot.receipt == nullptr ||
                   !data.particleVfx.softDepthSnapshot.receipt->copied.load(
                       std::memory_order_acquire))))
            {
                fail("Particle weighted-blended graph recording prerequisites are unavailable");
                return;
            }

            RHITexture* const accumulationTexture =
                context.GetTexture(data.accumulationHandle);
            RHITextureView* const accumulationView =
                context.GetTextureView(data.accumulationView);
            RHITextureView* const revealageView =
                context.GetTextureView(data.revealageView);
            RHITextureView* const depthView =
                context.GetTextureView(data.depthView);
            if (accumulationTexture == nullptr || accumulationView == nullptr ||
                revealageView == nullptr || depthView == nullptr ||
                context.GetBuffer(data.constantsHandle) !=
                    data.submission.constantsBuffer.Get() ||
                context.GetBuffer(data.instanceHandle) !=
                    data.submission.instanceBuffer.Get())
            {
                fail("Particle weighted-blended graph resources could not be realized");
                return;
            }
            RHIBuffer* const exposureState = context.GetBuffer(
                data.exposureStateReadHandle);
            if (exposureState == nullptr ||
                exposureState->GetSize() != SceneTextures::ExposureStateStride ||
                exposureState->GetStride() != SceneTextures::ExposureStateStride ||
                !HasFlag(exposureState->GetUsage(), RHIBufferUsage::Structured) ||
                !HasFlag(exposureState->GetUsage(),
                         RHIBufferUsage::ShaderResource))
            {
                fail("Particle weighted-blended exact committed exposure state is unavailable");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                accumulationView, RHILoadOp::Load, RHIStoreOp::Store);
            renderPassDesc.AddColorAttachment(
                revealageView, RHILoadOp::Load, RHIStoreOp::Store);
            renderPassDesc.SetDepthStencil(
                depthView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
            renderPassDesc.depthStencilAttachment.readOnly = true;
            renderPassDesc.SetRenderArea(
                0, 0, accumulationTexture->GetWidth(),
                accumulationTexture->GetHeight());

            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetViewport(data.execution.view.GetRHIViewport());
            commands.SetScissor(data.execution.view.GetRHIScissor());
            uint32 stageDrawCallCount = 0;
            for (uint32 batchIndex : data.batchIndices)
            {
                if (batchIndex >= data.submission.plan->batches.size())
                {
                    commands.EndRenderPass();
                    fail("Particle weighted-blended stage references a batch outside its immutable plan");
                    return;
                }
                const ParticleBillboardDrawBatch& batch =
                    data.submission.plan->batches[batchIndex];
                if (batch.blendMode !=
                        ParticleRenderSnapshotBlendMode::AlphaBlend &&
                    batch.blendMode !=
                        ParticleRenderSnapshotBlendMode::Premultiplied)
                {
                    commands.EndRenderPass();
                    fail("Particle weighted-blended stage rejected a non-alpha batch");
                    return;
                }
                RHIDescriptorSetRef descriptorSet =
                    CreateParticleDescriptorSet(
                        data, context, batchIndex, false);
                if (!descriptorSet)
                {
                    commands.EndRenderPass();
                    fail("Particle weighted-blended descriptor bindings are unavailable");
                    return;
                }
                commands.SetPipeline(
                    data.submission.weightedBlendedPipeline.Get());
                commands.SetDescriptorSet(0, descriptorSet.Get());
                commands.Draw(6, batch.itemCount, 0, batch.firstItem);
                ++stageDrawCallCount;
            }
            commands.EndRenderPass();

            stats.requested = true;
            stats.supported = true;
            stats.enabled = true;
            stats.graphPassScheduled = true;
            stats.uploadPublished = true;
            stats.weightedDrawCallCount = stageDrawCallCount;
            stats.drawCallCount = stats.weightedDrawCallCount +
                stats.directDrawCallCount;
            stats.drawSubmitted = stats.drawCallCount != 0;
            stats.exposureStateReady = true;
            stats.exposureStateReadDeclared = true;
            stats.exposureStateDescriptorBound = true;
            stats.exposureStateContentHash = data.exposureStateContentHash;
            if (data.owner != nullptr)
            {
                data.owner->PublishExecutionStats(stats);
            }
        }

        void ExecuteDistortionGraphPass(const ParticleGraphPassData& data,
                                        RenderGraphPassContext& context)
        {
            if (!data.contextValid || !data.submission.IsReady() ||
                !data.distortionView.IsValid() || !data.depthView.IsValid() ||
                !data.submission.distortionPipeline)
            {
                context.MarkExecutionFailed(
                    "Particle distortion graph recording prerequisites are unavailable");
                return;
            }
            RHITextureView* const distortionView = context.GetTextureView(
                data.distortionView);
            RHITextureView* const depthView = context.GetTextureView(data.depthView);
            RHITexture* const distortionTexture = context.GetTexture(
                data.particleVfx.distortionVectors);
            if (distortionView == nullptr || depthView == nullptr ||
                distortionTexture == nullptr)
            {
                context.MarkExecutionFailed(
                    "Particle distortion graph resources could not be realized");
                return;
            }
            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                distortionView, RHILoadOp::Clear, RHIStoreOp::Store);
            renderPassDesc.SetDepthStencil(
                depthView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
            renderPassDesc.depthStencilAttachment.readOnly = true;
            renderPassDesc.SetRenderArea(
                0, 0, distortionTexture->GetWidth(), distortionTexture->GetHeight());
            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetViewport(data.execution.view.GetRHIViewport());
            commands.SetScissor(data.execution.view.GetRHIScissor());
            for (uint32 batchIndex = 0;
                 batchIndex < data.submission.plan->batches.size();
                 ++batchIndex)
            {
                const ParticleBillboardDrawBatch& batch =
                    data.submission.plan->batches[batchIndex];
                if (batch.firstItem >= data.submission.plan->items.size() ||
                    data.submission.plan->items[batch.firstItem]
                        .effectDescriptor.distortionStrengthPixels <= 0.0f)
                {
                    continue;
                }
                RHIDescriptorSetRef descriptorSet = CreateParticleDescriptorSet(
                    data, context, batchIndex, false);
                if (!descriptorSet)
                {
                    commands.EndRenderPass();
                    context.MarkExecutionFailed(
                        "Particle distortion descriptor bindings are unavailable");
                    return;
                }
                commands.SetPipeline(data.submission.distortionPipeline.Get());
                commands.SetDescriptorSet(0, descriptorSet.Get());
                commands.Draw(6, batch.itemCount, 0, batch.firstItem);
            }
            commands.EndRenderPass();
            if (data.owner != nullptr)
            {
                ParticleFeaturePassStats stats = data.owner->GetStats();
                stats.distortionPassExecuted = true;
                data.owner->PublishExecutionStats(stats);
            }
        }

        void ExecuteDistortionCompositeGraphPass(
            const ParticleGraphPassData& data,
            RenderGraphPassContext& context)
        {
            if (!data.contextValid || !data.submission.IsReady() ||
                !data.compositeSourceView.IsValid() || !data.distortionView.IsValid() ||
                !data.compositeTargetView.IsValid() || !data.submission.compositePipeline)
            {
                context.MarkExecutionFailed(
                    "Particle distortion composite graph recording prerequisites are unavailable");
                return;
            }
            RHITextureView* const targetView = context.GetTextureView(
                data.compositeTargetView);
            RHITexture* const targetTexture = context.GetTexture(
                data.particleVfx.compositedSceneColor);
            if (targetView == nullptr || targetTexture == nullptr)
            {
                context.MarkExecutionFailed(
                    "Particle distortion composite output could not be realized");
                return;
            }
            RHIDescriptorSetRef descriptorSet = CreateParticleDescriptorSet(
                data, context, 0, true);
            if (!descriptorSet)
            {
                context.MarkExecutionFailed(
                    "Particle distortion composite descriptor bindings are unavailable");
                return;
            }
            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                targetView, RHILoadOp::Clear, RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(
                0, 0, targetTexture->GetWidth(), targetTexture->GetHeight());
            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetPipeline(data.submission.compositePipeline.Get());
            commands.SetDescriptorSet(0, descriptorSet.Get());
            commands.SetViewport(data.execution.view.GetRHIViewport());
            commands.SetScissor(data.execution.view.GetRHIScissor());
            commands.Draw(3, 1, 0, 0);
            commands.EndRenderPass();
            if (data.owner != nullptr)
            {
                ParticleFeaturePassStats stats = data.owner->GetStats();
                stats.distortionCompositeExecuted = true;
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
        m_lightHintRequestedCount = 0;
        m_lightHintAdmittedCount = 0;
        m_lightHintDroppedCount = 0;
        m_recordFrameSequence = 0;
        m_recordFrameIdentity = {};
        m_recordGraphIdentity = 0;
        m_recordGraphGeneration = 0;
        m_recordEpoch = 0;
        m_batchReceipts.clear();
        m_batchReceiptsOverflow = false;
        RebuildStats();
    }

void ParticleFeaturePass::SetShaderProgramProvider(
    IShaderProgramProvider* shaderProgramProvider)
{
    m_renderer->SetShaderProgramProvider(shaderProgramProvider);
    RebuildStats();
}

void ParticleFeaturePass::SetPipelineCache(PipelineCache* pipelineCache) noexcept
{
    m_pipelineCache = pipelineCache;
    RebuildStats();
}

    void ParticleFeaturePass::SetResourceRegistry(
        RenderResourceRegistry* resourceRegistry)
    {
        m_resourceRegistry = resourceRegistry;
        RebuildStats();
    }

    void ParticleFeaturePass::SetReverseZ(bool reverseZ)
    {
        m_renderer->SetReverseZ(reverseZ);
        RebuildStats();
    }

    ParticleVfxRequirements ParticleFeaturePass::GetVfxRequirements() const noexcept
    {
        ParticleVfxRequirements requirements;
        if (m_snapshot == nullptr ||
            m_snapshot->metadata.status != ParticleRenderSnapshotStatus::Complete ||
            !m_snapshot->metadata.complete)
        {
            return requirements;
        }

        for (const ParticleRenderSnapshotItem& item : m_snapshot->items)
        {
            if (!item.visible || item.aliveParticleCount == 0 ||
                item.payloadStatus !=
                    ParticleRenderSnapshotPayloadStatus::CpuPacketReady ||
                !item.effectDescriptor.IsStructurallyValid())
            {
                continue;
            }

            const ParticleRenderEffectDescriptor& effect =
                item.effectDescriptor;
            requirements.requested = true;
            requirements.softDepthRequested = requirements.softDepthRequested ||
                effect.softDepthMode != ParticleRenderSoftDepthMode::Disabled;
            requirements.softDepthRequired = requirements.softDepthRequired ||
                effect.softDepthMode == ParticleRenderSoftDepthMode::Required;
            requirements.reactiveMaskRequested = requirements.reactiveMaskRequested ||
                effect.reactiveContribution > 0.0f;
            requirements.distortionRequested = requirements.distortionRequested ||
                effect.distortionStrengthPixels > 0.0f;
        }
        requirements.exposureStateRequired = requirements.requested;
        return requirements;
    }

    bool ParticleFeaturePass::CollectLightingHints(
        std::vector<ParticleRenderLightHint>& outHints,
        std::string& outDiagnostic) const
    {
        outHints.clear();
        outDiagnostic.clear();
        if (m_snapshot == nullptr)
        {
            return true;
        }

        outHints.reserve(m_snapshot->items.size());
        for (const ParticleRenderSnapshotItem& item : m_snapshot->items)
        {
            if (item.payloadStatus !=
                    ParticleRenderSnapshotPayloadStatus::CpuPacketReady ||
                item.aliveParticleCount == 0)
            {
                continue;
            }
            if (!item.HasValidCpuPacket())
            {
                outDiagnostic =
                    "Particle light hint originates from a malformed sealed CPU packet";
                return false;
            }

            const ParticleRenderLightHint& hint = item.cpuPacket.lightHint;
            // A producer cannot inject a local light by re-sealing only the
            // packet: effect policy and packet value must agree exactly.
            if (hint.enabled != item.effectDescriptor.lightHint.enabled)
            {
                outDiagnostic =
                    "Particle light hint enablement does not match its immutable effect descriptor";
                return false;
            }
            if (hint.enabled)
            {
                if (!hint.IsStructurallyValid())
                {
                    outDiagnostic = "Particle light hint is structurally invalid";
                    return false;
                }
                const auto sourceIt = std::find_if(
                    item.cpuPacket.particles.begin(),
                    item.cpuPacket.particles.end(),
                    [&hint](const ParticleRenderParticleData& particle)
                    {
                        return particle.stableParticleId == hint.sourceParticleId;
                    });
                if (sourceIt == item.cpuPacket.particles.end())
                {
                    outDiagnostic =
                        "Particle light hint does not identify a packet particle";
                    return false;
                }

                const ParticleRenderLightHintSettings& policy =
                    item.effectDescriptor.lightHint;
                const Vec3 expectedColor{
                    policy.color.x * sourceIt->color.x,
                    policy.color.y * sourceIt->color.y,
                    policy.color.z * sourceIt->color.z};
                const float expectedIntensity =
                    policy.intensityCandela * hint.modulation;
                const auto nearlyEqual = [](float left, float right)
                {
                    constexpr float absoluteTolerance = 1.0e-5f;
                    constexpr float relativeTolerance = 1.0e-5f;
                    return std::abs(left - right) <= absoluteTolerance +
                        relativeTolerance * std::max(std::abs(left), std::abs(right));
                };
                const bool matchesSourceAndPolicy =
                    nearlyEqual(hint.position.x, sourceIt->position.x) &&
                    nearlyEqual(hint.position.y, sourceIt->position.y) &&
                    nearlyEqual(hint.position.z, sourceIt->position.z) &&
                    nearlyEqual(hint.color.x, expectedColor.x) &&
                    nearlyEqual(hint.color.y, expectedColor.y) &&
                    nearlyEqual(hint.color.z, expectedColor.z) &&
                    nearlyEqual(hint.intensityCandela, expectedIntensity) &&
                    nearlyEqual(hint.rangeMeters, policy.rangeMeters);
                if (!matchesSourceAndPolicy)
                {
                    outDiagnostic =
                        "Particle light hint does not match its sealed effect policy and source particle";
                    return false;
                }
                outHints.push_back(hint);
            }
        }

        std::sort(outHints.begin(), outHints.end(),
                  [](const ParticleRenderLightHint& left,
                     const ParticleRenderLightHint& right)
                  {
                      return left.stableLightId < right.stableLightId;
                  });
        for (size_t index = 1; index < outHints.size(); ++index)
        {
            if (outHints[index - 1].stableLightId == outHints[index].stableLightId)
            {
                outDiagnostic =
                    "Particle snapshot contains duplicate stable local-light identities";
                return false;
            }
        }
        return true;
    }

    void ParticleFeaturePass::SetLightingHintAdmissionStats(
        uint32 requested,
        uint32 admitted,
        uint32 dropped) noexcept
    {
        m_lightHintRequestedCount = requested;
        m_lightHintAdmittedCount = admitted;
        m_lightHintDroppedCount = dropped;
        m_stats.lightHintRequestedCount = requested;
        m_stats.lightHintAdmittedCount = admitted;
        m_stats.lightHintDroppedCount = dropped;
    }

    void ParticleFeaturePass::PublishExecutionStats(
        const ParticleFeaturePassStats& stats)
    {
        m_stats = stats;
    }

    bool ParticleFeaturePass::PreflightWeightedBlendedPipeline(
        std::string& outDiagnostic)
    {
        outDiagnostic.clear();
        if (m_renderer == nullptr)
        {
            outDiagnostic =
                "Particle weighted-blended renderer instance is unavailable";
            return false;
        }

        return m_renderer->PreflightWeightedBlendedPipeline(outDiagnostic);
    }

    ParticleFeaturePreparedRecordingResult
    ParticleFeaturePass::PrepareTranslucencyRecording(
        const TranslucencyFramePlan& plan,
        std::shared_ptr<const ParticleBillboardDrawPlan> drawPlan,
        const RenderPassRecordContext& context)
    {
        RebuildStats();
        ParticleFeaturePreparedRecordingResult result;
        result.translucencyPlanIdentity = plan.GetIdentityHash();
        const auto fail = [this, &result](
            ParticleFeatureRecordingCode code,
            std::string diagnostic)
        {
            result.code = code;
            result.diagnostic = std::move(diagnostic);
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason = result.diagnostic;
            return result;
        };

        if (!plan.IsStructurallyValid())
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidFramePlan,
                "Particle translucency preparation requires a structurally valid frame plan");
        }
        if (plan.HasRequiredCapabilityFailure())
        {
            return fail(
                ParticleFeatureRecordingCode::RequiredCapabilityUnavailable,
                "Particle translucency preparation rejected unavailable required WBOIT capability");
        }
        if (!plan.IsExecutable())
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidFramePlan,
                "Particle translucency frame plan is not executable");
        }
        if (!context.identity.IsValid() || context.identity.graph == nullptr ||
            !context.MatchesTargetGraph(*context.identity.graph) ||
            !context.IsFrameIdentityValid() ||
            context.featureFrameIdentity != plan.GetFrameIdentity() ||
            context.featureViewIdentity != plan.GetViewIdentity() ||
            context.identity.frameSequence !=
                plan.GetFrameIdentity().frameSequence)
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidRecordContext,
                "Particle translucency plan and graph recording identities do not match");
        }
        if (drawPlan == nullptr || !drawPlan->valid)
        {
            return fail(
                ParticleFeatureRecordingCode::PlanInventoryMismatch,
                drawPlan != nullptr && !drawPlan->diagnostic.empty()
                    ? drawPlan->diagnostic
                    : "Particle translucency preparation requires a valid immutable draw plan");
        }
        if (m_snapshot == nullptr || !m_stats.supported ||
            m_snapshot->metadata.sequence == 0 ||
            (!drawPlan->items.empty() && !m_stats.enabled))
        {
            return fail(
                ParticleFeatureRecordingCode::PlanInventoryMismatch,
                "Particle prepared draw plan has no matching complete snapshot generation");
        }

        const uint64 particleSourceIdentity =
            BuildParticleBillboardSourceIdentityHash(*drawPlan);
        const TranslucencyRouteSummary& routeSummary = plan.GetSummary();
        if (particleSourceIdentity == 0 ||
            particleSourceIdentity != plan.GetParticleSourceIdentityHash() ||
            routeSummary.particleBatchCandidateCount !=
                drawPlan->batches.size() ||
            routeSummary.particleItemCount != drawPlan->items.size())
        {
            return fail(
                ParticleFeatureRecordingCode::PlanInventoryMismatch,
                "Particle draw-plan inventory does not match the translucency frame plan");
        }

        std::vector<bool> routedBatches(drawPlan->batches.size(), false);
        std::vector<uint32> weightedBatchIndices;
        std::vector<uint32> directBatchIndices;
        uint32 weightedItemCount = 0;
        uint32 directItemCount = 0;
        uint32 optionalFallbackBatchCount = 0;
        uint32 optionalFallbackItemCount = 0;
        for (const TranslucencyPlanEntry& entry : plan.GetEntries())
        {
            if (entry.sourceKind != TranslucencySourceKind::Particle)
            {
                continue;
            }
            if (entry.sourceOrdinal >= drawPlan->batches.size() ||
                routedBatches[entry.sourceOrdinal])
            {
                return fail(
                    ParticleFeatureRecordingCode::PlanInventoryMismatch,
                    "Particle translucency frame plan contains a duplicate or out-of-range batch");
            }
            const ParticleBillboardDrawBatch& batch =
                drawPlan->batches[entry.sourceOrdinal];
            if (entry.sourceIdentityHash !=
                    BuildParticleBillboardBatchIdentityHash(
                        *drawPlan, entry.sourceOrdinal) ||
                entry.firstItem != batch.firstItem ||
                entry.itemCount != batch.itemCount ||
                entry.particleBlendMode != batch.blendMode)
            {
                return fail(
                    ParticleFeatureRecordingCode::PlanInventoryMismatch,
                    "Particle translucency route does not bind the exact immutable batch identity");
            }
            routedBatches[entry.sourceOrdinal] = true;

            switch (entry.route)
            {
                case TranslucencyRoute::WeightedBlended:
                    if (batch.blendMode !=
                            ParticleRenderSnapshotBlendMode::AlphaBlend &&
                        batch.blendMode !=
                            ParticleRenderSnapshotBlendMode::Premultiplied)
                    {
                        return fail(
                            ParticleFeatureRecordingCode::UnsupportedPlanRoute,
                            "Particle WBOIT route accepts only alpha and premultiplied batches");
                    }
                    weightedBatchIndices.push_back(entry.sourceOrdinal);
                    weightedItemCount += entry.itemCount;
                    break;

                case TranslucencyRoute::DirectAdditive:
                    if (batch.blendMode !=
                        ParticleRenderSnapshotBlendMode::Additive)
                    {
                        return fail(
                            ParticleFeatureRecordingCode::UnsupportedPlanRoute,
                            "Particle direct-additive route rejected a non-additive batch");
                    }
                    directBatchIndices.push_back(entry.sourceOrdinal);
                    directItemCount += entry.itemCount;
                    break;

                case TranslucencyRoute::SortedRefraction:
                    if (plan.GetWeightedBlendedResolution() !=
                            WeightedBlendedOITCapabilityResolution::
                                OptionalSortedFallback ||
                        (batch.blendMode !=
                             ParticleRenderSnapshotBlendMode::AlphaBlend &&
                         batch.blendMode !=
                             ParticleRenderSnapshotBlendMode::Premultiplied))
                    {
                        return fail(
                            ParticleFeatureRecordingCode::UnsupportedPlanRoute,
                            "Particle sorted direct fallback is legal only for optional WBOIT alpha fallback");
                    }
                    directBatchIndices.push_back(entry.sourceOrdinal);
                    directItemCount += entry.itemCount;
                    ++optionalFallbackBatchCount;
                    optionalFallbackItemCount += entry.itemCount;
                    break;

                case TranslucencyRoute::Unsupported:
                case TranslucencyRoute::Count:
                default:
                    return fail(
                        ParticleFeatureRecordingCode::UnsupportedPlanRoute,
                        "Particle translucency plan contains an unrecordable terminal route");
            }
        }
        if (std::any_of(
                routedBatches.begin(), routedBatches.end(),
                [](bool routed) { return !routed; }) ||
            static_cast<uint64>(weightedItemCount) + directItemCount !=
                drawPlan->items.size())
        {
            return fail(
                ParticleFeatureRecordingCode::PlanInventoryMismatch,
                "Particle translucency stages do not cover the draw plan exactly once");
        }

        result.weightedBatchCount =
            static_cast<uint32>(weightedBatchIndices.size());
        result.weightedItemCount = weightedItemCount;
        result.directBatchCount =
            static_cast<uint32>(directBatchIndices.size());
        result.directItemCount = directItemCount;
        if (drawPlan->items.empty())
        {
            m_stats.supported = true;
            m_stats.enabled = false;
            result.code = ParticleFeatureRecordingCode::NoWork;
            return result;
        }

        const RenderPassExecutionData execution =
            MakeRenderPassExecutionData(context);
        const bool contextValid = execution.MatchesTargetGraph(
                *context.identity.graph) &&
            execution.IsFrameIdentityValid() &&
            IsCompatibleFroxelVolumetricLighting(
                execution.volumetricLighting, execution.identity) &&
            m_pipelineCache != nullptr;
        if (!contextValid || !execution.view.depthTarget.IsValid())
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidRecordContext,
                "Particle translucency preparation requires current graph-owned view resources");
        }
        const ParticleVfxRecordInput particleVfx = context.particleVfx;
        if (const char* const exposureFailure =
                DescribeParticleExposureRecordFailure(
                    particleVfx, context.identity);
            exposureFailure != nullptr)
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidRecordContext,
                exposureFailure);
        }
        if (!particleVfx.IsCompatibleWith(
                context.identity,
                RenderOutputExtent{execution.view.viewportWidth,
                                   execution.view.viewportHeight}))
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidRecordContext,
                "Particle VFX auxiliary graph inputs belong to another recording");
        }
        std::string graphTargetDiagnostic;
        if (!ValidateParticleGraphTargets(
                *context.identity.graph,
                execution.view,
                particleVfx,
                graphTargetDiagnostic))
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidGraphTargets,
                std::move(graphTargetDiagnostic));
        }

        const bool softDepthSnapshotReady =
            particleVfx.softDepthSnapshot.texture.IsValid();
        Mat4 jitteredInverseProjection = Mat4Identity();
        const bool softDepthLinearizationReady = softDepthSnapshotReady &&
            TryGetFiniteJitteredInverseProjection(
                execution.view, jitteredInverseProjection);
        if (particleVfx.softDepthRequired &&
            !softDepthLinearizationReady)
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidGraphTargets,
                "Particle required soft depth needs a compatible D32 snapshot and finite inverse projection");
        }

        std::vector<ParticleTextureBinding> textureBindings;
        std::string textureBindingDiagnostic;
        if (!ResolveParticleTextureBindings(
                *drawPlan,
                m_resourceRegistry,
                textureBindings,
                m_stats,
                textureBindingDiagnostic))
        {
            return fail(
                ParticleFeatureRecordingCode::ResourcePreparationFailed,
                std::move(textureBindingDiagnostic));
        }
        std::vector<bool> spriteTextureAvailableByBatch;
        spriteTextureAvailableByBatch.reserve(textureBindings.size());
        for (const ParticleTextureBinding& binding : textureBindings)
        {
            spriteTextureAvailableByBatch.push_back(
                static_cast<bool>(binding.texture));
        }

        ParticleBillboardRenderer::Submission submission =
            m_renderer->Prepare(
                drawPlan,
                execution.view,
                softDepthLinearizationReady,
                spriteTextureAvailableByBatch,
                !weightedBatchIndices.empty());
        if (!submission.IsReady() ||
            (!weightedBatchIndices.empty() &&
             !submission.weightedBlendedPipeline))
        {
            return fail(
                ParticleFeatureRecordingCode::ResourcePreparationFailed,
                submission.diagnostic.empty()
                    ? "Particle billboard prepared submission is unavailable"
                    : submission.diagnostic);
        }

        m_stats.supported = true;
        m_stats.enabled = true;
        m_stats.uploadPublished = true;
        m_stats.instanceCount = drawPlan->GetInstanceCount();
        m_stats.uploadBytes = submission.uploadBytes;
        m_stats.frameSlot = submission.frameSlot;
        m_stats.resourceCreationCount = submission.resourceCreationCount;
        m_stats.pipelineCreationCount = submission.pipelineCreationCount;
        m_stats.softDepthSnapshotReady = softDepthSnapshotReady;
        m_stats.softDepthLinearizationReady =
            softDepthLinearizationReady;
        m_stats.softDepthEnabled = softDepthLinearizationReady;
        m_stats.softDepthOptionalFallback =
            m_stats.softDepthRequested && !m_stats.softDepthRequired &&
            !m_stats.softDepthEnabled;
        m_stats.reactiveMaskEnabled =
            execution.view.reactiveMaskTarget.IsValid();
        m_stats.reactiveMaskWriteEnabled = false;
        m_stats.distortionEnabled = particleVfx.distortionRequested;
        m_stats.distortionPassScheduled = false;
        m_stats.translucencyPlanIdentity = plan.GetIdentityHash();
        m_stats.translucencyParticleSourceIdentity = particleSourceIdentity;
        m_stats.preparationCount = 1;
        m_stats.uploadPublicationCount = 1;
        m_stats.weightedBatchCount = result.weightedBatchCount;
        m_stats.weightedItemCount = result.weightedItemCount;
        m_stats.directBatchCount = result.directBatchCount;
        m_stats.directItemCount = result.directItemCount;
        m_stats.optionalSortedFallbackBatchCount =
            optionalFallbackBatchCount;
        m_stats.optionalSortedFallbackItemCount =
            optionalFallbackItemCount;

        m_recordFrameSequence = context.identity.frameSequence;
        m_recordFrameIdentity = context.featureFrameIdentity;
        m_recordGraphIdentity = context.identity.graphIdentity;
        m_recordGraphGeneration =
            context.identity.graphRecordingGeneration;
        m_recordEpoch = context.identity.recordEpoch;
        m_stats.recordFrameSequence = m_recordFrameSequence;
        m_stats.recordFrameIdentity = m_recordFrameIdentity;
        m_stats.recordGraphIdentity = m_recordGraphIdentity;
        m_stats.recordGraphGeneration = m_recordGraphGeneration;
        m_stats.recordEpoch = m_recordEpoch;

        m_batchReceipts.clear();
        m_batchReceiptsOverflow = drawPlan->batches.size() >
            RVX_PARTICLE_BATCH_RECEIPT_CAPACITY;
        const size_t receiptCount = std::min<size_t>(
            drawPlan->batches.size(),
            RVX_PARTICLE_BATCH_RECEIPT_CAPACITY);
        m_batchReceipts.reserve(receiptCount);
        for (size_t batchIndex = 0; batchIndex < receiptCount; ++batchIndex)
        {
            const ParticleBillboardDrawBatch& batch =
                drawPlan->batches[batchIndex];
            const ParticleBillboardDrawItem& firstItem =
                drawPlan->items[batch.firstItem];
            const ParticleTextureBinding* const binding =
                batchIndex < textureBindings.size()
                    ? &textureBindings[batchIndex]
                    : nullptr;
            const TranslucencyPlanEntry* const route = plan.FindEntry(
                TranslucencySourceKind::Particle,
                static_cast<uint32>(batchIndex));
            m_batchReceipts.push_back(ParticleFeatureBatchReceipt{
                .route = route != nullptr
                    ? route->route : TranslucencyRoute::Unsupported,
                .planSourceOrdinal = static_cast<uint32>(batchIndex),
                .planSourceIdentity = route != nullptr
                    ? route->sourceIdentityHash : 0,
                .blendMode = batch.blendMode,
                .effectDescriptorHash = batch.effectDescriptorHash,
                .textureAssetId =
                    firstItem.effectDescriptor.flipbook.textureAssetId,
                .texture = batch.flipbookTexture,
                .textureResourceGeneration =
                    batch.flipbookTexture.generation,
                .textureContentRevision =
                    batch.flipbookContentRevision,
                .instanceCount = batch.itemCount,
                .textureResolved = binding != nullptr &&
                    static_cast<bool>(binding->texture),
            });
        }
        m_stats.batchReceipts = m_batchReceipts;
        m_stats.batchReceiptsOverflow = m_batchReceiptsOverflow;

        auto prepared =
            std::make_shared<ParticleFeaturePreparedRecording>();
        prepared->translucencyPlan =
            std::make_shared<TranslucencyFramePlan>(plan);
        prepared->submission = std::move(submission);
        prepared->execution = execution;
        prepared->initialStats = m_stats;
        prepared->textureBindings = std::move(textureBindings);
        prepared->particleVfx = particleVfx;
        prepared->pipelineCache = m_pipelineCache;
        prepared->results = context.results.get();
        prepared->owner = this;
        prepared->snapshot = m_snapshot;
        prepared->snapshotSequence = m_snapshot->metadata.sequence;
        prepared->weightedBatchIndices =
            std::move(weightedBatchIndices);
        prepared->directBatchIndices = std::move(directBatchIndices);
        prepared->graphResources =
            std::make_shared<ParticleGraphPassData>();
        prepared->contextValid = true;

        result.code = ParticleFeatureRecordingCode::Ready;
        result.recording = std::move(prepared);
        return result;
    }

    ParticleFeatureStageRecordResult
    ParticleFeaturePass::RecordWeightedBlended(
        RenderGraph& graph,
        const ParticleFeaturePreparedRecordingRef& prepared,
        const ParticleWeightedBlendedRecordTargets& targets)
    {
        ParticleFeatureStageRecordResult result;
        const auto fail = [this, &result](
            ParticleFeatureRecordingCode code,
            std::string diagnostic)
        {
            result.code = code;
            result.diagnostic = std::move(diagnostic);
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason = result.diagnostic;
            return result;
        };
        if (!prepared || prepared->owner != this ||
            !prepared->contextValid || !prepared->translucencyPlan ||
            prepared->translucencyPlan->GetIdentityHash() == 0 ||
            prepared->snapshot != m_snapshot ||
            prepared->snapshotSequence == 0 ||
            m_snapshot == nullptr ||
            prepared->snapshotSequence != m_snapshot->metadata.sequence ||
            !prepared->graphResources ||
            prepared->execution.identity.graph != &graph ||
            !prepared->execution.identity.Matches(graph) ||
            graph.GetGraphIdentity() !=
                prepared->execution.identity.graphIdentity ||
            graph.GetRecordingGeneration() !=
                prepared->execution.identity.graphRecordingGeneration)
        {
            return fail(
                ParticleFeatureRecordingCode::StalePreparedRecording,
                "Particle weighted-blended stage rejected a stale prepared recording");
        }
        result.translucencyPlanIdentity =
            prepared->translucencyPlan->GetIdentityHash();
        result.batchCount = static_cast<uint32>(
            prepared->weightedBatchIndices.size());
        result.itemCount = prepared->initialStats.weightedItemCount;
        if (prepared->weightedRecorded)
        {
            return fail(
                ParticleFeatureRecordingCode::StageAlreadyRecorded,
                "Particle weighted-blended stage was already recorded");
        }
        if (prepared->weightedBatchIndices.empty())
        {
            prepared->weightedRecorded = true;
            result.code = ParticleFeatureRecordingCode::NoWork;
            return result;
        }
        if (prepared->translucencyPlan->HasRequiredCapabilityFailure())
        {
            return fail(
                ParticleFeatureRecordingCode::RequiredCapabilityUnavailable,
                "Particle weighted-blended stage cannot fall back a required capability");
        }

        const RHITextureDesc* const accumulationDesc =
            graph.GetTextureDesc(targets.accumulation);
        const RHITextureDesc* const revealageDesc =
            graph.GetTextureDesc(targets.revealage);
        const ViewData& preparedView = prepared->execution.view;
        if (!targets.accumulation.IsValid() ||
            !targets.revealage.IsValid() ||
            !HasCurrentGraphProvenance(
                targets.accumulation, prepared->execution.identity) ||
            !HasCurrentGraphProvenance(
                targets.revealage, prepared->execution.identity) ||
            targets.accumulation.index == targets.revealage.index ||
            targets.accumulation.index == preparedView.colorTarget.index ||
            targets.accumulation.index ==
                preparedView.reactiveMaskTarget.index ||
            targets.accumulation.index == preparedView.depthTarget.index ||
            targets.revealage.index == preparedView.colorTarget.index ||
            targets.revealage.index ==
                preparedView.reactiveMaskTarget.index ||
            targets.revealage.index == preparedView.depthTarget.index ||
            accumulationDesc == nullptr || revealageDesc == nullptr ||
            accumulationDesc->format !=
                PipelineCache::GetWeightedBlendedOITAccumulationFormat() ||
            revealageDesc->format !=
                PipelineCache::GetWeightedBlendedOITRevealageFormat() ||
            accumulationDesc->width != preparedView.viewportWidth ||
            accumulationDesc->height != preparedView.viewportHeight ||
            revealageDesc->width != accumulationDesc->width ||
            revealageDesc->height != accumulationDesc->height ||
            accumulationDesc->sampleCount != RHISampleCount::Count1 ||
            revealageDesc->sampleCount != RHISampleCount::Count1)
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidGraphTargets,
                "Particle weighted-blended stage requires distinct current-graph RGBA16F/R16F attachments");
        }

        const auto graphResources = prepared->graphResources;
        graph.AddPass<ParticleGraphPassData>(
            "ParticleWeightedBlendedOIT",
            RenderGraphPassType::Graphics,
            [prepared, targets, graphResources](
                RenderGraphBuilder& builder,
                ParticleGraphPassData& data)
            {
                data.submission = prepared->submission;
                data.execution = prepared->execution;
                data.initialStats = prepared->initialStats;
                data.textureBindings = prepared->textureBindings;
                data.batchIndices = prepared->weightedBatchIndices;
                data.particleVfx = prepared->particleVfx;
                data.pipelineCache = prepared->pipelineCache;
                data.results = prepared->results;
                data.contextValid = prepared->contextValid;
                data.stage = ParticleFeatureGraphStage::WeightedBlended;
                data.owner = prepared->owner;
                if (!PrepareGraphResources(data, builder, false) ||
                    !PrepareWeightedBlendedGraphTargets(
                        data, builder, targets))
                {
                    data.contextValid = false;
                }
                prepared->commonResourcesPrepared = data.contextValid;
                *graphResources = data;
            },
            [](const ParticleGraphPassData& data,
               RenderGraphPassContext& graphContext)
            {
                ExecuteWeightedBlendedGraphPass(data, graphContext);
            });
        prepared->weightedRecorded = true;
        prepared->weightedAccumulation = targets.accumulation;
        prepared->weightedRevealage = targets.revealage;
        m_stats.graphPassScheduled = true;
        m_stats.weightedGraphPassScheduled = true;
        result.code = ParticleFeatureRecordingCode::Recorded;
        return result;
    }

    ParticleFeatureStageRecordResult ParticleFeaturePass::RecordDirect(
        RenderGraph& graph,
        const ParticleFeaturePreparedRecordingRef& prepared,
        const ParticleDirectRecordTargets& targets)
    {
        ParticleFeatureStageRecordResult result;
        result.hdrSceneColor = targets.hdrSceneColor;
        const auto fail = [this, &result](
            ParticleFeatureRecordingCode code,
            std::string diagnostic)
        {
            result.code = code;
            result.diagnostic = std::move(diagnostic);
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason = result.diagnostic;
            return result;
        };
        if (!prepared || prepared->owner != this ||
            !prepared->contextValid || !prepared->translucencyPlan ||
            prepared->snapshot != m_snapshot ||
            prepared->snapshotSequence == 0 ||
            m_snapshot == nullptr ||
            prepared->snapshotSequence != m_snapshot->metadata.sequence ||
            !prepared->graphResources ||
            prepared->execution.identity.graph != &graph ||
            !prepared->execution.identity.Matches(graph) ||
            graph.GetGraphIdentity() !=
                prepared->execution.identity.graphIdentity ||
            graph.GetRecordingGeneration() !=
                prepared->execution.identity.graphRecordingGeneration)
        {
            return fail(
                ParticleFeatureRecordingCode::StalePreparedRecording,
                "Particle direct stage rejected a stale prepared recording");
        }
        result.translucencyPlanIdentity =
            prepared->translucencyPlan->GetIdentityHash();
        result.batchCount =
            static_cast<uint32>(prepared->directBatchIndices.size());
        result.itemCount = prepared->initialStats.directItemCount;
        if (prepared->directRecorded)
        {
            return fail(
                ParticleFeatureRecordingCode::StageAlreadyRecorded,
                "Particle direct stage was already recorded");
        }
        if (!prepared->weightedBatchIndices.empty() &&
            !prepared->weightedRecorded)
        {
            return fail(
                ParticleFeatureRecordingCode::StageOrderViolation,
                "Particle direct stage must be recorded after weighted accumulation and feature composite");
        }
        const bool distortionRequested =
            prepared->particleVfx.distortionRequested;
        if (prepared->directBatchIndices.empty() && !distortionRequested)
        {
            prepared->directRecorded = true;
            result.code = ParticleFeatureRecordingCode::NoWork;
            return result;
        }
        if (!targets.hdrSceneColor.IsValid() ||
            !targets.reactiveMask.IsValid() ||
            !HasCurrentGraphProvenance(
                targets.hdrSceneColor, prepared->execution.identity) ||
            !HasCurrentGraphProvenance(
                targets.reactiveMask, prepared->execution.identity) ||
            (prepared->weightedAccumulation.IsValid() &&
             (targets.hdrSceneColor.index ==
                  prepared->weightedAccumulation.index ||
              targets.reactiveMask.index ==
                  prepared->weightedAccumulation.index)) ||
            (prepared->weightedRevealage.IsValid() &&
             (targets.hdrSceneColor.index ==
                  prepared->weightedRevealage.index ||
              targets.reactiveMask.index ==
                  prepared->weightedRevealage.index)) ||
            (distortionRequested &&
             (!prepared->particleVfx.distortionVectors.IsValid() ||
              !prepared->particleVfx.compositedSceneColor.IsValid() ||
              prepared->particleVfx.compositedSceneColor.index ==
                  targets.hdrSceneColor.index)))
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidGraphTargets,
                "Particle direct stage requires distinct graph-owned HDR, reactive and distortion outputs");
        }
        const RHITextureDesc* const directColorDesc =
            graph.GetTextureDesc(targets.hdrSceneColor);
        const RHITextureDesc* const directReactiveDesc =
            graph.GetTextureDesc(targets.reactiveMask);
        if (directColorDesc == nullptr || directReactiveDesc == nullptr ||
            directColorDesc->format != RHIFormat::RGBA16_FLOAT ||
            directReactiveDesc->format != RHIFormat::R8_UNORM ||
            directColorDesc->width !=
                prepared->execution.view.viewportWidth ||
            directColorDesc->height !=
                prepared->execution.view.viewportHeight ||
            directReactiveDesc->width != directColorDesc->width ||
            directReactiveDesc->height != directColorDesc->height ||
            directColorDesc->sampleCount != RHISampleCount::Count1 ||
            directReactiveDesc->sampleCount != RHISampleCount::Count1)
        {
            return fail(
                ParticleFeatureRecordingCode::InvalidGraphTargets,
                "Particle direct stage requires matching single-sample RGBA16F and R8 targets");
        }

        const auto graphResources = prepared->graphResources;
        if (!prepared->directBatchIndices.empty())
        {
            graph.AddPass<ParticleGraphPassData>(
                "ParticleDirectTranslucency",
                RenderGraphPassType::Graphics,
                [prepared, targets, graphResources](
                    RenderGraphBuilder& builder,
                    ParticleGraphPassData& data)
                {
                    if (prepared->commonResourcesPrepared)
                    {
                        data = *graphResources;
                        if (!PrepareReusedCommonGraphResources(data, builder))
                        {
                            data.contextValid = false;
                        }
                    }
                    else
                    {
                        data.submission = prepared->submission;
                        data.execution = prepared->execution;
                        data.initialStats = prepared->initialStats;
                        data.textureBindings = prepared->textureBindings;
                        data.particleVfx = prepared->particleVfx;
                        data.pipelineCache = prepared->pipelineCache;
                        data.results = prepared->results;
                        data.contextValid = prepared->contextValid;
                        data.owner = prepared->owner;
                        if (!PrepareGraphResources(data, builder, false))
                        {
                            data.contextValid = false;
                        }
                    }
                    data.batchIndices = prepared->directBatchIndices;
                    data.stage = ParticleFeatureGraphStage::Direct;
                    data.execution.view.colorTarget = targets.hdrSceneColor;
                    data.execution.view.reactiveMaskTarget =
                        targets.reactiveMask;
                    if (data.contextValid &&
                        !PrepareDirectGraphTargets(data, builder, targets))
                    {
                        data.contextValid = false;
                    }
                    prepared->commonResourcesPrepared = data.contextValid;
                    *graphResources = data;
                },
                [](const ParticleGraphPassData& data,
                   RenderGraphPassContext& graphContext)
                {
                    ExecuteGraphPass(data, graphContext);
                });
            m_stats.graphPassScheduled = true;
            m_stats.directGraphPassScheduled = true;
            m_stats.reactiveMaskEnabled = true;
            m_stats.reactiveMaskWriteEnabled = true;
        }

        if (distortionRequested)
        {
            graph.AddPass<ParticleGraphPassData>(
                "ParticleDistortion",
                RenderGraphPassType::Graphics,
                [prepared, targets, graphResources](
                    RenderGraphBuilder& builder,
                    ParticleGraphPassData& data)
                {
                    if (prepared->commonResourcesPrepared)
                    {
                        data = *graphResources;
                    }
                    else
                    {
                        data.submission = prepared->submission;
                        data.execution = prepared->execution;
                        data.initialStats = prepared->initialStats;
                        data.textureBindings = prepared->textureBindings;
                        data.particleVfx = prepared->particleVfx;
                        data.pipelineCache = prepared->pipelineCache;
                        data.results = prepared->results;
                        data.contextValid = prepared->contextValid;
                        data.owner = prepared->owner;
                        if (!PrepareGraphResources(data, builder, false))
                        {
                            data.contextValid = false;
                        }
                    }
                    data.colorHandle = targets.hdrSceneColor;
                    data.stage = ParticleFeatureGraphStage::Distortion;
                    if (data.contextValid &&
                        !PrepareDistortionGraphResources(data, builder))
                    {
                        data.contextValid = false;
                    }
                    prepared->commonResourcesPrepared = data.contextValid;
                    *graphResources = data;
                },
                [](const ParticleGraphPassData& data,
                   RenderGraphPassContext& graphContext)
                {
                    ExecuteDistortionGraphPass(data, graphContext);
                });
            graph.AddPass<ParticleGraphPassData>(
                "ParticleDistortionComposite",
                RenderGraphPassType::Graphics,
                [graphResources](RenderGraphBuilder& builder,
                                 ParticleGraphPassData& data)
                {
                    data = *graphResources;
                    data.stage =
                        ParticleFeatureGraphStage::DistortionComposite;
                    if (!PrepareDistortionCompositeGraphResources(
                            data, builder))
                    {
                        data.contextValid = false;
                    }
                    *graphResources = data;
                },
                [](const ParticleGraphPassData& data,
                   RenderGraphPassContext& graphContext)
                {
                    ExecuteDistortionCompositeGraphPass(
                        data, graphContext);
                });
            m_stats.distortionPassScheduled = true;
            result.hdrSceneColor =
                prepared->particleVfx.compositedSceneColor;
        }

        prepared->directRecorded = true;
        result.code = ParticleFeatureRecordingCode::Recorded;
        return result;
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
            execution.MatchesTargetGraph(graph) && execution.IsFrameIdentityValid() &&
            IsCompatibleFroxelVolumetricLighting(
                execution.volumetricLighting, execution.identity) &&
            m_pipelineCache != nullptr;
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
        const ParticleVfxRecordInput& particleVfx = context.particleVfx;
        if (const char* const exposureFailure =
                DescribeParticleExposureRecordFailure(
                    particleVfx, context.identity);
            exposureFailure != nullptr)
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason = exposureFailure;
            return;
        }
        if (!particleVfx.IsCompatibleWith(
                context.identity,
                RenderOutputExtent{execution.view.viewportWidth,
                                   execution.view.viewportHeight}))
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason =
                "Particle VFX auxiliary graph inputs belong to another recording";
            return;
        }
        std::string graphTargetDiagnostic;
        if (!ValidateParticleGraphTargets(
                graph, execution.view, particleVfx, graphTargetDiagnostic))
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason = std::move(graphTargetDiagnostic);
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

        const bool softDepthSnapshotReady =
            particleVfx.softDepthSnapshot.texture.IsValid();
        Mat4 jitteredInverseProjection = Mat4Identity();
        const bool softDepthLinearizationReady = softDepthSnapshotReady &&
            TryGetFiniteJitteredInverseProjection(
                execution.view, jitteredInverseProjection);
        if (particleVfx.softDepthRequired && !softDepthSnapshotReady)
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason =
                "Particle required soft depth has no compatible depth snapshot";
            return;
        }
        if (particleVfx.softDepthRequired && !softDepthLinearizationReady)
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason =
                "Particle required soft depth needs a finite inverse jittered projection matrix";
            return;
        }
        if (particleVfx.distortionRequested &&
            (!particleVfx.distortionVectors.IsValid() ||
             !particleVfx.compositedSceneColor.IsValid() ||
             particleVfx.compositedSceneColor.index ==
                 execution.view.colorTarget.index))
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason =
                "Particle distortion requires distinct graph-owned vector and HDR composite targets";
            return;
        }

        std::vector<ParticleTextureBinding> textureBindings;
        std::string textureBindingDiagnostic;
        if (!ResolveParticleTextureBindings(
                *plan,
                m_resourceRegistry,
                textureBindings,
                m_stats,
                textureBindingDiagnostic))
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason = std::move(textureBindingDiagnostic);
            return;
        }

        std::vector<bool> spriteTextureAvailableByBatch;
        spriteTextureAvailableByBatch.reserve(textureBindings.size());
        for (const ParticleTextureBinding& binding : textureBindings)
        {
            // A batch can only sample the exact texture generation which was
            // resolved from the publication view above. Optional flipbooks
            // deliberately use the immutable fallback texture when that
            // generation is no longer available.
            spriteTextureAvailableByBatch.push_back(binding.graphTexture.IsValid() ||
                                                     static_cast<bool>(binding.texture));
        }

        ParticleBillboardRenderer::Submission submission =
            m_renderer->Prepare(
                plan,
                execution.view,
                softDepthLinearizationReady,
                spriteTextureAvailableByBatch);
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
        m_stats.softDepthSnapshotReady = softDepthSnapshotReady;
        // The copied D32 snapshot is only usable by the soft-particle shader
        // after the exact per-view inverse projection data has passed the
        // finite-value gate above. Optional effects then explicitly use
        // their descriptor's fallback policy rather than sampling bad depth.
        m_stats.softDepthLinearizationReady = softDepthLinearizationReady;
        m_stats.softDepthEnabled = softDepthLinearizationReady;
        m_stats.softDepthOptionalFallback =
            m_stats.softDepthRequested && !m_stats.softDepthRequired &&
            !m_stats.softDepthEnabled;
        m_stats.reactiveMaskEnabled = execution.view.reactiveMaskTarget.IsValid();
        m_stats.reactiveMaskWriteEnabled = m_stats.reactiveMaskEnabled;
        m_stats.distortionEnabled = particleVfx.distortionRequested;
        // This stays false until every distortion-chain pass has completed
        // RenderGraph registration below. Composition may only publish the
        // replacement HDR target after that atomic registration point.
        m_stats.distortionPassScheduled = false;
        m_recordFrameSequence = context.identity.frameSequence;
        m_recordFrameIdentity = context.featureFrameIdentity;
        m_recordGraphIdentity = context.identity.graphIdentity;
        m_recordGraphGeneration = context.identity.graphRecordingGeneration;
        m_recordEpoch = context.identity.recordEpoch;
        m_batchReceipts.clear();
        m_batchReceiptsOverflow = plan->batches.size() >
            RVX_PARTICLE_BATCH_RECEIPT_CAPACITY;
        const size_t receiptCount = std::min<size_t>(
            plan->batches.size(), RVX_PARTICLE_BATCH_RECEIPT_CAPACITY);
        m_batchReceipts.reserve(receiptCount);
        for (size_t batchIndex = 0; batchIndex < receiptCount; ++batchIndex)
        {
            const ParticleBillboardDrawBatch& batch = plan->batches[batchIndex];
            const ParticleBillboardDrawItem& firstItem =
                plan->items[batch.firstItem];
            const ParticleTextureBinding* binding =
                batchIndex < textureBindings.size()
                    ? &textureBindings[batchIndex]
                    : nullptr;
            m_batchReceipts.push_back(ParticleFeatureBatchReceipt{
                .blendMode = batch.blendMode,
                .effectDescriptorHash = batch.effectDescriptorHash,
                .textureAssetId = firstItem.effectDescriptor.flipbook.textureAssetId,
                .texture = batch.flipbookTexture,
                .textureResourceGeneration = batch.flipbookTexture.generation,
                .textureContentRevision = batch.flipbookContentRevision,
                .instanceCount = batch.itemCount,
                .textureResolved = binding != nullptr &&
                    static_cast<bool>(binding->texture),
            });
        }
        m_stats.recordFrameSequence = m_recordFrameSequence;
        m_stats.recordFrameIdentity = m_recordFrameIdentity;
        m_stats.recordGraphIdentity = m_recordGraphIdentity;
        m_stats.recordGraphGeneration = m_recordGraphGeneration;
        m_stats.recordEpoch = m_recordEpoch;
        m_stats.batchReceipts = m_batchReceipts;
        m_stats.batchReceiptsOverflow = m_batchReceiptsOverflow;

        const ParticleFeaturePassStats recordStats = m_stats;
        const auto graphResources = std::make_shared<ParticleGraphPassData>();
        graph.AddPass<ParticleGraphPassData>(
            GetName(), GetPassType(),
            [submission = std::move(submission), execution, contextValid, recordStats,
             textureBindings = std::move(textureBindings), particleVfx,
             pipelineCache = m_pipelineCache, results = context.results.get(),
             graphResources, this](
                RenderGraphBuilder& builder, ParticleGraphPassData& data)
            {
                data.submission = submission;
                data.execution = execution;
                data.initialStats = recordStats;
                data.textureBindings = textureBindings;
                data.particleVfx = particleVfx;
                data.pipelineCache = pipelineCache;
                data.results = results;
                data.contextValid = contextValid;
                data.owner = this;
                if (!PrepareGraphResources(data, builder, true))
                {
                    data.contextValid = false;
                }
                *graphResources = data;
            },
            [](const ParticleGraphPassData& data, RenderGraphPassContext& graphContext)
            {
                ExecuteGraphPass(data, graphContext);
            });

        if (particleVfx.distortionRequested)
        {
            graph.AddPass<ParticleGraphPassData>(
                "ParticleDistortion",
                RenderGraphPassType::Graphics,
                [graphResources](RenderGraphBuilder& builder,
                                 ParticleGraphPassData& data)
                {
                    data = *graphResources;
                    if (!PrepareDistortionGraphResources(data, builder))
                    {
                        data.contextValid = false;
                    }
                    *graphResources = data;
                },
                [](const ParticleGraphPassData& data,
                   RenderGraphPassContext& graphContext)
                {
                    ExecuteDistortionGraphPass(data, graphContext);
                });
            graph.AddPass<ParticleGraphPassData>(
                "ParticleDistortionComposite",
                RenderGraphPassType::Graphics,
                [graphResources](RenderGraphBuilder& builder,
                                 ParticleGraphPassData& data)
                {
                    data = *graphResources;
                    if (!PrepareDistortionCompositeGraphResources(data, builder))
                    {
                        data.contextValid = false;
                    }
                    *graphResources = data;
                },
                [](const ParticleGraphPassData& data,
                   RenderGraphPassContext& graphContext)
                {
                    ExecuteDistortionCompositeGraphPass(data, graphContext);
                });
            // All three passes have registered against the same immutable
            // graph resources. Only now may composition route subsequent HDR
            // consumers to the composite target.
            m_stats.distortionPassScheduled = true;
        }
    }

    void ParticleFeaturePass::RebuildStats()
    {
        ParticleFeaturePassStats stats;
        stats.lightHintRequestedCount = m_lightHintRequestedCount;
        stats.lightHintAdmittedCount = m_lightHintAdmittedCount;
        stats.lightHintDroppedCount = m_lightHintDroppedCount;
        stats.recordFrameSequence = m_recordFrameSequence;
        stats.recordFrameIdentity = m_recordFrameIdentity;
        stats.recordGraphIdentity = m_recordGraphIdentity;
        stats.recordGraphGeneration = m_recordGraphGeneration;
        stats.recordEpoch = m_recordEpoch;
        stats.batchReceipts = m_batchReceipts;
        stats.batchReceiptsOverflow = m_batchReceiptsOverflow;
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
            if (item.effectDescriptor.softDepthMode !=
                ParticleRenderSoftDepthMode::Disabled)
            {
                stats.softDepthRequested = true;
                stats.softDepthRequired = stats.softDepthRequired ||
                    item.effectDescriptor.softDepthMode ==
                        ParticleRenderSoftDepthMode::Required;
            }
            stats.flipbookRequested = stats.flipbookRequested ||
                item.effectDescriptor.flipbook.IsBound();
            stats.reactiveMaskRequested = stats.reactiveMaskRequested ||
                item.effectDescriptor.reactiveContribution > 0.0f;
            stats.distortionRequested = stats.distortionRequested ||
                item.effectDescriptor.distortionStrengthPixels > 0.0f;
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
