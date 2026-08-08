#include "Render/Submission/RasterInstanceStream.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Submission/RenderInstanceBatchPlan.h"
#include "Render/Visibility/RenderVisibility.h"

#include <cstring>
#include <limits>
#include <utility>

namespace RVX
{
RHIBufferRef CreateRasterInstanceIndexBuffer(IRHIDevice& device,
                                             uint32 instanceCapacity,
                                             const char* debugName)
{
    if (instanceCapacity == 0)
    {
        return {};
    }
    RHIBufferDesc desc;
    desc.size = static_cast<uint64>(instanceCapacity) * sizeof(uint32);
    desc.usage = RHIBufferUsage::Vertex;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(uint32);
    desc.debugName = debugName ? debugName : "RasterInstanceIndices";
    RHIBufferRef buffer = device.CreateBuffer(desc);
    uint32* mapped = buffer ? static_cast<uint32*>(buffer->Map()) : nullptr;
    if (mapped == nullptr)
    {
        return {};
    }
    for (uint32 index = 0; index < instanceCapacity; ++index)
    {
        mapped[index] = index;
    }
    buffer->Unmap();
    return buffer;
}

bool CreateRasterInstanceStream(IRHIDevice& device,
                                std::span<const GPUInstanceData> instances,
                                const char* debugName,
                                RasterInstanceStream& outStream)
{
    outStream = {};
    if (instances.empty() ||
        instances.size() > static_cast<size_t>(std::numeric_limits<uint32>::max()))
    {
        return false;
    }

    RHIBufferDesc desc;
    desc.size = static_cast<uint64>(instances.size()) * sizeof(GPUInstanceData);
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(GPUInstanceData);
    desc.debugName = debugName ? debugName : "RasterInstanceStream";
    RHIBufferRef instanceBuffer = device.CreateBuffer(desc);
    void* mapped = instanceBuffer ? instanceBuffer->Map() : nullptr;
    if (mapped == nullptr)
    {
        return false;
    }
    std::memcpy(mapped, instances.data(), static_cast<size_t>(desc.size));
    instanceBuffer->Unmap();

    RHIBufferRef indexBuffer = CreateRasterInstanceIndexBuffer(
        device,
        static_cast<uint32>(instances.size()),
        debugName ? debugName : "RasterInstanceIndices");
    if (!indexBuffer)
    {
        return false;
    }

    outStream.instances = std::move(instanceBuffer);
    outStream.instanceIndices = std::move(indexBuffer);
    outStream.instanceCount = static_cast<uint32>(instances.size());
    return true;
}

bool BuildRasterInstanceData(
    const RenderInstanceBatchPlan& plan,
    const DirectDrawPacketBatch& directBatch,
    const RenderScene& scene,
    std::vector<GPUInstanceData>& outInstances)
{
    outInstances.clear();
    if (!plan.IsComplete() ||
        plan.executedPacketCount != directBatch.packets.size())
    {
        return false;
    }

    uint32 expectedInstanceCount = 0;
    for (const RenderInstanceBatch& batch : plan.batches)
    {
        if (batch.instanced)
        {
            expectedInstanceCount += static_cast<uint32>(batch.members.size());
        }
    }
    outInstances.reserve(expectedInstanceCount);
    for (const RenderInstanceBatch& batch : plan.batches)
    {
        if (!batch.instanced)
        {
            continue;
        }
        if (batch.members.empty() || batch.firstInstance != outInstances.size())
        {
            outInstances.clear();
            return false;
        }
        for (const RenderInstanceBatchMember& member : batch.members)
        {
            if (member.directPacketIndex >= directBatch.packets.size())
            {
                outInstances.clear();
                return false;
            }
            const DirectDrawPacket& draw =
                directBatch.packets[member.directPacketIndex];
            const RenderDrawPacket& packet = draw.packet;
            if (packet.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX ||
                packet.primitiveData >= scene.GetObjectCount())
            {
                outInstances.clear();
                return false;
            }
            const RenderObject& object = scene.GetObject(packet.primitiveData);
            if (packet.objectId == 0 || object.entityId != packet.objectId ||
                object.mesh != packet.geometryKey.mesh)
            {
                outInstances.clear();
                return false;
            }

            GPUInstanceData instance{};
            instance.worldMatrix = object.worldMatrix;
            instance.normalMatrix = object.normalMatrix;
            const RenderVisibilityGPUInput visibility =
                MakeRenderVisibilityGPUInput(object.bounds);
            const Vec3 center = visibility.forceVisible == 0
                ? object.bounds.GetCenter() : Vec3(0.0f);
            const float radius = visibility.forceVisible == 0
                ? length(object.bounds.GetExtent()) : 0.0f;
            instance.boundingSphere = Vec4(center, radius);
            instance.aabbMin = visibility.aabbMin;
            instance.aabbMax = visibility.aabbMax;
            instance.meshId = packet.geometryKey.mesh.slot;
            instance.materialId = batch.key.usesMaterialParameterTable
                ? packet.materialKey.material.slot
                : RVX_INVALID_INDEX;
            instance.indexCount = packet.arguments.indexCount;
            instance.firstIndex = packet.arguments.firstIndex;
            instance.vertexOffset = packet.arguments.vertexOffset;
            instance.sourceIndex = member.directPacketIndex;
            instance.candidateIndex = member.directPacketIndex;
            instance.forceVisible = visibility.forceVisible;
            outInstances.push_back(instance);
        }
    }
    return outInstances.size() == expectedInstanceCount;
}
} // namespace RVX
