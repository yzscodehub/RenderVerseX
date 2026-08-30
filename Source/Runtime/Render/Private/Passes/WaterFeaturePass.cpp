#include "Render/Passes/WaterFeaturePass.h"
#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderClipConvention.h"
#include "Render/Water/GerstnerReference.h"

#include "Core/MathTypes.h"
#include "ShaderContracts/ShaderProgramProvider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    constexpr uint32 RVX_WATER_FRAME_SLOT_COUNT = RVX_MAX_FRAME_COUNT;
    constexpr uint32 RVX_WATER_GRID_RESOLUTION = 64u;
    constexpr uint32 RVX_WATER_MAX_SURFACES_PER_FRAME = 64u;
    // 256-byte aligned descriptor range: the v3 water ABI is 544 bytes.
    constexpr uint32 RVX_WATER_CONSTANT_BUFFER_SIZE = 768u;
    constexpr uint32 RVX_WATER_SURFACE_RECEIPT_CAPACITY = 64u;
    constexpr uint32 RVX_WATER_EXPOSURE_STATE_BINDING = 5u;

    struct WaterGridVertex
    {
        Vec2 localPosition{};
    };
    static_assert(sizeof(WaterGridVertex) == sizeof(float32) * 2u);

    struct alignas(16) WaterSurfaceGPUConstants
    {
        Mat4 viewProjection = Mat4Identity();
        Mat4 inverseProjection = Mat4Identity();
        Vec4 cameraPositionAndPadding{};
        Vec4 inverseExtentAndReverseZ{};
        Vec4 surfacePositionAndDepth{};
        Vec4 surfaceSizeAndTransparency{};
        Vec4 shallowColorAndRefraction{};
        Vec4 deepColorAndReflection{};
        Vec4 foamColorAndParameters{};
        Vec4 absorptionAndNormalScale{};
        Vec4 foamAndFeatureParameters{};
        Vec4 detailTemporalPhases{};
        std::array<Vec4, RVX_WATER_MAX_GERSTNER_WAVES> waveDirectionAmplitude{};
        std::array<Vec4, RVX_WATER_MAX_GERSTNER_WAVES> waveSteepnessTemporalPhase{};
    };
    static_assert(sizeof(WaterSurfaceGPUConstants) <= RVX_WATER_CONSTANT_BUFFER_SIZE);

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

    [[nodiscard]] bool TryGetInverseProjection(
        const ViewData& view, Mat4& outInverseProjection) noexcept
    {
        if (!IsFiniteMatrix(view.jitteredProjectionMatrix))
        {
            return false;
        }
        outInverseProjection = inverse(view.jitteredProjectionMatrix);
        return IsFiniteMatrix(outInverseProjection);
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

    [[nodiscard]] bool IsReadyWaterItem(
        const WaterRenderSnapshotItem& item) noexcept
    {
        return item.payloadStatus == WaterRenderSnapshotPayloadStatus::Ready &&
            item.header.Validate(
                WaterRenderFeatureChannelDescriptor::ChannelId,
                WaterRenderFeatureChannelDescriptor::SchemaVersion) ==
                RenderFeatureChannelValidationCode::Valid &&
            item.IsStructurallyValid();
    }

    [[nodiscard]] bool HasReadyWaterItem(
        const WaterRenderSnapshot* snapshot) noexcept
    {
        if (snapshot == nullptr)
        {
            return false;
        }
        return std::any_of(snapshot->items.begin(), snapshot->items.end(),
            [](const WaterRenderSnapshotItem& item)
            {
                return item.payloadStatus ==
                    WaterRenderSnapshotPayloadStatus::Ready;
            });
    }
} // namespace

/** Render-owned persistent mesh/pipeline state for all water surfaces. */
class WaterSurfaceRenderer final
{
public:
    struct Submission
    {
        IRHIDevice* device = nullptr;
        RHIBufferRef constantsBuffer;
        RHIBufferRef gridVertexBuffer;
        RHIBufferRef gridIndexBuffer;
        RHIDescriptorSetLayoutRef descriptorSetLayout;
        RHISamplerRef linearClampSampler;
        RHIPipelineRef pipeline;
        uint32 frameSlot = RVX_INVALID_INDEX;
        uint32 surfaceCount = 0;
        uint32 gridVertexCount = 0;
        uint32 gridIndexCount = 0;
        uint64 uploadBytes = 0;
        uint64 resourceCreationCount = 0;
        uint64 pipelineCreationCount = 0;
        std::vector<WaterFeatureSurfaceReceipt> surfaceReceipts;
        bool receiptsOverflow = false;
        std::string diagnostic;

        [[nodiscard]] bool IsReady() const noexcept
        {
            return device != nullptr && constantsBuffer && gridVertexBuffer &&
                gridIndexBuffer && descriptorSetLayout && linearClampSampler &&
                pipeline && frameSlot != RVX_INVALID_INDEX && surfaceCount != 0 &&
                gridVertexCount != 0 && gridIndexCount != 0;
        }
    };

    void SetShaderProgramProvider(IShaderProgramProvider* provider) noexcept
    {
        if (m_shaderProgramProvider != provider)
        {
            ResetPipelineResources();
            m_shaderProgramProvider = provider;
            m_initializationAttempted = false;
        }
    }

    void SetReverseZ(bool reverseZ) noexcept
    {
        if (m_reverseZ != reverseZ)
        {
            ResetPipelineResources();
            m_reverseZ = reverseZ;
            m_initializationAttempted = false;
        }
    }

    void OnAdd(IRHIDevice* device)
    {
        if (m_device != device)
        {
            ResetAllResources();
            m_device = device;
        }
        if (m_device != nullptr && m_shaderProgramProvider != nullptr)
        {
            static_cast<void>(EnsureResources());
        }
    }

    void OnRemove() noexcept
    {
        ResetAllResources();
        m_device = nullptr;
    }

    [[nodiscard]] bool IsReady() const noexcept
    {
        return m_pipeline && m_descriptorSetLayout && m_linearClampSampler &&
            m_gridVertexBuffer && m_gridIndexBuffer;
    }

    [[nodiscard]] const std::string& GetUnavailableReason() const noexcept
    {
        return m_unavailableReason;
    }

