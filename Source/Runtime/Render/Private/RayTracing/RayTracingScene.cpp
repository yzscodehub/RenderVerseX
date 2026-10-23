#include "Render/RayTracing/RayTracingScene.h"

#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Resources/RenderResourceRegistry.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace RVX
{
namespace
{
    bool ShouldIncludeRenderMode(MaterialRenderMode mode, const RayTracingSceneOptions& options)
    {
        switch (mode)
        {
            case MaterialRenderMode::Opaque:
                return options.includeOpaque;
            case MaterialRenderMode::Masked:
                return options.includeMasked;
            case MaterialRenderMode::Transparent:
                return options.includeTransparent;
            default:
                return false;
        }
    }

    RHIFormat ToRayTracingIndexFormat(MeshUploadIndexType indexType)
    {
        switch (indexType)
        {
            case MeshUploadIndexType::UInt16:
                return RHIFormat::R16_UINT;
            case MeshUploadIndexType::UInt32:
                return RHIFormat::R32_UINT;
            case MeshUploadIndexType::UInt8:
            default:
                return RHIFormat::Unknown;
        }
    }

    uint32 ResolveRayTracingInstanceMask(uint32 optionMask, uint32 objectLayerMask)
    {
        return (optionMask & objectLayerMask) & 0xFFu;
    }

    uint32 GetIndexStride(MeshUploadIndexType indexType)
    {
        switch (indexType)
        {
            case MeshUploadIndexType::UInt8:
                return 1;
            case MeshUploadIndexType::UInt16:
                return 2;
            case MeshUploadIndexType::UInt32:
            default:
                return 4;
        }
    }

    uint64 PackHandle(RenderResourceHandle handle)
    {
        return (static_cast<uint64>(handle.slot) << 32U) |
               static_cast<uint64>(handle.generation);
    }

    RHIRayTracingGeometryFlags ToGeometryFlags(MaterialRenderMode mode)
    {
        return mode == MaterialRenderMode::Opaque
                   ? RHIRayTracingGeometryFlags::Opaque
                   : RHIRayTracingGeometryFlags::None;
    }

    RHIRayTracingInstanceFlags ToInstanceFlags(const RenderDrawItem& item)
    {
        RHIRayTracingInstanceFlags flags = item.renderMode == MaterialRenderMode::Opaque
                   ? RHIRayTracingInstanceFlags::ForceOpaque
                   : RHIRayTracingInstanceFlags::ForceNoOpaque;
        if (item.packet.pipelineKey.cullMode == MaterialCullMode::NoCull)
        {
            flags = flags | RHIRayTracingInstanceFlags::TriangleCullDisable;
        }
        else if (item.packet.pipelineKey.reverseWinding)
        {
            flags = flags | RHIRayTracingInstanceFlags::TriangleFrontCounterClockwise;
        }
        return flags;
    }

    bool IsSkinnedDrawItem(const RenderObject& object, const RenderDrawItem& item)
    {
        return object.HasSkinningData() || item.packet.pipelineKey.skinned ||
               (static_cast<uint32>(item.packet.flags) &
                static_cast<uint32>(RenderDrawFlags::Skinned)) != 0u;
    }

    RHIBuffer* SelectTextureCoordinateBuffer(const MeshGPUBuffers& buffers, uint32 uvSet)
    {
        switch (uvSet)
        {
            case 0: return buffers.uvBuffer;
            case 1: return buffers.uv1Buffer;
            default: return nullptr;
        }
    }

    uint32 EncodeRayTracingSamplerFlags(
        const MaterialUploadTextureBinding& texture)
    {
        constexpr uint32 RVX_RT_ALPHA_WRAP_S_CLAMP = 1u << 0;
        constexpr uint32 RVX_RT_ALPHA_WRAP_T_CLAMP = 1u << 1;
        constexpr uint32 RVX_RT_ALPHA_MAG_NEAREST = 1u << 2;
        constexpr uint32 RVX_RT_ALPHA_WRAP_S_MIRROR = 1u << 3;
        constexpr uint32 RVX_RT_ALPHA_WRAP_T_MIRROR = 1u << 4;

        uint32 flags = 0;
        if (texture.wrapS == MaterialUploadWrapMode::ClampToEdge ||
            texture.wrapS == MaterialUploadWrapMode::ClampToBorder)
        {
            flags |= RVX_RT_ALPHA_WRAP_S_CLAMP;
        }
        else if (texture.wrapS == MaterialUploadWrapMode::MirrorRepeat)
        {
            flags |= RVX_RT_ALPHA_WRAP_S_MIRROR;
        }
        if (texture.wrapT == MaterialUploadWrapMode::ClampToEdge ||
            texture.wrapT == MaterialUploadWrapMode::ClampToBorder)
        {
            flags |= RVX_RT_ALPHA_WRAP_T_CLAMP;
        }
        else if (texture.wrapT == MaterialUploadWrapMode::MirrorRepeat)
        {
            flags |= RVX_RT_ALPHA_WRAP_T_MIRROR;
        }
        if (texture.magFilter == MaterialUploadFilterMode::Nearest)
        {
            flags |= RVX_RT_ALPHA_MAG_NEAREST;
        }
        return flags;
    }

    RayTracingTextureSamplingMetadata BuildTextureSamplingMetadata(
        const MaterialUploadTextureBinding& texture)
    {
        RayTracingTextureSamplingMetadata metadata;
        metadata.uvSet = static_cast<uint32>(std::max(texture.uvSet, 0));
        metadata.samplerFlags = EncodeRayTracingSamplerFlags(texture);
        metadata.uvOffset = texture.offset;
        metadata.uvScale = texture.scale;
        metadata.uvRotation = texture.rotation;
        return metadata;
    }

    uint32 EncodeMaterialWorkflow(MaterialSourceWorkflow workflow)
    {
        return static_cast<uint32>(workflow);
    }

    void AddMaterialFlag(RayTracingMaterialMetadataFlags& flags,
                         RayTracingMaterialMetadataFlags flag,
                         bool enabled)
    {
        if (enabled)
        {
            flags |= flag;
        }
    }

    bool IsFiniteTransform(const Mat4& matrix)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!std::isfinite(matrix[column][row]))
                {
                    return false;
                }
            }
        }

        return true;
    }

    void WriteRowMajor3x4(float transform[12], const Mat4& matrix)
    {
        transform[0] = matrix[0][0];
        transform[1] = matrix[1][0];
        transform[2] = matrix[2][0];
        transform[3] = matrix[3][0];
        transform[4] = matrix[0][1];
        transform[5] = matrix[1][1];
        transform[6] = matrix[2][1];
        transform[7] = matrix[3][1];
        transform[8] = matrix[0][2];
        transform[9] = matrix[1][2];
        transform[10] = matrix[2][2];
        transform[11] = matrix[3][2];
    }

    void AddSkip(RayTracingSceneBuildPlan& plan,
                 RayTracingSceneSkipReason reason,
                 const RenderDrawItem& item,
                 const RenderObject* object,
                 const char* message)
    {
        RayTracingSceneSkip skip;
        skip.reason = reason;
        skip.objectIndex = item.objectIndex;
        skip.submeshIndex = item.submeshIndex;
        skip.meshId = PackHandle(object ? object->mesh : item.mesh);
        skip.message = message ? message : "";
        plan.skips.push_back(std::move(skip));
    }

    RayTracingMaterialMetadata BuildMaterialMetadata(const RenderDrawItem& item)
    {
        RayTracingMaterialMetadata metadata;
        metadata.materialId = PackHandle(item.material);

        RayTracingMaterialMetadataFlags flags = RayTracingMaterialMetadataFlags::None;
        AddMaterialFlag(flags,
                        RayTracingMaterialMetadataFlags::AlphaTest,
                        item.renderMode == MaterialRenderMode::Masked);
        AddMaterialFlag(flags,
                        RayTracingMaterialMetadataFlags::Transparent,
                        item.renderMode == MaterialRenderMode::Transparent);

        metadata.flags = static_cast<uint32>(flags);
        return metadata;
    }

    const MaterialUploadTextureBinding* FindMaterialBinding(
        const RenderMaterialResourceData& material,
        MaterialUploadTextureSlot slot)
    {
        const auto it = std::find_if(
            material.textureBindings.begin(),
            material.textureBindings.end(),
            [slot](const MaterialUploadTextureBinding& binding)
            {
                return binding.slot == slot;
            });
        return it != material.textureBindings.end() ? &*it : nullptr;
    }

    RayTracingMaterialMetadata BuildMaterialMetadata(
        const RenderDrawItem& item,
        const RenderResourceRegistry& registry)
    {
        RayTracingMaterialMetadata metadata = BuildMaterialMetadata(item);
        const RenderMaterialResourceData* material =
            registry.ResolveMaterial(item.material);
        if (material == nullptr || !material->metadataValid)
        {
            return metadata;
        }

        const MaterialSourceData& source = material->sourceData;
        metadata.materialId = PackHandle(item.material);
        metadata.baseColorFactor = source.baseColorFactor;
        metadata.emissiveColor = source.emissiveColor;
        metadata.emissiveStrength = source.emissiveStrength;
        metadata.metallicFactor = source.metallicFactor;
        metadata.roughnessFactor = source.roughnessFactor;
        metadata.alphaCutoff = source.alphaCutoff;
        metadata.normalScale = source.normalScale;
        metadata.workflow = EncodeMaterialWorkflow(source.workflow);

        RayTracingMaterialMetadataFlags flags =
            static_cast<RayTracingMaterialMetadataFlags>(metadata.flags);
        AddMaterialFlag(flags,
                        RayTracingMaterialMetadataFlags::AlphaTest,
                        source.alphaMode == MaterialSourceAlphaMode::Mask);
        AddMaterialFlag(flags,
                        RayTracingMaterialMetadataFlags::Transparent,
                        source.alphaMode == MaterialSourceAlphaMode::Blend);
        AddMaterialFlag(flags,
                        RayTracingMaterialMetadataFlags::DoubleSided,
                        source.doubleSided);
        AddMaterialFlag(flags,
                        RayTracingMaterialMetadataFlags::Unlit,
                        source.workflow == MaterialSourceWorkflow::Unlit);

        const auto applyBinding = [&](MaterialUploadTextureSlot slot,
                                      RayTracingMaterialMetadataFlags flag,
                                      uint64& textureId,
                                      RayTracingTextureSamplingMetadata& sampling)
        {
            const MaterialUploadTextureBinding* binding =
                FindMaterialBinding(*material, slot);
            if (binding == nullptr)
            {
                return;
            }
            AddMaterialFlag(flags, flag, true);
            textureId = PackHandle(binding->texture);
            sampling = BuildTextureSamplingMetadata(*binding);
        };
        applyBinding(MaterialUploadTextureSlot::BaseColor,
                     RayTracingMaterialMetadataFlags::HasBaseColorTexture,
                     metadata.baseColorTextureId,
                     metadata.baseColorTextureSampling);
        applyBinding(
            MaterialUploadTextureSlot::MetallicRoughness,
            RayTracingMaterialMetadataFlags::HasMetallicRoughnessTexture,
            metadata.metallicRoughnessTextureId,
            metadata.metallicRoughnessTextureSampling);
        applyBinding(MaterialUploadTextureSlot::Normal,
                     RayTracingMaterialMetadataFlags::HasNormalTexture,
                     metadata.normalTextureId,
                     metadata.normalTextureSampling);
        applyBinding(MaterialUploadTextureSlot::Emissive,
                     RayTracingMaterialMetadataFlags::HasEmissiveTexture,
                     metadata.emissiveTextureId,
                     metadata.emissiveTextureSampling);
        metadata.flags = static_cast<uint32>(flags);
        return metadata;
    }

    RayTracingAlphaTestMetadata BuildAlphaTestMetadata(
        const RenderDrawItem& item,
        const MeshGPUBuffers& buffers,
        const SubmeshGPUInfo& submesh,
        RHIFormat indexFormat,
        const RenderResourceRegistry& registry)
    {
        RayTracingAlphaTestMetadata metadata;
        metadata.enabled = item.renderMode == MaterialRenderMode::Masked;
        if (!metadata.enabled)
        {
            return metadata;
        }

        metadata.status = RayTracingMaskedMetadataStatus::MissingMaterial;
        metadata.hasIndexBuffer = buffers.indexBuffer != nullptr;
        metadata.indexBuffer = metadata.hasIndexBuffer ? buffers.indexBuffer : nullptr;
        metadata.hasNormalBuffer = buffers.hasNormals && buffers.normalBuffer != nullptr;
        metadata.normalBuffer = metadata.hasNormalBuffer ? buffers.normalBuffer : nullptr;
        metadata.hasTangentBuffer = buffers.hasTangents && buffers.tangentBuffer != nullptr;
        metadata.tangentBuffer = metadata.hasTangentBuffer ? buffers.tangentBuffer : nullptr;
        metadata.indexElementOffset = submesh.indexOffset;
        metadata.baseVertex = static_cast<uint32>(submesh.baseVertex);
        metadata.indexFormat = indexFormat;

        if (!metadata.hasIndexBuffer)
        {
            metadata.status = RayTracingMaskedMetadataStatus::MissingIndexBuffer;
            return metadata;
        }

        const RenderMaterialResourceData* material =
            registry.ResolveMaterial(item.material);
        if (material == nullptr || !material->metadataValid)
        {
            return metadata;
        }

        const MaterialSourceData& source = material->sourceData;
        if (source.alphaMode != MaterialSourceAlphaMode::Mask)
        {
            metadata.status = RayTracingMaskedMetadataStatus::InvalidMaterial;
            return metadata;
        }
        metadata.alphaCutoff = source.alphaCutoff;
        metadata.baseColorAlpha = source.baseColorFactor.a;
        const MaterialUploadTextureBinding* binding = FindMaterialBinding(
            *material, MaterialUploadTextureSlot::BaseColor);
        if (binding == nullptr)
        {
            metadata.status = RayTracingMaskedMetadataStatus::Complete;
            return metadata;
        }
        metadata.hasBaseColorTexture = true;
        metadata.hasResolvedBaseColorTexture =
            registry.IsGPUReadyExact(binding->texture);
        if (!metadata.hasResolvedBaseColorTexture)
        {
            metadata.status = RayTracingMaskedMetadataStatus::TextureNotReady;
            return metadata;
        }
        metadata.baseColorTextureId = PackHandle(binding->texture);
        metadata.baseColorUVSet = static_cast<uint32>(
            std::max(binding->uvSet, 0));
        if (metadata.baseColorUVSet > 1u)
        {
            metadata.status = RayTracingMaskedMetadataStatus::UnsupportedUVSet;
            return metadata;
        }
        if (!buffers.HasTextureCoordinateSet(metadata.baseColorUVSet))
        {
            metadata.status = RayTracingMaskedMetadataStatus::MissingUVSet;
            return metadata;
        }
        metadata.uvBuffer = SelectTextureCoordinateBuffer(
            buffers, metadata.baseColorUVSet);
        metadata.hasUVBuffer = metadata.uvBuffer != nullptr;
        if (!metadata.hasUVBuffer)
        {
            metadata.status = RayTracingMaskedMetadataStatus::MissingUVSet;
            return metadata;
        }
        metadata.baseColorSamplerFlags =
            EncodeRayTracingSamplerFlags(*binding);
        metadata.baseColorUVOffset = binding->offset;
        metadata.baseColorUVScale = binding->scale;
        metadata.baseColorUVRotation = binding->rotation;
        metadata.status = RayTracingMaskedMetadataStatus::Complete;
        return metadata;
    }

    uint32 FindOrAddBLASBuild(RayTracingSceneBuildPlan& plan,
                              const RayTracingBLASKey& key,
                              const RHIRayTracingGeometryDesc& geometry,
                              const RayTracingSceneOptions& options)
    {
        const auto it = std::find_if(
            plan.blasBuilds.begin(),
            plan.blasBuilds.end(),
            [&key](const RayTracingBLASBuild& build)
            {
                return build.key == key;
            });

        if (it != plan.blasBuilds.end())
        {
            return static_cast<uint32>(std::distance(plan.blasBuilds.begin(), it));
        }

        RayTracingBLASBuild build;
        build.key = key;
        build.desc.geometries.push_back(geometry);
        build.desc.buildFlags = options.blasBuildFlags;
        build.desc.debugName = "RayTracingSceneBLAS";
        plan.blasBuilds.push_back(std::move(build));
        return static_cast<uint32>(plan.blasBuilds.size() - 1);
    }
} // namespace

