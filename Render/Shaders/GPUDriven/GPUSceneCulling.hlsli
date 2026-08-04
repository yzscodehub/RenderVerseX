#ifndef RVX_GPU_SCENE_CULLING_HLSLI
#define RVX_GPU_SCENE_CULLING_HLSLI

// Mirrors the packed rows in Render/GPUScene/GPUSceneSchema.h.  This include
// is intentionally shared by every GPU-scene compute entry point so a shader
// cannot reinterpret the six persistent tables with a local ABI.

#define RVX_GPU_SCENE_SCHEMA_VERSION 1u
#define RVX_GPU_SCENE_ROW_FLAG_LIVE 1u
#define RVX_GPU_SCENE_ROW_FLAG_TOMBSTONE 2u
#define RVX_GPU_SCENE_BOUNDS_FLAG_FORCE_VISIBLE 1u
#define RVX_GPU_SCENE_BOUNDS_FLAG_INVALID 2u

// 40 bytes; mirrors RVX::GPUSceneCullingCandidate.
struct GPUSceneCullingCandidate
{
    uint primitiveSlot;
    uint primitiveGeneration;
    uint drawSlot;
    uint drawGeneration;
    uint objectIdLow;
    uint objectIdHigh;
    uint requiredPassMask;
    uint drawGroupIndex;
    uint drawGroupCommandOffset;
    uint rasterInstanceIndex;
};

// 32 bytes.
struct GPUSceneRowHeader
{
    uint2 objectId;
    uint schemaVersion;
    uint generation;
    uint flags;
    uint3 padding;
};

// 80 bytes.
struct GPUScenePrimitiveRow
{
    GPUSceneRowHeader header;
    uint2 bounds;
    uint2 transform;
    uint2 firstDraw;
    uint drawCount;
    uint primitiveFlags;
    uint layerMask;
    uint padding0;
    uint2 sortKey;
};

// 96 bytes.
struct GPUSceneBoundsRow
{
    GPUSceneRowHeader header;
    float4 minimum;
    float4 maximum;
    float4 sphere;
    uint boundsFlags;
    uint3 padding;
};

// 192 bytes.
struct GPUSceneTransformRow
{
    GPUSceneRowHeader header;
    float4 worldFromLocal[3];
    float4 previousWorldFromLocal[3];
    float4 normalFromLocal[3];
    uint transformFlags;
    uint3 padding;
};

// 96 bytes.
struct GPUSceneMaterialRow
{
    GPUSceneRowHeader header;
    uint resourceSlot;
    uint resourceGeneration;
    uint2 materialId;
    uint materialFlags;
    uint shadingModel;
    uint2 padding0;
    float4 baseColor;
    float metallic;
    float roughness;
    float emissiveIntensity;
    float opacity;
};

// 80 bytes.
struct GPUSceneGeometryRow
{
    GPUSceneRowHeader header;
    uint resourceSlot;
    uint resourceGeneration;
    uint2 geometryId;
    uint submeshIndex;
    uint firstIndex;
    int vertexOffset;
    uint indexCount;
    uint topology;
    uint geometryFlags;
    uint2 padding;
};

// 112 bytes.
struct GPUSceneDrawMetadataRow
{
    GPUSceneRowHeader header;
    uint2 primitive;
    uint2 material;
    uint2 geometry;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
    uint passMask;
    uint materialVariant;
    uint padding0;
    uint2 pipelineKey;
    uint2 sortKey;
    uint2 padding;
};

bool GPUSceneIsValidRef(uint2 reference)
{
    return reference.x != 0u && reference.y != 0u;
}

bool GPUSceneIsLiveHeader(
    GPUSceneRowHeader header,
    uint expectedGeneration,
    uint2 expectedObjectId)
{
    return header.schemaVersion == RVX_GPU_SCENE_SCHEMA_VERSION &&
        header.generation == expectedGeneration &&
        all(header.objectId == expectedObjectId) &&
        (header.flags & RVX_GPU_SCENE_ROW_FLAG_LIVE) != 0u &&
        (header.flags & RVX_GPU_SCENE_ROW_FLAG_TOMBSTONE) == 0u;
}