    [[nodiscard]] Submission Prepare(const WaterRenderSnapshot& snapshot,
                                     const EffectFrameTime& effectTime,
                                     const ViewData& view)
    {
        Submission submission;
        submission.resourceCreationCount = m_resourceCreationCount;
        submission.pipelineCreationCount = m_pipelineCreationCount;
        if (!std::isfinite(effectTime.simulationSeconds))
        {
            submission.diagnostic = "Water requires a finite frozen EffectFrameTime";
            return submission;
        }
        Vec4 detailTemporalPhases{};
        if (!EvaluateWaterDetailTemporalPhases(
                effectTime.simulationSeconds, detailTemporalPhases))
        {
            submission.diagnostic =
                "Water could not reduce frozen EffectFrameTime detail phases";
            return submission;
        }
        if (m_device == nullptr)
        {
            submission.diagnostic = "Water renderer has no RHI device";
            return submission;
        }
        if (!EnsureResources())
        {
            submission.diagnostic = m_unavailableReason;
            return submission;
        }
        const uint32 frameSlot = m_device->GetCurrentFrameIndex();
        if (frameSlot >= m_frameSlots.size())
        {
            submission.diagnostic = "Water renderer received an invalid frame slot";
            return submission;
        }
        FrameSlot& slot = m_frameSlots[frameSlot];
        if (!EnsureFrameSlot(slot))
        {
            submission.diagnostic = m_unavailableReason;
            return submission;
        }

        Mat4 inverseProjection{};
        if (!TryGetInverseProjection(view, inverseProjection) ||
            view.viewportX != 0 || view.viewportY != 0 ||
            view.viewportWidth == 0 || view.viewportHeight == 0)
        {
            submission.diagnostic =
                "Water requires finite jittered projection and a valid zero-origin viewport";
            return submission;
        }

        slot.constants.clear();
        slot.constants.reserve(std::min<size_t>(
            snapshot.items.size(), RVX_WATER_MAX_SURFACES_PER_FRAME));
        submission.surfaceReceipts.reserve(std::min<size_t>(
            snapshot.items.size(), RVX_WATER_SURFACE_RECEIPT_CAPACITY));
        for (const WaterRenderSnapshotItem& item : snapshot.items)
        {
            if (item.payloadStatus == WaterRenderSnapshotPayloadStatus::Disabled)
            {
                continue;
            }
            if (!IsReadyWaterItem(item))
            {
                submission.diagnostic =
                    "Water snapshot contains a non-canonical Ready payload";
                return submission;
            }
            if (slot.constants.size() >= RVX_WATER_MAX_SURFACES_PER_FRAME)
            {
                submission.diagnostic = "Water snapshot exceeds the fixed surface budget";
                return submission;
            }
            WaterSurfaceGPUConstants constants;
            // ViewData is semantic clip space. Apply the same one-time
            // backend conversion as PipelineCache before this feature-owned
            // vertex shader consumes it.
            constants.viewProjection = ApplyBackendClipConvention(
                view.jitteredViewProjectionMatrix,
                m_device->GetBackendType());
            constants.inverseProjection = inverseProjection;
            constants.cameraPositionAndPadding = Vec4(view.cameraPosition, 0.0f);
            constants.inverseExtentAndReverseZ = Vec4(
                1.0f / static_cast<float32>(view.viewportWidth),
                1.0f / static_cast<float32>(view.viewportHeight),
                m_reverseZ ? 1.0f : 0.0f,
                0.0f);
            constants.surfacePositionAndDepth = Vec4(item.worldPosition, item.depth);
            constants.surfaceSizeAndTransparency = Vec4(
                item.size.x, item.size.y, item.transparency,
                item.roughness);
            constants.shallowColorAndRefraction = Vec4(
                item.shallowColor, item.refractionStrength);
            constants.deepColorAndReflection = Vec4(
                item.deepColor, item.reflectionStrength);
            constants.foamColorAndParameters = Vec4(
                item.foamColor, item.foamEnabled ? item.foamIntensity : 0.0f);
            constants.absorptionAndNormalScale = Vec4(
                item.absorptionCoefficient, item.normalScale);
            constants.foamAndFeatureParameters = Vec4(
                item.foamEnabled ? item.foamIntensity : 0.0f,
                item.foamCrestThreshold,
                item.reflectionEnabled ? 1.0f : 0.0f,
                item.refractionEnabled ? 1.0f : 0.0f);
            constants.detailTemporalPhases = detailTemporalPhases;
            for (uint32 waveIndex = 0;
                 waveIndex < RVX_WATER_MAX_GERSTNER_WAVES;
                 ++waveIndex)
            {
                const bool waveEnabled = waveIndex < item.gerstnerWaves.waveCount;
                if (!waveEnabled)
                {
                    continue;
                }
                const WaterGerstnerWave& wave = item.gerstnerWaves.waves[waveIndex];
                WaterGerstnerWaveFrameTerms terms;
                if (!EvaluateWaterGerstnerWaveFrameTerms(
                        wave, effectTime.simulationSeconds, terms))
                {
                    submission.diagnostic =
                        "Water could not reduce a Gerstner wave temporal phase";
                    return submission;
                }
                WaterGerstnerGpuWaveFrameTerms gpuTerms;
                if (!PackWaterGerstnerWaveFrameTermsForGpu(terms, gpuTerms))
                {
                    submission.diagnostic =
                        "Water could not pack a Gerstner wave for the GPU ABI";
                    return submission;
                }
                constants.waveDirectionAmplitude[waveIndex] = Vec4(
                    wave.direction.x, wave.direction.y, wave.amplitude,
                    wave.wavelength);
                constants.waveSteepnessTemporalPhase[waveIndex] = Vec4(
                    wave.steepness, gpuTerms.waveNumber, gpuTerms.temporalPhase, 1.0f);
            }
            slot.constants.push_back(constants);
            if (submission.surfaceReceipts.size() < RVX_WATER_SURFACE_RECEIPT_CAPACITY)
            {
                submission.surfaceReceipts.push_back(WaterFeatureSurfaceReceipt{
                    .componentId = item.componentId,
                    .contentHash = item.contentHash,
                    .waveContentHash = item.gerstnerWaves.contentHash,
                    .waveCount = item.gerstnerWaves.waveCount,
                });
            }
            else
            {
                submission.receiptsOverflow = true;
            }
        }
        if (slot.constants.empty())
        {
            submission.diagnostic = "Water snapshot has no renderable Ready surface";
            return submission;
        }
        const uint64 byteCount = static_cast<uint64>(slot.constants.size()) *
            RVX_WATER_CONSTANT_BUFFER_SIZE;
        std::vector<std::byte> packedConstants(byteCount);
        for (size_t index = 0; index < slot.constants.size(); ++index)
        {
            std::memcpy(packedConstants.data() +
                            index * RVX_WATER_CONSTANT_BUFFER_SIZE,
                        &slot.constants[index], sizeof(WaterSurfaceGPUConstants));
        }
        if (!Upload(slot.constantsBuffer.Get(), packedConstants.data(), byteCount))
        {
            submission.diagnostic = "Water constants upload failed to publish";
            return submission;
        }
        submission.device = m_device;
        submission.constantsBuffer = slot.constantsBuffer;
        submission.gridVertexBuffer = m_gridVertexBuffer;
        submission.gridIndexBuffer = m_gridIndexBuffer;
        submission.descriptorSetLayout = m_descriptorSetLayout;
        submission.linearClampSampler = m_linearClampSampler;
        submission.pipeline = m_pipeline;
        submission.frameSlot = frameSlot;
        submission.surfaceCount = static_cast<uint32>(slot.constants.size());
        submission.gridVertexCount = m_gridVertexCount;
        submission.gridIndexCount = m_gridIndexCount;
        submission.uploadBytes = byteCount;
        submission.resourceCreationCount = m_resourceCreationCount;
        submission.pipelineCreationCount = m_pipelineCreationCount;
        return submission;
    }

private:
    struct FrameSlot
    {
        RHIBufferRef constantsBuffer;
        std::vector<WaterSurfaceGPUConstants> constants;
    };

