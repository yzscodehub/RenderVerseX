// =============================================================================
// DefaultLit.hlsl - Default lit material shader for model rendering
// =============================================================================
//
// Descriptor sets:
//   set 0 / space0: frame data
//   set 1 / space1: object data
//   set 2 / space2: material data, textures, and per-texture samplers
//
// Vertex inputs come from separate vertex buffers:
//   Slot 0: Position buffer (float3)
//   Slot 1: Normal buffer (float3)
//   Slot 2: UV buffer (float2)
//   Slot 3: Tangent buffer (float4)
//   Slot 4: Bone indices buffer (uint4)
//   Slot 5: Bone weights buffer (float4)
//   Slot 6: GPU-driven global instance index buffer (uint)
// =============================================================================

// GPU-scene's private compatibility cbuffer retains this fixed tail. Direct
// raster skinning uses Include/Skinning.hlsli and never indexes that tail.
#define RVX_MAX_OBJECT_SKINNING_MATRICES 128

#include "Include/BRDF.hlsli"
#include "Include/AmbientOcclusion.hlsli"
#include "Include/Exposure.hlsli"
#include "Include/GPUInstanceData.hlsli"
#include "Include/Lighting.hlsli"
#include "Include/SurfaceMaterial.hlsli"
#include "Include/ToonLighting.hlsli"
#include "GPUDriven/GPUSceneRaster.hlsli"
#include "Translucency/WeightedBlendedOIT.hlsli"

// =============================================================================
// Constant Buffers
// =============================================================================

cbuffer ViewConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float4 CameraPosition_Time;
    float4 LightDirection_Intensity;
    float4 DirectionalLightColor_ReverseZ;
    float4 IBLDiffuseAmbient;   // rgb: color, a: diffuse intensity
    float4 IBLSpecularAmbient;  // rgb: color, a: specular intensity
    float4 IBLTextureParams;    // x: enabled, y: prefiltered mip count, z: intensity, w: ambient floor intensity
    float4 CameraForwardAndShadowCascadeCount; // xyz: camera forward, w: active cascade count
    float4x4 DirectionalShadowViewProjections[4];
    float4 DirectionalShadowParams; // x: enabled, y: depth bias, z: strength, w: UV-space PCF filter step
    float4 DirectionalShadowReceiverParams; // x: receiver normal bias in world units
    float4 DirectionalShadowCascadeSplits; // absolute camera-forward split distances
    float4 DirectionalShadowCascadeFadeDistances; // absolute fade widths before split boundaries
    float4 RayTracedShadowParams; // x: enabled, y: screen-space filter radius in pixels, z: composition mode
};

#if !defined(RVX_GPU_SCENE_RASTER)
cbuffer ObjectConstants : register(b0, space1)
{
    float4x4 World;
    float4x4 NormalMatrix;
    float4x4 PreviousWorldViewProjection;
    float4 ObjectVelocityParams; // x: previous WVP valid, y: receives shadow
    float4 SkinningParams; // x: enabled, y: matrix count, z/w: current/previous offsets
};
#endif

cbuffer LightConstants : register(b3, space0)
{
    float4 MainLightDirection_Intensity;
    uint4 MainLightColor_ShadowMapIndexBits;
    float4x4 MainLightSpaceMatrix;
    uint NumPointLights;
    uint NumSpotLights;
    float2 LightPadding;
    float4x4 LocalShadowViewProjections[32];
    uint LocalShadowPageCount;
    float LocalShadowInvResolution;
    float LocalShadowDepthBias;
    float LocalShadowNormalBias;
};

cbuffer ClusterConstants : register(b7, space0)
{
    float4 ClusterSize;         // x/y/z cluster counts, w total cluster count
    float4 ClusterScreenParams; // x/y viewport size, z/w near/far planes
    float4x4 ClusterInvProj;
};

struct GPUCluster
{
    uint offset;
    uint count;
    uint pointCount;
    uint spotCount;
};

cbuffer MaterialConstants : register(b0, space2)
{
    RVX_MATERIAL_PARAMETER_FIELDS;
};

#define CameraPosition CameraPosition_Time.xyz
#define Time CameraPosition_Time.w
#define ReceivesShadow ObjectVelocityParams.y
#define LightDirection LightDirection_Intensity.xyz
#define DirectionalLightIntensity LightDirection_Intensity.w
#define ReverseZ DirectionalLightColor_ReverseZ.w
#define DirectionalLightColor DirectionalLightColor_ReverseZ.xyz
#define EmissiveColor EmissiveColor_Strength.xyz
#define EmissiveStrength EmissiveColor_Strength.w

DirectionalLight ResolveMainLight()
{
    DirectionalLight light;
    light.direction = MainLightDirection_Intensity.xyz;
    light.intensity = MainLightDirection_Intensity.w;
    light.color = asfloat(MainLightColor_ShadowMapIndexBits.xyz);
    light.shadowMapIndex = asint(MainLightColor_ShadowMapIndexBits.w);
    light.lightSpaceMatrix = MainLightSpaceMatrix;
    return light;
}

#define MainLight ResolveMainLight()

Texture2D BaseColorTexture : register(t1, space2);
Texture2D NormalTexture : register(t2, space2);
Texture2D MetallicRoughnessTexture : register(t3, space2);
Texture2D OcclusionTexture : register(t4, space2);
Texture2D EmissiveTexture : register(t5, space2);
Texture2D ToonRampTexture : register(t6, space2);
Texture2D TransmissionTexture : register(t7, space2);
Texture2D ThicknessTexture : register(t8, space2);
SamplerState BaseColorSampler : register(s9, space2);
SamplerState NormalSampler : register(s10, space2);
SamplerState MetallicRoughnessSampler : register(s11, space2);
SamplerState OcclusionSampler : register(s12, space2);
SamplerState EmissiveSampler : register(s13, space2);
SamplerState ToonRampSampler : register(s14, space2);
SamplerState TransmissionSampler : register(s15, space2);
SamplerState ThicknessSampler : register(s16, space2);
StructuredBuffer<MaterialParameterData> MaterialParameterTable : register(t17, space2);
Texture2DArray<float> DirectionalShadowMapTexture : register(t1, space0);
SamplerState DirectionalShadowSampler : register(s2, space0);
StructuredBuffer<PointLight> PointLights : register(t4, space0);
StructuredBuffer<SpotLight> SpotLights : register(t5, space0);
Texture2D<float> RayTracedShadowMaskTexture : register(t6, space0);
StructuredBuffer<GPUCluster> ClusterData : register(t8, space0);
StructuredBuffer<uint> ClusterLightIndices : register(t9, space0);
TextureCube IrradianceTexture : register(t10, space0);
TextureCube PrefilteredEnvironmentTexture : register(t11, space0);
Texture2D BRDFLUTTexture : register(t12, space0);
SamplerState IBLLinearClampSampler : register(s13, space0);
// The graph imports exactly one committed prior-presented state for the
// entire frame. GBuffer export intentionally never reads this buffer; every
// Forward HDR surface, including transparent and Unlit, does.
StructuredBuffer<RVXExposureState> ExposureStateBuffer : register(t14, space0);
// Renderer-owned R8 visibility field. Opaque receives the exact GTAO output;
// later Forward surfaces receive PipelineCache's neutral white fallback.
Texture2D<float> AmbientOcclusionTexture : register(t15, space0);
// Late graph-owned opaque HDR pyramid after Reflection/Sky. ThinTransmission
// samples its explicit roughness-squared mip; it never samples the active RT.
Texture2D OpaqueSceneColorPyramidTexture : register(t16, space0);
// Renderer-owned full-capacity D32 Texture2DArray. It shares the existing
// point-clamp depth sampler at s2 with directional CSM sampling.
Texture2DArray<float> LocalShadowAtlasTexture : register(t17, space0);
// Integrated froxel field: rgb is accumulated in-scattering and alpha is
// camera-to-slice transmittance. Opaque/geometry records bind the immutable
// neutral (0,0,0,1) 3D texture; late surfaces bind the exact graph volume.
Texture3D<float4> FroxelVolumetricLightingTexture : register(t18, space0);
#define LocalShadowAtlasSampler DirectionalShadowSampler
#if !defined(RVX_GPU_SCENE_RASTER)
StructuredBuffer<GPUInstanceData> GPUDrivenInstances : register(t1, space1);
#endif

