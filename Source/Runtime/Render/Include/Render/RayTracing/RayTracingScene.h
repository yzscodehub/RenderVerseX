#pragma once

/**
 * @file RayTracingScene.h
 * @brief Render-scene to ray-tracing acceleration-structure build planning
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Material/MaterialClassification.h"
#include "RenderContracts/RenderIdentity.h"
#include "RHI/RHIRayTracing.h"

#include <span>
#include <string>
#include <vector>

namespace RVX
{
    class RenderResourceRegistry;
    class RenderScene;

    enum class RayTracingSceneSkipReason : uint8
    {
        None = 0,
        ShadowCastingDisabled,
        TransparentMaterial,
        InstanceMaskZero,
        InvalidTransform,
        MissingMeshResource,
        MeshNotGPUReady,
        InvalidSubmesh,
        UnsupportedPrimitive,
        UnsupportedIndexFormat,
        UnsupportedBaseVertex,
        InvalidGeometry,
        SkinnedGeometryUnsupported,
        InvalidMaterialPolicy,
        IncompleteMaskedMetadata
    };

    /**
     * @brief Ray tracing only accepts stable rigid geometry in this milestone.
     *
     * Static is deliberately a separate value even though the current retained
     * scene does not yet expose static-mobility metadata.  That prevents a
     * future mobility bit from silently changing the BLAS/TLAS policy.
     */
    enum class RayTracingGeometryClassification : uint8
    {
        Static = 0,
        Rigid,
        SkinnedUnsupported
    };

    enum class RayTracingMaskedMetadataStatus : uint8
    {
        NotMasked = 0,
        Complete,
        MissingMaterial,
        InvalidMaterial,
        TextureNotReady,
        UnsupportedUVSet,
        MissingUVSet,
        MissingIndexBuffer
    };

    struct RayTracingSceneOptions
    {
        bool includeOpaque = true;
        bool includeMasked = true;
        bool includeTransparent = false;
        bool requireShadowCasting = false;
        uint32 instanceMask = 0xFF;
        RHIAccelerationStructureBuildFlags blasBuildFlags =
            RHIAccelerationStructureBuildFlags::PreferFastTrace;
        RHIAccelerationStructureBuildFlags tlasBuildFlags =
            RHIAccelerationStructureBuildFlags::PreferFastTrace;
    };

    struct RayTracingBLASKey
    {
        RenderResourceHandle mesh;
        uint64 meshId = 0;
        /** Exact registry revision for the mesh generation used by this BLAS. */
        uint64 meshContentRevision = 0;
        uint32 submeshIndex = 0;
        MaterialRenderMode renderMode = MaterialRenderMode::Opaque;

        bool operator==(const RayTracingBLASKey& other) const
        {
            return mesh == other.mesh &&
                   meshId == other.meshId &&
                   meshContentRevision == other.meshContentRevision &&
                   submeshIndex == other.submeshIndex &&
                   renderMode == other.renderMode;
        }
    };

    struct RayTracingBLASBuild
    {
        RayTracingBLASKey key;
        RHIBottomLevelASDesc desc;
        uint32 instanceCount = 0;
    };

    enum class RayTracingMaterialMetadataFlags : uint32
    {
        None = 0,
        AlphaTest = 1 << 0,
        Transparent = 1 << 1,
        DoubleSided = 1 << 2,
        HasBaseColorTexture = 1 << 3,
        HasMetallicRoughnessTexture = 1 << 4,
        HasNormalTexture = 1 << 5,
        HasEmissiveTexture = 1 << 6,
        Unlit = 1 << 7,
        ShadowCaster = 1 << 8
    };

    inline RayTracingMaterialMetadataFlags operator|(
        RayTracingMaterialMetadataFlags a,
        RayTracingMaterialMetadataFlags b)
    {
        return static_cast<RayTracingMaterialMetadataFlags>(
            static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline RayTracingMaterialMetadataFlags& operator|=(
        RayTracingMaterialMetadataFlags& a,
        RayTracingMaterialMetadataFlags b)
    {
        a = a | b;
        return a;
    }

    inline bool HasRayTracingMaterialFlag(uint32 flags,
                                          RayTracingMaterialMetadataFlags flag)
    {
        return (flags & static_cast<uint32>(flag)) != 0u;
    }

    struct RayTracingTextureSamplingMetadata
    {
        uint32 uvSet = 0;
        uint32 samplerFlags = 0;
        Vec2 uvOffset{0.0f, 0.0f};
        Vec2 uvScale{1.0f, 1.0f};
        float uvRotation = 0.0f;
        float reserved = 0.0f;
    };

    /**
     * @brief Per-instance material data consumed by ray tracing hit shaders.
     *
     * This mirrors the raster material classification path but stores compact
     * PBR factors in TLAS InstanceID order so future RT shadows/reflections/GI
     * can share the same material table.
     */
    struct RayTracingMaterialMetadata
    {
        Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
        Vec3 emissiveColor{0.0f, 0.0f, 0.0f};
        float emissiveStrength = 1.0f;
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float alphaCutoff = 0.5f;
        float normalScale = 1.0f;
        uint32 flags = 0;
        uint32 workflow = 0;
        uint64 materialId = 0;
        uint64 baseColorTextureId = 0;
        uint64 metallicRoughnessTextureId = 0;
        uint64 normalTextureId = 0;
        uint64 emissiveTextureId = 0;
        RayTracingTextureSamplingMetadata baseColorTextureSampling;
        RayTracingTextureSamplingMetadata metallicRoughnessTextureSampling;
        RayTracingTextureSamplingMetadata normalTextureSampling;
        RayTracingTextureSamplingMetadata emissiveTextureSampling;
    };

    /**
     * @brief Per-instance alpha-test data required by ray tracing any-hit shaders.
     *
     * The data is recorded in the scene build plan first so a later GPU metadata
     * buffer can mirror the same dense TLAS instance order used by InstanceID().
     */
    struct RayTracingAlphaTestMetadata
    {
        bool enabled = false;
        RayTracingMaskedMetadataStatus status = RayTracingMaskedMetadataStatus::NotMasked;
        bool hasBaseColorTexture = false;
        bool hasResolvedBaseColorTexture = false;
        bool hasIndexBuffer = false;
        bool hasUVBuffer = false;
        bool hasNormalBuffer = false;
        bool hasTangentBuffer = false;
        uint32 baseColorUVSet = 0;
        uint32 indexElementOffset = 0;
        uint32 baseVertex = 0;
        uint32 baseColorSamplerFlags = 0;
        float alphaCutoff = 0.5f;
        float baseColorAlpha = 1.0f;
        Vec2 baseColorUVOffset{0.0f, 0.0f};
        Vec2 baseColorUVScale{1.0f, 1.0f};
        float baseColorUVRotation = 0.0f;
        RHIFormat indexFormat = RHIFormat::Unknown;
        uint64 baseColorTextureId = 0;
        RHIBuffer* indexBuffer = nullptr;
        RHIBuffer* uvBuffer = nullptr;
        RHIBuffer* normalBuffer = nullptr;
        RHIBuffer* tangentBuffer = nullptr;
    };

    struct RayTracingTLASInstance
    {
        uint32 blasIndex = 0;
        uint32 objectIndex = 0;
        uint32 submeshIndex = 0;
        uint64 entityId = 0;
        uint64 materialId = 0;
        uint64 materialContentRevision = 0;
        MaterialRenderMode renderMode = MaterialRenderMode::Opaque;
        RayTracingGeometryClassification geometryClassification =
            RayTracingGeometryClassification::Rigid;
        RayTracingMaterialMetadata material;
        RayTracingAlphaTestMetadata alphaTest;
        RHIRayTracingInstanceDesc desc;
    };

    struct RayTracingSceneSkip
    {
        RayTracingSceneSkipReason reason = RayTracingSceneSkipReason::None;
        uint32 objectIndex = 0;
        uint32 submeshIndex = 0;
        uint64 meshId = 0;
        std::string message;
    };

    struct RayTracingSceneBuildStats
    {
        size_t candidateObjectCount = 0;
        size_t drawItemCount = 0;
        size_t blasBuildCount = 0;
        size_t instanceCount = 0;
        size_t alphaTestedInstanceCount = 0;
        size_t skippedCount = 0;
    };

    struct RayTracingSceneBuildPlan
    {
        std::vector<RayTracingBLASBuild> blasBuilds;
        std::vector<RayTracingTLASInstance> instances;
        std::vector<RayTracingSceneSkip> skips;
        RayTracingSceneBuildStats stats;
        /** False makes required/optional RT paths fail closed instead of treating masked input as opaque. */
        bool maskedMetadataComplete = true;
        RHIAccelerationStructureBuildFlags tlasBuildFlags =
            RHIAccelerationStructureBuildFlags::PreferFastTrace;

        bool HasWork() const { return !blasBuilds.empty() && !instances.empty(); }
    };

    /**
     * @brief Exact topology identity used to decide whether a TLAS may update.
     *
     * Transform values are intentionally excluded: a transform-only change can
     * use an in-place TLAS update when the RHI reports that capability.
     */
    struct RayTracingTLASStructuralRecord
    {
        RenderResourceHandle mesh;
        uint64 meshContentRevision = 0;
        uint32 submeshIndex = 0;
        uint64 entityId = 0;
        uint64 materialId = 0;
        uint64 materialContentRevision = 0;
        MaterialRenderMode renderMode = MaterialRenderMode::Opaque;
        uint32 materialFlags = 0;
        uint32 materialWorkflow = 0;
        uint32 instanceMask = 0;
        uint32 instanceFlags = 0;
        uint32 hitGroupIndex = 0;
        RayTracingGeometryClassification geometryClassification =
            RayTracingGeometryClassification::Rigid;
        RayTracingMaskedMetadataStatus maskedMetadataStatus =
            RayTracingMaskedMetadataStatus::NotMasked;
        uint32 baseColorUVSet = 0;
        uint64 baseColorTextureId = 0;

        [[nodiscard]] bool operator==(const RayTracingTLASStructuralRecord& other) const noexcept = default;
    };

    struct RayTracingTLASStructuralSignature
    {
        uint32 instanceCount = 0;
        /** Canonical ordered records are the update safety decision; hash is diagnostics only. */
        std::vector<RayTracingTLASStructuralRecord> records;
        uint64 hash = 0;

        [[nodiscard]] bool operator==(const RayTracingTLASStructuralSignature& other) const
        {
            return instanceCount == other.instanceCount && records == other.records;
        }
    };

    RayTracingSceneBuildPlan BuildRayTracingSceneBuildPlan(
        const RenderScene& scene,
        std::span<const uint32_t> candidateObjectIndices,
        const RenderResourceRegistry& registry,
        const RayTracingSceneOptions& options = {});

    [[nodiscard]] RayTracingTLASStructuralSignature BuildRayTracingTLASStructuralSignature(
        const RayTracingSceneBuildPlan& plan);

    RHITopLevelASDesc BuildRayTracingTopLevelDesc(
        const RayTracingSceneBuildPlan& plan,
        std::span<RHIAccelerationStructure* const> blasResources);

} // namespace RVX