RayTracingSceneBuildPlan BuildRayTracingSceneBuildPlan(
    const RenderScene& scene,
    std::span<const uint32_t> candidateObjectIndices,
    const RenderResourceRegistry& registry,
    const RayTracingSceneOptions& options)
{
    RayTracingSceneBuildPlan plan;
    plan.tlasBuildFlags = options.tlasBuildFlags;
    plan.stats.candidateObjectCount = candidateObjectIndices.size();

    std::vector<RenderDrawItem> opaqueDrawItems;
    std::vector<RenderDrawItem> maskedDrawItems;
    std::vector<RenderDrawItem> transparentDrawItems;
    const std::vector<uint32_t> candidateIndices(
        candidateObjectIndices.begin(), candidateObjectIndices.end());
    BuildMaterialDrawLists(scene,
                           candidateIndices,
                           Vec3(0.0f),
                           opaqueDrawItems,
                           maskedDrawItems,
                           transparentDrawItems);

    std::vector<RenderDrawItem> drawItems;
    if (options.includeOpaque)
    {
        drawItems.insert(drawItems.end(),
                         opaqueDrawItems.begin(),
                         opaqueDrawItems.end());
    }
    if (options.includeMasked)
    {
        drawItems.insert(drawItems.end(),
                         maskedDrawItems.begin(),
                         maskedDrawItems.end());
    }
    if (options.includeTransparent)
    {
        drawItems.insert(drawItems.end(),
                         transparentDrawItems.begin(),
                         transparentDrawItems.end());
    }
    else
    {
        for (const RenderDrawItem& item : transparentDrawItems)
        {
            const RenderObject* object =
                item.objectIndex < scene.GetObjectCount()
                    ? &scene.GetObject(item.objectIndex)
                    : nullptr;
            AddSkip(plan,
                    RayTracingSceneSkipReason::TransparentMaterial,
                    item,
                    object,
                    "transparent material excluded");
        }
    }

    // TLAS instance IDs must remain deterministic across the opaque/masked
    // classification lists.  Rendering order is irrelevant for AS contents.
    std::stable_sort(drawItems.begin(), drawItems.end(), [](const RenderDrawItem& lhs,
                                                            const RenderDrawItem& rhs)
    {
        if (lhs.objectIndex != rhs.objectIndex)
            return lhs.objectIndex < rhs.objectIndex;
        if (lhs.submeshIndex != rhs.submeshIndex)
            return lhs.submeshIndex < rhs.submeshIndex;
        if (lhs.mesh != rhs.mesh)
            return PackHandle(lhs.mesh) < PackHandle(rhs.mesh);
        if (lhs.material != rhs.material)
            return PackHandle(lhs.material) < PackHandle(rhs.material);
        if (lhs.renderMode != rhs.renderMode)
            return static_cast<uint32>(lhs.renderMode) < static_cast<uint32>(rhs.renderMode);
        return lhs.sortKey < rhs.sortKey;
    });

    plan.stats.drawItemCount = drawItems.size();
    for (const RenderDrawItem& item : drawItems)
    {
        if (!ShouldIncludeRenderMode(item.renderMode, options) ||
            item.objectIndex >= scene.GetObjectCount())
        {
            continue;
        }

        const RenderObject& object = scene.GetObject(item.objectIndex);
        if (!item.packet.pipelineKey.materialPolicyValid)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::InvalidMaterialPolicy,
                    item,
                    &object,
                    "ray tracing requires a resolved material policy");
            continue;
        }
        if (IsSkinnedDrawItem(object, item))
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::SkinnedGeometryUnsupported,
                    item,
                    &object,
                    "skinned ray tracing geometry is unsupported");
            continue;
        }
        if (options.requireShadowCasting && !object.castsShadow)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::ShadowCastingDisabled,
                    item,
                    &object,
                    "shadow casting disabled");
            continue;
        }
        const uint32 instanceMask = ResolveRayTracingInstanceMask(
            options.instanceMask, object.layerMask);
        if (instanceMask == 0)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::InstanceMaskZero,
                    item,
                    &object,
                    "ray tracing instance mask is zero");
            continue;
        }
        if (!IsFiniteTransform(object.worldMatrix))
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::InvalidTransform,
                    item,
                    &object,
                    "ray tracing instance transform is not finite");
            continue;
        }

        const RenderMeshResourceData* mesh = registry.ResolveMesh(item.mesh);
        if (mesh == nullptr || mesh->createInfo.vertexCount == 0 ||
            mesh->createInfo.indexCount == 0)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::MissingMeshResource,
                    item,
                    &object,
                    "missing exact mesh metadata");
            continue;
        }
        const MeshUploadPrimitiveTopology topology =
            item.submeshIndex < mesh->submeshes.size()
                ? mesh->submeshes[item.submeshIndex].topology
                : mesh->createInfo.topology;
        if (topology != MeshUploadPrimitiveTopology::Triangles)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::UnsupportedPrimitive,
                    item,
                    &object,
                    "only triangle geometry is supported");
            continue;
        }

        const MeshGPUBuffers buffers = registry.ResolveMeshBuffers(item.mesh);
        if (!buffers.IsValid())
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::MeshNotGPUReady,
                    item,
                    &object,
                    "exact mesh GPU buffers are not ready");
            continue;
        }
        if (item.submeshIndex >= buffers.submeshes.size())
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::InvalidSubmesh,
                    item,
                    &object,
                    "submesh index is outside exact GPU metadata");
            continue;
        }

        const RHIFormat indexFormat =
            ToRayTracingIndexFormat(mesh->createInfo.indexType);
        if (!IsRHIIndexFormatSupportedForRayTracing(indexFormat))
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::UnsupportedIndexFormat,
                    item,
                    &object,
                    "mesh index format is unsupported for ray tracing");
            continue;
        }
        const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
        if (submesh.indexCount == 0)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::InvalidSubmesh,
                    item,
                    &object,
                    "submesh has no indices");
            continue;
        }
        if (submesh.baseVertex < 0)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::UnsupportedBaseVertex,
                    item,
                    &object,
                    "negative baseVertex cannot be represented in AS geometry");
            continue;
        }
        const uint32 baseVertex = static_cast<uint32>(submesh.baseVertex);
        if (baseVertex >= mesh->createInfo.vertexCount)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::InvalidSubmesh,
                    item,
                    &object,
                    "baseVertex is outside the vertex buffer");
            continue;
        }

        const uint32 vertexStride = buffers.positionBuffer->GetStride() != 0
            ? buffers.positionBuffer->GetStride()
            : static_cast<uint32>(sizeof(Vec3));
        RHIRayTracingGeometryDesc geometry;
        geometry.type = RHIRayTracingGeometryType::Triangles;
        geometry.flags = ToGeometryFlags(item.renderMode);
        geometry.triangles.vertexBuffer = buffers.positionBuffer;
        geometry.triangles.vertexOffset =
            static_cast<uint64>(baseVertex) * vertexStride;
        geometry.triangles.vertexStride = vertexStride;
        geometry.triangles.vertexFormat = RHIFormat::RGB32_FLOAT;
        geometry.triangles.vertexCount = static_cast<uint32>(
            mesh->createInfo.vertexCount - baseVertex);
        geometry.triangles.indexBuffer = buffers.indexBuffer;
        geometry.triangles.indexOffset =
            static_cast<uint64>(submesh.indexOffset) *
            GetIndexStride(mesh->createInfo.indexType);
        geometry.triangles.indexFormat = indexFormat;
        geometry.triangles.indexCount = submesh.indexCount;

        const RHIRayTracingValidationResult validation =
            ValidateRHIRayTracingGeometryDesc(geometry);
        if (!validation)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::InvalidGeometry,
                    item,
                    &object,
                    validation.message);
            continue;
        }

        RayTracingAlphaTestMetadata alphaTest = BuildAlphaTestMetadata(
            item, buffers, submesh, indexFormat, registry);
        if (alphaTest.enabled &&
            alphaTest.status != RayTracingMaskedMetadataStatus::Complete)
        {
            plan.maskedMetadataComplete = false;
            AddSkip(plan,
                    RayTracingSceneSkipReason::IncompleteMaskedMetadata,
                    item,
                    &object,
                    "masked ray tracing metadata is incomplete");
            continue;
        }

        RayTracingBLASKey key;
        key.mesh = item.mesh;
        key.meshId = PackHandle(item.mesh);
        key.meshContentRevision = registry.GetContentRevision(item.mesh);
        if (key.meshContentRevision == 0)
        {
            AddSkip(plan,
                    RayTracingSceneSkipReason::MissingMeshResource,
                    item,
                    &object,
                    "exact mesh content revision is unavailable");
            continue;
        }
        key.submeshIndex = item.submeshIndex;
        key.renderMode = item.renderMode;
        const uint32 blasIndex =
            FindOrAddBLASBuild(plan, key, geometry, options);
        ++plan.blasBuilds[blasIndex].instanceCount;

        RayTracingTLASInstance instance;
        instance.blasIndex = blasIndex;
        instance.objectIndex = item.objectIndex;
        instance.submeshIndex = item.submeshIndex;
        instance.entityId = object.entityId;
        instance.materialId = PackHandle(item.material);
        instance.materialContentRevision = registry.GetContentRevision(item.material);
        instance.renderMode = item.renderMode;
        instance.material = BuildMaterialMetadata(item, registry);
        instance.geometryClassification = RayTracingGeometryClassification::Rigid;
        if (object.castsShadow)
        {
            instance.material.flags |= static_cast<uint32>(
                RayTracingMaterialMetadataFlags::ShadowCaster);
        }
        instance.alphaTest = std::move(alphaTest);
        WriteRowMajor3x4(instance.desc.transform, object.worldMatrix);
        instance.desc.instanceId = static_cast<uint32>(plan.instances.size());
        instance.desc.instanceMask = instanceMask;
        instance.desc.instanceContributionToHitGroupIndex = 0;
        instance.desc.flags = ToInstanceFlags(item);
        if (instance.alphaTest.enabled)
        {
            ++plan.stats.alphaTestedInstanceCount;
        }
        plan.instances.push_back(std::move(instance));
    }

    plan.stats.blasBuildCount = plan.blasBuilds.size();
    plan.stats.instanceCount = plan.instances.size();
    plan.stats.skippedCount = plan.skips.size();
    return plan;
}