    void ResetPipelineResources() noexcept
    {
        m_vertexShader.Reset();
        m_pixelShader.Reset();
        m_linearClampSampler.Reset();
        m_descriptorSetLayout.Reset();
        m_pipelineLayout.Reset();
        m_pipeline.Reset();
        m_unavailableReason.clear();
    }

    void ResetAllResources() noexcept
    {
        ResetPipelineResources();
        m_initializationAttempted = false;
        m_gridVertexBuffer.Reset();
        m_gridIndexBuffer.Reset();
        m_gridVertexCount = 0;
        m_gridIndexCount = 0;
        for (FrameSlot& slot : m_frameSlots)
        {
            slot.constantsBuffer.Reset();
            slot.constants.clear();
        }
    }

    [[nodiscard]] bool EnsureResources()
    {
        if (IsReady())
        {
            return true;
        }
        if (m_initializationAttempted)
        {
            return false;
        }
        m_initializationAttempted = true;
        if (m_device == nullptr)
        {
            m_unavailableReason = "Water renderer has no RHI device";
            return false;
        }
        if (m_shaderProgramProvider == nullptr)
        {
            m_unavailableReason =
                "Water renderer requires an injected cooked shader program provider";
            return false;
        }
        ResetPipelineResources();
        if (!EnsureSharedGrid())
        {
            return false;
        }

        RHIDescriptorSetLayoutDesc setLayoutDesc;
        setLayoutDesc.debugName = "WaterSurface.DescriptorSetLayout";
        setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer,
                                 RHIShaderStage::Vertex | RHIShaderStage::Pixel);
        setLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture,
                                 RHIShaderStage::Pixel);
        setLayoutDesc.AddBinding(2, RHIBindingType::SampledTexture,
                                 RHIShaderStage::Pixel);
        setLayoutDesc.AddBinding(3, RHIBindingType::Sampler,
                                 RHIShaderStage::Pixel);
        setLayoutDesc.AddBinding(4, RHIBindingType::SampledTexture,
                                 RHIShaderStage::Pixel);
        setLayoutDesc.AddBinding(RVX_WATER_EXPOSURE_STATE_BINDING,
                                 RHIBindingType::ShaderResourceBuffer,
                                 RHIShaderStage::Pixel);
        m_descriptorSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
        if (!m_descriptorSetLayout)
        {
            m_unavailableReason = "Water descriptor-set layout creation failed";
            return false;
        }
        ++m_resourceCreationCount;
        RHIPipelineLayoutDesc pipelineLayoutDesc;
        pipelineLayoutDesc.debugName = "WaterSurface.PipelineLayout";
        pipelineLayoutDesc.setLayouts = {m_descriptorSetLayout.Get()};
        m_pipelineLayout = m_device->CreatePipelineLayout(pipelineLayoutDesc);
        if (!m_pipelineLayout)
        {
            m_unavailableReason = "Water pipeline layout creation failed";
            return false;
        }
        ++m_resourceCreationCount;

        const auto loadProgram = [this](RHIShaderStage stage, const char* entryPoint)
        {
            ShaderProgramRequest request;
            request.logicalAssetId = "Water/WaterSurface.hlsl";
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
        if (!vertex.Succeeded() || !pixel.Succeeded())
        {
            m_unavailableReason = "Water cooked shader program load failed: " +
                (!vertex.Succeeded() ? vertex.diagnostic : pixel.diagnostic);
            return false;
        }
        m_vertexShader = vertex.shader;
        m_pixelShader = pixel.shader;
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "WaterSurface.LinearClampSampler";
        m_linearClampSampler = m_device->CreateSampler(samplerDesc);
        if (!m_linearClampSampler)
        {
            m_unavailableReason = "Water sampler creation failed";
            return false;
        }
        ++m_resourceCreationCount;
        RHIGraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = m_vertexShader.Get();
        pipelineDesc.pixelShader = m_pixelShader.Get();
        pipelineDesc.inputLayout.AddElement("POSITION", RHIFormat::RG32_FLOAT);
        pipelineDesc.rasterizerState = RHIRasterizerState::NoCull();
        pipelineDesc.depthStencilState = RHIDepthStencilState::ReadOnly();
        pipelineDesc.depthStencilState.depthCompareOp = m_reverseZ
            ? RHICompareOp::GreaterEqual : RHICompareOp::LessEqual;
        pipelineDesc.blendState.independentBlendEnable = true;
        // Refraction has already composited the immutable opaque snapshot in
        // the pixel program. Blending that result again against the active
        // HDR target darkens low-opacity water, so the composed color is the
        // sole RT0 write. RT1 remains additive for the reactive mask.
        pipelineDesc.blendState.renderTargets[0] = RHIRenderTargetBlendState::Opaque();
        pipelineDesc.blendState.renderTargets[1] = RHIRenderTargetBlendState::Additive();
        pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
        pipelineDesc.numRenderTargets = 2;
        pipelineDesc.renderTargetFormats[0] = RHIFormat::RGBA16_FLOAT;
        pipelineDesc.renderTargetFormats[1] = RHIFormat::R8_UNORM;
        pipelineDesc.depthStencilFormat = RHIFormat::D32_FLOAT;
        pipelineDesc.sampleCount = RHISampleCount::Count1;
        pipelineDesc.pipelineLayout = m_pipelineLayout.Get();
        pipelineDesc.debugName = "WaterSurface.GerstnerPipeline";
        m_pipeline = m_device->CreateGraphicsPipeline(pipelineDesc);
        if (!m_pipeline)
        {
            m_unavailableReason = "Water graphics pipeline creation failed";
            return false;
        }
        ++m_pipelineCreationCount;
        return true;
    }

    [[nodiscard]] bool EnsureSharedGrid()
    {
        if (m_gridVertexBuffer && m_gridIndexBuffer)
        {
            return true;
        }
        const uint32 vertexStride = RVX_WATER_GRID_RESOLUTION + 1u;
        std::vector<WaterGridVertex> vertices;
        std::vector<uint32> indices;
        vertices.reserve(static_cast<size_t>(vertexStride) * vertexStride);
        indices.reserve(static_cast<size_t>(RVX_WATER_GRID_RESOLUTION) *
                        RVX_WATER_GRID_RESOLUTION * 6u);
        for (uint32 z = 0; z <= RVX_WATER_GRID_RESOLUTION; ++z)
        {
            for (uint32 x = 0; x <= RVX_WATER_GRID_RESOLUTION; ++x)
            {
                vertices.push_back({Vec2(
                    static_cast<float32>(x) / RVX_WATER_GRID_RESOLUTION - 0.5f,
                    static_cast<float32>(z) / RVX_WATER_GRID_RESOLUTION - 0.5f)});
            }
        }
        for (uint32 z = 0; z < RVX_WATER_GRID_RESOLUTION; ++z)
        {
            for (uint32 x = 0; x < RVX_WATER_GRID_RESOLUTION; ++x)
            {
                const uint32 topLeft = z * vertexStride + x;
                indices.insert(indices.end(), {
                    topLeft, topLeft + vertexStride, topLeft + 1u,
                    topLeft + 1u, topLeft + vertexStride, topLeft + vertexStride + 1u});
            }
        }
        RHIBufferDesc vertexDesc;
        vertexDesc.size = static_cast<uint64>(vertices.size()) * sizeof(WaterGridVertex);
        vertexDesc.usage = RHIBufferUsage::Vertex;
        vertexDesc.memoryType = RHIMemoryType::Upload;
        vertexDesc.stride = sizeof(WaterGridVertex);
        vertexDesc.debugName = "WaterSurface.SharedGridVertices";
        RHIBufferRef vertexBuffer = m_device->CreateBuffer(vertexDesc);
        RHIBufferDesc indexDesc;
        indexDesc.size = static_cast<uint64>(indices.size()) * sizeof(uint32);
        indexDesc.usage = RHIBufferUsage::Index;
        indexDesc.memoryType = RHIMemoryType::Upload;
        indexDesc.stride = sizeof(uint32);
        indexDesc.debugName = "WaterSurface.SharedGridIndices";
        RHIBufferRef indexBuffer = m_device->CreateBuffer(indexDesc);
        if (!vertexBuffer || !indexBuffer ||
            !Upload(vertexBuffer.Get(), vertices.data(), vertexDesc.size) ||
            !Upload(indexBuffer.Get(), indices.data(), indexDesc.size))
        {
            m_unavailableReason = "Water shared grid creation/upload failed";
            return false;
        }
        m_gridVertexBuffer = std::move(vertexBuffer);
        m_gridIndexBuffer = std::move(indexBuffer);
        m_gridVertexCount = static_cast<uint32>(vertices.size());
        m_gridIndexCount = static_cast<uint32>(indices.size());
        m_resourceCreationCount += 2u;
        return true;
    }

    [[nodiscard]] bool EnsureFrameSlot(FrameSlot& slot)
    {
        if (slot.constantsBuffer)
        {
            return true;
        }
        RHIBufferDesc constantsDesc;
        constantsDesc.size = static_cast<uint64>(RVX_WATER_MAX_SURFACES_PER_FRAME) *
            RVX_WATER_CONSTANT_BUFFER_SIZE;
        constantsDesc.usage = RHIBufferUsage::Constant;
        constantsDesc.memoryType = RHIMemoryType::Upload;
        constantsDesc.debugName = "WaterSurface.FrameConstants";
        slot.constantsBuffer = m_device->CreateBuffer(constantsDesc);
        if (!slot.constantsBuffer)
        {
            m_unavailableReason = "Water frame constants buffer creation failed";
            return false;
        }
        ++m_resourceCreationCount;
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
    std::array<FrameSlot, RVX_WATER_FRAME_SLOT_COUNT> m_frameSlots{};
    RHIBufferRef m_gridVertexBuffer;
    RHIBufferRef m_gridIndexBuffer;
    uint32 m_gridVertexCount = 0;
    uint32 m_gridIndexCount = 0;
    RHIDescriptorSetLayoutRef m_descriptorSetLayout;
    RHIPipelineLayoutRef m_pipelineLayout;
    RHIShaderRef m_vertexShader;
    RHIShaderRef m_pixelShader;
    RHISamplerRef m_linearClampSampler;
    RHIPipelineRef m_pipeline;
    uint64 m_resourceCreationCount = 0;
    uint64 m_pipelineCreationCount = 0;
    bool m_initializationAttempted = false;
    std::string m_unavailableReason;
};

