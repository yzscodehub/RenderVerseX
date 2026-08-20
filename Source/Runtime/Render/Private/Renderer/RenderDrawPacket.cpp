#include "Render/Renderer/RenderDrawPacket.h"

#include <cmath>
#include <limits>

namespace RVX
{
namespace
{
    constexpr uint64 FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64 FNV_PRIME = 1099511628211ULL;

    void HashByte(uint64& hash, uint8 value) noexcept
    {
        hash ^= value;
        hash *= FNV_PRIME;
    }

    template <typename TValue>
    void HashValue(uint64& hash, TValue value) noexcept
    {
        for (uint32 shift = 0; shift < sizeof(TValue) * 8U; shift += 8U)
        {
            HashByte(hash, static_cast<uint8>(value >> shift));
        }
    }

    MaterialPipelineVariant ToPipelineVariant(RenderMaterialMode mode) noexcept
    {
        switch (mode)
        {
            case RenderMaterialMode::Masked:
                return MaterialPipelineVariant::Masked;
            case RenderMaterialMode::Transparent:
                return MaterialPipelineVariant::Transparent;
            case RenderMaterialMode::Opaque:
            default:
                return MaterialPipelineVariant::Opaque;
        }
    }

    RenderPassKind ToMaterialPass(RenderMaterialMode mode) noexcept
    {
        return mode == RenderMaterialMode::Transparent
                   ? RenderPassKind::Transparent
                   : RenderPassKind::Opaque;
    }

