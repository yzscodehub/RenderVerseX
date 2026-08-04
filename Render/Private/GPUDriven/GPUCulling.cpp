/**
 * @file GPUCulling.cpp
 * @brief GPU-driven culling implementation
 */

#include "Render/GPUDriven/GPUCulling.h"
#include "Core/Log.h"
#include "GPUScene/GPUSceneUploader.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Visibility/RenderVisibility.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "ShaderCompiler/ShaderManager.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace RVX
{
namespace
{
    static_assert(RVX_GPU_SCENE_CULLING_TABLE_COUNT ==
                  GPU_SCENE_RESIDENT_TABLE_COUNT);

    RHIBufferUsage MakeGpuWritableStructuredUsage(RHIBufferUsage baseUsage)
    {
        return baseUsage |
               RHIBufferUsage::Structured |
               RHIBufferUsage::ShaderResource |
               RHIBufferUsage::UnorderedAccess |
               RHIBufferUsage::CopyDst;
    }

    MaterialPipelineVariant GetGPUCullingPipelineVariant(MaterialRenderMode mode)
    {
        switch (mode)
        {
            case MaterialRenderMode::Masked:
                return MaterialPipelineVariant::Masked;
            case MaterialRenderMode::Transparent:
                return MaterialPipelineVariant::Transparent;
            case MaterialRenderMode::Opaque:
            default:
                return MaterialPipelineVariant::Opaque;
        }
    }

    std::filesystem::path FindGPUCullingShaderPath()
    {
        std::filesystem::path path = std::filesystem::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            std::filesystem::path candidate = path / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl";
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }

            if (!path.has_parent_path())
            {
                break;
            }
            path = path.parent_path();
        }

        return {};
    }

    std::filesystem::path FindGPUSceneCullingShaderPath()
    {
        std::filesystem::path path = std::filesystem::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            std::filesystem::path candidate = path / "Render" / "Shaders" /
                "GPUDriven" / "GPUSceneCulling.hlsl";
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }

            if (!path.has_parent_path())
            {
                break;
            }
            path = path.parent_path();
        }

        return {};
    }

    GPUCullingConstants MakeDefaultGPUCullingConstants()
    {
        GPUCullingConstants constants{};
        constants.viewProj = Mat4(1.0f);
        for (Vec4& plane : constants.frustumPlanes)
        {
            plane = Vec4(0.0f);
        }
        constants.cameraPosition = Vec4(0.0f);
        constants.params = Vec4(0.0f);
        std::fill_n(constants.counts, 4, 0u);
        std::fill_n(constants.gpuSceneTableCounts0, 4, 0u);
        std::fill_n(constants.gpuSceneTableCounts1, 4, 0u);
        return constants;
    }

    uint32 GetGPUSceneTableRowStride(uint32 tableIndex)
    {
        switch (static_cast<GPUSceneResidentTable>(tableIndex))
        {
            case GPUSceneResidentTable::Primitives:
                return sizeof(GPUScenePrimitiveRow);
            case GPUSceneResidentTable::Bounds:
                return sizeof(GPUSceneBoundsRow);
            case GPUSceneResidentTable::Transforms:
                return sizeof(GPUSceneTransformRow);
            case GPUSceneResidentTable::Materials:
                return sizeof(GPUSceneMaterialRow);
            case GPUSceneResidentTable::Geometries:
                return sizeof(GPUSceneGeometryRow);
            case GPUSceneResidentTable::Draws:
                return sizeof(GPUSceneDrawMetadataRow);
            case GPUSceneResidentTable::Count:
            default:
                return 0;
        }
    }
} // namespace

// ============================================================================
// GPUCulling
// ============================================================================

GPUCulling::~GPUCulling()
{
    Shutdown();
}

void GPUCulling::Initialize(IRHIDevice* device,
                            const GPUCullingConfig& config,
                            uint32 frameSlotCount)
{
    m_device = device;
    m_config = config;
    m_occlusionRequested = config.enableOcclusionCulling;
    // HZB production inputs and validation are not implemented yet. Preserve
    // the request for diagnostics, but never silently run a partial path.
    m_config.enableOcclusionCulling = false;
    m_config.twoPhaseOcclusion = false;
    frameSlotCount = std::clamp(frameSlotCount, 1u, RVX_MAX_FRAME_COUNT);
    m_frameInputs.resize(frameSlotCount);
    m_activeFrameSlot = 0;
    m_instances.reserve(config.maxInstances);
    CreateResources();
    CreatePipelineResources();
}

void GPUCulling::Shutdown()
{
    m_instanceIndexBuffer.Reset();
    m_visibilityBuffer.Reset();
    m_visibleInstanceBuffer.Reset();
    m_indirectBuffer.Reset();
    m_drawCountBuffer.Reset();
    m_frameInputs.clear();
    m_activeFrameSlot = 0;
    m_frustumCullShader.Reset();
    m_compactShader.Reset();
    m_cullingDescriptorSetLayout.Reset();
    m_cullingPipelineLayout.Reset();
    m_frustumCullPipeline.Reset();
    m_occlusionCullPipeline.Reset();
    m_compactPipeline.Reset();
    m_gpuSceneFrustumCullShader.Reset();
    m_gpuSceneCompactShader.Reset();
    m_gpuSceneDescriptorSetLayout.Reset();
    m_gpuScenePipelineLayout.Reset();
    m_gpuSceneFrustumCullPipeline.Reset();
    m_gpuSceneCompactPipeline.Reset();
    m_gpuSceneDescriptorSet.Reset();
    m_gpuSceneTableBuffers = {};
    m_gpuSceneTableCapacities = {};
    m_gpuSceneLeaseVersion = 0;
    m_gpuSceneCandidates.clear();
    m_gpuSceneCandidateVersion = 0;
    m_gpuSceneEnabled = false;
    m_statsBuffer.Reset();
    m_transientUploadBuffers.clear();
    m_pendingOwnerRetirements.clear();
    m_device = nullptr;
    m_lastFallbackReason = GPUCullingFallbackReason::None;
    m_pipelineFallbackReason = GPUCullingFallbackReason::None;
    m_accessSnapshots = {};
}

void GPUCulling::RetireOwnerSnapshots(
    const GPUCompletionToken& completion,
    RenderRetirementQueue& retirement)
{
    FlushRenderOwnerRetirements(
        m_pendingOwnerRetirements, completion, retirement);
}

bool GPUCulling::RetainSubmissionResources(
    RenderSubmissionResourceBatch& batch)
{
    for (const RHIBufferRef& buffer : m_transientUploadBuffers)
    {
        if (!batch.Retain(buffer, buffer ? buffer->GetSize() : 0))
        {
            return false;
        }
    }
    m_transientUploadBuffers.clear();
    return true;
}

bool GPUCulling::RetainSealedSubmissionResources(
    RenderSubmissionResourceBatch& batch)
{
    // A sealed state may outlive both the graph callbacks that recorded it and
    // the source culler that created it.  Retain every RHI object that its
    // recorded cull or indirect commands can reference; RenderGraph only owns
    // graph-created resources and must not be relied upon for these objects.
    if (!RetainSubmissionResources(batch))
    {
        return false;
    }

    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (inputs == nullptr)
    {
        return false;
    }

    const auto retain = [&batch]<typename T>(const Ref<T>& object,
                                              uint64 estimatedBytes = 0)
    {
        return !object || batch.Retain(object, estimatedBytes);
    };
    const auto retainBuffer = [&retain](const RHIBufferRef& buffer)
    {
        return retain(buffer, buffer ? buffer->GetSize() : 0);
    };

    const bool retainedCoreResources = retainBuffer(inputs->instanceBuffer) &&
        retainBuffer(inputs->gpuSceneCandidateBuffer) &&
        retainBuffer(inputs->constantsBuffer) &&
        retain(inputs->descriptorSet) &&
        retainBuffer(m_instanceIndexBuffer) &&
        retainBuffer(m_visibilityBuffer) &&
        retainBuffer(m_visibleInstanceBuffer) &&
        retainBuffer(m_indirectBuffer) &&
        retainBuffer(m_drawCountBuffer) &&
        retainBuffer(m_statsBuffer) &&
        retain(m_frustumCullShader) &&
        retain(m_compactShader) &&
        retain(m_cullingDescriptorSetLayout) &&
        retain(m_cullingPipelineLayout) &&
        retain(m_frustumCullPipeline) &&
        retain(m_occlusionCullPipeline) &&
        retain(m_compactPipeline) &&
        retain(m_gpuSceneFrustumCullShader) &&
        retain(m_gpuSceneCompactShader) &&
        retain(m_gpuSceneDescriptorSetLayout) &&
        retain(m_gpuScenePipelineLayout) &&
        retain(m_gpuSceneFrustumCullPipeline) &&
        retain(m_gpuSceneCompactPipeline) &&
        retain(m_gpuSceneDescriptorSet);
    if (!retainedCoreResources)
    {
        return false;
    }

    for (const RHIBufferRef& tableBuffer : m_gpuSceneTableBuffers)
    {
        if (!retainBuffer(tableBuffer))
        {
            return false;
        }
    }
    return true;
}

void GPUCulling::SetConfig(const GPUCullingConfig& config)
{
    bool needsResize = config.maxInstances != m_config.maxInstances;
    m_config = config;
    m_occlusionRequested = config.enableOcclusionCulling;
    m_config.enableOcclusionCulling = false;
    m_config.twoPhaseOcclusion = false;

    if (needsResize)
    {
        CreateResources();
        CreatePipelineResources();
    }
}

bool GPUCulling::SetFrameSlot(uint32 frameSlot)
{
    if (frameSlot >= m_frameInputs.size())
    {
        return false;
    }

    m_activeFrameSlot = frameSlot;
    RefreshActiveInputAccessSnapshots();
    return true;
}