#include "Include/Skinning.hlsli"

// =============================================================================
// Vertex Shader Input/Output
// =============================================================================

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent  : TANGENT;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
    float2 TexCoord1 : TEXCOORD1;
};

// Direct rigid draws do not bind skinning or GPU-driven instance streams.
struct RigidDirectVSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent  : TANGENT;
    float2 TexCoord1 : TEXCOORD1;
};

struct RigidVSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent  : TANGENT;
    float2 TexCoord1 : TEXCOORD1;
    uint InstanceIndex : INSTANCE_INDEX;
};

struct PSInput
{
    float4 Position    : SV_POSITION;
    float3 WorldPos    : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord    : TEXCOORD2;
    float4 WorldTangent : TEXCOORD3;
    nointerpolation float ReceivesShadowValue : TEXCOORD4;
    nointerpolation uint MaterialParameterSlot : TEXCOORD5;
    float2 TexCoord1 : TEXCOORD6;
};

// =============================================================================
// Vertex Shader
// =============================================================================

PSInput VSMain(VSInput input)
{
    PSInput output;

    const float4 localPosition = RVXResolveCurrentSkinningPosition(
        input.Position, input.BoneIndices, input.BoneWeights);
    const float3 localNormal = RVXResolveCurrentSkinningVector(
        input.Normal, input.BoneIndices, input.BoneWeights);
    const float3 localTangent = RVXResolveCurrentSkinningVector(
        input.Tangent.xyz, input.BoneIndices, input.BoneWeights);

    float4 worldPos = mul(World, localPosition);
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = normalize(mul((float3x3)NormalMatrix, localNormal));
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    output.WorldTangent = float4(
        normalize(mul((float3x3)World, localTangent)),
        input.Tangent.w * RVXResolveTransformHandedness((float3x3)World));
    output.ReceivesShadowValue = ReceivesShadow;
    output.MaterialParameterSlot = 0xFFFFFFFFu;

    return output;
}

PSInput VSMainRigid(RigidDirectVSInput input)
{
    PSInput output;

    const float4 worldPos = RVXTransformRigidAffinePosition(
        World[0], World[1], World[2], input.Position);
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = normalize(mul((float3x3)NormalMatrix, input.Normal));
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    output.WorldTangent = float4(
        normalize(mul((float3x3)World, input.Tangent.xyz)),
        input.Tangent.w * RVXResolveTransformHandedness((float3x3)World));
    output.ReceivesShadowValue = ReceivesShadow;
    output.MaterialParameterSlot = 0xFFFFFFFFu;

    return output;
}

#if !defined(RVX_GPU_SCENE_RASTER)
PSInput VSMainGPUDriven(
    RigidVSInput input)
{
    GPUInstanceData instance = GPUDrivenInstances[input.InstanceIndex];

    PSInput output;

    const float4 worldPos = RVXTransformRigidAffinePosition(
        instance.worldMatrix[0],
        instance.worldMatrix[1],
        instance.worldMatrix[2],
        input.Position);
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = normalize(mul((float3x3)instance.normalMatrix, input.Normal));
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    output.WorldTangent = float4(
        normalize(mul((float3x3)instance.worldMatrix, input.Tangent.xyz)),
        input.Tangent.w * RVXResolveTransformHandedness(
            (float3x3)instance.worldMatrix));
    output.ReceivesShadowValue = ReceivesShadow;
    output.MaterialParameterSlot = 0xFFFFFFFFu;

    return output;
}

PSInput VSMainInstancedMaterial(
    RigidVSInput input)
{
    GPUInstanceData instance = GPUDrivenInstances[input.InstanceIndex];

    PSInput output;
    const float4 worldPos = RVXTransformRigidAffinePosition(
        instance.worldMatrix[0],
        instance.worldMatrix[1],
        instance.worldMatrix[2],
        input.Position);
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = normalize(mul((float3x3)instance.normalMatrix, input.Normal));
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    output.WorldTangent = float4(
        normalize(mul((float3x3)instance.worldMatrix, input.Tangent.xyz)),
        input.Tangent.w * RVXResolveTransformHandedness(
            (float3x3)instance.worldMatrix));
    output.ReceivesShadowValue = ReceivesShadow;
    output.MaterialParameterSlot = instance.materialId;
    return output;
}
#endif

#if defined(RVX_GPU_SCENE_RASTER)
PSInput VSMainGPUScene(RigidVSInput input)
{
    PSInput output;
    GPUSceneTransformRow transform;
    uint primitiveFlags;
    uint materialParameterSlot;
    if (!GPUSceneResolveRasterTransform(
            input.InstanceIndex,
            transform,
            primitiveFlags,
            materialParameterSlot))
    {
        output.Position = GPUSceneInvalidClipPosition();
        output.WorldPos = float3(0.0f, 0.0f, 0.0f);
        output.WorldNormal = float3(0.0f, 0.0f, 0.0f);
        output.TexCoord = float2(0.0f, 0.0f);
        output.TexCoord1 = float2(0.0f, 0.0f);
        output.WorldTangent = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.ReceivesShadowValue = 0.0f;
        output.MaterialParameterSlot = 0xFFFFFFFFu;
        return output;
    }

    const float4 worldPos = GPUSceneTransformPosition(transform, input.Position);
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = GPUSceneSafeNormalize(
        GPUSceneTransformNormal(transform, input.Normal),
        float3(0.0f, 0.0f, 1.0f));
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    output.WorldTangent = float4(
        GPUSceneSafeNormalize(GPUSceneTransformTangent(transform, input.Tangent.xyz),
                               float3(1.0f, 0.0f, 0.0f)),
        input.Tangent.w * RVXResolveTransformHandedness(float3x3(
            transform.worldFromLocal[0].xyz,
            transform.worldFromLocal[1].xyz,
            transform.worldFromLocal[2].xyz)));
    output.ReceivesShadowValue =
        (primitiveFlags & RVX_GPU_SCENE_PRIMITIVE_RECEIVES_SHADOW) != 0u
            ? 1.0f
            : 0.0f;
    output.MaterialParameterSlot = 0xFFFFFFFFu;
    return output;
}