    RenderDrawFlags ToDrawFlags(RenderBatchFlags flags) noexcept
    {
        RenderDrawFlags result = RenderDrawFlags::None;
        if (HasRenderBatchFlag(flags, RenderBatchFlags::Skinned))
            result = result | RenderDrawFlags::Skinned;
        if (HasRenderBatchFlag(flags, RenderBatchFlags::Masked))
            result = result | RenderDrawFlags::Masked;
        if (HasRenderBatchFlag(flags, RenderBatchFlags::Transparent))
            result = result | RenderDrawFlags::Transparent;
        if (HasRenderBatchFlag(flags, RenderBatchFlags::CastsShadow))
            result = result | RenderDrawFlags::CastsShadow;
        if (HasRenderBatchFlag(flags, RenderBatchFlags::ReceivesShadow))
            result = result | RenderDrawFlags::ReceivesShadow;
        if (HasRenderBatchFlag(flags, RenderBatchFlags::MissingMaterial))
            result = result | RenderDrawFlags::MissingMaterial;
        return result;
    }
} // namespace

bool PipelineKey::operator==(const PipelineKey& other) const noexcept
{
    return materialVariant == other.materialVariant && topology == other.topology &&
           skinned == other.skinned;
}

bool GeometryBindingKey::operator==(const GeometryBindingKey& other) const noexcept
{
    return mesh == other.mesh && submeshIndex == other.submeshIndex &&
           indexType == other.indexType;
}

bool MaterialBindingKey::operator==(const MaterialBindingKey& other) const noexcept
{
    return material == other.material && materialMode == other.materialMode;
}

uint64 GetStableHash(const PipelineKey& key) noexcept
{
    uint64 hash = FNV_OFFSET;
    HashValue(hash, static_cast<uint8>(key.materialVariant));
    HashValue(hash, static_cast<uint8>(key.topology));
    HashValue(hash, static_cast<uint8>(key.skinned));
    return hash;
}

uint64 GetStableHash(const GeometryBindingKey& key) noexcept
{
    uint64 hash = FNV_OFFSET;
    HashValue(hash, key.mesh.slot);
    HashValue(hash, key.mesh.generation);
    HashValue(hash, key.submeshIndex);
    HashValue(hash, static_cast<uint8>(key.indexType));
    return hash;
}

uint64 GetStableHash(const MaterialBindingKey& key) noexcept
{
    uint64 hash = FNV_OFFSET;
    HashValue(hash, key.material.slot);
    HashValue(hash, key.material.generation);
    HashValue(hash, static_cast<uint8>(key.materialMode));
    return hash;
}

size_t PipelineKeyHasher::operator()(const PipelineKey& key) const noexcept
{
    return static_cast<size_t>(GetStableHash(key));
}

size_t GeometryBindingKeyHasher::operator()(const GeometryBindingKey& key) const noexcept
{
    return static_cast<size_t>(GetStableHash(key));
}

size_t MaterialBindingKeyHasher::operator()(const MaterialBindingKey& key) const noexcept
{
    return static_cast<size_t>(GetStableHash(key));
}

MeshBatchBuildResult BuildMeshBatches(const MeshBatchBuildInput& input)
{
    MeshBatchBuildResult result;
    if (input.objectId == 0)
    {
        result.code = MeshBatchBuildCode::InvalidObject;
        return result;
    }
    if (!input.mesh.IsValid())
    {
        result.code = MeshBatchBuildCode::InvalidMesh;
        return result;
    }
    if (input.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX)
    {
        result.code = MeshBatchBuildCode::InvalidPrimitiveData;
        return result;
    }
    const bool finiteBounds = std::isfinite(input.boundsMin.x) &&
                              std::isfinite(input.boundsMin.y) &&
                              std::isfinite(input.boundsMin.z) &&
                              std::isfinite(input.boundsMax.x) &&
                              std::isfinite(input.boundsMax.y) &&
                              std::isfinite(input.boundsMax.z);
    if (!finiteBounds || input.boundsMin.x > input.boundsMax.x ||
        input.boundsMin.y > input.boundsMax.y ||
        input.boundsMin.z > input.boundsMax.z)
    {
        result.code = MeshBatchBuildCode::MalformedBounds;
        return result;
    }
    if (input.submeshes.empty())
    {
        result.code = MeshBatchBuildCode::MissingSubmesh;
        return result;
    }

    result.batches.reserve(input.submeshes.size());
    for (size_t index = 0; index < input.submeshes.size(); ++index)
    {
        const MeshBatchSourceSubmesh& source = input.submeshes[index];
        const bool validMode =
            source.materialMode == RenderMaterialMode::Opaque ||
            source.materialMode == RenderMaterialMode::Masked ||
            source.materialMode == RenderMaterialMode::Transparent;
        if (source.submeshIndex != index || source.geometry.indexCount == 0 ||
            source.geometry.indexOffset >
                std::numeric_limits<uint32>::max() - source.geometry.indexCount ||
            !validMode)
        {
            result.batches.clear();
            result.code = MeshBatchBuildCode::InvalidSubmesh;
            return result;
        }

        constexpr uint32 DERIVED_FLAGS =
            static_cast<uint32>(RenderBatchFlags::Masked) |
            static_cast<uint32>(RenderBatchFlags::Transparent) |
            static_cast<uint32>(RenderBatchFlags::MissingMaterial);
        RenderBatchFlags flags = static_cast<RenderBatchFlags>(
            static_cast<uint32>(input.flags) & ~DERIVED_FLAGS);
        if (source.materialMode == RenderMaterialMode::Masked)
        {
            flags |= RenderBatchFlags::Masked;
        }
        if (source.materialMode == RenderMaterialMode::Transparent)
        {
            flags |= RenderBatchFlags::Transparent;
        }
        if (!source.material.IsValid())
        {
            flags |= RenderBatchFlags::MissingMaterial;
        }
        result.batches.push_back(MeshBatch{
            input.objectId,
            input.mesh,
            source.material,
            source.submeshIndex,
            input.primitiveData,
            input.indexType,
            source.geometry,
            source.materialMode,
            flags});
        result.batches.back().objectRevision = input.objectRevision;
    }
    result.code = MeshBatchBuildCode::Success;
    return result;
}

RenderDrawPacket BuildLegacyMaterialDrawPacket(const MeshBatch& batch) noexcept
{
    RenderDrawPacket packet;
    packet.objectId = batch.objectId;
    packet.primitiveData = batch.primitiveData;
    packet.submeshIndex = batch.submeshIndex;
    packet.pass = ToMaterialPass(batch.materialMode);
    packet.pipelineKey.materialVariant = ToPipelineVariant(batch.materialMode);
    packet.pipelineKey.topology = batch.geometry.topology;
    packet.pipelineKey.skinned = HasRenderBatchFlag(
        batch.flags, RenderBatchFlags::Skinned);
    packet.geometryKey.mesh = batch.mesh;
    packet.geometryKey.submeshIndex = batch.submeshIndex;
    packet.geometryKey.indexType = batch.indexType;
    packet.materialKey.material = batch.material;
    packet.materialKey.materialMode = batch.materialMode;
    packet.arguments.indexCount = batch.geometry.indexCount;
    packet.arguments.firstIndex = batch.geometry.indexOffset;
    packet.arguments.vertexOffset = batch.geometry.baseVertex;
    packet.flags = ToDrawFlags(batch.flags);
    return packet;
}
} // namespace RVX