RHIBuffer* GPUCulling::GetInstanceBuffer() const
{
    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    return inputs != nullptr ? inputs->instanceBuffer.Get() : nullptr;
}

RHIBuffer* GPUCulling::GetGPUSceneCandidateBuffer() const
{
    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    return inputs != nullptr ? inputs->gpuSceneCandidateBuffer.Get() : nullptr;
}

RHIBuffer* GPUCulling::GetCullingConstantsBuffer() const
{
    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    return inputs != nullptr ? inputs->constantsBuffer.Get() : nullptr;
}

const GPUCullingAccessSnapshots& GPUCulling::GetAccessSnapshots() const
{
    return m_accessSnapshots;
}

void GPUCulling::CommitAccessSnapshots(
    const GPUCullingAccessSnapshots& snapshots)
{
    m_accessSnapshots = snapshots;
    if (GPUCullingFrameInputs* inputs = GetActiveFrameInputs())
    {
        inputs->instanceAccess = snapshots.instances;
        inputs->gpuSceneCandidateAccess = snapshots.gpuSceneCandidates;
        inputs->constantsAccess = snapshots.constants;
    }
}

GPUCulling::GPUCullingFrameInputs* GPUCulling::GetActiveFrameInputs()
{
    return m_activeFrameSlot < m_frameInputs.size()
        ? &m_frameInputs[m_activeFrameSlot]
        : nullptr;
}

const GPUCulling::GPUCullingFrameInputs*
    GPUCulling::GetActiveFrameInputs() const
{
    return m_activeFrameSlot < m_frameInputs.size()
        ? &m_frameInputs[m_activeFrameSlot]
        : nullptr;
}

void GPUCulling::RefreshActiveInputAccessSnapshots()
{
    if (const GPUCullingFrameInputs* inputs = GetActiveFrameInputs())
    {
        m_accessSnapshots.instances = inputs->instanceAccess;
        m_accessSnapshots.gpuSceneCandidates = inputs->gpuSceneCandidateAccess;
        m_accessSnapshots.constants = inputs->constantsAccess;
    }
}

void GPUCulling::QueueFrameInputRetirements()
{
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        QueueRenderOwnerRetirement(
            inputs.instanceBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.gpuSceneCandidateBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.constantsBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.descriptorSet, m_pendingOwnerRetirements);
    }
    m_frameInputs.clear();
}

void GPUCulling::CreateResources()
{
    if (!m_device) return;

    const uint32 frameSlotCount = std::max(
        1u, static_cast<uint32>(m_frameInputs.size()));
    QueueFrameInputRetirements();
    m_frameInputs.resize(frameSlotCount);
    m_activeFrameSlot = std::min(m_activeFrameSlot, frameSlotCount - 1u);

    QueueRenderOwnerRetirement(m_instanceIndexBuffer, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_visibilityBuffer, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_visibleInstanceBuffer, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_indirectBuffer, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_drawCountBuffer, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_statsBuffer, m_pendingOwnerRetirements);
    m_accessSnapshots = {};

    const bool gpuWritableOutputs = SupportsGpuExecution();
    const RHIMemoryType cpuOutputMemoryType = RHIMemoryType::Upload;

    RHIBufferDesc desc;

    // Per-flight CPU-written inputs. RenderContext waits the selected frame
    // slot before SceneRenderer selects it, so writing slot N never races the
    // GPU consuming slot N from an earlier frame.
    desc.size = m_config.maxInstances * sizeof(GPUInstanceData);
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(GPUInstanceData);
    desc.debugName = "GPUCulling.InstanceBuffer";
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.instanceBuffer = m_device->CreateBuffer(desc);
        if (inputs.instanceBuffer)
        {
            inputs.instanceAccess = MakeRHIBufferAccessSnapshot(
                RHIResourceState::ShaderResource,
                RHIShaderStage::Compute,
                GPUQueueDomain::Graphics,
                RHIContentValidity::Unknown);
        }
    }

    // Indirect firstInstance offsets per-instance vertex fetches, but is not
    // folded into SV_InstanceID. Keep an identity stream so GPU vertex shaders
    // can recover the global structured-buffer index on every backend.
    desc.size = static_cast<uint64>(m_config.maxInstances) * sizeof(uint32);
    desc.usage = RHIBufferUsage::Vertex;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(uint32);
    desc.debugName = "GPUCulling.InstanceIndexBuffer";
    m_instanceIndexBuffer = m_device->CreateBuffer(desc);
    if (m_instanceIndexBuffer && m_config.maxInstances > 0)
    {
        uint32* instanceIndices = static_cast<uint32*>(m_instanceIndexBuffer->Map());
        if (!instanceIndices)
        {
            RVX_RENDER_ERROR("GPUCulling: failed to map instance index vertex buffer");
            m_instanceIndexBuffer.Reset();
        }
        else
        {
            for (uint32 instanceIndex = 0;
                 instanceIndex < m_config.maxInstances;
                 ++instanceIndex)
            {
                instanceIndices[instanceIndex] = instanceIndex;
            }
            m_instanceIndexBuffer->Unmap();
        }
    }

    // Visibility flag buffer (GPU cull pass output, compact pass input)
    desc.size = m_config.maxInstances * sizeof(uint32);
    desc.usage = gpuWritableOutputs
        ? MakeGpuWritableStructuredUsage(RHIBufferUsage::None)
        : RHIBufferUsage::None;
    desc.memoryType = gpuWritableOutputs ? RHIMemoryType::Default : cpuOutputMemoryType;
    desc.stride = sizeof(uint32);
    desc.debugName = "GPUCulling.VisibilityBuffer";
    m_visibilityBuffer = m_device->CreateBuffer(desc);

    // Visible instance buffer (output)
    desc.size = m_config.maxInstances * sizeof(uint32);
    desc.usage = gpuWritableOutputs
        ? MakeGpuWritableStructuredUsage(RHIBufferUsage::None)
        : RHIBufferUsage::None;
    desc.memoryType = gpuWritableOutputs ? RHIMemoryType::Default : cpuOutputMemoryType;
    desc.stride = sizeof(uint32);
    desc.debugName = "GPUCulling.VisibleInstanceBuffer";
    m_visibleInstanceBuffer = m_device->CreateBuffer(desc);

    // Indirect draw buffer
    desc.size = m_config.maxInstances * sizeof(IndirectDrawIndexedCommand);
    desc.usage = gpuWritableOutputs
        ? MakeGpuWritableStructuredUsage(RHIBufferUsage::IndirectArgs)
        : (RHIBufferUsage::IndirectArgs | RHIBufferUsage::CopyDst);
    desc.memoryType = RHIMemoryType::Default;
    desc.stride = sizeof(IndirectDrawIndexedCommand);
    desc.debugName = "GPUCulling.IndirectDrawBuffer";
    m_indirectBuffer = m_device->CreateBuffer(desc);

    // Draw count buffer. Element 0 is the legacy total draw count; grouped
    // draws use one counter per GPUCullingDrawGroup at the same index.
    desc.size = std::max<uint64>(sizeof(uint32) * 4,
                                 (static_cast<uint64>(m_config.maxInstances) + 1u) *
                                     sizeof(uint32));
    desc.usage = gpuWritableOutputs
        ? MakeGpuWritableStructuredUsage(RHIBufferUsage::IndirectArgs)
        : RHIBufferUsage::None;
    desc.memoryType = gpuWritableOutputs ? RHIMemoryType::Default : cpuOutputMemoryType;
    desc.stride = sizeof(uint32);
    desc.debugName = "GPUCulling.DrawCountBuffer";
    m_drawCountBuffer = m_device->CreateBuffer(desc);

    // Per-flight culling constants must not be overwritten while a previous
    // compute dispatch is still consuming them.
    desc.size = 256;
    desc.usage = RHIBufferUsage::Constant;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = 0;
    desc.debugName = "GPUCulling.ConstantsBuffer";
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.constantsBuffer = m_device->CreateBuffer(desc);
        if (inputs.constantsBuffer)
        {
            inputs.constantsAccess = MakeRHIBufferAccessSnapshot(
                RHIResourceState::ConstantBuffer,
                RHIShaderStage::Compute,
                GPUQueueDomain::Graphics,
                RHIContentValidity::Unknown);
        }
    }
    if (m_instanceIndexBuffer)
    {
        m_accessSnapshots.instanceIndices = MakeRHIBufferAccessSnapshot(
            RHIResourceState::VertexBuffer,
            RHIShaderStage::Vertex,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Unknown);
    }
    RefreshActiveInputAccessSnapshots();

    // Statistics buffer (optional)
    if (m_statsEnabled)
    {
        desc.size = sizeof(uint32) * 8;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Readback;
        desc.stride = sizeof(uint32);
        desc.debugName = "GPUCulling.StatsBuffer";
        m_statsBuffer = m_device->CreateBuffer(desc);
    }
}