namespace
{
    struct WaterGraphPassData
    {
        WaterSurfaceRenderer::Submission submission{};
        RenderPassExecutionData execution{};
        WaterFeaturePassStats initialStats{};
        WaterSurfaceRecordInput waterSurface{};
        VolumetricLightingRecordInput volumetricLighting{};
        PipelineCache* pipelineCache = nullptr;
        RGBufferHandle constantsHandle{};
        RGBufferHandle gridVertexHandle{};
        RGBufferHandle gridIndexHandle{};
        RGTextureHandle colorHandle{};
        RGTextureHandle depthHandle{};
        RGTextureHandle reactiveMaskHandle{};
        RGTextureHandle opaqueSnapshotHandle{};
        RGTextureHandle depthSnapshotHandle{};
        RGTextureHandle froxelVolumetricLightingHandle{};
        RGBufferHandle exposureStateReadHandle{};
        RGTextureViewHandle colorView{};
        RGTextureViewHandle depthView{};
        RGTextureViewHandle reactiveMaskView{};
        RGTextureViewHandle opaqueSnapshotView{};
        RGTextureViewHandle depthSnapshotView{};
        RGTextureViewHandle froxelVolumetricLightingView{};
        RHITextureViewRef neutralFroxelVolumetricLightingView{};
        uint64 exposureStateContentHash = 0;
        bool contextValid = false;
        WaterFeaturePass* owner = nullptr;
    };

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