bool GPUSceneCandidateSlotsInRange(
    GPUSceneCullingCandidate candidate,
    uint primitiveCapacity,
    uint boundsCapacity,
    uint transformCapacity,
    uint materialCapacity,
    uint geometryCapacity,
    uint drawCapacity)
{
    return candidate.primitiveSlot != 0u &&
        candidate.primitiveSlot < primitiveCapacity &&
        candidate.drawSlot != 0u && candidate.drawSlot < drawCapacity &&
        boundsCapacity != 0u && transformCapacity != 0u &&
        materialCapacity != 0u && geometryCapacity != 0u;
}

// Shared exact-generation validation semantics.  The caller must have first
// loaded only in-range rows from the six descriptor-bound scene tables.
bool GPUSceneValidateCandidateRows(
    GPUSceneCullingCandidate candidate,
    GPUScenePrimitiveRow primitive,
    GPUSceneBoundsRow bounds,
    GPUSceneTransformRow transform,
    GPUSceneMaterialRow material,
    GPUSceneGeometryRow geometry,
    GPUSceneDrawMetadataRow draw)
{
    const uint2 objectId = uint2(candidate.objectIdLow, candidate.objectIdHigh);
    const uint2 primitiveRef = uint2(candidate.primitiveSlot, candidate.primitiveGeneration);
    const uint2 drawRef = uint2(candidate.drawSlot, candidate.drawGeneration);
    if ((objectId.x == 0u && objectId.y == 0u) ||
        candidate.requiredPassMask == 0u ||
        (candidate.requiredPassMask & (candidate.requiredPassMask - 1u)) != 0u ||
        !GPUSceneIsValidRef(primitiveRef) ||
        !GPUSceneIsValidRef(drawRef) ||
        !GPUSceneIsLiveHeader(primitive.header, primitiveRef.y, objectId) ||
        !GPUSceneIsValidRef(primitive.bounds) ||
        !GPUSceneIsValidRef(primitive.transform) ||
        !GPUSceneIsValidRef(primitive.firstDraw) || primitive.drawCount == 0u ||
        !GPUSceneIsLiveHeader(bounds.header, primitive.bounds.y, objectId) ||
        !GPUSceneIsLiveHeader(transform.header, primitive.transform.y, objectId))
    {
        return false;
    }

    const uint drawRangeEnd = primitive.firstDraw.x + primitive.drawCount;
    if (drawRangeEnd < primitive.firstDraw.x ||
        drawRef.y != primitive.firstDraw.y || drawRef.x < primitive.firstDraw.x ||
        drawRef.x >= drawRangeEnd ||
        !GPUSceneIsLiveHeader(draw.header, drawRef.y, objectId) ||
        any(draw.primitive != primitiveRef) || !GPUSceneIsValidRef(draw.material) ||
        !GPUSceneIsValidRef(draw.geometry) ||
        !GPUSceneIsLiveHeader(material.header, draw.material.y, objectId) ||
        !GPUSceneIsLiveHeader(geometry.header, draw.geometry.y, objectId) ||
        (draw.passMask & candidate.requiredPassMask) == 0u ||
        draw.indexCount == 0u || draw.instanceCount != 1u ||
        draw.firstInstance != 0u ||
        geometry.indexCount != draw.indexCount ||
        geometry.firstIndex != draw.firstIndex ||
        geometry.vertexOffset != draw.vertexOffset)
    {
        return false;
    }

    return true;
}

bool GPUSceneBoundsForceVisible(GPUSceneBoundsRow bounds)
{
    return (bounds.boundsFlags &
            (RVX_GPU_SCENE_BOUNDS_FLAG_FORCE_VISIBLE |
             RVX_GPU_SCENE_BOUNDS_FLAG_INVALID)) != 0u;
}

#endif
