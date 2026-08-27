#include "Render/Passes/ParticleFeaturePass.h"

#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "Render/Particles/ParticleBillboardDrawPlan.h"
#include "Resources/RenderResourceRegistry.h"
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
        constexpr uint32 RVX_PARTICLE_BATCH_RECEIPT_CAPACITY = 256u;
        constexpr uint32 RVX_PARTICLE_FALLBACK_TEXTURE_ROW_PITCH = 256u;
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

            if (particleVfx.softDepthSnapshot.IsValid())
            {
                const RHITextureDesc* const softDepthDesc =
                    graph.GetTextureDesc(particleVfx.softDepthSnapshot);
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

        [[nodiscard]] Submission Prepare(
            std::shared_ptr<const ParticleBillboardDrawPlan> plan,
            const ViewData& view,
            bool softDepthAvailable,
            const std::vector<bool>& spriteTextureAvailableByBatch)
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
            setLayoutDesc.AddBinding(4, RHIBindingType::Sampler,
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
        RHISamplerRef m_linearClampSampler;
        RHITextureRef m_fallbackTexture;
        RHITextureViewRef m_fallbackTextureView;
        std::array<RHIPipelineRef, 3> m_pipelines{};
        RHIPipelineRef m_distortionPipeline;
        RHIPipelineRef m_compositePipeline;
        uint64 m_resourceCreationCount = 0;
        uint64 m_pipelineCreationCount = 0;
        std::string m_unavailableReason;
    };

    namespace
    {
        struct ParticleTextureBinding
        {
            RenderResourceHandle resource{};
            RHITextureRef texture;
            RHITextureAccessSnapshot accessSnapshot{};
            bool required = false;
            RGTextureHandle graphTexture{};
            RGTextureViewHandle graphView{};
        };

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
            RGTextureViewHandle colorView{};
            RGTextureViewHandle depthView{};
            RGTextureViewHandle reactiveMaskView{};
            RGTextureViewHandle softDepthView{};
            RGTextureViewHandle distortionView{};
            RGTextureViewHandle compositeSourceView{};
            RGTextureViewHandle compositeTargetView{};
            std::vector<ParticleTextureBinding> textureBindings;
            ParticleVfxRecordInput particleVfx{};
            RenderPassRecordResults* results = nullptr;
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
            if (view.viewportX != 0 || view.viewportY != 0 ||
                view.viewportWidth == 0 || view.viewportHeight == 0 ||
                colorDesc == nullptr || colorDesc->format != RHIFormat::RGBA16_FLOAT ||
                colorDesc->width != view.viewportWidth ||
                colorDesc->height != view.viewportHeight)
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
                !Retain(builder, data.submission.compositePipeline.Get()))
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
            reactiveMaskViewDesc.debugName = "ParticleBillboard.ReactiveMaskRTV";
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
                data.particleVfx.softDepthSnapshot.IsValid())
            {
                const RHITextureDesc* const softDepthDesc = builder.GetTextureDesc(
                    data.particleVfx.softDepthSnapshot);
                if (softDepthDesc == nullptr ||
                    softDepthDesc->format != RHIFormat::D32_FLOAT ||
                    softDepthDesc->width != colorDesc->width ||
                    softDepthDesc->height != colorDesc->height)
                {
                    return false;
                }
                data.softDepthHandle = builder.Read(
                    data.particleVfx.softDepthSnapshot,
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

        [[nodiscard]] bool PrepareDistortionGraphResources(
            ParticleGraphPassData& data,
            RenderGraphBuilder& builder)
        {
            if (!data.contextValid || !data.submission.IsReady() ||
                !data.particleVfx.distortionRequested ||
                !data.particleVfx.distortionVectors.IsValid() ||
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
                data.distortionView.IsValid();
        }

        [[nodiscard]] bool PrepareDistortionCompositeGraphResources(
            ParticleGraphPassData& data,
            RenderGraphBuilder& builder)
        {
            if (!data.contextValid || !data.submission.IsReady() ||
                !data.particleVfx.distortionRequested ||
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
            if (spriteView == nullptr || depthOrDistortionView == nullptr)
            {
                return {};
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
                .BindSampler(4, data.submission.linearClampSampler.Get());
            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet || !descriptorSet->IsReadyForBinding(
                    data.submission.descriptorSetLayout.Get()) ||
                !context.RetainSubmissionResource(descriptorSet))
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
                !data.colorHandle.IsValid() || !data.colorView.IsValid() ||
                !data.reactiveMaskHandle.IsValid() || !data.reactiveMaskView.IsValid() ||
                !data.depthHandle.IsValid() || !data.depthView.IsValid() ||
                (data.particleVfx.softDepthRequired &&
                 (!data.softDepthHandle.IsValid() || !data.softDepthView.IsValid())))
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
            for (uint32 batchIndex = 0;
                 batchIndex < data.submission.plan->batches.size();
                 ++batchIndex)
            {
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
                ++stats.drawCallCount;
            }
            commands.EndRenderPass();
            stats.drawSubmitted = stats.drawCallCount != 0;
            stats.reactiveMaskWriteDrawCount = stats.drawCallCount;
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
        const ParticleVfxRecordInput& particleVfx = context.particleVfx;
        if (!particleVfx.IsEmpty() &&
            !particleVfx.IsCompatibleWith(context.identity))
        {
            ++m_stats.fallbackCount;
            m_stats.supported = false;
            m_stats.enabled = false;
            m_stats.unsupportedReason =
                "Particle VFX graph inputs belong to a different graph recording";
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
            particleVfx.softDepthSnapshot.IsValid();
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
             results = context.results.get(), graphResources, this](
                RenderGraphBuilder& builder, ParticleGraphPassData& data)
            {
                data.submission = submission;
                data.execution = execution;
                data.initialStats = recordStats;
                data.textureBindings = textureBindings;
                data.particleVfx = particleVfx;
                data.results = results;
                data.contextValid = contextValid;
                data.owner = this;
                if (!PrepareGraphResources(data, builder))
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
            if (!graphResources->contextValid)
            {
                ++m_stats.fallbackCount;
                m_stats.supported = false;
                m_stats.enabled = false;
                m_stats.unsupportedReason =
                    "Particle distortion could not reuse the color-pass graph resources";
                return;
            }
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
            if (!graphResources->contextValid)
            {
                ++m_stats.fallbackCount;
                m_stats.supported = false;
                m_stats.enabled = false;
                m_stats.unsupportedReason =
                    "Particle distortion pass graph registration failed";
                return;
            }
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
            if (!graphResources->contextValid)
            {
                ++m_stats.fallbackCount;
                m_stats.supported = false;
                m_stats.enabled = false;
                m_stats.unsupportedReason =
                    "Particle distortion composite graph registration failed";
                return;
            }
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