void GPUCulling::CreatePipelineResources()
{
    m_pipelineFallbackReason = GPUCullingFallbackReason::None;
    QueueRenderOwnerRetirement(m_frustumCullShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_compactShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_cullingDescriptorSetLayout, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_cullingPipelineLayout, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_frustumCullPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_occlusionCullPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_compactPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneFrustumCullShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneCompactShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneDescriptorSetLayout, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuScenePipelineLayout, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneFrustumCullPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneCompactPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneDescriptorSet, m_pendingOwnerRetirements);
    m_gpuSceneTableBuffers = {};
    m_gpuSceneTableCapacities = {};
    m_gpuSceneLeaseVersion = 0;
    m_gpuSceneEnabled = false;

    if (!SupportsGpuExecution())
    {
        m_pipelineFallbackReason = EvaluateGpuExecution(false).fallbackReason;
        return;
    }

    const bool hasCompleteFrameInputs =
        !m_frameInputs.empty() &&
        std::all_of(m_frameInputs.begin(), m_frameInputs.end(),
                    [](const GPUCullingFrameInputs& inputs)
                    {
                        return inputs.instanceBuffer && inputs.constantsBuffer;
                    });
    if (!hasCompleteFrameInputs || !m_instanceIndexBuffer ||
        !m_visibilityBuffer ||
        !m_visibleInstanceBuffer ||
        !m_indirectBuffer ||
        !m_drawCountBuffer)
    {
        m_pipelineFallbackReason = GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return;
    }

    const std::filesystem::path shaderPath = FindGPUCullingShaderPath();
    if (shaderPath.empty())
    {
        RVX_RENDER_WARN("GPUCulling: GPU shader file not found; CPU fallback remains active");
        m_pipelineFallbackReason = GPUCullingFallbackReason::ShaderFileMissing;
        return;
    }

    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "GPUCulling.DescriptorSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(2, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(3, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(4, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(5, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    m_cullingDescriptorSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_cullingDescriptorSetLayout)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create descriptor set layout; CPU fallback remains active");
        m_pipelineFallbackReason = GPUCullingFallbackReason::DescriptorSetLayoutCreationFailed;
        return;
    }

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.debugName = "GPUCulling.PipelineLayout";
    pipelineLayoutDesc.setLayouts = {m_cullingDescriptorSetLayout.Get()};
    m_cullingPipelineLayout = m_device->CreatePipelineLayout(pipelineLayoutDesc);
    if (!m_cullingPipelineLayout)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create pipeline layout; CPU fallback remains active");
        m_pipelineFallbackReason = GPUCullingFallbackReason::PipelineLayoutCreationFailed;
        return;
    }

    ShaderManagerConfig shaderConfig;
    shaderConfig.enableDiskCache = false;
    shaderConfig.enableAsyncCompile = false;
    shaderConfig.enableHotReload = false;
    ShaderManager shaderManager(shaderConfig);

    ShaderLoadDesc shaderDesc;
    shaderDesc.path = shaderPath.string();
    shaderDesc.stage = RHIShaderStage::Compute;
    shaderDesc.backend = m_device->GetBackendType();
    shaderDesc.enableDebugInfo = false;
    shaderDesc.enableOptimization = true;
    if (shaderDesc.backend == RHIBackendType::DX12)
    {
        shaderDesc.targetProfile = "cs_6_0";
    }

    shaderDesc.entryPoint = "CSFrustumCull";
    ShaderLoadResult frustumResult = shaderManager.LoadFromFile(m_device, shaderDesc);
    if (!frustumResult.compileResult.success || !frustumResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile frustum cull shader: {}",
                        frustumResult.compileResult.errorMessage);
        m_pipelineFallbackReason = GPUCullingFallbackReason::ShaderCompilationFailed;
        return;
    }
    m_frustumCullShader = frustumResult.shader;

    shaderDesc.entryPoint = "CSCompactDraws";
    ShaderLoadResult compactResult = shaderManager.LoadFromFile(m_device, shaderDesc);
    if (!compactResult.compileResult.success || !compactResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile compact shader: {}",
                        compactResult.compileResult.errorMessage);
        m_pipelineFallbackReason = GPUCullingFallbackReason::ShaderCompilationFailed;
        return;
    }
    m_compactShader = compactResult.shader;

    RHIComputePipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = m_cullingPipelineLayout.Get();

    pipelineDesc.computeShader = m_frustumCullShader.Get();
    pipelineDesc.debugName = "GPUCulling.FrustumCullPipeline";
    m_frustumCullPipeline = m_device->CreateComputePipeline(pipelineDesc);

    pipelineDesc.computeShader = m_compactShader.Get();
    pipelineDesc.debugName = "GPUCulling.CompactPipeline";
    m_compactPipeline = m_device->CreateComputePipeline(pipelineDesc);

    if (!m_frustumCullPipeline || !m_compactPipeline)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create compute pipelines; CPU fallback remains active");
        m_frustumCullPipeline.Reset();
        m_compactPipeline.Reset();
        m_pipelineFallbackReason = GPUCullingFallbackReason::PipelineCreationFailed;
        return;
    }

    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.debugName = "GPUCulling.DescriptorSet";
        descriptorDesc.SetLayout(m_cullingDescriptorSetLayout.Get())
            .BindBuffer(0, inputs.constantsBuffer.Get())
            .BindBuffer(1, inputs.instanceBuffer.Get())
            .BindBuffer(2, m_visibilityBuffer.Get())
            .BindBuffer(3, m_visibleInstanceBuffer.Get())
            .BindBuffer(4, m_indirectBuffer.Get())
            .BindBuffer(5, m_drawCountBuffer.Get());
        inputs.descriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
        if (!inputs.descriptorSet)
        {
            RVX_RENDER_WARN("GPUCulling: failed to create frame-slot descriptor set; CPU fallback remains active");
            m_frustumCullPipeline.Reset();
            m_compactPipeline.Reset();
            m_pipelineFallbackReason =
                GPUCullingFallbackReason::DescriptorSetCreationFailed;
            return;
        }
    }

    // The GPU-scene path deliberately has an independent descriptor layout:
    // binding 1 is a stable candidate stream, while bindings 6..11 are the
    // six rows retained by one exact GPU-scene lease.  No normal culling
    // descriptor or execution path is changed here.
    const std::filesystem::path gpuSceneShaderPath = FindGPUSceneCullingShaderPath();
    if (gpuSceneShaderPath.empty())
    {
        RVX_RENDER_WARN("GPUCulling: GPU-scene shader file not found; GPU-scene recording remains disabled");
        return;
    }

    RHIDescriptorSetLayoutDesc gpuSceneSetLayoutDesc;
    gpuSceneSetLayoutDesc.debugName = "GPUCulling.GPUSceneDescriptorSetLayout";
    gpuSceneSetLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(2, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(3, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(4, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(5, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    for (uint32 binding = 6; binding < 12; ++binding)
    {
        gpuSceneSetLayoutDesc.AddBinding(
            binding, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Compute);
    }
    m_gpuSceneDescriptorSetLayout = m_device->CreateDescriptorSetLayout(gpuSceneSetLayoutDesc);
    if (!m_gpuSceneDescriptorSetLayout)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create GPU-scene descriptor layout; GPU-scene recording remains disabled");
        return;
    }

    RHIPipelineLayoutDesc gpuScenePipelineLayoutDesc;
    gpuScenePipelineLayoutDesc.debugName = "GPUCulling.GPUScenePipelineLayout";
    gpuScenePipelineLayoutDesc.setLayouts = {m_gpuSceneDescriptorSetLayout.Get()};
    m_gpuScenePipelineLayout = m_device->CreatePipelineLayout(gpuScenePipelineLayoutDesc);
    if (!m_gpuScenePipelineLayout)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create GPU-scene pipeline layout; GPU-scene recording remains disabled");
        m_gpuSceneDescriptorSetLayout.Reset();
        return;
    }

    ShaderLoadDesc gpuSceneShaderDesc = shaderDesc;
    gpuSceneShaderDesc.path = gpuSceneShaderPath.string();
    gpuSceneShaderDesc.entryPoint = "CSGPUSceneFrustumCull";
    ShaderLoadResult gpuSceneFrustumResult = shaderManager.LoadFromFile(
        m_device, gpuSceneShaderDesc);
    if (!gpuSceneFrustumResult.compileResult.success || !gpuSceneFrustumResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile GPU-scene frustum shader: {}",
                        gpuSceneFrustumResult.compileResult.errorMessage);
        m_gpuSceneDescriptorSetLayout.Reset();
        m_gpuScenePipelineLayout.Reset();
        return;
    }
    m_gpuSceneFrustumCullShader = gpuSceneFrustumResult.shader;

    gpuSceneShaderDesc.entryPoint = "CSGPUSceneCompactDraws";
    ShaderLoadResult gpuSceneCompactResult = shaderManager.LoadFromFile(
        m_device, gpuSceneShaderDesc);
    if (!gpuSceneCompactResult.compileResult.success || !gpuSceneCompactResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile GPU-scene compact shader: {}",
                        gpuSceneCompactResult.compileResult.errorMessage);
        m_gpuSceneFrustumCullShader.Reset();
        m_gpuSceneDescriptorSetLayout.Reset();
        m_gpuScenePipelineLayout.Reset();
        return;
    }
    m_gpuSceneCompactShader = gpuSceneCompactResult.shader;

    RHIComputePipelineDesc gpuScenePipelineDesc;
    gpuScenePipelineDesc.pipelineLayout = m_gpuScenePipelineLayout.Get();
    gpuScenePipelineDesc.computeShader = m_gpuSceneFrustumCullShader.Get();
    gpuScenePipelineDesc.debugName = "GPUCulling.GPUSceneFrustumCullPipeline";
    m_gpuSceneFrustumCullPipeline = m_device->CreateComputePipeline(gpuScenePipelineDesc);
    gpuScenePipelineDesc.computeShader = m_gpuSceneCompactShader.Get();
    gpuScenePipelineDesc.debugName = "GPUCulling.GPUSceneCompactPipeline";
    m_gpuSceneCompactPipeline = m_device->CreateComputePipeline(gpuScenePipelineDesc);
    if (!m_gpuSceneFrustumCullPipeline || !m_gpuSceneCompactPipeline)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create GPU-scene pipelines; GPU-scene recording remains disabled");
        m_gpuSceneFrustumCullPipeline.Reset();
        m_gpuSceneCompactPipeline.Reset();
        m_gpuSceneFrustumCullShader.Reset();
        m_gpuSceneCompactShader.Reset();
        m_gpuSceneDescriptorSetLayout.Reset();
        m_gpuScenePipelineLayout.Reset();
    }
}