    [[nodiscard]] bool PrepareGraphResources(WaterGraphPassData& data,
                                             RenderGraphBuilder& builder)
    {
        if (!data.contextValid || !data.submission.IsReady() ||
            !data.waterSurface.IsCompatibleWith(data.execution.identity) ||
            !IsSameBufferHandle(data.waterSurface.exposureState,
                                data.execution.view.exposureState) ||
            data.waterSurface.exposureStateContentHash !=
                data.execution.view.exposureStateContentHash ||
            !MakeExposureStateMetadata(data.execution.view).IsValid() ||
            !IsCompatibleFroxelVolumetricLighting(
                data.volumetricLighting, data.execution.identity) ||
            data.pipelineCache == nullptr ||
            !data.execution.view.colorTarget.IsValid() ||
            !data.execution.view.depthTarget.IsValid() ||
            !data.execution.view.reactiveMaskTarget.IsValid() ||
            !data.execution.view.exposureState.IsValid() ||
            data.execution.view.exposureStateContentHash == 0)
        {
            return false;
        }
        const ViewData& view = data.execution.view;
        const RHITextureDesc* const colorDesc = builder.GetTextureDesc(view.colorTarget);
        const RHITextureDesc* const depthDesc = builder.GetTextureDesc(view.depthTarget);
        const RHITextureDesc* const reactiveDesc = builder.GetTextureDesc(
            view.reactiveMaskTarget);
        const RHITextureDesc* const opaqueSnapshotDesc = builder.GetTextureDesc(
            data.waterSurface.opaqueSceneSnapshot.texture);
        const RHITextureDesc* const depthSnapshotDesc = builder.GetTextureDesc(
            data.waterSurface.depthSnapshot);
        const RHITextureDesc* const froxelVolumetricLightingDesc =
            data.volumetricLighting.identityRoute ? nullptr :
            builder.GetTextureDesc(
                data.volumetricLighting.integratedScatteringTransmittance);
        const RHIBufferDesc* const exposureStateDesc = builder.GetBufferDesc(
            data.waterSurface.exposureState);
        const auto exactExtent = [&view](const RHITextureDesc* desc)
        {
            return desc != nullptr && desc->width == view.viewportWidth &&
                desc->height == view.viewportHeight &&
                desc->sampleCount == RHISampleCount::Count1;
        };
        if (view.viewportX != 0 || view.viewportY != 0 ||
            !exactExtent(colorDesc) || !exactExtent(depthDesc) ||
            !exactExtent(reactiveDesc) || !exactExtent(opaqueSnapshotDesc) ||
            !exactExtent(depthSnapshotDesc) || colorDesc->format != RHIFormat::RGBA16_FLOAT ||
            depthDesc->format != RHIFormat::D32_FLOAT ||
            reactiveDesc->format != RHIFormat::R8_UNORM ||
            opaqueSnapshotDesc->format != RHIFormat::RGBA16_FLOAT ||
            depthSnapshotDesc->format != RHIFormat::D32_FLOAT ||
            exposureStateDesc == nullptr ||
            exposureStateDesc->size != SceneTextures::ExposureStateStride ||
            exposureStateDesc->stride != SceneTextures::ExposureStateStride ||
            !HasFlag(exposureStateDesc->usage, RHIBufferUsage::Structured) ||
            !HasFlag(exposureStateDesc->usage, RHIBufferUsage::ShaderResource) ||
            (!data.volumetricLighting.identityRoute &&
             !IsExactFroxelVolumetricTexture(
                 froxelVolumetricLightingDesc, data.volumetricLighting)))
        {
            return false;
        }
        if (!Retain(builder, data.submission.constantsBuffer.Get()) ||
            !Retain(builder, data.submission.gridVertexBuffer.Get()) ||
            !Retain(builder, data.submission.gridIndexBuffer.Get()) ||
            !Retain(builder, data.submission.descriptorSetLayout.Get()) ||
            !Retain(builder, data.submission.linearClampSampler.Get()) ||
            !Retain(builder, data.submission.pipeline.Get()) ||
            (data.volumetricLighting.identityRoute &&
             !Retain(builder,
                     data.pipelineCache->GetNeutralVolumetricLightingView().Get())))
        {
            return false;
        }
        data.constantsHandle = builder.ImportBuffer(
            data.submission.constantsBuffer,
            MakeRHIBufferAccessSnapshot(RHIResourceState::ConstantBuffer,
                                        RHIShaderStage::Vertex | RHIShaderStage::Pixel));
        data.gridVertexHandle = builder.ImportBuffer(
            data.submission.gridVertexBuffer,
            MakeRHIBufferAccessSnapshot(RHIResourceState::VertexBuffer,
                                        RHIShaderStage::Vertex));
        data.gridIndexHandle = builder.ImportBuffer(
            data.submission.gridIndexBuffer,
            MakeRHIBufferAccessSnapshot(RHIResourceState::IndexBuffer,
                                        RHIShaderStage::Vertex));
        data.constantsHandle = builder.Read(data.constantsHandle,
            MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                  RHIShaderStage::Vertex | RHIShaderStage::Pixel));
        data.gridVertexHandle = builder.Read(data.gridVertexHandle,
            MakeRHIAccessSnapshot(RHIResourceState::VertexBuffer, RHIShaderStage::Vertex));
        data.gridIndexHandle = builder.Read(data.gridIndexHandle,
            MakeRHIAccessSnapshot(RHIResourceState::IndexBuffer, RHIShaderStage::Vertex));
        if (!data.constantsHandle.IsValid() || !data.gridVertexHandle.IsValid() ||
            !data.gridIndexHandle.IsValid())
        {
            return false;
        }
        data.colorHandle = view.colorTarget;
        data.reactiveMaskHandle = view.reactiveMaskTarget;
        data.depthHandle = view.depthTarget;
        data.opaqueSnapshotHandle = builder.Read(
            data.waterSurface.opaqueSceneSnapshot.texture,
            MakeRGAccessDesc(RHIResourceState::ShaderResource, RHIShaderStage::Pixel));
        data.depthSnapshotHandle = builder.Read(data.waterSurface.depthSnapshot,
            MakeRGAccessDesc(RHIResourceState::ShaderResource, RHIShaderStage::Pixel));
        data.exposureStateReadHandle = builder.Read(
            data.waterSurface.exposureState,
            MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                  RHIShaderStage::Pixel));
        data.exposureStateContentHash = data.waterSurface.exposureStateContentHash;
        if (!data.volumetricLighting.identityRoute)
        {
            data.froxelVolumetricLightingHandle = builder.Read(
                data.volumetricLighting.integratedScatteringTransmittance,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
        }
        RHITextureViewDesc colorViewDesc;
        colorViewDesc.type = RHITextureViewType::RenderTarget;
        colorViewDesc.format = RHIFormat::RGBA16_FLOAT;
        colorViewDesc.dimension = RHITextureDimension::Texture2D;
        colorViewDesc.debugName = "WaterSurface.ColorRTV";
        data.colorView = builder.CreateTextureView(data.colorHandle, colorViewDesc);
        data.colorView = builder.ReadWrite(data.colorView,
            MakeRGAccessDesc(RHIResourceState::RenderTarget, RHIShaderStage::Pixel));
        RHITextureViewDesc reactiveViewDesc;
        reactiveViewDesc.type = RHITextureViewType::RenderTarget;
        reactiveViewDesc.format = RHIFormat::R8_UNORM;
        reactiveViewDesc.dimension = RHITextureDimension::Texture2D;
        reactiveViewDesc.debugName = "WaterSurface.ReactiveMaskRTV";
        data.reactiveMaskView = builder.CreateTextureView(data.reactiveMaskHandle,
                                                           reactiveViewDesc);
        data.reactiveMaskView = builder.ReadWrite(data.reactiveMaskView,
            MakeRGAccessDesc(RHIResourceState::RenderTarget, RHIShaderStage::Pixel));
        RHITextureViewDesc depthViewDesc;
        depthViewDesc.type = RHITextureViewType::DepthStencil;
        depthViewDesc.format = RHIFormat::D32_FLOAT;
        depthViewDesc.dimension = RHITextureDimension::Texture2D;
        depthViewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        depthViewDesc.debugName = "WaterSurface.DepthDSV";
        data.depthView = builder.CreateTextureView(data.depthHandle, depthViewDesc);
        data.depthView = builder.Read(data.depthView,
            MakeRGAccessDesc(RHIResourceState::DepthRead,
                             RHIShaderStage::Vertex | RHIShaderStage::Pixel));
        RHITextureViewDesc opaqueViewDesc;
        opaqueViewDesc.type = RHITextureViewType::ShaderResource;
        opaqueViewDesc.format = RHIFormat::RGBA16_FLOAT;
        opaqueViewDesc.dimension = RHITextureDimension::Texture2D;
        opaqueViewDesc.debugName = "WaterSurface.OpaqueSceneSnapshotSRV";
        data.opaqueSnapshotView = builder.CreateTextureView(data.opaqueSnapshotHandle,
                                                             opaqueViewDesc);
        data.opaqueSnapshotView = builder.Read(data.opaqueSnapshotView,
            MakeRGAccessDesc(RHIResourceState::ShaderResource, RHIShaderStage::Pixel));
        RHITextureViewDesc depthSnapshotViewDesc;
        depthSnapshotViewDesc.type = RHITextureViewType::ShaderResource;
        depthSnapshotViewDesc.format = RHIFormat::D32_FLOAT;
        depthSnapshotViewDesc.dimension = RHITextureDimension::Texture2D;
        depthSnapshotViewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        depthSnapshotViewDesc.debugName = "WaterSurface.DepthSnapshotSRV";
        data.depthSnapshotView = builder.CreateTextureView(data.depthSnapshotHandle,
                                                            depthSnapshotViewDesc);
        data.depthSnapshotView = builder.Read(data.depthSnapshotView,
            MakeRGAccessDesc(RHIResourceState::ShaderResource, RHIShaderStage::Pixel));
        if (data.volumetricLighting.identityRoute)
        {
            data.neutralFroxelVolumetricLightingView =
                data.pipelineCache->GetNeutralVolumetricLightingView();
        }
        else
        {
            RHITextureViewDesc froxelViewDesc;
            froxelViewDesc.type = RHITextureViewType::ShaderResource;
            froxelViewDesc.format = RHIFormat::RGBA16_FLOAT;
            froxelViewDesc.dimension = RHITextureDimension::Texture3D;
            froxelViewDesc.debugName = "WaterSurface.FroxelVolumetricLightingSRV";
            data.froxelVolumetricLightingView = builder.CreateTextureView(
                data.froxelVolumetricLightingHandle, froxelViewDesc);
            data.froxelVolumetricLightingView = builder.Read(
                data.froxelVolumetricLightingView,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
        }
        return data.colorView.IsValid() && data.reactiveMaskView.IsValid() &&
            data.depthView.IsValid() && data.opaqueSnapshotHandle.IsValid() &&
            data.opaqueSnapshotView.IsValid() && data.depthSnapshotHandle.IsValid() &&
            data.depthSnapshotView.IsValid() && data.exposureStateReadHandle.IsValid() &&
            data.exposureStateContentHash != 0 &&
            (data.volumetricLighting.identityRoute
                ? static_cast<bool>(data.neutralFroxelVolumetricLightingView)
                : data.froxelVolumetricLightingHandle.IsValid() &&
                    data.froxelVolumetricLightingView.IsValid());
    }

    [[nodiscard]] RHIDescriptorSetRef CreateDescriptorSet(
        const WaterGraphPassData& data, RenderGraphPassContext& context,
        uint32 surfaceIndex)
    {
        if (data.submission.device == nullptr ||
            surfaceIndex >= data.submission.surfaceCount ||
            data.submission.constantsBuffer == nullptr ||
            data.submission.descriptorSetLayout == nullptr ||
            data.submission.linearClampSampler == nullptr)
        {
            return {};
        }
        RHITextureView* const opaqueSnapshot = context.GetTextureView(
            data.opaqueSnapshotView);
        RHITextureView* const depthSnapshot = context.GetTextureView(
            data.depthSnapshotView);
        RHITextureView* froxelVolumetricLighting =
            data.volumetricLighting.identityRoute
                ? data.neutralFroxelVolumetricLightingView.Get()
                : context.GetTextureView(data.froxelVolumetricLightingView);
        if (opaqueSnapshot == nullptr || depthSnapshot == nullptr ||
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
            !HasFlag(exposureState->GetUsage(), RHIBufferUsage::ShaderResource))
        {
            return {};
        }
        if (!data.volumetricLighting.identityRoute)
        {
            RHITexture* const froxelTexture = context.GetTexture(
                data.froxelVolumetricLightingHandle);
            const FroxelGridMetadata& grid = data.volumetricLighting.froxelGrid;
            if (froxelTexture == nullptr ||
                froxelVolumetricLighting->GetTexture() != froxelTexture ||
                froxelVolumetricLighting->GetFormat() != RHIFormat::RGBA16_FLOAT ||
                froxelTexture->GetDimension() != RHITextureDimension::Texture3D ||
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
        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.debugName = "WaterSurface.DescriptorSet";
        descriptorDesc.SetLayout(data.submission.descriptorSetLayout.Get())
            .BindBuffer(0, data.submission.constantsBuffer.Get(),
                        static_cast<uint64>(surfaceIndex) *
                            RVX_WATER_CONSTANT_BUFFER_SIZE,
                        RVX_WATER_CONSTANT_BUFFER_SIZE)
            .BindTexture(1, opaqueSnapshot)
            .BindTexture(2, depthSnapshot)
            .BindSampler(3, data.submission.linearClampSampler.Get())
            .BindTexture(4, froxelVolumetricLighting)
            .BindBuffer(RVX_WATER_EXPOSURE_STATE_BINDING, exposureState);
        RHIDescriptorSetRef descriptorSet = data.submission.device->CreateDescriptorSet(
            descriptorDesc);
        if (!descriptorSet || !descriptorSet->IsReadyForBinding(
                data.submission.descriptorSetLayout.Get()) ||
            !context.RetainSubmissionResource(descriptorSet) ||
            !context.RetainSubmissionResource(Ref<RefCounted>(exposureState)))
        {
            return {};
        }
        return descriptorSet;
    }

    void ExecuteGraphPass(const WaterGraphPassData& data,
                          RenderGraphPassContext& context)
    {
        WaterFeaturePassStats stats = data.initialStats;
        stats.graphPassScheduled = true;
        stats.uploadPublished = data.submission.IsReady();
        stats.itemCount = data.submission.surfaceCount;
        stats.uploadBytes = data.submission.uploadBytes;
        stats.frameSlot = data.submission.frameSlot;
        stats.resourceCreationCount = data.submission.resourceCreationCount;
        stats.pipelineCreationCount = data.submission.pipelineCreationCount;
        stats.gridVertexCount = data.submission.gridVertexCount;
        stats.gridIndexCount = data.submission.gridIndexCount;
        const auto fail = [&stats, &data, &context](const char* reason)
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
            !data.colorView.IsValid() || !data.reactiveMaskView.IsValid() ||
            !data.depthView.IsValid() || !data.opaqueSnapshotView.IsValid() ||
            !data.depthSnapshotView.IsValid() ||
            !data.exposureStateReadHandle.IsValid() ||
            data.exposureStateContentHash == 0 ||
            !IsCompatibleFroxelVolumetricLighting(
                data.volumetricLighting, data.execution.identity) ||
            (data.volumetricLighting.identityRoute
                ? !data.neutralFroxelVolumetricLightingView
                : !data.froxelVolumetricLightingHandle.IsValid() ||
                    !data.froxelVolumetricLightingView.IsValid()))
        {
            fail("Water graph recording prerequisites are unavailable");
            return;
        }
        RHITexture* const colorTexture = context.GetTexture(data.colorHandle);
        RHITextureView* const colorView = context.GetTextureView(data.colorView);
        RHITextureView* const reactiveMaskView = context.GetTextureView(
            data.reactiveMaskView);
        RHITextureView* const depthView = context.GetTextureView(data.depthView);
        if (colorTexture == nullptr || colorView == nullptr ||
            reactiveMaskView == nullptr || depthView == nullptr ||
            context.GetBuffer(data.constantsHandle) != data.submission.constantsBuffer.Get() ||
            context.GetBuffer(data.gridVertexHandle) != data.submission.gridVertexBuffer.Get() ||
            context.GetBuffer(data.gridIndexHandle) != data.submission.gridIndexBuffer.Get())
        {
            fail("Water graph resources could not be realized with their recorded identity");
            return;
        }
        RHIRenderPassDesc renderPassDesc;
        renderPassDesc.AddColorAttachment(colorView, RHILoadOp::Load, RHIStoreOp::Store);
        renderPassDesc.AddColorAttachment(reactiveMaskView, RHILoadOp::Load,
                                          RHIStoreOp::Store);
        renderPassDesc.SetDepthStencil(depthView, RHILoadOp::Load, RHIStoreOp::Store,
                                       1.0f, 0);
        renderPassDesc.depthStencilAttachment.readOnly = true;
        renderPassDesc.SetRenderArea(0, 0, colorTexture->GetWidth(), colorTexture->GetHeight());
        RHICommandContext& commands = context.Commands();
        commands.BeginRenderPass(renderPassDesc);
        commands.SetViewport(data.execution.view.GetRHIViewport());
        commands.SetScissor(data.execution.view.GetRHIScissor());
        commands.SetPipeline(data.submission.pipeline.Get());
        commands.SetVertexBuffer(0, data.submission.gridVertexBuffer.Get());
        commands.SetIndexBuffer(data.submission.gridIndexBuffer.Get(), RHIFormat::R32_UINT);
        for (uint32 surfaceIndex = 0; surfaceIndex < data.submission.surfaceCount;
             ++surfaceIndex)
        {
            RHIDescriptorSetRef descriptorSet = CreateDescriptorSet(data, context, surfaceIndex);
            if (!descriptorSet)
            {
                commands.EndRenderPass();
                fail("Water descriptor bindings are unavailable");
                return;
            }
            commands.SetDescriptorSet(0, descriptorSet.Get());
            commands.DrawIndexed(data.submission.gridIndexCount, 1, 0, 0, 0);
            ++stats.drawCallCount;
        }
        commands.EndRenderPass();
        if (stats.drawCallCount == 0)
        {
            fail("Water submitted no draw despite a non-empty canonical snapshot");
            return;
        }
        stats.drawSubmitted = true;
        stats.supported = true;
        stats.enabled = true;
        stats.opaqueSceneSnapshotReady = data.waterSurface.opaqueSceneSnapshot.receipt != nullptr &&
            data.waterSurface.opaqueSceneSnapshot.receipt->copied.load(
                std::memory_order_acquire);
        stats.depthSnapshotReady = data.waterSurface.depthReceipt != nullptr &&
            data.waterSurface.depthReceipt->copied.load(std::memory_order_acquire);
        stats.exposureStateReady = true;
        stats.exposureStateReadDeclared = true;
        stats.exposureStateDescriptorBound = true;
        stats.exposureStateContentHash = data.exposureStateContentHash;
        if (!stats.opaqueSceneSnapshotReady || !stats.depthSnapshotReady ||
            !stats.exposureStateReady || !stats.exposureStateReadDeclared ||
            !stats.exposureStateDescriptorBound ||
            stats.exposureStateContentHash == 0)
        {
            fail("Water snapshot or committed exposure-state binding was not completed before draw execution");
            return;
        }
        stats.depthLinearizationReady = true;
        stats.layeredNormalsEvaluated = true;
        stats.fresnelEvaluated = true;
        stats.thicknessEvaluated = true;
        stats.beerLambertEvaluated = true;
        stats.foamEvaluated = true;
        stats.reactiveMaskWriteEnabled = true;
        stats.reactiveMaskWriteDrawCount = stats.drawCallCount;
        if (data.owner != nullptr)
        {
            data.owner->PublishExecutionStats(stats);
        }
    }
} // namespace

WaterFeaturePass::WaterFeaturePass()
    : m_renderer(std::make_unique<WaterSurfaceRenderer>())
{
}

WaterFeaturePass::~WaterFeaturePass() = default;

void WaterFeaturePass::SetSnapshot(const WaterRenderSnapshot* snapshot)
{
    m_snapshot = snapshot;
    RebuildStats();
}

void WaterFeaturePass::SetShaderProgramProvider(
    IShaderProgramProvider* shaderProgramProvider)
{
    m_renderer->SetShaderProgramProvider(shaderProgramProvider);
    RebuildStats();
}

void WaterFeaturePass::SetPipelineCache(PipelineCache* pipelineCache) noexcept
{
    m_pipelineCache = pipelineCache;
    RebuildStats();
}

void WaterFeaturePass::SetReverseZ(bool reverseZ)
{
    m_renderer->SetReverseZ(reverseZ);
    RebuildStats();
}

WaterSurfaceRequirements WaterFeaturePass::GetRequirements() const noexcept
{
    WaterSurfaceRequirements requirements;
    requirements.requested = HasReadyWaterItem(m_snapshot);
    requirements.opaqueSceneSnapshotRequired = requirements.requested;
    requirements.depthSnapshotRequired = requirements.requested;
    requirements.reactiveMaskRequired = requirements.requested;
    requirements.exposureStateRequired = requirements.requested;
    return requirements;
}

void WaterFeaturePass::PublishExecutionStats(const WaterFeaturePassStats& stats)
{
    m_stats = stats;
}

void WaterFeaturePass::OnAdd(IRHIDevice* device)
{
    m_renderer->OnAdd(device);
    RebuildStats();
}

void WaterFeaturePass::OnRemove()
{
    m_renderer->OnRemove();
    RebuildStats();
}

void WaterFeaturePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    (void)builder;
    (void)view;
    RebuildStats();
}