PSInput VSMainGPUSceneInstancedMaterial(RigidVSInput input)
{
    PSInput output = VSMainGPUScene(input);
    GPUSceneTransformRow transform;
    uint primitiveFlags;
    uint materialParameterSlot;
    if (GPUSceneResolveRasterTransform(
            input.InstanceIndex,
            transform,
            primitiveFlags,
            materialParameterSlot))
    {
        output.MaterialParameterSlot = materialParameterSlot;
    }
    return output;
}
#endif

// =============================================================================
// Pixel Shader
// =============================================================================

float3 SafeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return lenSq > 1.0e-8 ? value * rsqrt(lenSq) : fallback;
}

#include "Include/DirectionalShadow.hlsli"
#include "Include/LocalShadow.hlsli"

static const uint RVX_INVALID_CLUSTER_INDEX = 0xffffffffu;
static const uint RVX_MAX_CLUSTERED_LIGHTS_PER_PIXEL = 100u;

bool IsClusteredLightingEnabled()
{
    return ClusterSize.w >= 1.0 &&
           ClusterSize.x >= 1.0 &&
           ClusterSize.y >= 1.0 &&
           ClusterSize.z >= 1.0 &&
           ClusterScreenParams.x >= 1.0 &&
           ClusterScreenParams.y >= 1.0 &&
           ClusterScreenParams.w > ClusterScreenParams.z;
}

uint ResolveClusterIndex(float4 svPosition, float3 worldPos)
{
    if (!IsClusteredLightingEnabled())
    {
        return RVX_INVALID_CLUSTER_INDEX;
    }

    const uint clusterCountX = max((uint)round(ClusterSize.x), 1u);
    const uint clusterCountY = max((uint)round(ClusterSize.y), 1u);
    const uint clusterCountZ = max((uint)round(ClusterSize.z), 1u);
    const uint totalClusterCount = max((uint)round(ClusterSize.w), 1u);

    const float screenX = saturate(svPosition.x / max(ClusterScreenParams.x, 1.0));
    const float screenY = saturate(svPosition.y / max(ClusterScreenParams.y, 1.0));
    const uint clusterX = min((uint)(screenX * clusterCountX), clusterCountX - 1u);
    const uint clusterY = min((uint)(screenY * clusterCountY), clusterCountY - 1u);

    const float nearPlane = max(ClusterScreenParams.z, 0.0001);
    const float farPlane = max(ClusterScreenParams.w, nearPlane + 0.0001);
    const float viewDepth = GetDirectionalShadowViewDepth(worldPos);
    if (viewDepth <= nearPlane || viewDepth >= farPlane)
    {
        return RVX_INVALID_CLUSTER_INDEX;
    }

    const float depthSlice = saturate(log(max(viewDepth, nearPlane) / nearPlane) /
                                     max(log(farPlane / nearPlane), 0.0001));
    const uint clusterZ = min((uint)(depthSlice * clusterCountZ), clusterCountZ - 1u);
    const uint clusterIndex = clusterX +
                              clusterY * clusterCountX +
                              clusterZ * clusterCountX * clusterCountY;

    return clusterIndex < totalClusterCount ? clusterIndex : RVX_INVALID_CLUSTER_INDEX;
}

float3 EvaluateLinearLocalLights(
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    float metallic,
    float roughness,
    bool receivesShadow)
{
    float3 localLight = float3(0.0, 0.0, 0.0);

    const uint pointLightCount = min(NumPointLights, 256u);
    [loop]
    for (uint pointLightIndex = 0; pointLightIndex < pointLightCount; ++pointLightIndex)
    {
        localLight += EvaluatePointLight(
            PointLights[pointLightIndex],
            normal,
            viewDir,
            worldPos,
            baseColor,
            metallic,
            roughness,
            receivesShadow
                ? RVX_SamplePointLocalShadow(
                    PointLights[pointLightIndex], worldPos, normal)
                : 1.0);
    }

    const uint spotLightCount = min(NumSpotLights, 128u);
    [loop]
    for (uint spotLightIndex = 0; spotLightIndex < spotLightCount; ++spotLightIndex)
    {
        localLight += EvaluateSpotLight(
            SpotLights[spotLightIndex],
            normal,
            viewDir,
            worldPos,
            baseColor,
            metallic,
            roughness,
            receivesShadow
                ? RVX_SampleSpotLocalShadow(
                    SpotLights[spotLightIndex], worldPos, normal)
                : 1.0);
    }

    return localLight;
}

float3 EvaluateClusteredLocalLights(
    float4 svPosition,
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    float metallic,
    float roughness,
    bool receivesShadow)
{
    const uint clusterIndex = ResolveClusterIndex(svPosition, worldPos);
    if (clusterIndex == RVX_INVALID_CLUSTER_INDEX)
    {
        return EvaluateLinearLocalLights(
            normal, viewDir, worldPos, baseColor, metallic, roughness,
            receivesShadow);
    }

    const GPUCluster cluster = ClusterData[clusterIndex];
    const uint clusteredLightCount = min(cluster.count, RVX_MAX_CLUSTERED_LIGHTS_PER_PIXEL);
    float3 localLight = float3(0.0, 0.0, 0.0);

    [loop]
    for (uint clusteredLightIndex = 0; clusteredLightIndex < clusteredLightCount; ++clusteredLightIndex)
    {
        const uint packedLightIndex = ClusterLightIndices[cluster.offset + clusteredLightIndex];
        const uint lightIndex = packedLightIndex & 0xffffu;
        const uint lightType = (packedLightIndex >> 16u) & 0xffffu;

        if (lightType == 0u && lightIndex < NumPointLights)
        {
            localLight += EvaluatePointLight(
                PointLights[lightIndex],
                normal,
                viewDir,
                worldPos,
                baseColor,
                metallic,
                roughness,
                receivesShadow
                    ? RVX_SamplePointLocalShadow(
                        PointLights[lightIndex], worldPos, normal)
                    : 1.0);
        }
        else if (lightType == 1u && lightIndex < NumSpotLights)
        {
            localLight += EvaluateSpotLight(
                SpotLights[lightIndex],
                normal,
                viewDir,
                worldPos,
                baseColor,
                metallic,
                roughness,
                receivesShadow
                    ? RVX_SampleSpotLocalShadow(
                        SpotLights[lightIndex], worldPos, normal)
                    : 1.0);
        }
    }

    return localLight;
}