GPUCullingExecutionDecision GPUCulling::EvaluateGpuExecution(bool requirePipelineResources) const
{
    GPUCullingExecutionDecision decision;
    if (!m_device)
    {
        decision.fallbackReason = GPUCullingFallbackReason::DeviceMissing;
        return decision;
    }

    const RHICapabilities& capabilities = m_device->GetCapabilities();
    if (!capabilities.supportsComputePipeline)
    {
        decision.fallbackReason = GPUCullingFallbackReason::ComputePipelineUnsupported;
        return decision;
    }

    if (!capabilities.supportsDescriptorSets)
    {
        decision.fallbackReason = GPUCullingFallbackReason::DescriptorSetsUnsupported;
        return decision;
    }

    if (!capabilities.indexedIndirectExecution.supportsCountBuffer)
    {
        decision.fallbackReason = GPUCullingFallbackReason::IndirectDrawCountUnsupported;
        return decision;
    }

    if (m_device->GetBackendType() != RHIBackendType::DX12)
    {
        decision.fallbackReason = GPUCullingFallbackReason::ShaderBackendUnsupported;
        return decision;
    }

    decision.gpuCapable = true;
    if (!requirePipelineResources)
    {
        decision.mode = GPUCullingExecutionMode::GpuCompute;
        decision.fallbackReason = GPUCullingFallbackReason::None;
        return decision;
    }

    if (m_pipelineFallbackReason != GPUCullingFallbackReason::None)
    {
        decision.fallbackReason = m_pipelineFallbackReason;
        return decision;
    }

    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!m_frustumCullPipeline || !m_compactPipeline || inputs == nullptr ||
        !inputs->descriptorSet)
    {
        decision.fallbackReason = GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return decision;
    }

    decision.mode = GPUCullingExecutionMode::GpuCompute;
    decision.fallbackReason = GPUCullingFallbackReason::None;
    decision.pipelineReady = true;
    return decision;
}

bool GPUCulling::SupportsGpuExecution() const
{
    return EvaluateGpuExecution(false).gpuCapable;
}

bool GPUCulling::IsGPUSceneExecutionReady() const
{
    return EvaluateGpuExecution(false).gpuCapable &&
        m_gpuSceneDescriptorSetLayout &&
        m_gpuScenePipelineLayout &&
        m_gpuSceneFrustumCullPipeline &&
        m_gpuSceneCompactPipeline;
}

GPUCullingExecutionDecision GPUCulling::GetExecutionDecision() const
{
    return EvaluateGpuExecution(true);
}

std::shared_ptr<GPUCullingRecordedState> GPUCulling::SealForGraph(
    const GPUCullingRecordingIdentity& identity) const
{
    if (!identity.IsValid() || m_device == nullptr ||
        GetActiveFrameInputs() == nullptr)
    {
        RVX_RENDER_WARN("GPUCulling: rejected an invalid graph-recording seal");
        return nullptr;
    }

    auto recordedState = std::make_shared<GPUCullingRecordedState>();
    GPUCulling& sealed = recordedState->m_culling;
    sealed.m_device = m_device;
    sealed.m_config = m_config;
    sealed.m_occlusionRequested = m_occlusionRequested;
    sealed.m_statsEnabled = m_statsEnabled;
    sealed.m_frameInputs.resize(1);
    sealed.CreateResources();

    // Pipelines and layouts are immutable RHI objects.  The recorded state
    // owns strong references to them, while its descriptor set below binds
    // only the new per-recording buffers created above.
    sealed.m_frustumCullShader = m_frustumCullShader;
    sealed.m_compactShader = m_compactShader;
    sealed.m_cullingDescriptorSetLayout = m_cullingDescriptorSetLayout;
    sealed.m_cullingPipelineLayout = m_cullingPipelineLayout;
    sealed.m_frustumCullPipeline = m_frustumCullPipeline;
    sealed.m_occlusionCullPipeline = m_occlusionCullPipeline;
    sealed.m_compactPipeline = m_compactPipeline;
    sealed.m_pipelineFallbackReason = m_pipelineFallbackReason;

    GPUCullingFrameInputs* sealedInputs = sealed.GetActiveFrameInputs();
    if (sealed.m_cullingDescriptorSetLayout && sealedInputs != nullptr &&
        sealedInputs->constantsBuffer && sealedInputs->instanceBuffer &&
        sealed.m_visibilityBuffer && sealed.m_visibleInstanceBuffer &&
        sealed.m_indirectBuffer && sealed.m_drawCountBuffer)
    {
        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.debugName = "GPUCulling.RecordedDescriptorSet";
        descriptorDesc.SetLayout(sealed.m_cullingDescriptorSetLayout.Get())
            .BindBuffer(0, sealedInputs->constantsBuffer.Get())
            .BindBuffer(1, sealedInputs->instanceBuffer.Get())
            .BindBuffer(2, sealed.m_visibilityBuffer.Get())
            .BindBuffer(3, sealed.m_visibleInstanceBuffer.Get())
            .BindBuffer(4, sealed.m_indirectBuffer.Get())
            .BindBuffer(5, sealed.m_drawCountBuffer.Get());
        sealedInputs->descriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
        if (!sealedInputs->descriptorSet)
        {
            sealed.m_frustumCullPipeline.Reset();
            sealed.m_compactPipeline.Reset();
            sealed.m_pipelineFallbackReason =
                GPUCullingFallbackReason::DescriptorSetCreationFailed;
        }
    }

    sealed.m_instances = m_instances;
    sealed.m_gpuSceneCandidates = m_gpuSceneCandidates;
    sealed.m_gpuSceneCandidateVersion = m_gpuSceneCandidateVersion;
    sealed.m_visibleInstanceIndices = m_visibleInstanceIndices;
    sealed.m_visibleSourceIndices = m_visibleSourceIndices;
    sealed.m_indirectCommands = m_indirectCommands;
    sealed.m_groupDrawCounts = m_groupDrawCounts;
    sealed.m_drawGroups = m_drawGroups;
    sealed.m_instanceCount = m_instanceCount;
    sealed.m_drawCount = m_drawCount;
    sealed.m_activeDrawGroupIndex = m_activeDrawGroupIndex;
    sealed.m_usedCpuFallbackLastCull = m_usedCpuFallbackLastCull;
    sealed.m_usedGpuExecutionLastCull = m_usedGpuExecutionLastCull;
    sealed.m_lastFallbackReason = m_lastFallbackReason;
    sealed.m_stats = m_stats;
    sealed.UploadInstances();

    recordedState->m_identity = identity;
    recordedState->m_sourceFrameSlot = m_activeFrameSlot;
    return recordedState;
}

std::shared_ptr<GPUCullingRecordedState> GPUCulling::SealForGPUSceneGraph(
    const GPUCullingRecordingIdentity& identity,
    const GPUSceneResidentGraphLease& lease) const
{
    std::shared_ptr<GPUCullingRecordedState> recordedState = SealForGraph(identity);
    if (!recordedState)
    {
        return nullptr;
    }

    GPUCulling& sealed = recordedState->m_culling;
    sealed.m_gpuSceneFrustumCullShader = m_gpuSceneFrustumCullShader;
    sealed.m_gpuSceneCompactShader = m_gpuSceneCompactShader;
    sealed.m_gpuSceneDescriptorSetLayout = m_gpuSceneDescriptorSetLayout;
    sealed.m_gpuScenePipelineLayout = m_gpuScenePipelineLayout;
    sealed.m_gpuSceneFrustumCullPipeline = m_gpuSceneFrustumCullPipeline;
    sealed.m_gpuSceneCompactPipeline = m_gpuSceneCompactPipeline;
    if (!sealed.ConfigureGPUSceneRecording(lease))
    {
        return nullptr;
    }
    return recordedState;
}

void GPUCulling::BeginFrame()
{
    m_instances.clear();
    m_gpuSceneCandidates.clear();
    m_gpuSceneCandidateVersion = 0;
    m_visibleInstanceIndices.clear();
    m_visibleSourceIndices.clear();
    m_indirectCommands.clear();
    m_groupDrawCounts.clear();
    m_drawGroups.clear();
    m_instanceCount = 0;
    m_drawCount = 0;
    m_activeDrawGroupIndex = RVX_INVALID_INDEX;
    m_usedCpuFallbackLastCull = false;
    m_usedGpuExecutionLastCull = false;
    m_lastFallbackReason = GPUCullingFallbackReason::None;
    m_stats = {};
}

uint32 GPUCulling::BeginDrawGroup(uint64 meshId,
                                  uint64 materialId,
                                  MaterialPipelineVariant pipelineVariant,
                                  RenderResourceHandle mesh,
                                  RenderResourceHandle material)
{
    if (m_drawGroups.size() >= m_config.maxInstances)
    {
        return RVX_INVALID_INDEX;
    }

    GPUCullingDrawGroup group;
    group.mesh = mesh;
    group.material = material;
    group.meshId = meshId;
    group.materialId = materialId;
    group.pipelineVariant = pipelineVariant;
    group.commandOffset = m_instanceCount;
    const uint32 groupIndex = static_cast<uint32>(m_drawGroups.size());
    group.countBufferOffset = static_cast<uint32>((groupIndex + 1) * sizeof(uint32));
    m_drawGroups.push_back(group);
    m_groupDrawCounts.push_back(0);
    m_activeDrawGroupIndex = groupIndex;
    return groupIndex;
}

void GPUCulling::EndDrawGroup()
{
    m_activeDrawGroupIndex = RVX_INVALID_INDEX;
}

uint32 GPUCulling::EnsureDefaultDrawGroup()
{
    if (m_activeDrawGroupIndex != RVX_INVALID_INDEX)
    {
        return m_activeDrawGroupIndex;
    }

    if (m_drawGroups.empty())
    {
        return BeginDrawGroup(0);
    }

    return 0;
}