RayTracingTLASStructuralSignature BuildRayTracingTLASStructuralSignature(
    const RayTracingSceneBuildPlan& plan)
{
    constexpr uint64 RVX_FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64 RVX_FNV_PRIME = 1099511628211ull;
    auto append = [&](uint64& hash, uint64 value) noexcept
    {
        for (uint32 byte = 0; byte < sizeof(value); ++byte)
        {
            hash ^= (value >> (byte * 8u)) & 0xFFu;
            hash *= RVX_FNV_PRIME;
        }
    };

    RayTracingTLASStructuralSignature signature;
    signature.instanceCount = static_cast<uint32>(plan.instances.size());
    signature.records.reserve(plan.instances.size());
    uint64 hash = RVX_FNV_OFFSET;
    append(hash, signature.instanceCount);
    for (const RayTracingTLASInstance& instance : plan.instances)
    {
        if (instance.blasIndex >= plan.blasBuilds.size())
        {
            append(hash, ~0ull);
            signature.records.push_back({});
            continue;
        }

        const RayTracingBLASKey& key = plan.blasBuilds[instance.blasIndex].key;
        RayTracingTLASStructuralRecord record;
        record.mesh = key.mesh;
        record.meshContentRevision = key.meshContentRevision;
        record.submeshIndex = key.submeshIndex;
        record.entityId = instance.entityId;
        record.materialId = instance.materialId;
        record.materialContentRevision = instance.materialContentRevision;
        record.renderMode = instance.renderMode;
        record.materialFlags = instance.material.flags;
        record.materialWorkflow = instance.material.workflow;
        record.instanceMask = instance.desc.instanceMask;
        record.instanceFlags = static_cast<uint32>(instance.desc.flags);
        record.hitGroupIndex = instance.desc.instanceContributionToHitGroupIndex;
        record.geometryClassification = instance.geometryClassification;
        record.maskedMetadataStatus = instance.alphaTest.status;
        record.baseColorUVSet = instance.alphaTest.baseColorUVSet;
        record.baseColorTextureId = instance.alphaTest.baseColorTextureId;
        signature.records.push_back(record);
        append(hash, PackHandle(key.mesh));
        append(hash, key.meshContentRevision);
        append(hash, key.submeshIndex);
        append(hash, static_cast<uint32>(key.renderMode));
        append(hash, instance.entityId);
        append(hash, instance.materialId);
        append(hash, instance.materialContentRevision);
        append(hash, static_cast<uint32>(instance.renderMode));
        append(hash, instance.material.flags);
        append(hash, instance.material.workflow);
        append(hash, static_cast<uint32>(instance.geometryClassification));
        append(hash, instance.desc.instanceMask);
        append(hash, static_cast<uint32>(instance.desc.flags));
        append(hash, instance.alphaTest.enabled ? 1u : 0u);
        append(hash, static_cast<uint32>(instance.alphaTest.status));
        append(hash, instance.alphaTest.baseColorUVSet);
        append(hash, instance.alphaTest.baseColorTextureId);
    }
    signature.hash = hash;
    return signature;
}

RHITopLevelASDesc BuildRayTracingTopLevelDesc(
    const RayTracingSceneBuildPlan& plan,
    std::span<RHIAccelerationStructure* const> blasResources)
{
    RHITopLevelASDesc desc;
    desc.buildFlags = plan.tlasBuildFlags;
    desc.debugName = "RayTracingSceneTLAS";
    desc.instances.reserve(plan.instances.size());

    for (const RayTracingTLASInstance& plannedInstance : plan.instances)
    {
        if (plannedInstance.blasIndex >= blasResources.size())
            continue;

        RHIAccelerationStructure* blas = blasResources[plannedInstance.blasIndex];
        if (!blas)
            continue;

        RHIRayTracingInstanceDesc instance = plannedInstance.desc;
        instance.bottomLevel = blas;
        desc.instances.push_back(instance);
    }

    return desc;
}

} // namespace RVX