float3 EvaluateToonPointLight(
    PointLight light,
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    MaterialParameterData material,
    bool receivesShadow)
{
    const float3 lightVector = light.position - worldPos;
    const float distanceToLight = length(lightVector);
    if (distanceToLight > light.range || distanceToLight <= 0.0f ||
        light.range <= 1.0e-4f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return RVX_EvaluateToonDirectLight(
        normal,
        viewDir,
        lightVector / distanceToLight,
        baseColor,
        light.color * light.intensity * AttenuatePoint(distanceToLight, light.range),
        receivesShadow
            ? RVX_SamplePointLocalShadow(light, worldPos, normal)
            : 1.0f,
        material.ToonBands,
        material.ToonSpecularThreshold,
        material.ToonRimStrength,
        material.ToonShadowTint.xyz);
}

float3 EvaluateToonSpotLight(
    SpotLight light,
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    MaterialParameterData material,
    bool receivesShadow)
{
    const float3 lightVector = light.position - worldPos;
    const float distanceToLight = length(lightVector);
    if (distanceToLight > light.range || distanceToLight <= 0.0f ||
        light.range <= 1.0e-4f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float3 lightDirection = lightVector / distanceToLight;
    const float cone = AttenuateSpot(
        -lightDirection,
        normalize(light.direction),
        light.innerConeAngle,
        light.outerConeAngle);
    if (cone <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return RVX_EvaluateToonDirectLight(
        normal,
        viewDir,
        lightDirection,
        baseColor,
        light.color * light.intensity *
            AttenuatePoint(distanceToLight, light.range) * cone,
        receivesShadow
            ? RVX_SampleSpotLocalShadow(light, worldPos, normal)
            : 1.0f,
        material.ToonBands,
        material.ToonSpecularThreshold,
        material.ToonRimStrength,
        material.ToonShadowTint.xyz);
}

float3 EvaluateToonLinearLocalLights(
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    MaterialParameterData material,
    bool receivesShadow)
{
    float3 localLight = float3(0.0f, 0.0f, 0.0f);
    [loop]
    for (uint index = 0; index < min(NumPointLights, 256u); ++index)
    {
        localLight += EvaluateToonPointLight(
            PointLights[index], normal, viewDir, worldPos, baseColor, material,
            receivesShadow);
    }
    [loop]
    for (uint index = 0; index < min(NumSpotLights, 128u); ++index)
    {
        localLight += EvaluateToonSpotLight(
            SpotLights[index], normal, viewDir, worldPos, baseColor, material,
            receivesShadow);
    }
    return localLight;
}

float3 EvaluateToonClusteredLocalLights(
    float4 svPosition,
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    MaterialParameterData material,
    bool receivesShadow)
{
    const uint clusterIndex = ResolveClusterIndex(svPosition, worldPos);
    if (clusterIndex == RVX_INVALID_CLUSTER_INDEX)
    {
        return EvaluateToonLinearLocalLights(
            normal, viewDir, worldPos, baseColor, material, receivesShadow);
    }

    const GPUCluster cluster = ClusterData[clusterIndex];
    const uint lightCount = min(cluster.count, RVX_MAX_CLUSTERED_LIGHTS_PER_PIXEL);
    float3 localLight = float3(0.0f, 0.0f, 0.0f);
    [loop]
    for (uint index = 0; index < lightCount; ++index)
    {
        const uint packedIndex = ClusterLightIndices[cluster.offset + index];
        const uint lightIndex = packedIndex & 0xffffu;
        const uint lightType = (packedIndex >> 16u) & 0xffffu;
        if (lightType == 0u && lightIndex < NumPointLights)
        {
            localLight += EvaluateToonPointLight(
                PointLights[lightIndex], normal, viewDir, worldPos, baseColor,
                material, receivesShadow);
        }
        else if (lightType == 1u && lightIndex < NumSpotLights)
        {
            localLight += EvaluateToonSpotLight(
                SpotLights[lightIndex], normal, viewDir, worldPos, baseColor,
                material, receivesShadow);
        }
    }
    return localLight;
}

float SampleRayTracedShadowMask(float4 screenPosition)
{
    if (RayTracedShadowParams.x <= 0.5)
    {
        return 1.0;
    }

    uint width = 0;
    uint height = 0;
    RayTracedShadowMaskTexture.GetDimensions(width, height);
    if (width == 0 || height == 0)
    {
        return 1.0;
    }

    int2 pixel = int2(screenPosition.xy);
    int2 maxPixel = int2((int)width - 1, (int)height - 1);
    pixel = clamp(pixel, int2(0, 0), maxPixel);

    int filterRadius = (int)round(clamp(RayTracedShadowParams.y, 0.0, 3.0));
    if (filterRadius <= 0)
    {
        return saturate(RayTracedShadowMaskTexture.Load(int3(pixel, 0)).r);
    }

    float visibility = 0.0;
    visibility += RayTracedShadowMaskTexture.Load(int3(pixel, 0)).r * 4.0;
    visibility += RayTracedShadowMaskTexture.Load(int3(clamp(pixel + int2( filterRadius, 0), int2(0, 0), maxPixel), 0)).r * 2.0;
    visibility += RayTracedShadowMaskTexture.Load(int3(clamp(pixel + int2(-filterRadius, 0), int2(0, 0), maxPixel), 0)).r * 2.0;
    visibility += RayTracedShadowMaskTexture.Load(int3(clamp(pixel + int2(0,  filterRadius), int2(0, 0), maxPixel), 0)).r * 2.0;
    visibility += RayTracedShadowMaskTexture.Load(int3(clamp(pixel + int2(0, -filterRadius), int2(0, 0), maxPixel), 0)).r * 2.0;
    visibility += RayTracedShadowMaskTexture.Load(int3(clamp(pixel + int2( filterRadius,  filterRadius), int2(0, 0), maxPixel), 0)).r;
    visibility += RayTracedShadowMaskTexture.Load(int3(clamp(pixel + int2(-filterRadius,  filterRadius), int2(0, 0), maxPixel), 0)).r;
    visibility += RayTracedShadowMaskTexture.Load(int3(clamp(pixel + int2( filterRadius, -filterRadius), int2(0, 0), maxPixel), 0)).r;
    visibility += RayTracedShadowMaskTexture.Load(int3(clamp(pixel + int2(-filterRadius, -filterRadius), int2(0, 0), maxPixel), 0)).r;

    return saturate(visibility * (1.0 / 16.0));
}

float ComposeDirectionalShadowVisibility(float rasterVisibility, float rayTracedVisibility)
{
    if (RayTracedShadowParams.x <= 0.5)
    {
        return rasterVisibility;
    }

    const float compositionMode = round(RayTracedShadowParams.z);
    if (compositionMode >= 1.0)
    {
        return rayTracedVisibility;
    }

    return rasterVisibility * rayTracedVisibility;
}

MaterialParameterData ResolveMaterialParameters(uint materialSlot)
{
    if (materialSlot != 0xFFFFFFFFu)
    {
        return MaterialParameterTable[materialSlot];
    }

    MaterialParameterData material;
    material.BaseColorFactor = BaseColorFactor;
    material.MetallicFactor = MetallicFactor;
    material.RoughnessFactor = RoughnessFactor;
    material.NormalScale = NormalScale;
    material.OcclusionStrength = OcclusionStrength;
    material.EmissiveColor_Strength = EmissiveColor_Strength;
    material.TextureFlags = TextureFlags;
    material.AlphaMode = AlphaMode;
    material.AlphaCutoff = AlphaCutoff;
    material.Workflow = Workflow;
    material.DoubleSided = DoubleSided;
    material.ShadingModel = ShadingModel;
    material.MaterialTableIndex = MaterialTableIndex;
    material.ToonBands = ToonBands;
    material.ToonSpecularThreshold = ToonSpecularThreshold;
    material.ToonRimStrength = ToonRimStrength;
    material.ToonOutlineWidth = ToonOutlineWidth;
    material.BlendMode = BlendMode;
    material.ToonShadowTint = ToonShadowTint;
    material.ThinTransmissionParams = ThinTransmissionParams;
    material.AttenuationColor = AttenuationColor;
    material.BaseColorUVScaleOffset = BaseColorUVScaleOffset;
    material.BaseColorUVRotationSet = BaseColorUVRotationSet;
    material.NormalUVScaleOffset = NormalUVScaleOffset;
    material.NormalUVRotationSet = NormalUVRotationSet;
    material.MetallicRoughnessUVScaleOffset =
        MetallicRoughnessUVScaleOffset;
    material.MetallicRoughnessUVRotationSet =
        MetallicRoughnessUVRotationSet;
    material.OcclusionUVScaleOffset = OcclusionUVScaleOffset;
    material.OcclusionUVRotationSet = OcclusionUVRotationSet;
    material.EmissiveUVScaleOffset = EmissiveUVScaleOffset;
    material.EmissiveUVRotationSet = EmissiveUVRotationSet;
    material.ToonRampUVScaleOffset = ToonRampUVScaleOffset;
    material.ToonRampUVRotationSet = ToonRampUVRotationSet;
    material.TransmissionUVScaleOffset = TransmissionUVScaleOffset;
    material.TransmissionUVRotationSet = TransmissionUVRotationSet;
    material.ThicknessUVScaleOffset = ThicknessUVScaleOffset;
    material.ThicknessUVRotationSet = ThicknessUVRotationSet;
    return material;
}

float SampleScreenSpaceAmbientOcclusion(float4 screenPosition)
{
    uint width = 0;
    uint height = 0;
    AmbientOcclusionTexture.GetDimensions(width, height);
    if (width == 0 || height == 0)
    {
        // PipelineCache normally binds a 1.0 fallback for non-opaque
        // surfaces. Keep the shader-side backstop identity-preserving.
        return 1.0;
    }

    const int2 maximumPixel = int2((int)width - 1, (int)height - 1);
    const int2 pixel = clamp(int2(screenPosition.xy), int2(0, 0), maximumPixel);
    return RVX_SaturateAmbientOcclusion(
        AmbientOcclusionTexture.Load(int3(pixel, 0)));
}

struct GBufferOutput
{
    float4 baseColorAO : SV_Target0;
    float4 normalRoughnessMetallic : SV_Target1;
    float4 emissiveReserved : SV_Target2;
    uint materialTableIndex : SV_Target3;
};

float3 RVXApplyVolumetricSurfaceExtinction(
    float4 pixelPosition,
    float3 surfaceRadiance)
{
    uint froxelWidth = 0u;
    uint froxelHeight = 0u;
    uint froxelDepth = 0u;
    FroxelVolumetricLightingTexture.GetDimensions(
        froxelWidth, froxelHeight, froxelDepth);
    if (froxelWidth == 0u || froxelHeight == 0u || froxelDepth == 0u)
    {
        return surfaceRadiance;
    }

    const float2 viewportExtent = max(ClusterScreenParams.xy, 1.0f.xx);
    const float2 screenUV = saturate(pixelPosition.xy / viewportExtent);
    const float canonicalDepth = ReverseZ > 0.5f
        ? 1.0f - saturate(pixelPosition.z)
        : saturate(pixelPosition.z);
    const float4 integrated = FroxelVolumetricLightingTexture.SampleLevel(
        IBLLinearClampSampler,
        float3(screenUV, canonicalDepth),
        0.0f);
    // The opaque scene below a late surface already contains the volume's
    // in-scattering. Only attenuate this surface's own radiance here; adding
    // integrated.rgb a second time would double the participating medium.
    return surfaceRadiance * saturate(integrated.a);
}

float2 EncodeGBufferOctNormal(float3 normal)
{
    normal /= max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1.0e-6f);
    float2 encoded = normal.xy;
    if (normal.z < 0.0f)
    {
        encoded = (1.0f - abs(encoded.yx)) *
                  float2(encoded.x >= 0.0f ? 1.0f : -1.0f,
                         encoded.y >= 0.0f ? 1.0f : -1.0f);
    }
    return encoded * 0.5f + 0.5f;
}

// Texture/material evaluation is deliberately shared by every opaque export
// path. This keeps Forward+ surface-data, Deferred GBuffer and ordinary
// Forward HDR shading on the same UV, alpha, normal-map and double-sided ABI.
struct ResolvedForwardSurfaceData
{
    MaterialParameterData material;
    float4 baseColor;
    float metallic;
    float roughness;
    float occlusion;
    float3 emissive;
    float3 normal;
};

ResolvedForwardSurfaceData ResolveForwardSurfaceData(
    PSInput input,
    bool isFrontFace)
{
    ResolvedForwardSurfaceData surface;
    surface.material = ResolveMaterialParameters(input.MaterialParameterSlot);
    const float2 baseColorUV = RVXResolveMaterialUV(
        surface.material,
        MATERIAL_TEXTURE_SLOT_BASE_COLOR,
        input.TexCoord,
        input.TexCoord1);
    surface.baseColor = surface.material.BaseColorFactor;
    if ((surface.material.TextureFlags & MATERIAL_TEXTURE_BASE_COLOR) != 0)
    {
        surface.baseColor *= BaseColorTexture.Sample(BaseColorSampler, baseColorUV);
    }
    RVXApplyAlphaMask(surface.material, surface.baseColor.a);

    surface.metallic = surface.material.MetallicFactor;
    surface.roughness = surface.material.RoughnessFactor;
    if ((surface.material.TextureFlags & MATERIAL_TEXTURE_METALLIC_ROUGHNESS) != 0)
    {
        const float2 uv = RVXResolveMaterialUV(
            surface.material,
            MATERIAL_TEXTURE_SLOT_METALLIC_ROUGHNESS,
            input.TexCoord,
            input.TexCoord1);
        const float4 metallicRoughness = MetallicRoughnessTexture.Sample(
            MetallicRoughnessSampler, uv);
        surface.roughness *= metallicRoughness.g;
        surface.metallic *= metallicRoughness.b;
    }

    surface.occlusion = 1.0f;
    if ((surface.material.TextureFlags & MATERIAL_TEXTURE_OCCLUSION) != 0)
    {
        surface.occlusion = lerp(
            1.0f,
            OcclusionTexture.Sample(
                OcclusionSampler,
                RVXResolveMaterialUV(
                    surface.material,
                    MATERIAL_TEXTURE_SLOT_OCCLUSION,
                    input.TexCoord,
                    input.TexCoord1)).r,
            surface.material.OcclusionStrength);
    }

    surface.emissive = surface.material.EmissiveColor_Strength.xyz *
                        surface.material.EmissiveColor_Strength.w;
    if ((surface.material.TextureFlags & MATERIAL_TEXTURE_EMISSIVE) != 0)
    {
        surface.emissive *= EmissiveTexture.Sample(
            EmissiveSampler,
            RVXResolveMaterialUV(
                surface.material,
                MATERIAL_TEXTURE_SLOT_EMISSIVE,
                input.TexCoord,
                input.TexCoord1)).rgb;
    }

    surface.normal = SafeNormalize(
        input.WorldNormal, float3(0.0f, 0.0f, 1.0f));
    float4 doubleSidedTangent = input.WorldTangent;
    RVXResolveFrontFaceBasis(
        isFrontFace,
        surface.material.DoubleSided,
        surface.normal,
        doubleSidedTangent);
    if ((surface.material.TextureFlags & MATERIAL_TEXTURE_NORMAL) != 0)
    {
        surface.normal = RVXSampleNormalMap(
            NormalTexture,
            NormalSampler,
            RVXResolveMaterialUV(
                surface.material,
                MATERIAL_TEXTURE_SLOT_NORMAL,
                input.TexCoord,
                input.TexCoord1),
            input.WorldPos,
            surface.normal,
            doubleSidedTangent,
            surface.material.NormalScale);
    }
    return surface;
}

/**
 * Canonical deferred surface export. It deliberately uses the same typed UV,
 * alpha-mask, front-face and normal-map helpers as PSMain. CPU routing permits
 * StandardLit and ToonLit opaque/masked packets only. The shading-model and
 * legacy workflow checks remain shader-side fail-closed backstops.
 */
GBufferOutput PSGBuffer(PSInput input, bool isFrontFace : SV_IsFrontFace)
{
    const ResolvedForwardSurfaceData surface = ResolveForwardSurfaceData(
        input, isFrontFace);
    if (surface.material.Workflow != MATERIAL_WORKFLOW_METALLIC_ROUGHNESS ||
        (surface.material.ShadingModel != MATERIAL_SHADING_MODEL_STANDARD_LIT &&
         surface.material.ShadingModel != MATERIAL_SHADING_MODEL_TOON_LIT))
    {
        clip(-1.0f);
    }

    GBufferOutput output;
    output.baseColorAO = float4(surface.baseColor.rgb, saturate(surface.occlusion));
    output.normalRoughnessMetallic = float4(
        EncodeGBufferOctNormal(SafeNormalize(
            surface.normal, float3(0.0f, 0.0f, 1.0f))),
        clamp(surface.roughness, 0.04f, 1.0f),
        saturate(surface.metallic));
    // GBuffer2.w is the reserved per-surface flag lane. Preserve the frozen
    // object-level shadow receiver policy so Deferred and Forward+ consume
    // the same semantic input without changing the stable material identity.
    output.emissiveReserved = float4(
        surface.emissive,
        input.ReceivesShadowValue > 0.5f ? 1.0f : 0.0f);
    // MaterialSystem publishes descriptor-only rows by stable handle slot.
    // Preserve that slot exactly so Deferred and screen-space consumers cannot
    // infer a shading model from legacy workflow parameters.
    output.materialTableIndex = surface.material.MaterialTableIndex;
    return output;
}

float4 ShadeForwardSurface(
    PSInput input,
    ResolvedForwardSurfaceData surface)
{
    const MaterialParameterData material = surface.material;
    const float4 baseColor = surface.baseColor;
    const float metallic = surface.metallic;
    const float roughness = surface.roughness;
    const float occlusion = surface.occlusion;
    const float3 emissive = surface.emissive;

    const RVXExposureState exposureState = ExposureStateBuffer[0];
    if (!RVXExposureStateIsValid(exposureState))
    {
        // A missing or malformed state must never silently become unit
        // exposure. The graph-side contract rejects absent state; this is the
        // shader-side fail-closed backstop for corrupt payloads.
        clip(-1.0);
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float preExposure = RVXExposureProducerScale(exposureState);

    if (material.ShadingModel == MATERIAL_SHADING_MODEL_UNLIT)
    {
        if (material.Workflow != MATERIAL_WORKFLOW_UNLIT)
        {
            clip(-1.0f);
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        const float3 unlit = (baseColor.rgb + emissive) * preExposure;
        return float4(
            RVXApplyVolumetricSurfaceExtinction(input.Position, unlit),
            baseColor.a);
    }

    if ((material.ShadingModel != MATERIAL_SHADING_MODEL_STANDARD_LIT &&
         material.ShadingModel != MATERIAL_SHADING_MODEL_TOON_LIT) ||
        material.Workflow != MATERIAL_WORKFLOW_METALLIC_ROUGHNESS)
    {
        // Unsupported workflows must never be approximated as StandardLit.
        // The CPU policy rejects these before recording; clip is a defensive
        // shader-side fail-closed guard for malformed/stale material data.
        clip(-1.0f);
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const float3 viewDir = SafeNormalize(
        CameraPosition - input.WorldPos, float3(0.0, 0.0, 1.0));
    const float3 normal = surface.normal;
    float3 toLight = SafeNormalize(-LightDirection, float3(0.0, 1.0, 0.0));
    float clampedRoughness = clamp(roughness, 0.04, 1.0);
    float3 f0 = ComputeF0(baseColor.rgb, metallic);

    const bool receivesShadow = input.ReceivesShadowValue > 0.5;
    const float rasterShadowVisibility =
        receivesShadow ? SampleDirectionalShadow(input.WorldPos, normal) : 1.0;
    const float rayTracedShadowVisibility =
        receivesShadow ? SampleRayTracedShadowMask(input.Position) : 1.0;
    const float shadowVisibility =
        ComposeDirectionalShadowVisibility(rasterShadowVisibility, rayTracedShadowVisibility);
    float3 directLight;
    if (material.ShadingModel == MATERIAL_SHADING_MODEL_TOON_LIT)
    {
        directLight = RVX_EvaluateToonDirectLight(
            normal,
            viewDir,
            toLight,
            baseColor.rgb,
            DirectionalLightColor * DirectionalLightIntensity,
            shadowVisibility,
            material.ToonBands,
            material.ToonSpecularThreshold,
            material.ToonRimStrength,
            material.ToonShadowTint.xyz);
        directLight += EvaluateToonClusteredLocalLights(
            input.Position,
            normal,
            viewDir,
            input.WorldPos,
            baseColor.rgb,
            material,
            receivesShadow);
    }
    else
    {
        directLight = EvaluatePBR(
            normal,
            viewDir,
            toLight,
            baseColor.rgb,
            metallic,
            clampedRoughness,
            DirectionalLightColor * DirectionalLightIntensity,
            shadowVisibility);
        directLight += EvaluateClusteredLocalLights(
            input.Position,
            normal,
            viewDir,
            input.WorldPos,
            baseColor.rgb,
            metallic,
            clampedRoughness,
            receivesShadow);
    }

    float nDotV = max(dot(normal, viewDir), 0.001);
    const float screenAmbientOcclusion =
        SampleScreenSpaceAmbientOcclusion(input.Position);
    float3 fresnel = F_SchlickRoughness(nDotV, f0, clampedRoughness);
    float3 ambientDiffuse;
    float3 ambientSpecular;
    if (IBLTextureParams.x > 0.5)
    {
        float3 irradiance = IrradianceTexture.SampleLevel(
            IBLLinearClampSampler,
            normal,
            0.0f).rgb * IBLTextureParams.z;
        float prefilteredMip = clampedRoughness * max(IBLTextureParams.y - 1.0, 0.0);
        float3 reflectionDir = reflect(-viewDir, normal);
        float3 prefilteredColor = PrefilteredEnvironmentTexture.SampleLevel(
            IBLLinearClampSampler,
            reflectionDir,
            prefilteredMip).rgb * IBLTextureParams.z;
        float2 brdf = BRDFLUTTexture.SampleLevel(
            IBLLinearClampSampler,
            float2(nDotV, clampedRoughness),
            0.0f).rg;

        float3 diffuseEnergy = baseColor.rgb * (1.0 - fresnel) * (1.0 - metallic);
        ambientDiffuse = RVX_ApplyIndirectDiffuseAmbientOcclusion(
            diffuseEnergy * irradiance * occlusion,
            screenAmbientOcclusion);
        ambientSpecular = RVX_ApplyIndirectSpecularAmbientOcclusion(
            prefilteredColor * (fresnel * brdf.x + brdf.y) * occlusion,
            screenAmbientOcclusion,
            clampedRoughness,
            nDotV);
    }
    else
    {
        float3 iblDiffuse = IBLDiffuseAmbient.rgb * IBLDiffuseAmbient.a;
        float3 iblSpecular = IBLSpecularAmbient.rgb * IBLSpecularAmbient.a;
        ambientDiffuse = RVX_ApplyIndirectDiffuseAmbientOcclusion(
            baseColor.rgb * (1.0 - fresnel) * (1.0 - metallic) *
                occlusion * iblDiffuse,
            screenAmbientOcclusion);
        ambientSpecular = RVX_ApplyIndirectSpecularAmbientOcclusion(
            f0 * occlusion * (1.0 - clampedRoughness) * iblSpecular,
            screenAmbientOcclusion,
            clampedRoughness,
            nDotV);
    }
    float3 ambientFloor = RVX_ApplyIndirectDiffuseAmbientOcclusion(
        baseColor.rgb * max(IBLTextureParams.w, 0.0),
        screenAmbientOcclusion);
    float3 finalColor = ambientDiffuse + ambientSpecular + directLight + emissive + ambientFloor;

    return float4(
        RVXApplyVolumetricSurfaceExtinction(
            input.Position, finalColor * preExposure),
        baseColor.a);
}

float4 PSMain(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_TARGET
{
    return ShadeForwardSurface(
        input, ResolveForwardSurfaceData(input, isFrontFace));
}

struct ForwardTranslucencyOutput
{
    float4 hdrSceneColor : SV_Target0;
    float reactiveMask : SV_Target1;
};

float3 RVXApplyThinTransmission(
    PSInput input,
    ResolvedForwardSurfaceData surface,
    float3 litColor)
{
    float transmission = saturate(surface.material.ThinTransmissionParams.x);
    if ((surface.material.TextureFlags & MATERIAL_TEXTURE_TRANSMISSION) != 0u)
    {
        transmission *= TransmissionTexture.Sample(
            TransmissionSampler,
            RVXResolveMaterialUV(surface.material,
                                 MATERIAL_TEXTURE_SLOT_TRANSMISSION,
                                 input.TexCoord,
                                 input.TexCoord1)).r;
    }
    if (surface.material.ShadingModel != MATERIAL_SHADING_MODEL_THIN_TRANSMISSION ||
        transmission <= 0.0f)
    {
        return litColor;
    }

    uint pyramidWidth = 1u;
    uint pyramidHeight = 1u;
    uint pyramidMipCount = 1u;
    OpaqueSceneColorPyramidTexture.GetDimensions(
        0u, pyramidWidth, pyramidHeight, pyramidMipCount);
    // ClusterScreenParams.xy is the frozen full-resolution scene/source
    // extent. It remains the UV denominator even when the pyramid base is
    // half resolution on High, so refraction never treats mip-zero texels as
    // screen pixels.
    const float2 sourceExtent = max(ClusterScreenParams.xy, 1.0f.xx);
    const float ior = max(surface.material.ThinTransmissionParams.y, 1.0f);
    float thickness = max(surface.material.ThinTransmissionParams.z, 0.0f);
    if ((surface.material.TextureFlags & MATERIAL_TEXTURE_THICKNESS) != 0u)
    {
        thickness *= ThicknessTexture.Sample(
            ThicknessSampler,
            RVXResolveMaterialUV(surface.material,
                                 MATERIAL_TEXTURE_SLOT_THICKNESS,
                                 input.TexCoord,
                                 input.TexCoord1)).g;
    }
    const float attenuationDistance = surface.material.ThinTransmissionParams.w;
    const float3 viewDirection = RVXSafeNormalize(
        CameraPosition - input.WorldPos, float3(0.0f, 0.0f, 1.0f));
    const float f0 = pow((ior - 1.0f) / (ior + 1.0f), 2.0f);
    const float fresnel = f0 + (1.0f - f0) *
        pow(1.0f - saturate(dot(surface.normal, viewDirection)), 5.0f);
    const float refractionScale = (ior - 1.0f) / ior *
        (0.5f + saturate(surface.roughness));
    const float2 baseUV = saturate(input.Position.xy / sourceExtent);
    const float4 baseClip = mul(float4(input.WorldPos, 1.0f), ViewProjection);
    const float4 normalClip = mul(
        float4(input.WorldPos + surface.normal, 1.0f), ViewProjection);
    const float2 screenNormal = (normalClip.xy / max(abs(normalClip.w), 1.0e-4f) -
                                 baseClip.xy / max(abs(baseClip.w), 1.0e-4f)) *
        float2(0.5f, -0.5f);
    const float2 offset = screenNormal * refractionScale * thickness;
    const float refractionLod = saturate(surface.roughness * surface.roughness) *
        max((float)pyramidMipCount - 1.0f, 0.0f);
    const float3 refracted = OpaqueSceneColorPyramidTexture.SampleLevel(
        IBLLinearClampSampler, saturate(baseUV + offset), refractionLod).rgb;
    // attenuationDistance==0 is the canonical infinite-distance/no-absorption case.
    const float3 absorption = attenuationDistance > 0.0f
        ? exp(-max(thickness, 0.0f) / attenuationDistance *
              max(-log(max(surface.material.AttenuationColor.xyz, 1.0e-4f)), 0.0f))
        : float3(1.0f, 1.0f, 1.0f);
    const float3 transmitted = refracted * absorption;
    return lerp(transmitted, litColor, saturate(fresnel + (1.0f - transmission)));
}

ForwardTranslucencyOutput PSForwardTranslucency(
    PSInput input,
    bool isFrontFace : SV_IsFrontFace)
{
    const ResolvedForwardSurfaceData surface = ResolveForwardSurfaceData(input, isFrontFace);
    ForwardTranslucencyOutput output;
    const float3 litColor = ShadeForwardSurface(input, surface).rgb;
    float3 outputColor = RVXApplyThinTransmission(input, surface, litColor);
    if (surface.material.BlendMode == MATERIAL_BLEND_PREMULTIPLIED)
    {
        outputColor *= surface.baseColor.a;
    }
    output.hdrSceneColor = float4(outputColor, surface.baseColor.a);
    // The MRT is intentionally an overwrite R8 target, so any executed
    // transparent fragment suppresses stale temporal history at that pixel.
    output.reactiveMask = 1.0f;
    return output;
}

RVXWeightedBlendedOITTargets PSWeightedBlendedOIT(
    PSInput input,
    bool isFrontFace : SV_IsFrontFace)
{
    const ResolvedForwardSurfaceData surface =
        ResolveForwardSurfaceData(input, isFrontFace);
    const bool eligibleShadingModel =
        surface.material.ShadingModel == MATERIAL_SHADING_MODEL_STANDARD_LIT ||
        surface.material.ShadingModel == MATERIAL_SHADING_MODEL_UNLIT;
    const bool eligibleBlendMode =
        surface.material.BlendMode == MATERIAL_BLEND_ALPHA ||
        surface.material.BlendMode == MATERIAL_BLEND_PREMULTIPLIED;

    // The immutable CPU plan owns routing. This guard is a fail-safe against
    // accidentally sending refractive, additive, masked, or Toon content into
    // the order-independent accumulation targets.
    if (!eligibleShadingModel || !eligibleBlendMode)
    {
        discard;
    }

    const float3 straightColor = ShadeForwardSurface(input, surface).rgb;
    return RVXEncodeWeightedBlendedOIT(
        straightColor,
        surface.baseColor.a,
        RVXCanonicalizeWeightedBlendedOITDepth(
            input.Position.z, ReverseZ > 0.5f));
}

/**
 * Exact materialization data for reflection replacement. The first term is
 * the pre-exposed indirect-specular contribution already present in scene
 * HDR. The second is dimensionless and can materialize a screen-space or
 * local-probe radiance source without reapplying Fresnel, BRDF, material AO,
 * or specular AO.
 */
struct IndirectSpecularSurfaceData
{
    float3 baseIndirectSpecular;
    float3 specularResponse;
};

IndirectSpecularSurfaceData EvaluateForwardIndirectSpecular(
    PSInput input,
    ResolvedForwardSurfaceData surface)
{
    IndirectSpecularSurfaceData output;
    output.baseIndirectSpecular = 0.0f;
    output.specularResponse = 0.0f;

    const MaterialParameterData material = surface.material;
    if ((material.ShadingModel != MATERIAL_SHADING_MODEL_STANDARD_LIT &&
         material.ShadingModel != MATERIAL_SHADING_MODEL_TOON_LIT) ||
        material.Workflow != MATERIAL_WORKFLOW_METALLIC_ROUGHNESS)
    {
        return output;
    }

    const RVXExposureState exposureState = ExposureStateBuffer[0];
    if (!RVXExposureStateIsValid(exposureState))
    {
        return output;
    }

    const float3 viewDir = SafeNormalize(
        CameraPosition - input.WorldPos, float3(0.0f, 0.0f, 1.0f));
    const float3 normal = surface.normal;
    const float roughness = clamp(surface.roughness, 0.04f, 1.0f);
    const float3 f0 = ComputeF0(surface.baseColor.rgb, surface.metallic);
    const float nDotV = max(dot(normal, viewDir), 0.001f);
    const float screenAmbientOcclusion =
        SampleScreenSpaceAmbientOcclusion(input.Position);
    const float specularAO = RVX_IndirectSpecularOcclusion(
        screenAmbientOcclusion, roughness, nDotV);

    float3 sourceRadiance;
    if (IBLTextureParams.x > 0.5f)
    {
        const float3 reflectionDir = reflect(-viewDir, normal);
        const float prefilteredMip = roughness *
            max(IBLTextureParams.y - 1.0f, 0.0f);
        sourceRadiance = PrefilteredEnvironmentTexture.SampleLevel(
            IBLLinearClampSampler, reflectionDir, prefilteredMip).rgb *
            IBLTextureParams.z;
        const float2 brdf = BRDFLUTTexture.SampleLevel(
            IBLLinearClampSampler, float2(nDotV, roughness), 0.0f).rg;
        output.specularResponse =
            (F_SchlickRoughness(nDotV, f0, roughness) * brdf.x + brdf.y) *
            surface.occlusion * specularAO;
    }
    else
    {
        sourceRadiance = IBLSpecularAmbient.rgb * IBLSpecularAmbient.a;
        output.specularResponse = f0 * surface.occlusion *
            (1.0f - roughness) * specularAO;
    }

    output.baseIndirectSpecular = sourceRadiance *
        output.specularResponse * RVXExposureProducerScale(exposureState);
    return output;
}

/**
 * Forward+ opaque/masked export for exact reflection replacement. The HDR
 * result comes from the exact same PBR/Toon function as PSMain; the remaining
 * MRTs publish the canonical surface guides, baseline, and response without
 * changing the material's raster route.
 */
struct ForwardSurfaceDataOutput
{
    float4 hdrSceneColor : SV_Target0;
    float4 normalRoughnessMetallic : SV_Target1;
    uint materialTableIndex : SV_Target2;
    float4 baseIndirectSpecular : SV_Target3;
    float4 specularResponse : SV_Target4;
};

ForwardSurfaceDataOutput PSForwardSurfaceData(
    PSInput input,
    bool isFrontFace : SV_IsFrontFace)
{
    const ResolvedForwardSurfaceData surface = ResolveForwardSurfaceData(
        input, isFrontFace);
    ForwardSurfaceDataOutput output;
    output.hdrSceneColor = ShadeForwardSurface(input, surface);
    output.normalRoughnessMetallic = float4(
        EncodeGBufferOctNormal(SafeNormalize(
            surface.normal, float3(0.0f, 0.0f, 1.0f))),
        clamp(surface.roughness, 0.04f, 1.0f),
        saturate(surface.metallic));
    output.materialTableIndex = surface.material.MaterialTableIndex;
    const IndirectSpecularSurfaceData indirectSpecular =
        EvaluateForwardIndirectSpecular(input, surface);
    output.baseIndirectSpecular = float4(
        indirectSpecular.baseIndirectSpecular, 1.0f);
    output.specularResponse = float4(
        indirectSpecular.specularResponse, 1.0f);
    return output;
}