uint32 GPUCulling::AddInstance(const GPUInstanceData& instance)
{
    if (m_instanceCount >= m_config.maxInstances)
    {
        return RVX_INVALID_INDEX;
    }

    const uint32 groupIndex = EnsureDefaultDrawGroup();
    if (groupIndex == RVX_INVALID_INDEX || groupIndex >= m_drawGroups.size())
    {
        return RVX_INVALID_INDEX;
    }

    uint32 index = m_instanceCount++;
    GPUInstanceData groupedInstance = instance;
    groupedInstance.drawGroupIndex = groupIndex;
    groupedInstance.drawGroupCommandOffset = m_drawGroups[groupIndex].commandOffset;
    m_instances.push_back(groupedInstance);
    ++m_drawGroups[groupIndex].maxDrawCount;
    return index;
}

void GPUCulling::AddInstances(const GPUInstanceData* instances, uint32 count)
{
    uint32 availableSlots = m_config.maxInstances - m_instanceCount;
    count = std::min(count, availableSlots);

    for (uint32 i = 0; i < count; ++i)
    {
        AddInstance(instances[i]);
    }
}

uint32 GPUCulling::AddDrawItemInstance(const RenderScene& scene,
                                       const RenderDrawItem& drawItem,
                                       const GPUIndexedDrawDesc& drawDesc,
                                       uint32 sourceIndex)
{
    if (drawItem.objectIndex >= scene.GetObjectCount() || drawDesc.indexCount == 0)
    {
        return RVX_INVALID_INDEX;
    }

    const RenderObject& object = scene.GetObject(drawItem.objectIndex);
    if (!object.visible)
    {
        return RVX_INVALID_INDEX;
    }

    const RenderVisibilityGPUInput visibilityInput =
        MakeRenderVisibilityGPUInput(object.bounds);
    const Vec3 center = visibilityInput.forceVisible == 0
        ? object.bounds.GetCenter()
        : Vec3(0.0f);
    const float radius = visibilityInput.forceVisible == 0
        ? length(object.bounds.GetExtent())
        : 0.0f;

    GPUInstanceData instance = {};
    instance.worldMatrix = object.worldMatrix;
    instance.normalMatrix = object.normalMatrix;
    instance.boundingSphere = Vec4(center, radius);
    instance.aabbMin = visibilityInput.aabbMin;
    instance.aabbMax = visibilityInput.aabbMax;
    instance.meshId = drawItem.mesh.slot;
    instance.materialId = drawItem.material.slot;
    instance.indexCount = drawDesc.indexCount;
    instance.firstIndex = drawDesc.firstIndex;
    instance.vertexOffset = drawDesc.vertexOffset;
    instance.sourceIndex = sourceIndex;
    instance.candidateIndex = sourceIndex;
    instance.forceVisible = visibilityInput.forceVisible;
    if (m_activeDrawGroupIndex == RVX_INVALID_INDEX && m_drawGroups.empty())
    {
        BeginDrawGroup((static_cast<uint64>(drawItem.mesh.slot) << 32U) |
                           drawItem.mesh.generation,
                       (static_cast<uint64>(drawItem.material.slot) << 32U) |
                           drawItem.material.generation,
                       GetGPUCullingPipelineVariant(drawItem.renderMode),
                       drawItem.mesh,
                       drawItem.material);
    }
    else if (m_activeDrawGroupIndex != RVX_INVALID_INDEX && m_activeDrawGroupIndex < m_drawGroups.size())
    {
        GPUCullingDrawGroup& group = m_drawGroups[m_activeDrawGroupIndex];
        group.mesh = drawItem.mesh;
        group.material = drawItem.material;
        group.meshId = (static_cast<uint64>(drawItem.mesh.slot) << 32U) |
                       drawItem.mesh.generation;
        group.materialId =
            (static_cast<uint64>(drawItem.material.slot) << 32U) |
            drawItem.material.generation;
        group.pipelineVariant = GetGPUCullingPipelineVariant(drawItem.renderMode);
    }
    return AddInstance(instance);
}

uint32 GPUCulling::AddVisibilityCandidateInstance(
    const RenderScene& scene,
    const RenderVisibilityCandidate& candidate,
    const RenderDrawPacket& packet,
    const GPUIndexedDrawDesc& drawDesc)
{
    if (candidate.candidateIndex == RVX_INVALID_INDEX ||
        candidate.sourcePacketIndex == RVX_INVALID_INDEX ||
        candidate.pass == RenderPassKind::None ||
        candidate.pass != packet.pass ||
        packet.primitiveData != candidate.objectIndex ||
        packet.objectId == 0 ||
        candidate.objectIndex >= scene.GetObjectCount() ||
        !candidate.objectVisible || !candidate.drawable ||
        drawDesc.indexCount == 0 ||
        drawDesc.indexCount != packet.arguments.indexCount ||
        drawDesc.firstIndex != packet.arguments.firstIndex ||
        drawDesc.vertexOffset != packet.arguments.vertexOffset)
    {
        return RVX_INVALID_INDEX;
    }

    const RenderObject& object = scene.GetObject(candidate.objectIndex);
    if (!object.visible || !object.drawable ||
        object.entityId != packet.objectId ||
        object.mesh != packet.geometryKey.mesh)
    {
        return RVX_INVALID_INDEX;
    }
    const RenderVisibilityGPUInput visibilityInput =
        MakeRenderVisibilityGPUInput(candidate.worldBounds);
    const Vec3 center = visibilityInput.forceVisible == 0
        ? candidate.worldBounds.GetCenter()
        : Vec3(0.0f);
    const float radius = visibilityInput.forceVisible == 0
        ? length(candidate.worldBounds.GetExtent())
        : 0.0f;

    GPUInstanceData instance{};
    instance.worldMatrix = object.worldMatrix;
    instance.normalMatrix = object.normalMatrix;
    instance.boundingSphere = Vec4(center, radius);
    instance.aabbMin = visibilityInput.aabbMin;
    instance.aabbMax = visibilityInput.aabbMax;
    instance.meshId = packet.geometryKey.mesh.slot;
    instance.materialId = packet.materialKey.material.slot;
    instance.indexCount = drawDesc.indexCount;
    instance.firstIndex = drawDesc.firstIndex;
    instance.vertexOffset = drawDesc.vertexOffset;
    instance.sourceIndex = candidate.sourcePacketIndex;
    instance.candidateIndex = candidate.candidateIndex;
    instance.forceVisible = visibilityInput.forceVisible;
    return AddInstance(instance);
}

bool GPUCulling::AddGPUSceneCandidate(
    const GPUSceneCullingCandidate& candidate,
    uint64 committedVersion)
{
    if (committedVersion == 0 || candidate.primitiveSlot == 0 ||
        candidate.primitiveGeneration == 0 || candidate.drawSlot == 0 ||
        candidate.drawGeneration == 0 ||
        (candidate.objectIdLow == 0 && candidate.objectIdHigh == 0) ||
        candidate.requiredPassMask == 0 ||
        (candidate.requiredPassMask & (candidate.requiredPassMask - 1u)) != 0 ||
        m_instances.empty() ||
        candidate.rasterInstanceIndex != m_instanceCount - 1u ||
        candidate.rasterInstanceIndex != m_gpuSceneCandidates.size() ||
        candidate.rasterInstanceIndex >= m_instances.size())
    {
        return false;
    }

    const GPUInstanceData& instance = m_instances[candidate.rasterInstanceIndex];
    if (candidate.drawGroupIndex != instance.drawGroupIndex ||
        candidate.drawGroupCommandOffset != instance.drawGroupCommandOffset ||
        candidate.drawGroupIndex >= m_drawGroups.size())
    {
        return false;
    }

    if (m_gpuSceneCandidateVersion != 0 &&
        m_gpuSceneCandidateVersion != committedVersion)
    {
        return false;
    }

    m_gpuSceneCandidateVersion = committedVersion;
    m_gpuSceneCandidates.push_back(candidate);
    return true;
}

void GPUCulling::InvalidateGPUSceneCandidates() noexcept
{
    m_gpuSceneCandidates.clear();
    m_gpuSceneCandidateVersion = 0;
}

bool GPUCulling::HasCompleteGPUSceneCandidates() const noexcept
{
    return m_gpuSceneCandidateVersion != 0 &&
        m_instanceCount != 0 &&
        m_gpuSceneCandidates.size() == m_instances.size() &&
        m_gpuSceneCandidates.size() == m_instanceCount;
}

void GPUCulling::EndFrame()
{
    UploadInstances();
}


void GPUCulling::UploadInstances()
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (m_instances.empty() || inputs == nullptr || !inputs->instanceBuffer)
    {
        return;
    }

    // Map and copy instance data
    void* mapped = inputs->instanceBuffer->Map();
    if (mapped)
    {
        std::memcpy(mapped, m_instances.data(),
                    m_instances.size() * sizeof(GPUInstanceData));
        inputs->instanceBuffer->Unmap();
        const GPUQueueDomain lastGpuDomain =
            inputs->instanceAccess.uniformAccess.domain;
        inputs->instanceAccess = MakeRHIBufferAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::Compute,
            lastGpuDomain,
            RHIContentValidity::Valid);
        m_accessSnapshots.instances = inputs->instanceAccess;
    }

    m_stats.totalInstances = m_instanceCount;
}

