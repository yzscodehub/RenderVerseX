#include "Resource/RenderUploadRequestBuilder.h"

#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace RVX::Resource
{
namespace
{
    struct TextureFormatLayout
    {
        uint32 blockWidth = 1;
        uint32 blockHeight = 1;
        uint32 bytesPerBlock = 0;
    };

    bool CheckedAdd(uint64 left, uint64 right, uint64& result)
    {
        if (left > std::numeric_limits<uint64>::max() - right)
            return false;
        result = left + right;
        return true;
    }

    bool CheckedMultiply(uint64 left, uint64 right, uint64& result)
    {
        if (left != 0 && right > std::numeric_limits<uint64>::max() / left)
            return false;
        result = left * right;
        return true;
    }

    bool AppendRange(std::vector<uint8>& destination,
                     const void* source,
                     size_t size,
                     size_t stride,
                     UploadByteRange& range)
    {
        if (source == nullptr || size == 0)
        {
            range = {};
            return true;
        }
        if (stride == 0 || size > std::numeric_limits<uint64>::max() ||
            destination.size() > std::numeric_limits<uint64>::max() - size)
        {
            return false;
        }

        range.offset = static_cast<uint64>(destination.size());
        range.size = static_cast<uint64>(size);
        if (stride > std::numeric_limits<uint32>::max())
            return false;
        range.stride = static_cast<uint32>(stride);
        const auto* bytes = static_cast<const uint8*>(source);
        destination.insert(destination.end(), bytes, bytes + size);
        return true;
    }

    MeshUploadIndexType ConvertIndexType(RenderMeshIndexType type)
    {
        switch (type)
        {
            case RenderMeshIndexType::UInt8: return MeshUploadIndexType::UInt8;
            case RenderMeshIndexType::UInt16: return MeshUploadIndexType::UInt16;
            case RenderMeshIndexType::UInt32: return MeshUploadIndexType::UInt32;
        }
        return MeshUploadIndexType::UInt32;
    }

    uint32 GetIndexStride(MeshUploadIndexType type)
    {
        switch (type)
        {
            case MeshUploadIndexType::UInt8: return 1;
            case MeshUploadIndexType::UInt16: return 2;
            case MeshUploadIndexType::UInt32: return 4;
        }
        return 0;
    }

    MeshUploadPrimitiveTopology ConvertTopology(RenderPrimitiveTopology topology)
    {
        switch (topology)
        {
            case RenderPrimitiveTopology::TriangleStrip:
                return MeshUploadPrimitiveTopology::TriangleStrip;
            case RenderPrimitiveTopology::TriangleFan:
                return MeshUploadPrimitiveTopology::TriangleFan;
            case RenderPrimitiveTopology::Lines:
                return MeshUploadPrimitiveTopology::Lines;
            case RenderPrimitiveTopology::LineStrip:
                return MeshUploadPrimitiveTopology::LineStrip;
            case RenderPrimitiveTopology::LineLoop:
                return MeshUploadPrimitiveTopology::LineLoop;
            case RenderPrimitiveTopology::Points:
                return MeshUploadPrimitiveTopology::Points;
            case RenderPrimitiveTopology::Triangles:
            default:
                return MeshUploadPrimitiveTopology::Triangles;
        }
    }

    TextureUploadFormat ConvertTextureFormat(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::RGBA8: return TextureUploadFormat::RGBA8;
            case TextureFormat::RGBA16F: return TextureUploadFormat::RGBA16F;
            case TextureFormat::RGBA32F: return TextureUploadFormat::RGBA32F;
            case TextureFormat::RGB8: return TextureUploadFormat::RGB8;
            case TextureFormat::RG8: return TextureUploadFormat::RG8;
            case TextureFormat::R8: return TextureUploadFormat::R8;
            case TextureFormat::BC1: return TextureUploadFormat::BC1;
            case TextureFormat::BC3: return TextureUploadFormat::BC3;
            case TextureFormat::BC5: return TextureUploadFormat::BC5;
            case TextureFormat::BC7: return TextureUploadFormat::BC7;
            case TextureFormat::Unknown: return TextureUploadFormat::Unknown;
        }
        return TextureUploadFormat::Unknown;
    }

    TextureFormatLayout GetTextureLayout(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::RGBA8: return {1, 1, 4};
            case TextureFormat::RGBA16F: return {1, 1, 8};
            case TextureFormat::RGBA32F: return {1, 1, 16};
            case TextureFormat::RGB8: return {1, 1, 3};
            case TextureFormat::RG8: return {1, 1, 2};
            case TextureFormat::R8: return {1, 1, 1};
            case TextureFormat::BC1: return {4, 4, 8};
            case TextureFormat::BC3:
            case TextureFormat::BC5:
            case TextureFormat::BC7: return {4, 4, 16};
            case TextureFormat::Unknown: return {};
        }
        return {};
    }

    MaterialUploadWrapMode ConvertWrap(RenderTextureWrapMode mode)
    {
        switch (mode)
        {
            case RenderTextureWrapMode::MirrorRepeat:
                return MaterialUploadWrapMode::MirrorRepeat;
            case RenderTextureWrapMode::ClampToEdge:
                return MaterialUploadWrapMode::ClampToEdge;
            case RenderTextureWrapMode::ClampToBorder:
                return MaterialUploadWrapMode::ClampToBorder;
            case RenderTextureWrapMode::Repeat:
            default:
                return MaterialUploadWrapMode::Repeat;
        }
    }

    MaterialUploadFilterMode ConvertFilter(RenderTextureFilterMode mode)
    {
        switch (mode)
        {
            case RenderTextureFilterMode::Nearest:
                return MaterialUploadFilterMode::Nearest;
            case RenderTextureFilterMode::NearestMipmapNearest:
                return MaterialUploadFilterMode::NearestMipmapNearest;
            case RenderTextureFilterMode::LinearMipmapNearest:
                return MaterialUploadFilterMode::LinearMipmapNearest;
            case RenderTextureFilterMode::NearestMipmapLinear:
                return MaterialUploadFilterMode::NearestMipmapLinear;
            case RenderTextureFilterMode::LinearMipmapLinear:
                return MaterialUploadFilterMode::LinearMipmapLinear;
            case RenderTextureFilterMode::Linear:
            default:
                return MaterialUploadFilterMode::Linear;
        }
    }

    RenderUploadRequestBuildResult FinishRequest(
        ResourceUploadRequestCreateInfo info)
    {
        ResourceUploadRequestCreateResult created =
            ResourceUploadRequest::Create(std::move(info));
        if (created.code != ResourceUploadRequestCreateCode::Created)
        {
            return {RenderUploadRequestBuildCode::RequestRejected,
                    created.code,
                    {}};
        }
        return {RenderUploadRequestBuildCode::Built,
                created.code,
                std::move(created.request)};
    }

    RenderUploadRequestBuildResult BuildMesh(
        const MeshResource& resource,
        RenderResourceHandle handle,
        uint64 sequence,
        RenderUploadPriority priority,
        uint64 sourceRevision)
    {
        const RenderMeshUploadData source = resource.GetUploadData();
        const AABB bounds = resource.GetBounds();
        if (source.vertexCount == 0 || !source.position.IsValid() ||
            !bounds.IsValid())
        {
            return {RenderUploadRequestBuildCode::InvalidResource};
        }

        MeshUploadPayload payload;
        payload.createInfo.vertexCount = static_cast<uint64>(source.vertexCount);
        payload.createInfo.indexCount = static_cast<uint64>(source.indexCount);
        payload.createInfo.indexType = ConvertIndexType(source.indexType);
        payload.createInfo.topology = ConvertTopology(source.primitive);
        payload.createInfo.boundsMin = bounds.GetMin();
        payload.createInfo.boundsMax = bounds.GetMax();

        if ((source.indexCount != 0 &&
             !AppendRange(payload.bytes,
                          source.indexData,
                          source.indexDataSize,
                          GetIndexStride(payload.createInfo.indexType),
                          payload.indexRange)) ||
            !AppendRange(payload.bytes,
                         source.position.data,
                         source.position.size,
                         source.position.stride,
                         payload.positionRange) ||
            !AppendRange(payload.bytes,
                         source.normal.data,
                         source.normal.size,
                         source.normal.stride,
                         payload.normalRange) ||
            !AppendRange(payload.bytes,
                         source.uv.data,
                         source.uv.size,
                         source.uv.stride,
                         payload.uvRange) ||
            !AppendRange(payload.bytes,
                         source.tangent.data,
                         source.tangent.size,
                         source.tangent.stride,
                         payload.tangentRange) ||
            !AppendRange(payload.bytes,
                         source.boneIndices.data,
                         source.boneIndices.size,
                         source.boneIndices.stride,
                         payload.boneIndexRange) ||
            !AppendRange(payload.bytes,
                         source.boneWeights.data,
                         source.boneWeights.size,
                         source.boneWeights.stride,
                         payload.boneWeightRange))
        {
            return {RenderUploadRequestBuildCode::PayloadOverflow};
        }

        if (source.indexCount != 0)
        {
            payload.submeshes.reserve(source.submeshes.size());
            for (const RenderMeshSubmeshUploadInfo& submesh : source.submeshes)
            {
                payload.submeshes.push_back(MeshUploadSubmesh{
                    submesh.indexOffset,
                    submesh.indexCount,
                    submesh.baseVertex,
                    ConvertTopology(submesh.primitive)});
            }
        }

        uint64 submeshBytes = 0;
        uint64 declaredBytes = 0;
        if (!CheckedMultiply(static_cast<uint64>(payload.submeshes.size()),
                             sizeof(MeshUploadSubmesh),
                             submeshBytes) ||
            !CheckedAdd(static_cast<uint64>(payload.bytes.size()),
                        submeshBytes,
                        declaredBytes))
        {
            return {RenderUploadRequestBuildCode::PayloadOverflow};
        }

        ResourceUploadRequestCreateInfo info;
        info.sequence = sequence;
        info.assetId = AssetId{resource.GetId()};
        info.handle = handle;
        info.kind = RenderResourceKind::Mesh;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = declaredBytes;
        info.priority = priority;
        info.provenance.sourceRevision = sourceRevision;
        info.provenance.sourceKind = static_cast<uint32>(resource.GetType());
        return FinishRequest(std::move(info));
    }

    RenderUploadRequestBuildResult BuildTexture(
        const TextureResource& resource,
        RenderResourceHandle handle,
        uint64 sequence,
        RenderUploadPriority priority,
        uint64 sourceRevision)
    {
        const TextureMetadata& metadata = resource.GetMetadata();
        const TextureFormatLayout layout = GetTextureLayout(metadata.format);
        if (metadata.width == 0 || metadata.height == 0 ||
            metadata.depth == 0 || metadata.mipLevels == 0 ||
            metadata.arrayLayers == 0 || layout.bytesPerBlock == 0 ||
            resource.GetData().empty())
        {
            return {RenderUploadRequestBuildCode::InvalidResource};
        }

        TextureUploadPayload payload;
        payload.createInfo.width = metadata.width;
        payload.createInfo.height = metadata.height;
        payload.createInfo.depth = metadata.depth;
        payload.createInfo.mipLevels = metadata.mipLevels;
        payload.createInfo.arrayLayers = metadata.arrayLayers;
        payload.createInfo.format = ConvertTextureFormat(metadata.format);
        payload.createInfo.isCubemap = metadata.isCubemap;
        payload.createInfo.isArray = metadata.isArray;
        payload.createInfo.isSRGB = metadata.isSRGB;
        payload.bytes = resource.GetData();

        uint64 offset = 0;
        for (uint32 mip = 0; mip < metadata.mipLevels; ++mip)
        {
            const uint32 width =
                mip >= 32U ? 1U : std::max(1U, metadata.width >> mip);
            const uint32 height =
                mip >= 32U ? 1U : std::max(1U, metadata.height >> mip);
            const uint32 depth =
                mip >= 32U ? 1U : std::max(1U, metadata.depth >> mip);
            const uint64 blocksWide =
                (static_cast<uint64>(width) + layout.blockWidth - 1U) /
                layout.blockWidth;
            const uint64 blocksHigh =
                (static_cast<uint64>(height) + layout.blockHeight - 1U) /
                layout.blockHeight;
            uint64 rowPitch = 0;
            uint64 slicePitch = 0;
            uint64 subresourceBytes = 0;
            if (!CheckedMultiply(blocksWide,
                                 layout.bytesPerBlock,
                                 rowPitch) ||
                !CheckedMultiply(rowPitch, blocksHigh, slicePitch) ||
                !CheckedMultiply(slicePitch, depth, subresourceBytes))
            {
                return {RenderUploadRequestBuildCode::PayloadOverflow};
            }

            for (uint32 layer = 0; layer < metadata.arrayLayers; ++layer)
            {
                uint64 end = 0;
                if (!CheckedAdd(offset, subresourceBytes, end) ||
                    end > payload.bytes.size())
                {
                    return {RenderUploadRequestBuildCode::InvalidResource};
                }
                payload.subresources.push_back(TextureUploadSubresource{
                    UploadByteRange{offset, subresourceBytes, 0},
                    mip,
                    layer,
                    rowPitch,
                    slicePitch});
                offset = end;
            }
        }
        if (offset != payload.bytes.size())
        {
            return {RenderUploadRequestBuildCode::InvalidResource};
        }

        uint64 subresourceTableBytes = 0;
        uint64 declaredBytes = 0;
        if (!CheckedMultiply(
                static_cast<uint64>(payload.subresources.size()),
                sizeof(TextureUploadSubresource),
                subresourceTableBytes) ||
            !CheckedAdd(static_cast<uint64>(payload.bytes.size()),
                        subresourceTableBytes,
                        declaredBytes))
        {
            return {RenderUploadRequestBuildCode::PayloadOverflow};
        }

        ResourceUploadRequestCreateInfo info;
        info.sequence = sequence;
        info.assetId = AssetId{resource.GetId()};
        info.handle = handle;
        info.kind = RenderResourceKind::Texture;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = declaredBytes;
        info.priority = priority;
        info.provenance.sourceRevision = sourceRevision;
        info.provenance.sourceKind = static_cast<uint32>(resource.GetType());
        return FinishRequest(std::move(info));
    }

    RenderUploadRequestBuildResult BuildMaterial(
        const MaterialResource& resource,
        RenderResourceHandle handle,
        uint64 sequence,
        const RenderResourceDependencyResolver& dependencyResolver,
        RenderUploadPriority priority,
        uint64 sourceRevision)
    {
        MaterialUploadPayload payload;
        payload.sourceData = resource.GetRenderMaterialSourceData();

        struct SlotPair
        {
            RenderMaterialTextureSlot source;
            MaterialUploadTextureSlot destination;
        };
        constexpr SlotPair slots[] = {
            {RenderMaterialTextureSlot::BaseColor,
             MaterialUploadTextureSlot::BaseColor},
            {RenderMaterialTextureSlot::Normal,
             MaterialUploadTextureSlot::Normal},
            {RenderMaterialTextureSlot::MetallicRoughness,
             MaterialUploadTextureSlot::MetallicRoughness},
            {RenderMaterialTextureSlot::Occlusion,
             MaterialUploadTextureSlot::Occlusion},
            {RenderMaterialTextureSlot::Emissive,
             MaterialUploadTextureSlot::Emissive}};

        std::vector<RenderResourceHandle> dependencies;
        for (const SlotPair& slot : slots)
        {
            const RenderMaterialTextureBinding source =
                resource.GetRenderMaterialTextureBinding(slot.source);
            if (!source.IsValid())
                continue;
            if (!dependencyResolver)
            {
                return {RenderUploadRequestBuildCode::DependencyUnavailable};
            }
            const uint64 textureId =
                source.textureId != 0
                    ? source.textureId
                    : source.texture->GetRenderResourceId();
            const RenderResourceHandle texture = dependencyResolver(
                AssetId{textureId}, RenderResourceKind::Texture);
            if (!texture.IsValid())
            {
                return {RenderUploadRequestBuildCode::DependencyUnavailable};
            }

            MaterialUploadTextureBinding binding;
            binding.slot = slot.destination;
            binding.texture = texture;
            binding.uvSet = source.uvSet;
            binding.offset = source.offset;
            binding.scale = source.scale;
            binding.rotation = source.rotation;
            binding.wrapS = ConvertWrap(source.wrapS);
            binding.wrapT = ConvertWrap(source.wrapT);
            binding.minFilter = ConvertFilter(source.minFilter);
            binding.magFilter = ConvertFilter(source.magFilter);
            payload.textureBindings.push_back(binding);
            payload.sourceData.textureFlags |=
                1U << static_cast<uint32>(slot.destination);
            if (std::find(dependencies.begin(), dependencies.end(), texture) ==
                dependencies.end())
            {
                dependencies.push_back(texture);
            }
        }

        uint64 bindingBytes = 0;
        uint64 dependencyBytes = 0;
        uint64 declaredBytes = 0;
        if (!CheckedMultiply(
                static_cast<uint64>(payload.textureBindings.size()),
                sizeof(MaterialUploadTextureBinding),
                bindingBytes) ||
            !CheckedMultiply(static_cast<uint64>(dependencies.size()),
                             sizeof(RenderResourceHandle),
                             dependencyBytes) ||
            !CheckedAdd(bindingBytes, dependencyBytes, declaredBytes))
        {
            return {RenderUploadRequestBuildCode::PayloadOverflow};
        }

        ResourceUploadRequestCreateInfo info;
        info.sequence = sequence;
        info.assetId = AssetId{resource.GetId()};
        info.handle = handle;
        info.kind = RenderResourceKind::Material;
        info.payload = std::move(payload);
        info.dependencies = std::move(dependencies);
        info.declaredPayloadBytes = declaredBytes;
        info.priority = priority;
        info.provenance.sourceRevision = sourceRevision;
        info.provenance.sourceKind = static_cast<uint32>(resource.GetType());
        return FinishRequest(std::move(info));
    }
} // namespace

    RenderUploadRequestBuildResult RenderUploadRequestBuilder::Build(
        const IResource& resource,
        RenderResourceHandle handle,
        uint64 sequence,
        const RenderResourceDependencyResolver& dependencyResolver,
        RenderUploadPriority priority,
        uint64 sourceRevision)
    {
        if (resource.GetId() == InvalidResourceId || !handle.IsValid() ||
            sequence == 0)
        {
            return {RenderUploadRequestBuildCode::InvalidResource};
        }

        switch (resource.GetType())
        {
            case ResourceType::Mesh:
            {
                const auto* mesh = dynamic_cast<const MeshResource*>(&resource);
                return mesh != nullptr
                           ? BuildMesh(*mesh,
                                       handle,
                                       sequence,
                                       priority,
                                       sourceRevision)
                           : RenderUploadRequestBuildResult{
                                 RenderUploadRequestBuildCode::InvalidResource};
            }
            case ResourceType::Texture:
            {
                const auto* texture =
                    dynamic_cast<const TextureResource*>(&resource);
                return texture != nullptr
                           ? BuildTexture(*texture,
                                          handle,
                                          sequence,
                                          priority,
                                          sourceRevision)
                           : RenderUploadRequestBuildResult{
                                 RenderUploadRequestBuildCode::InvalidResource};
            }
            case ResourceType::Material:
            {
                const auto* material =
                    dynamic_cast<const MaterialResource*>(&resource);
                return material != nullptr
                           ? BuildMaterial(*material,
                                           handle,
                                           sequence,
                                           dependencyResolver,
                                           priority,
                                           sourceRevision)
                           : RenderUploadRequestBuildResult{
                                 RenderUploadRequestBuildCode::InvalidResource};
            }
            default:
                return {RenderUploadRequestBuildCode::UnsupportedResource};
        }
    }
} // namespace RVX::Resource
