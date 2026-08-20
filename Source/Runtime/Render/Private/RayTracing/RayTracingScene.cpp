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

    RHIRayTracingInstanceFlags ToInstanceFlags(MaterialRenderMode mode)
    {
        return mode == MaterialRenderMode::Opaque
                   ? RHIRayTracingInstanceFlags::ForceOpaque
                   : RHIRayTracingInstanceFlags::ForceNoOpaque;
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

    RayTracingAlphaTestMetadata BuildAlphaTestMetadata(const RenderDrawItem& item,
                                                       const MeshGPUBuffers& buffers,
                                                       const SubmeshGPUInfo& submesh,
                                                       RHIFormat indexFormat)
    {
        RayTracingAlphaTestMetadata metadata;
        metadata.enabled = item.renderMode == MaterialRenderMode::Masked;
        metadata.hasIndexBuffer = buffers.indexBuffer != nullptr;
        metadata.hasUVBuffer = buffers.hasUVs && buffers.uvBuffer != nullptr;
        metadata.hasNormalBuffer = buffers.hasNormals && buffers.normalBuffer != nullptr;
        metadata.hasTangentBuffer = buffers.hasTangents && buffers.tangentBuffer != nullptr;
        metadata.indexBuffer = metadata.hasIndexBuffer ? buffers.indexBuffer : nullptr;
        metadata.uvBuffer = metadata.hasUVBuffer ? buffers.uvBuffer : nullptr;
        metadata.normalBuffer = metadata.hasNormalBuffer ? buffers.normalBuffer : nullptr;
        metadata.tangentBuffer = metadata.hasTangentBuffer ? buffers.tangentBuffer : nullptr;
        metadata.indexElementOffset = submesh.indexOffset;
        metadata.baseVertex = static_cast<uint32>(submesh.baseVertex);
        metadata.indexFormat = indexFormat;
        if (!metadata.enabled)
        {
            return metadata;
        }

        return metadata;
    }

    RayTracingAlphaTestMetadata BuildAlphaTestMetadata(
        const RenderDrawItem& item,
        const MeshGPUBuffers& buffers,
        const SubmeshGPUInfo& submesh,
        RHIFormat indexFormat,
        const RenderResourceRegistry& registry)
    {
        RayTracingAlphaTestMetadata metadata =
            BuildAlphaTestMetadata(item, buffers, submesh, indexFormat);
        const RenderMaterialResourceData* material =
            registry.ResolveMaterial(item.material);
        if (material == nullptr || !material->metadataValid)
        {
            return metadata;
        }

        const MaterialSourceData& source = material->sourceData;
        metadata.alphaCutoff = source.alphaCutoff;
        metadata.baseColorAlpha = source.baseColorFactor.a;
        const MaterialUploadTextureBinding* binding = FindMaterialBinding(
            *material, MaterialUploadTextureSlot::BaseColor);
        if (binding == nullptr)
        {
            return metadata;
        }
        metadata.hasBaseColorTexture = true;
        metadata.hasResolvedBaseColorTexture =
            registry.IsGPUReadyExact(binding->texture);
        metadata.baseColorTextureId = PackHandle(binding->texture);
        metadata.baseColorUVSet = static_cast<uint32>(
            std::max(binding->uvSet, 0));
        metadata.baseColorSamplerFlags =
            EncodeRayTracingSamplerFlags(*binding);
        metadata.baseColorUVOffset = binding->offset;
        metadata.baseColorUVScale = binding->scale;
        metadata.baseColorUVRotation = binding->rotation;
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
    std::span<const uint32_t> visibleObjectIndices,
    const RenderResourceRegistry& registry,
    const RayTracingSceneOptions& options)
{
    RayTracingSceneBuildPlan plan;
    plan.tlasBuildFlags = options.tlasBuildFlags;
    plan.stats.visibleObjectCount = visibleObjectIndices.size();

    std::vector<RenderDrawItem> opaqueDrawItems;
    std::vector<RenderDrawItem> maskedDrawItems;
    std::vector<RenderDrawItem> transparentDrawItems;
    const std::vector<uint32_t> visibleIndices(
        visibleObjectIndices.begin(), visibleObjectIndices.end());
    BuildMaterialDrawLists(scene,
                           visibleIndices,
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

    plan.stats.drawItemCount = drawItems.size();
    for (const RenderDrawItem& item : drawItems)
    {
        if (!ShouldIncludeRenderMode(item.renderMode, options) ||
            item.objectIndex >= scene.GetObjectCount())
        {
            continue;
        }

        const RenderObject& object = scene.GetObject(item.objectIndex);
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

        RayTracingBLASKey key;
        key.mesh = item.mesh;
        key.meshId = PackHandle(item.mesh);
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
        instance.renderMode = item.renderMode;
        instance.material = BuildMaterialMetadata(item, registry);
        if (object.castsShadow)
        {
            instance.material.flags |= static_cast<uint32>(
                RayTracingMaterialMetadataFlags::ShadowCaster);
        }
        instance.alphaTest = BuildAlphaTestMetadata(
            item, buffers, submesh, indexFormat, registry);
        WriteRowMajor3x4(instance.desc.transform, object.worldMatrix);
        instance.desc.instanceId = static_cast<uint32>(plan.instances.size());
        instance.desc.instanceMask = instanceMask;
        instance.desc.instanceContributionToHitGroupIndex = 0;
        instance.desc.flags = ToInstanceFlags(item.renderMode);
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