void GPUCulling::BuildCpuCullResults(const Mat4& viewMatrix, const Vec4* frustumPlanes)
{
    m_visibleInstanceIndices.clear();
    m_visibleSourceIndices.clear();
    m_indirectCommands.clear();
    std::fill(m_groupDrawCounts.begin(), m_groupDrawCounts.end(), 0);
    for (GPUCullingDrawGroup& group : m_drawGroups)
    {
        group.visibleDrawCount = 0;
    }
    m_drawCount = 0;
    m_stats = {};
    m_stats.totalInstances = m_instanceCount;

    const Vec3 cameraPosition = Vec3(inverse(viewMatrix)[3]);

    for (uint32 instanceIndex = 0; instanceIndex < m_instanceCount; ++instanceIndex)
    {
        const GPUInstanceData& instance = m_instances[instanceIndex];
        const Vec3 center(instance.boundingSphere.x, instance.boundingSphere.y, instance.boundingSphere.z);
        const Vec3 extent = Vec3(instance.aabbMax - instance.aabbMin) * 0.5f;

        bool visible = true;
        if (m_config.enableFrustumCulling && instance.forceVisible == 0)
        {
            for (uint32 planeIndex = 0; planeIndex < 6; ++planeIndex)
            {
                const Vec4& plane = frustumPlanes[planeIndex];
                const float distanceToPlane = plane.x * center.x + plane.y * center.y +
                                              plane.z * center.z + plane.w;
                const Vec3 normal(plane.x, plane.y, plane.z);
                if (dot(normal, normal) <= 1.0e-12f)
                {
                    continue;
                }
                const float projectedRadius = dot(glm::abs(normal), extent);
                if (distanceToPlane < -projectedRadius)
                {
                    visible = false;
                    ++m_stats.frustumCulled;
                    break;
                }
            }
        }

        if (!visible)
        {
            continue;
        }

        if (visible && instance.forceVisible == 0 &&
            m_config.enableDistanceCulling && m_config.maxDrawDistance > 0.0f)
        {
            const float radius = std::max(instance.boundingSphere.w, 0.0f);
            const float distanceToCamera = length(center - cameraPosition);
            if (distanceToCamera - radius > m_config.maxDrawDistance)
            {
                ++m_stats.distanceCulled;
                continue;
            }
        }

        if (instance.indexCount == 0)
        {
            continue;
        }

        m_visibleInstanceIndices.push_back(instanceIndex);
        if (instance.sourceIndex != RVX_INVALID_INDEX)
        {
            m_visibleSourceIndices.push_back(instance.sourceIndex);
        }

        IndirectDrawIndexedCommand command = {};
        command.indexCount = instance.indexCount;
        command.instanceCount = 1;
        command.firstIndex = instance.firstIndex;
        command.vertexOffset = instance.vertexOffset;
        command.firstInstance = instanceIndex;

        uint32 commandIndex = m_drawCount;
        if (instance.drawGroupIndex < m_drawGroups.size())
        {
            GPUCullingDrawGroup& group = m_drawGroups[instance.drawGroupIndex];
            commandIndex = group.commandOffset + group.visibleDrawCount;
            ++group.visibleDrawCount;
            if (instance.drawGroupIndex < m_groupDrawCounts.size())
            {
                m_groupDrawCounts[instance.drawGroupIndex] = group.visibleDrawCount;
            }
        }

        if (m_indirectCommands.size() <= commandIndex)
        {
            m_indirectCommands.resize(static_cast<size_t>(commandIndex) + 1);
        }
        m_indirectCommands[commandIndex] = command;
        ++m_drawCount;
    }

    m_stats.visibleInstances = static_cast<uint32>(m_visibleInstanceIndices.size());
}

bool GPUCulling::UploadBufferData(RHIBuffer* buffer,
                                  const void* data,
                                  uint64 size,
                                  RHICommandContext* ctx)
{
    if (!buffer || !data || size == 0)
    {
        return true;
    }

    if (buffer->GetMemoryType() != RHIMemoryType::Default)
    {
        if (void* mapped = buffer->Map())
        {
            std::memcpy(mapped, data, static_cast<size_t>(size));
            buffer->Unmap();
            return true;
        }
    }

    if (!ctx || !m_device)
    {
        return false;
    }

    RHIBufferDesc stagingDesc;
    stagingDesc.size = size;
    stagingDesc.usage = RHIBufferUsage::CopySrc;
    stagingDesc.memoryType = RHIMemoryType::Upload;
    stagingDesc.debugName = "GPUCulling.TransientUpload";

    RHIBufferRef stagingBuffer = m_device->CreateBuffer(stagingDesc);
    if (!stagingBuffer)
    {
        return false;
    }

    void* stagingMapped = stagingBuffer->Map();
    if (!stagingMapped)
    {
        return false;
    }

    std::memcpy(stagingMapped, data, static_cast<size_t>(size));
    stagingBuffer->Unmap();
    ctx->CopyBuffer(stagingBuffer.Get(), buffer, 0, 0, size);
    m_transientUploadBuffers.push_back(stagingBuffer);
    return true;
}

void GPUCulling::UploadCullOutputs(RHICommandContext* ctx)
{
    if (m_drawCountBuffer)
    {
        std::vector<uint32> drawCounts;
        drawCounts.reserve(m_groupDrawCounts.size() + 1);
        drawCounts.push_back(m_drawCount);
        drawCounts.insert(drawCounts.end(), m_groupDrawCounts.begin(), m_groupDrawCounts.end());
        UploadBufferData(m_drawCountBuffer.Get(),
                         drawCounts.data(),
                         static_cast<uint64>(drawCounts.size() * sizeof(uint32)),
                         ctx);
    }

    if (!m_visibleInstanceIndices.empty() && m_visibleInstanceBuffer)
    {
        UploadBufferData(m_visibleInstanceBuffer.Get(),
                         m_visibleInstanceIndices.data(),
                         static_cast<uint64>(m_visibleInstanceIndices.size() * sizeof(uint32)),
                         ctx);
    }

    if (!m_indirectCommands.empty() && m_indirectBuffer)
    {
        UploadBufferData(m_indirectBuffer.Get(),
                         m_indirectCommands.data(),
                         static_cast<uint64>(m_indirectCommands.size() * sizeof(IndirectDrawIndexedCommand)),
                         ctx);
    }
}

void GPUCulling::ExtractFrustumPlanes(const Mat4& viewProj, Vec4* planes)
{
    // Extract 6 frustum planes from view-projection matrix
    // Left, Right, Bottom, Top, Near, Far

    // Left plane
    planes[0] = Vec4(
        viewProj[0][3] + viewProj[0][0],
        viewProj[1][3] + viewProj[1][0],
        viewProj[2][3] + viewProj[2][0],
        viewProj[3][3] + viewProj[3][0]
    );

    // Right plane
    planes[1] = Vec4(
        viewProj[0][3] - viewProj[0][0],
        viewProj[1][3] - viewProj[1][0],
        viewProj[2][3] - viewProj[2][0],
        viewProj[3][3] - viewProj[3][0]
    );

    // Bottom plane
    planes[2] = Vec4(
        viewProj[0][3] + viewProj[0][1],
        viewProj[1][3] + viewProj[1][1],
        viewProj[2][3] + viewProj[2][1],
        viewProj[3][3] + viewProj[3][1]
    );

    // Top plane
    planes[3] = Vec4(
        viewProj[0][3] - viewProj[0][1],
        viewProj[1][3] - viewProj[1][1],
        viewProj[2][3] - viewProj[2][1],
        viewProj[3][3] - viewProj[3][1]
    );

    // Near plane
    planes[4] = Vec4(
        viewProj[0][2],
        viewProj[1][2],
        viewProj[2][2],
        viewProj[3][2]
    );

    // Far plane
    planes[5] = Vec4(
        viewProj[0][3] - viewProj[0][2],
        viewProj[1][3] - viewProj[1][2],
        viewProj[2][3] - viewProj[2][2],
        viewProj[3][3] - viewProj[3][2]
    );

    // Normalize planes
    for (int i = 0; i < 6; ++i)
    {
        float len = std::sqrt(planes[i].x * planes[i].x +
                              planes[i].y * planes[i].y +
                              planes[i].z * planes[i].z);
        if (len > 0.0001f)
        {
            planes[i] /= len;
        }
    }
}