void WaterFeaturePass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    (void)ctx;
    (void)view;
    RebuildStats();
    if (m_stats.requested)
    {
        ++m_stats.fallbackCount;
        m_stats.drawSubmitted = false;
        m_stats.unsupportedReason =
            "Water execution requires RenderGraph-owned HDR and D32 snapshots";
    }
}

void WaterFeaturePass::Execute(RenderGraphPassContext& context, const ViewData& view)
{
    (void)view;
    context.MarkExecutionFailed(
        "WaterFeaturePass must record through its graph-aware AddToGraph override");
}

void WaterFeaturePass::AddToGraph(RenderGraph& graph,
                                  const RenderPassRecordContext& context)
{
    RebuildStats();
    if (!m_stats.requested)
    {
        return;
    }
    const RenderPassExecutionData execution = MakeRenderPassExecutionData(context);
    const bool contextValid = context.MatchesTargetGraph(graph) &&
        execution.MatchesTargetGraph(graph) && execution.IsFrameIdentityValid() &&
        context.waterSurface.IsCompatibleWith(context.identity) &&
        IsCompatibleFroxelVolumetricLighting(
            context.volumetricLighting, context.identity) &&
        m_pipelineCache != nullptr;
    if (!contextValid || m_snapshot == nullptr)
    {
        ++m_stats.fallbackCount;
        m_stats.supported = false;
        m_stats.enabled = false;
        m_stats.unsupportedReason =
            "Water requires current graph-owned HDR and D32 snapshot inputs";
        return;
    }
    WaterSurfaceRenderer::Submission submission = m_renderer->Prepare(
        *m_snapshot, context.effectTime, execution.view);
    if (!submission.IsReady())
    {
        ++m_stats.fallbackCount;
        m_stats.supported = false;
        m_stats.enabled = false;
        m_stats.unsupportedReason = submission.diagnostic.empty()
            ? "Water renderer resources are unavailable"
            : submission.diagnostic;
        return;
    }
    m_stats.supported = true;
    m_stats.enabled = true;
    m_stats.graphPassScheduled = true;
    m_stats.uploadPublished = true;
    m_stats.itemCount = submission.surfaceCount;
    m_stats.waveCount = 0;
    for (const WaterFeatureSurfaceReceipt& receipt : submission.surfaceReceipts)
    {
        m_stats.waveCount += receipt.waveCount;
    }
    m_stats.uploadBytes = submission.uploadBytes;
    m_stats.frameSlot = submission.frameSlot;
    m_stats.resourceCreationCount = submission.resourceCreationCount;
    m_stats.pipelineCreationCount = submission.pipelineCreationCount;
    m_stats.gridVertexCount = submission.gridVertexCount;
    m_stats.gridIndexCount = submission.gridIndexCount;
    m_stats.recordFrameIdentity = context.featureFrameIdentity;
    m_stats.recordGraphIdentity = context.identity.graphIdentity;
    m_stats.recordGraphGeneration = context.identity.graphRecordingGeneration;
    m_stats.recordEpoch = context.identity.recordEpoch;
    m_stats.surfaceReceipts = submission.surfaceReceipts;
    m_stats.surfaceReceiptsOverflow = submission.receiptsOverflow;
    const WaterFeaturePassStats recordStats = m_stats;
    graph.AddPass<WaterGraphPassData>(
        GetName(), GetPassType(),
        [submission = std::move(submission), execution, contextValid,
         recordStats, waterSurface = context.waterSurface,
         volumetricLighting = context.volumetricLighting,
         pipelineCache = m_pipelineCache, this](
            RenderGraphBuilder& builder, WaterGraphPassData& data)
        {
            data.submission = submission;
            data.execution = execution;
            data.initialStats = recordStats;
            data.waterSurface = waterSurface;
            data.volumetricLighting = volumetricLighting;
            data.pipelineCache = pipelineCache;
            data.contextValid = contextValid;
            data.owner = this;
            if (!PrepareGraphResources(data, builder))
            {
                data.contextValid = false;
            }
        },
        [](const WaterGraphPassData& data, RenderGraphPassContext& graphContext)
        {
            ExecuteGraphPass(data, graphContext);
        });
}

