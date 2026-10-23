#ifndef RVX_RAY_TRACING_SCENE_METADATA_HLSLI
#define RVX_RAY_TRACING_SCENE_METADATA_HLSLI

static const uint RT_ALPHA_TEST_ENABLED = 1u << 0;
static const uint RT_ALPHA_HAS_BASE_COLOR_TEXTURE = 1u << 1;
static const uint RT_ALPHA_HAS_RESOLVED_BASE_COLOR_TEXTURE = 1u << 2;
static const uint RT_ALPHA_HAS_UV_BUFFER = 1u << 3;
static const uint RT_ALPHA_HAS_INDEX_BUFFER = 1u << 4;
static const uint RT_ALPHA_INDEX_FORMAT_UINT32 = 1u << 5;
static const uint RT_ALPHA_INDEX_FORMAT_UINT16 = 1u << 6;
static const uint RT_ALPHA_HAS_NORMAL_BUFFER = 1u << 7;
static const uint RT_ALPHA_HAS_TANGENT_BUFFER = 1u << 8;
static const uint RT_INVALID_INDEX = 0xFFFFFFFFu;
static const uint RT_ALPHA_WRAP_S_CLAMP = 1u << 0;
static const uint RT_ALPHA_WRAP_T_CLAMP = 1u << 1;
static const uint RT_ALPHA_MAG_NEAREST = 1u << 2;
static const uint RT_ALPHA_WRAP_S_MIRROR = 1u << 3;
static const uint RT_ALPHA_WRAP_T_MIRROR = 1u << 4;

static const uint RT_MATERIAL_ALPHA_TEST = 1u << 0;
static const uint RT_MATERIAL_TRANSPARENT = 1u << 1;
static const uint RT_MATERIAL_DOUBLE_SIDED = 1u << 2;
static const uint RT_MATERIAL_HAS_BASE_COLOR_TEXTURE = 1u << 3;
static const uint RT_MATERIAL_HAS_METALLIC_ROUGHNESS_TEXTURE = 1u << 4;
static const uint RT_MATERIAL_HAS_NORMAL_TEXTURE = 1u << 5;
static const uint RT_MATERIAL_HAS_EMISSIVE_TEXTURE = 1u << 6;
static const uint RT_MATERIAL_UNLIT = 1u << 7;
static const uint RT_MATERIAL_SHADOW_CASTER = 1u << 8;

struct RayTracingInstanceAlphaMetadata
{
    uint Flags;
    uint BaseColorUVSet;
    float AlphaCutoff;
    float BaseColorAlpha;
    uint2 BaseColorTextureId;
    uint BaseColorTextureTableIndex;
    uint IndexBufferTableIndex;
    uint UVBufferTableIndex;
    uint IndexElementOffset;
    uint BaseVertex;
    uint BaseColorSamplerFlags;
    uint NormalBufferTableIndex;
    uint TangentBufferTableIndex;
    float2 BaseColorUVOffset;
    float2 BaseColorUVScale;
    float BaseColorUVRotation;
    float Reserved2;
};

struct RayTracingMaterialTextureSamplingMetadata
{
    float2 UVOffset;
    float2 UVScale;
    float UVRotation;
    uint UVSet;
    uint SamplerFlags;
    uint Reserved;
};

struct RayTracingInstanceMaterialMetadata
{
    float4 BaseColorFactor;
    float4 EmissiveFactor;
    float4 MaterialFactors; // x: metallic, y: roughness, z: alpha cutoff, w: normal scale
    uint Flags;
    uint2 MaterialId;
    uint Workflow;
    uint BaseColorTextureTableIndex;
    uint MetallicRoughnessTextureTableIndex;
    uint NormalTextureTableIndex;
    uint EmissiveTextureTableIndex;
    RayTracingMaterialTextureSamplingMetadata BaseColorTextureSampling;
    RayTracingMaterialTextureSamplingMetadata MetallicRoughnessTextureSampling;
    RayTracingMaterialTextureSamplingMetadata NormalTextureSampling;
    RayTracingMaterialTextureSamplingMetadata EmissiveTextureSampling;
};

#endif // RVX_RAY_TRACING_SCENE_METADATA_HLSLI