void GPUCulling::Cull(RHICommandContext& ctx,
                      const Mat4& viewMatrix,
                      const Mat4& projMatrix,
                      RHITexture* hiZTexture)
{
    m_visibleInstanceIndices.clear();
    m_visibleSourceIndices.clear();
    m_indirectCommands.clear();
    std::fill(m_groupDrawCounts.begin(), m_groupDrawCounts.end(), 0);
    for (GPUCullingDrawGroup& group : m_drawGroups)
    {
        group.visibleDrawCount = 0;
    }
    m_drawCount = 0;
    m_usedCpuFallbackLastCull = false;
    m_usedGpuExecutionLastCull = false;
    m_lastFallbackReason = GPUCullingFallbackReason::None;

    if (m_instanceCount == 0)
    {
        UploadCullOutputs(&ctx);
        return;
    }

    Mat4 viewProj = projMatrix * viewMatrix;

    // Update the shared, fixed ABI.  The GPU-scene table capacity blocks are
    // explicitly zero for Tier 1 culling so no stale lease values can affect
    // a normal dispatch.
    GPUCullingConstants constants = MakeDefaultGPUCullingConstants();

    constants.viewProj = viewProj;
    ExtractFrustumPlanes(viewProj, constants.frustumPlanes);
    constants.cameraPosition = Vec4(inverse(viewMatrix)[3]);
    constants.params = Vec4(
        m_config.maxDrawDistance,
        0.0f,
        m_config.enableFrustumCulling ? 1.0f : 0.0f,
        m_config.enableDistanceCulling ? 1.0f : 0.0f
    );
    constants.counts[0] = m_instanceCount;
    constants.counts[1] = static_cast<uint32>(m_drawGroups.size());

    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (inputs != nullptr && inputs->constantsBuffer)
    {
        if (UploadBufferData(
                inputs->constantsBuffer.Get(), &constants, sizeof(constants), &ctx))
        {
            const GPUQueueDomain lastGpuDomain =
                inputs->constantsAccess.uniformAccess.domain;
            inputs->constantsAccess = MakeRHIBufferAccessSnapshot(
                RHIResourceState::ConstantBuffer,
                RHIShaderStage::Compute,
                lastGpuDomain,
                RHIContentValidity::Valid);
            m_accessSnapshots.constants = inputs->constantsAccess;
        }
    }

    const GPUCullingExecutionDecision executionDecision = EvaluateGpuExecution(true);
    if (executionDecision.mode != GPUCullingExecutionMode::GpuCompute)
    {
        m_usedCpuFallbackLastCull = true;
        m_lastFallbackReason = executionDecision.fallbackReason;
        const Mat4 cpuViewProj = projMatrix * viewMatrix;
        Vec4 frustumPlanes[6];
        ExtractFrustumPlanes(cpuViewProj, frustumPlanes);
        BuildCpuCullResults(viewMatrix, frustumPlanes);
        UploadCullOutputs(&ctx);
        return;
    }

    // Dispatch frustum culling compute shader
    ctx.SetPipeline(m_frustumCullPipeline.Get());
    ctx.SetDescriptorSet(0, inputs->descriptorSet.Get());
    // CSFrustumCull clears total + every per-group counter. Dispatch enough
    // threads for both the instance stream and a potentially sparse group set.
    const uint32 clearThreadCount = std::max(
        m_instanceCount,
        static_cast<uint32>(m_drawGroups.size()) + 1u);
    uint32 groupCount = (clearThreadCount + 63u) / 64u;
    ctx.Dispatch(groupCount, 1, 1);

    const RHIAccessSnapshot computeUAVAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics);
    const auto insertCullUAVBarriers = [&ctx, this, &computeUAVAccess]()
    {
        ctx.BufferBarrier(m_visibilityBuffer.Get(), computeUAVAccess, computeUAVAccess);
        ctx.BufferBarrier(m_indirectBuffer.Get(), computeUAVAccess, computeUAVAccess);
        ctx.BufferBarrier(m_drawCountBuffer.Get(), computeUAVAccess, computeUAVAccess);
    };

    // Frustum writes are consumed by compaction. Keep the barrier before an
    // optional occlusion pass as well, because it is another visibility consumer.
    insertCullUAVBarriers();

    // Dispatch occlusion culling if enabled and HiZ available
    if (m_config.enableOcclusionCulling && hiZTexture && m_occlusionCullPipeline)
    {
        ctx.SetPipeline(m_occlusionCullPipeline.Get());
        // Bind HiZ texture and buffers...
        ctx.Dispatch(groupCount, 1, 1);
        insertCullUAVBarriers();
    }

    // Compact visible instances into draw commands
    ctx.SetPipeline(m_compactPipeline.Get());
    ctx.SetDescriptorSet(0, inputs->descriptorSet.Get());
    ctx.Dispatch(groupCount, 1, 1);

    m_usedGpuExecutionLastCull = true;
    m_lastFallbackReason = GPUCullingFallbackReason::None;
    m_stats.totalInstances = m_instanceCount;
}

void GPUCulling::UploadGPUSceneCandidates()
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!HasCompleteGPUSceneCandidates() || inputs == nullptr ||
        !inputs->gpuSceneCandidateBuffer)
    {
        return;
    }

    void* mapped = inputs->gpuSceneCandidateBuffer->Map();
    if (!mapped)
    {
        RVX_RENDER_ERROR("GPUCulling: failed to map GPU-scene candidate buffer");
        return;
    }

    std::memcpy(mapped,
                m_gpuSceneCandidates.data(),
                m_gpuSceneCandidates.size() * sizeof(GPUSceneCullingCandidate));
    inputs->gpuSceneCandidateBuffer->Unmap();
    const GPUQueueDomain lastGpuDomain =
        inputs->gpuSceneCandidateAccess.uniformAccess.domain;
    inputs->gpuSceneCandidateAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Valid);
    m_accessSnapshots.gpuSceneCandidates = inputs->gpuSceneCandidateAccess;
}

bool GPUCulling::ConfigureGPUSceneRecording(
    const GPUSceneResidentGraphLease& lease)
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!lease.IsValid() || !HasCompleteGPUSceneCandidates() ||
        lease.version != m_gpuSceneCandidateVersion ||
        !IsGPUSceneExecutionReady() || inputs == nullptr ||
        !inputs->constantsBuffer ||
        !m_visibilityBuffer || !m_visibleInstanceBuffer || !m_indirectBuffer ||
        !m_drawCountBuffer)
    {
        return false;
    }

    if (!inputs->gpuSceneCandidateBuffer)
    {
        RHIBufferDesc candidateDesc;
        candidateDesc.size =
            static_cast<uint64>(m_config.maxInstances) * sizeof(GPUSceneCullingCandidate);
        candidateDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        candidateDesc.memoryType = RHIMemoryType::Upload;
        candidateDesc.stride = sizeof(GPUSceneCullingCandidate);
        candidateDesc.debugName = "GPUCulling.GPUSceneCandidateBuffer";
        inputs->gpuSceneCandidateBuffer = m_device->CreateBuffer(candidateDesc);
        if (!inputs->gpuSceneCandidateBuffer)
        {
            return false;
        }
        inputs->gpuSceneCandidateAccess = MakeRHIBufferAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::Compute,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Unknown);
    }

    UploadGPUSceneCandidates();
    if (inputs->gpuSceneCandidateAccess.uniformAccess.contentValidity !=
        RHIContentValidity::Valid)
    {
        return false;
    }

    // A GPU-scene descriptor is usable only with constants written from the
    // same exact lease.  This seals all capacity reads before any later
    // consumer can dispatch these pipelines.
    GPUCullingConstants constants = MakeDefaultGPUCullingConstants();
    constants.counts[0] = m_instanceCount;
    constants.counts[1] = static_cast<uint32>(m_drawGroups.size());
    for (uint32 tableIndex = 0;
         tableIndex < RVX_GPU_SCENE_CULLING_TABLE_COUNT;
         ++tableIndex)
    {
        if (lease.capacities[tableIndex] == 0)
        {
            return false;
        }
        const uint32 rowStride = GetGPUSceneTableRowStride(tableIndex);
        const uint64 requiredBytes =
            static_cast<uint64>(lease.capacities[tableIndex]) * rowStride;
        if (rowStride == 0 || !lease.buffers[tableIndex] ||
            lease.buffers[tableIndex]->GetStride() != rowStride ||
            requiredBytes > lease.buffers[tableIndex]->GetSize())
        {
            return false;
        }
        if (tableIndex < 4)
        {
            constants.gpuSceneTableCounts0[tableIndex] =
                lease.capacities[tableIndex];
        }
        else
        {
            constants.gpuSceneTableCounts1[tableIndex - 4] =
                lease.capacities[tableIndex];
        }
    }
    if (!UploadBufferData(
            inputs->constantsBuffer.Get(), &constants, sizeof(constants), nullptr))
    {
        return false;
    }
    const GPUQueueDomain lastGpuDomain =
        inputs->constantsAccess.uniformAccess.domain;
    inputs->constantsAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Valid);
    m_accessSnapshots.constants = inputs->constantsAccess;

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.debugName = "GPUCulling.GPUSceneRecordedDescriptorSet";
    descriptorDesc.SetLayout(m_gpuSceneDescriptorSetLayout.Get())
        .BindBuffer(0, inputs->constantsBuffer.Get())
        .BindBuffer(1, inputs->gpuSceneCandidateBuffer.Get())
        .BindBuffer(2, m_visibilityBuffer.Get())
        .BindBuffer(3, m_visibleInstanceBuffer.Get())
        .BindBuffer(4, m_indirectBuffer.Get())
        .BindBuffer(5, m_drawCountBuffer.Get());
    for (uint32 tableIndex = 0;
         tableIndex < RVX_GPU_SCENE_CULLING_TABLE_COUNT;
         ++tableIndex)
    {
        descriptorDesc.BindBuffer(6 + tableIndex, lease.buffers[tableIndex].Get());
    }

    m_gpuSceneDescriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
    if (!m_gpuSceneDescriptorSet)
    {
        return false;
    }

    m_gpuSceneTableBuffers = lease.buffers;
    m_gpuSceneTableCapacities = lease.capacities;
    m_gpuSceneLeaseVersion = lease.version;
    m_gpuSceneEnabled = true;
    return true;
}