void WaterFeaturePass::RebuildStats()
{
    WaterFeaturePassStats stats;
    const WaterSurfaceRequirements requirements = GetRequirements();
    stats.requested = requirements.requested;
    if (!requirements.requested)
    {
        stats.supported = true;
        stats.enabled = false;
        m_stats = std::move(stats);
        return;
    }
    if (m_snapshot == nullptr ||
        m_snapshot->metadata.status != WaterRenderSnapshotStatus::Complete ||
        !m_snapshot->metadata.complete)
    {
        stats.unsupportedReason = "Water snapshot is incomplete";
        m_stats = std::move(stats);
        return;
    }
    for (const WaterRenderSnapshotItem& item : m_snapshot->items)
    {
        if (item.payloadStatus == WaterRenderSnapshotPayloadStatus::Disabled)
        {
            continue;
        }
        if (!IsReadyWaterItem(item))
        {
            stats.unsupportedReason =
                "Water snapshot contains a non-canonical Ready payload";
            m_stats = std::move(stats);
            return;
        }
        ++stats.itemCount;
        stats.waveCount += item.gerstnerWaves.waveCount;
    }
    stats.supported = m_renderer->IsReady();
    stats.enabled = stats.supported && stats.itemCount != 0;
    if (!stats.supported)
    {
        stats.unsupportedReason = m_renderer->GetUnavailableReason().empty()
            ? "Water renderer resources are not initialized"
            : m_renderer->GetUnavailableReason();
    }
    m_stats = std::move(stats);
}
} // namespace RVX