bool GPUCulling::CullGPUScene(RHICommandContext& ctx,
                               const Mat4& viewMatrix,
                               const Mat4& projMatrix)
{
    m_visibleInstanceIndices.clear();
    m_visibleSourceIndices.clear();
    m_indirectCommands.clear();
    std::fill(m_groupDrawCounts.begin(), m_groupDrawCounts.end(), 0);
    for (GPUCullingDrawGroup& group : m_drawGroups)
    {
        group.visibleDrawCount = 0;
    }
    m_drawCount = 0;
    m_usedCpuFallbackLastCull = false;
    m_usedGpuExecutionLastCull = false;
    m_lastFallbackReason = GPUCullingFallbackReason::PipelineResourcesUnavailable;

    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!m_gpuSceneEnabled || !HasCompleteGPUSceneCandidates() ||
        m_gpuSceneLeaseVersion == 0 ||
        m_gpuSceneLeaseVersion != m_gpuSceneCandidateVersion ||
        !IsGPUSceneExecutionReady() || !m_gpuSceneDescriptorSet ||
        inputs == nullptr || !inputs->constantsBuffer ||
        !inputs->gpuSceneCandidateBuffer ||
        inputs->gpuSceneCandidateAccess.uniformAccess.contentValidity !=
            RHIContentValidity::Valid ||
        !m_visibilityBuffer || !m_visibleInstanceBuffer || !m_indirectBuffer ||
        !m_drawCountBuffer)
    {
        return false;
    }

    for (uint32 tableIndex = 0;
         tableIndex < RVX_GPU_SCENE_CULLING_TABLE_COUNT;
         ++tableIndex)
    {
        if (!m_gpuSceneTableBuffers[tableIndex] ||
            m_gpuSceneTableCapacities[tableIndex] == 0)
        {
            return false;
        }
    }

    const Mat4 viewProj = projMatrix * viewMatrix;
    GPUCullingConstants constants = MakeDefaultGPUCullingConstants();
    constants.viewProj = viewProj;
    ExtractFrustumPlanes(viewProj, constants.frustumPlanes);
    constants.cameraPosition = Vec4(inverse(viewMatrix)[3]);
    constants.params = Vec4(
        m_config.maxDrawDistance,
        0.0f,
        m_config.enableFrustumCulling ? 1.0f : 0.0f,
        m_config.enableDistanceCulling ? 1.0f : 0.0f);
    constants.counts[0] = m_instanceCount;
    constants.counts[1] = static_cast<uint32>(m_drawGroups.size());
    for (uint32 tableIndex = 0;
         tableIndex < RVX_GPU_SCENE_CULLING_TABLE_COUNT;
         ++tableIndex)
    {
        if (tableIndex < 4)
        {
            constants.gpuSceneTableCounts0[tableIndex] =
                m_gpuSceneTableCapacities[tableIndex];
        }
        else
        {
            constants.gpuSceneTableCounts1[tableIndex - 4] =
                m_gpuSceneTableCapacities[tableIndex];
        }
    }

    if (!UploadBufferData(
            inputs->constantsBuffer.Get(), &constants, sizeof(constants), &ctx))
    {
        return false;
    }
    const GPUQueueDomain lastGpuDomain =
        inputs->constantsAccess.uniformAccess.domain;
    inputs->constantsAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Valid);
    m_accessSnapshots.constants = inputs->constantsAccess;

    const uint32 clearThreadCount = std::max(
        m_instanceCount,
        static_cast<uint32>(m_drawGroups.size()) + 1u);
    const uint32 groupCount = (clearThreadCount + 63u) / 64u;

    ctx.SetPipeline(m_gpuSceneFrustumCullPipeline.Get());
    ctx.SetDescriptorSet(0, m_gpuSceneDescriptorSet.Get());
    ctx.Dispatch(groupCount, 1, 1);

    const RHIAccessSnapshot computeUAVAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics);
    ctx.BufferBarrier(m_visibilityBuffer.Get(), computeUAVAccess, computeUAVAccess);
    ctx.BufferBarrier(m_indirectBuffer.Get(), computeUAVAccess, computeUAVAccess);
    ctx.BufferBarrier(m_drawCountBuffer.Get(), computeUAVAccess, computeUAVAccess);

    ctx.SetPipeline(m_gpuSceneCompactPipeline.Get());
    ctx.SetDescriptorSet(0, m_gpuSceneDescriptorSet.Get());
    ctx.Dispatch(groupCount, 1, 1);

    m_usedGpuExecutionLastCull = true;
    m_lastFallbackReason = GPUCullingFallbackReason::None;
    m_stats.totalInstances = m_instanceCount;
    return true;
}

void GPUCulling::CullCpuFallback(const Mat4& viewMatrix, const Mat4& projMatrix)
{
    m_visibleInstanceIndices.clear();
    m_visibleSourceIndices.clear();
    m_indirectCommands.clear();
    std::fill(m_groupDrawCounts.begin(), m_groupDrawCounts.end(), 0);
    for (GPUCullingDrawGroup& group : m_drawGroups)
    {
        group.visibleDrawCount = 0;
    }
    m_drawCount = 0;
    m_usedCpuFallbackLastCull = true;
    m_usedGpuExecutionLastCull = false;
    m_lastFallbackReason = GPUCullingFallbackReason::None;

    if (m_instanceCount == 0)
    {
        m_stats = {};
        UploadCullOutputs();
        return;
    }

    const Mat4 viewProj = projMatrix * viewMatrix;
    Vec4 frustumPlanes[6];
    ExtractFrustumPlanes(viewProj, frustumPlanes);
    BuildCpuCullResults(viewMatrix, frustumPlanes);
    UploadCullOutputs();
}

GPUCullingIndexedIndirectSubmission GPUCulling::BuildIndexedIndirectSubmission(
    uint32 maxDrawCount) const
{
    GPUCullingIndexedIndirectSubmission submission;
    if (!m_indirectBuffer)
    {
        return submission;
    }

    if (m_drawGroups.size() > 1)
    {
        return submission;
    }

    submission.capabilities = m_device != nullptr
        ? &m_device->GetCapabilities()
        : nullptr;
    submission.execution.argumentBuffer = m_indirectBuffer.Get();
    submission.execution.commandStride = sizeof(IndirectDrawIndexedCommand);

    if (m_usedGpuExecutionLastCull && m_drawCountBuffer)
    {
        submission.execution.mode = RHIIndirectExecutionMode::CountBuffer;
        submission.execution.countBuffer = m_drawCountBuffer.Get();
        submission.execution.maxDrawCount = maxDrawCount > 0
            ? std::min(m_instanceCount, maxDrawCount)
            : m_instanceCount;
        return submission;
    }

    submission.execution.mode = RHIIndirectExecutionMode::FixedCount;
    submission.execution.maxDrawCount = maxDrawCount > 0
        ? std::min(m_drawCount, maxDrawCount)
        : m_drawCount;
    return submission;
}

GPUCullingIndexedIndirectSubmission GPUCulling::BuildIndexedIndirectGroupSubmission(
    uint32 groupIndex) const
{
    GPUCullingIndexedIndirectSubmission submission;
    if (!m_indirectBuffer || groupIndex >= m_drawGroups.size())
    {
        return submission;
    }

    const GPUCullingDrawGroup& group = m_drawGroups[groupIndex];
    submission.capabilities = m_device != nullptr
        ? &m_device->GetCapabilities()
        : nullptr;
    submission.execution.argumentBuffer = m_indirectBuffer.Get();
    submission.execution.argumentOffset =
        static_cast<uint64>(group.commandOffset) * sizeof(IndirectDrawIndexedCommand);
    submission.execution.commandStride = sizeof(IndirectDrawIndexedCommand);

    if (m_usedGpuExecutionLastCull && m_drawCountBuffer)
    {
        submission.execution.mode = RHIIndirectExecutionMode::CountBuffer;
        submission.execution.countBuffer = m_drawCountBuffer.Get();
        submission.execution.countOffset = group.countBufferOffset;
        submission.execution.maxDrawCount = group.maxDrawCount;
        return submission;
    }

    submission.execution.mode = RHIIndirectExecutionMode::FixedCount;
    submission.execution.maxDrawCount = group.visibleDrawCount;
    return submission;
}

// ============================================================================
// MeshletRenderer
// ============================================================================

MeshletRenderer::~MeshletRenderer()
{
    Shutdown();
}

void MeshletRenderer::Initialize(IRHIDevice* device)
{
    m_device = device;
}

void MeshletRenderer::Shutdown()
{
    m_meshletBuffer.Reset();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_visibleMeshletBuffer.Reset();
    m_meshletCullPipeline.Reset();
    m_meshletDrawPipeline.Reset();
    m_device = nullptr;
}

void MeshletRenderer::GenerateMeshlets(
    const Vec3* vertices,
    uint32 vertexCount,
    const uint32* indices,
    uint32 indexCount,
    uint32 maxVertices,
    uint32 maxTriangles,
    std::vector<Meshlet>& outMeshlets)
{
    // Simple meshlet generation algorithm
    // For production, use meshoptimizer or similar

    outMeshlets.clear();

    uint32 triangleCount = indexCount / 3;
    uint32 currentTriangle = 0;

    while (currentTriangle < triangleCount)
    {
        Meshlet meshlet = {};
        meshlet.vertexOffset = 0;  // Would be calculated based on vertex deduplication
        meshlet.triangleOffset = currentTriangle * 3;
        meshlet.vertexCount = 0;
        meshlet.triangleCount = 0;

        // Calculate bounding sphere
        Vec3 center(0.0f);
        float radius = 0.0f;

        // Add triangles to meshlet. This simple generator does not deduplicate
        // vertices, so maxVertices is treated as a conservative triangle cap.
        const uint32 vertexLimitedTriangles = maxVertices > 0 ? std::max(1u, maxVertices / 3u) : maxTriangles;
        const uint32 trianglesPerMeshlet = std::max(1u, std::min(maxTriangles, vertexLimitedTriangles));
        uint32 trianglesToAdd = std::min(trianglesPerMeshlet, triangleCount - currentTriangle);
        meshlet.triangleCount = trianglesToAdd;

        // Calculate bounding sphere from vertices in this meshlet
        for (uint32 t = 0; t < trianglesToAdd; ++t)
        {
            for (int v = 0; v < 3; ++v)
            {
                uint32 idx = indices[(currentTriangle + t) * 3 + v];
                if (idx < vertexCount)
                {
                    center += vertices[idx];
                    meshlet.vertexCount++;
                }
            }
        }

        if (meshlet.vertexCount > 0)
        {
            center /= static_cast<float>(meshlet.vertexCount);
        }

        // Calculate radius
        for (uint32 t = 0; t < trianglesToAdd; ++t)
        {
            for (int v = 0; v < 3; ++v)
            {
                uint32 idx = indices[(currentTriangle + t) * 3 + v];
                if (idx < vertexCount)
                {
                    float dist = length(vertices[idx] - center);
                    radius = std::max(radius, dist);
                }
            }
        }

        meshlet.boundingSphere = Vec4(center, radius);

        // TODO: Calculate cone for backface culling

        outMeshlets.push_back(meshlet);
        currentTriangle += trianglesToAdd;
    }
}

void MeshletRenderer::Render(RHICommandContext& ctx,
                              const Mat4& viewMatrix,
                              const Mat4& projMatrix)
{
    (void)ctx;
    (void)viewMatrix;
    (void)projMatrix;

    // TODO: Implement meshlet rendering
    // 1. Cull meshlets using compute shader
    // 2. Generate indirect draw commands
    // 3. Execute mesh shader or indirect draws
}

} // namespace RVX
