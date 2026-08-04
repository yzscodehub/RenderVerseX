// =============================================================================
// DefaultLit.hlsl - Default lit material shader for model rendering
// =============================================================================
//
// Descriptor sets:
//   set 0 / space0: frame data
//   set 1 / space1: object data
//   set 2 / space2: material data and environment IBL textures
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

#define RVX_MAX_OBJECT_SKINNING_MATRICES 128

#include "Include/BRDF.hlsli"
#include "Include/GPUInstanceData.hlsli"
#include "Include/Lighting.hlsli"
#if defined(RVX_GPU_SCENE_RASTER)
#include "GPUDriven/GPUSceneRaster.hlsli"
#endif

#define MATERIAL_TEXTURE_BASE_COLOR 0x01
#define MATERIAL_TEXTURE_NORMAL 0x02
#define MATERIAL_TEXTURE_METALLIC_ROUGHNESS 0x04
#define MATERIAL_TEXTURE_OCCLUSION 0x08
#define MATERIAL_TEXTURE_EMISSIVE 0x10

#define MATERIAL_ALPHA_OPAQUE 0
#define MATERIAL_ALPHA_MASK 1
#define MATERIAL_ALPHA_BLEND 2

#define MATERIAL_WORKFLOW_METALLIC_ROUGHNESS 0
#define MATERIAL_WORKFLOW_SPECULAR_GLOSSINESS 1
#define MATERIAL_WORKFLOW_UNLIT 2

// =============================================================================
// Constant Buffers
// =============================================================================

cbuffer ViewConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float4 CameraPosition_Time;
    float4 LightDirection_Intensity;
    float4 DirectionalLightColor_Padding;
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
    float4 SkinningParams; // x: enabled, y: matrix count
    float4x4 SkinningMatrices[RVX_MAX_OBJECT_SKINNING_MATRICES];
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
    float4 BaseColorFactor;
    float MetallicFactor;
    float RoughnessFactor;
    float NormalScale;
    float OcclusionStrength;
    float4 EmissiveColor_Strength;
    uint TextureFlags;
    uint AlphaMode;
    float AlphaCutoff;
    uint Workflow;
    uint4 DoubleSided_MaterialPaddingBits;
};

#define CameraPosition CameraPosition_Time.xyz
#define Time CameraPosition_Time.w
#define ReceivesShadow ObjectVelocityParams.y
#define LightDirection LightDirection_Intensity.xyz
#define DirectionalLightIntensity LightDirection_Intensity.w
#define DirectionalLightColor DirectionalLightColor_Padding.xyz
#define DirectionalLightColorPadding DirectionalLightColor_Padding.w
#define EmissiveColor EmissiveColor_Strength.xyz
#define EmissiveStrength EmissiveColor_Strength.w
#define DoubleSided DoubleSided_MaterialPaddingBits.x
#define MaterialPadding asfloat(DoubleSided_MaterialPaddingBits.yzw)

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
SamplerState MaterialSampler : register(s6, space2);
TextureCube IrradianceTexture : register(t7, space2);
TextureCube PrefilteredEnvironmentTexture : register(t8, space2);
Texture2D BRDFLUTTexture : register(t9, space2);
Texture2DArray<float> DirectionalShadowMapTexture : register(t1, space0);
SamplerState DirectionalShadowSampler : register(s2, space0);
StructuredBuffer<PointLight> PointLights : register(t4, space0);
StructuredBuffer<SpotLight> SpotLights : register(t5, space0);
Texture2D<float> RayTracedShadowMaskTexture : register(t6, space0);
StructuredBuffer<GPUCluster> ClusterData : register(t8, space0);
StructuredBuffer<uint> ClusterLightIndices : register(t9, space0);
#if !defined(RVX_GPU_SCENE_RASTER)
StructuredBuffer<GPUInstanceData> GPUDrivenInstances : register(t1, space1);
#endif

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
};

// Direct rigid draws do not bind skinning or GPU-driven instance streams.
struct RigidDirectVSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent  : TANGENT;
};

struct RigidVSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent  : TANGENT;
    uint InstanceIndex : INSTANCE_INDEX;
};

struct PSInput
{
    float4 Position    : SV_POSITION;
    float3 WorldPos    : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord    : TEXCOORD2;
    float4 WorldTangent : TEXCOORD3;
};

// =============================================================================
// Vertex Shader
// =============================================================================

float4 ResolveSkinningPosition(float3 position, uint4 boneIndices, float4 boneWeights)
{
    if (SkinningParams.x <= 0.5f || SkinningParams.y <= 0.5f)
    {
        return float4(position, 1.0f);
    }

    const float weightSum = boneWeights.x + boneWeights.y + boneWeights.z + boneWeights.w;
    if (weightSum <= 1.0e-5f)
    {
        return float4(position, 1.0f);
    }

    const float4 weights = boneWeights / weightSum;
    const uint boneCount = (uint)SkinningParams.y;
    const uint4 safeIndices = min(boneIndices, boneCount - 1u);
    const float4 localPosition = float4(position, 1.0f);

    return mul(SkinningMatrices[safeIndices.x], localPosition) * weights.x +
           mul(SkinningMatrices[safeIndices.y], localPosition) * weights.y +
           mul(SkinningMatrices[safeIndices.z], localPosition) * weights.z +
           mul(SkinningMatrices[safeIndices.w], localPosition) * weights.w;
}

float3 ResolveSkinningVector(float3 vectorValue, uint4 boneIndices, float4 boneWeights)
{
    if (SkinningParams.x <= 0.5f || SkinningParams.y <= 0.5f)
    {
        return vectorValue;
    }

    const float weightSum = boneWeights.x + boneWeights.y + boneWeights.z + boneWeights.w;
    if (weightSum <= 1.0e-5f)
    {
        return vectorValue;
    }

    const float4 weights = boneWeights / weightSum;
    const uint boneCount = (uint)SkinningParams.y;
    const uint4 safeIndices = min(boneIndices, boneCount - 1u);
    const float4 localVector = float4(vectorValue, 0.0f);

    const float4 skinnedVector =
        mul(SkinningMatrices[safeIndices.x], localVector) * weights.x +
        mul(SkinningMatrices[safeIndices.y], localVector) * weights.y +
        mul(SkinningMatrices[safeIndices.z], localVector) * weights.z +
        mul(SkinningMatrices[safeIndices.w], localVector) * weights.w;

    return skinnedVector.xyz;
}

PSInput VSMain(VSInput input)
{
    PSInput output;

    const float4 localPosition = ResolveSkinningPosition(input.Position, input.BoneIndices, input.BoneWeights);
    const float3 localNormal = ResolveSkinningVector(input.Normal, input.BoneIndices, input.BoneWeights);
    const float3 localTangent = ResolveSkinningVector(input.Tangent.xyz, input.BoneIndices, input.BoneWeights);

    float4 worldPos = mul(World, localPosition);
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = normalize(mul((float3x3)NormalMatrix, localNormal));
    output.TexCoord = input.TexCoord;
    output.WorldTangent = float4(normalize(mul((float3x3)World, localTangent)), input.Tangent.w);

    return output;
}

PSInput VSMainRigid(RigidDirectVSInput input)
{
    PSInput output;

    float4 worldPos = mul(World, float4(input.Position, 1.0f));
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = normalize(mul((float3x3)NormalMatrix, input.Normal));
    output.TexCoord = input.TexCoord;
    output.WorldTangent = float4(normalize(mul((float3x3)World, input.Tangent.xyz)), input.Tangent.w);

    return output;
}

#if !defined(RVX_GPU_SCENE_RASTER)
PSInput VSMainGPUDriven(
    RigidVSInput input)
{
    GPUInstanceData instance = GPUDrivenInstances[input.InstanceIndex];

    PSInput output;

    float4 worldPos = mul(instance.worldMatrix, float4(input.Position, 1.0));
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = normalize(mul((float3x3)instance.normalMatrix, input.Normal));
    output.TexCoord = input.TexCoord;
    output.WorldTangent = float4(normalize(mul((float3x3)instance.worldMatrix, input.Tangent.xyz)), input.Tangent.w);

    return output;
}
#endif

#if defined(RVX_GPU_SCENE_RASTER)
PSInput VSMainGPUScene(RigidVSInput input)
{
    PSInput output;
    GPUSceneTransformRow transform;
    if (!GPUSceneResolveRasterTransform(input.InstanceIndex, transform))
    {
        output.Position = GPUSceneInvalidClipPosition();
        output.WorldPos = float3(0.0f, 0.0f, 0.0f);
        output.WorldNormal = float3(0.0f, 0.0f, 0.0f);
        output.TexCoord = float2(0.0f, 0.0f);
        output.WorldTangent = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return output;
    }

    const float4 worldPos = GPUSceneTransformPosition(transform, input.Position);
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = GPUSceneSafeNormalize(
        GPUSceneTransformNormal(transform, input.Normal),
        float3(0.0f, 0.0f, 1.0f));
    output.TexCoord = input.TexCoord;
    output.WorldTangent = float4(
        GPUSceneSafeNormalize(GPUSceneTransformTangent(transform, input.Tangent.xyz),
                               float3(1.0f, 0.0f, 0.0f)),
        input.Tangent.w);
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

float3 SampleNormalMap(float2 uv, float3 worldNormal, float4 worldTangent)
{
    float3 tangentNormal = NormalTexture.Sample(MaterialSampler, uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= NormalScale;

    float3 n = SafeNormalize(worldNormal, float3(0.0, 0.0, 1.0));
    float3 t = SafeNormalize(worldTangent.xyz, float3(1.0, 0.0, 0.0));
    t = SafeNormalize(t - n * dot(n, t), float3(1.0, 0.0, 0.0));

    float tangentSign = worldTangent.w >= 0.0 ? 1.0 : -1.0;
    float3 b = SafeNormalize(cross(n, t) * tangentSign, float3(0.0, 1.0, 0.0));
    float3x3 tbn = float3x3(t, b, n);

    return SafeNormalize(mul(tangentNormal, tbn), n);
}

int SelectDirectionalShadowCascade(float3 worldPos)
{
    int cascadeCount = (int)clamp(round(CameraForwardAndShadowCascadeCount.w), 1.0, 4.0);
    float3 cameraForward = SafeNormalize(CameraForwardAndShadowCascadeCount.xyz, float3(0.0, 0.0, -1.0));
    float viewDepth = dot(worldPos - CameraPosition, cameraForward);
    int cascadeIndex = 0;

    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        if (i + 1 < cascadeCount && viewDepth > DirectionalShadowCascadeSplits[i])
        {
            cascadeIndex = i + 1;
        }
    }

    return cascadeIndex;
}

float GetDirectionalShadowViewDepth(float3 worldPos)
{
    float3 cameraForward = SafeNormalize(CameraForwardAndShadowCascadeCount.xyz, float3(0.0, 0.0, -1.0));
    return dot(worldPos - CameraPosition, cameraForward);
}

float CompareDirectionalShadowDepth(float2 uv, float compareDepth, int cascadeIndex)
{
    float storedDepth = DirectionalShadowMapTexture.SampleLevel(
        DirectionalShadowSampler,
        float3(uv, (float)cascadeIndex),
        0).r;
    return compareDepth <= storedDepth ? 1.0 : 0.0;
}

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
    float roughness)
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
            roughness);
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
            1.0);
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
    float roughness)
{
    const uint clusterIndex = ResolveClusterIndex(svPosition, worldPos);
    if (clusterIndex == RVX_INVALID_CLUSTER_INDEX)
    {
        return EvaluateLinearLocalLights(normal, viewDir, worldPos, baseColor, metallic, roughness);
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
                roughness);
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
                1.0);
        }
    }

    return localLight;
}

static const int RVX_DIRECTIONAL_SHADOW_POISSON_TAP_COUNT = 16;
static const float2 RVX_DIRECTIONAL_SHADOW_POISSON_DISK[16] =
{
    float2(0.000000, 0.000000),
    float2(-0.326212, -0.405810),
    float2(-0.840144, -0.073580),
    float2(-0.695914, 0.457137),
    float2(-0.203345, 0.620716),
    float2(0.962340, -0.194983),
    float2(0.473434, -0.480026),
    float2(0.519456, 0.767022),
    float2(0.185461, -0.893124),
    float2(0.507431, 0.064425),
    float2(0.896420, 0.412458),
    float2(-0.321940, -0.932615),
    float2(-0.791559, -0.597710),
    float2(-0.040088, 0.536087),
    float2(0.342312, -0.217275),
    float2(-0.620000, 0.000000)
};

float SampleDirectionalShadowPCF(float2 shadowUV, float compareDepth, float filterStep, int cascadeIndex)
{
    float filterStepUv = max(filterStep, 0.0);
    if (filterStepUv <= 1.0e-7)
    {
        return CompareDirectionalShadowDepth(shadowUV, compareDepth, cascadeIndex);
    }

    float visibility = 0.0;
    float tapCount = 0.0;

    [unroll]
    for (int i = 0; i < RVX_DIRECTIONAL_SHADOW_POISSON_TAP_COUNT; ++i)
    {
        float2 tapUV = shadowUV + RVX_DIRECTIONAL_SHADOW_POISSON_DISK[i] * filterStepUv;
        if (!any(tapUV < 0.0) && !any(tapUV > 1.0))
        {
            visibility += CompareDirectionalShadowDepth(tapUV, compareDepth, cascadeIndex);
            tapCount += 1.0;
        }
    }

    return tapCount > 0.5 ? visibility / tapCount : 1.0;
}

float SampleDirectionalShadowCascade(float3 worldPos, float3 worldNormal, int cascadeIndex)
{
    float3 receiverNormal = SafeNormalize(worldNormal, float3(0.0, 1.0, 0.0));
    float normalBias = max(DirectionalShadowReceiverParams.x, 0.0);
    float3 biasedWorldPos = worldPos + receiverNormal * normalBias;
    float4 shadowClip = mul(DirectionalShadowViewProjections[cascadeIndex], float4(biasedWorldPos, 1.0));
    if (abs(shadowClip.w) <= 1.0e-6)
    {
        return 1.0;
    }

    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    float2 shadowUV = float2(shadowNdc.x * 0.5 + 0.5, 0.5 - shadowNdc.y * 0.5);
    if (any(shadowUV < 0.0) || any(shadowUV > 1.0) || shadowNdc.z < 0.0 || shadowNdc.z > 1.0)
    {
        return 1.0;
    }

    float compareDepth = shadowNdc.z - max(DirectionalShadowParams.y, 0.0);
    float lit = SampleDirectionalShadowPCF(shadowUV, compareDepth, DirectionalShadowParams.w, cascadeIndex);
    return lerp(1.0 - saturate(DirectionalShadowParams.z), 1.0, lit);
}

float SampleDirectionalShadow(float3 worldPos, float3 worldNormal)
{
    if (DirectionalShadowParams.x <= 0.5)
    {
        return 1.0;
    }

    int cascadeCount = (int)clamp(round(CameraForwardAndShadowCascadeCount.w), 1.0, 4.0);
    int cascadeIndex = SelectDirectionalShadowCascade(worldPos);
    float shadow = SampleDirectionalShadowCascade(worldPos, worldNormal, cascadeIndex);
    int nextCascadeIndex = cascadeIndex + 1;

    if (nextCascadeIndex < cascadeCount)
    {
        float viewDepth = GetDirectionalShadowViewDepth(worldPos);
        float splitDistance = DirectionalShadowCascadeSplits[cascadeIndex];
        float fadeDistance = DirectionalShadowCascadeFadeDistances[cascadeIndex];
        float fadeStart = splitDistance - fadeDistance;
        bool insideFadeBand = fadeDistance > 1.0e-5 &&
                              viewDepth >= fadeStart &&
                              viewDepth <= splitDistance;
        if (insideFadeBand)
        {
            float fadeT = saturate((viewDepth - fadeStart) / fadeDistance);
            fadeT = fadeT * fadeT * (3.0 - 2.0 * fadeT);
            float nextShadow = SampleDirectionalShadowCascade(worldPos, worldNormal, nextCascadeIndex);
            shadow = lerp(shadow, nextShadow, fadeT);
        }
    }

    return shadow;
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

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 baseColor = BaseColorFactor;
    if ((TextureFlags & MATERIAL_TEXTURE_BASE_COLOR) != 0)
    {
        baseColor *= BaseColorTexture.Sample(MaterialSampler, input.TexCoord);
    }

    if (AlphaMode == MATERIAL_ALPHA_MASK && baseColor.a < AlphaCutoff)
    {
        discard;
    }

    float metallic = MetallicFactor;
    float roughness = RoughnessFactor;
    if ((TextureFlags & MATERIAL_TEXTURE_METALLIC_ROUGHNESS) != 0)
    {
        float4 mr = MetallicRoughnessTexture.Sample(MaterialSampler, input.TexCoord);
        roughness *= mr.g;
        metallic *= mr.b;
    }

    float occlusion = 1.0;
    if ((TextureFlags & MATERIAL_TEXTURE_OCCLUSION) != 0)
    {
        occlusion = lerp(1.0, OcclusionTexture.Sample(MaterialSampler, input.TexCoord).r, OcclusionStrength);
    }

    float3 emissive = EmissiveColor * EmissiveStrength;
    if ((TextureFlags & MATERIAL_TEXTURE_EMISSIVE) != 0)
    {
        emissive *= EmissiveTexture.Sample(MaterialSampler, input.TexCoord).rgb;
    }

    if (Workflow == MATERIAL_WORKFLOW_UNLIT)
    {
        return float4(baseColor.rgb + emissive, baseColor.a);
    }

    if (Workflow == MATERIAL_WORKFLOW_SPECULAR_GLOSSINESS)
    {
        // Compatibility fallback until explicit specular/glossiness factors and textures exist.
        metallic = 0.0;
    }

    float3 viewDir = SafeNormalize(CameraPosition - input.WorldPos, float3(0.0, 0.0, 1.0));
    float3 normal = SafeNormalize(input.WorldNormal, float3(0.0, 0.0, 1.0));
    float4 doubleSidedTangent = input.WorldTangent;
    if (DoubleSided != 0 && dot(normal, viewDir) < 0.0)
    {
        normal = -normal;
        doubleSidedTangent.w = -doubleSidedTangent.w;
    }

    if ((TextureFlags & MATERIAL_TEXTURE_NORMAL) != 0)
    {
        normal = SampleNormalMap(input.TexCoord, normal, doubleSidedTangent);
    }

    float3 toLight = SafeNormalize(-LightDirection, float3(0.0, 1.0, 0.0));
    float clampedRoughness = clamp(roughness, 0.04, 1.0);
    float3 f0 = ComputeF0(baseColor.rgb, metallic);

    const bool receivesShadow = ReceivesShadow > 0.5;
    const float rasterShadowVisibility =
        receivesShadow ? SampleDirectionalShadow(input.WorldPos, normal) : 1.0;
    const float rayTracedShadowVisibility =
        receivesShadow ? SampleRayTracedShadowMask(input.Position) : 1.0;
    const float shadowVisibility =
        ComposeDirectionalShadowVisibility(rasterShadowVisibility, rayTracedShadowVisibility);
    float3 directLight = EvaluatePBR(
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
        clampedRoughness);

    float nDotV = max(dot(normal, viewDir), 0.001);
    float3 fresnel = F_SchlickRoughness(nDotV, f0, clampedRoughness);
    float3 ambientDiffuse;
    float3 ambientSpecular;
    if (IBLTextureParams.x > 0.5)
    {
        float3 irradiance = IrradianceTexture.Sample(MaterialSampler, normal).rgb * IBLTextureParams.z;
        float prefilteredMip = clampedRoughness * max(IBLTextureParams.y - 1.0, 0.0);
        float3 reflectionDir = reflect(-viewDir, normal);
        float3 prefilteredColor = PrefilteredEnvironmentTexture.SampleLevel(
            MaterialSampler,
            reflectionDir,
            prefilteredMip).rgb * IBLTextureParams.z;
        float2 brdf = BRDFLUTTexture.Sample(MaterialSampler, float2(nDotV, clampedRoughness)).rg;

        float3 diffuseEnergy = baseColor.rgb * (1.0 - fresnel) * (1.0 - metallic);
        ambientDiffuse = diffuseEnergy * irradiance * occlusion;
        ambientSpecular = prefilteredColor * (fresnel * brdf.x + brdf.y) * occlusion;
    }
    else
    {
        float3 iblDiffuse = IBLDiffuseAmbient.rgb * IBLDiffuseAmbient.a;
        float3 iblSpecular = IBLSpecularAmbient.rgb * IBLSpecularAmbient.a;
        ambientDiffuse = baseColor.rgb * (1.0 - fresnel) * (1.0 - metallic) * occlusion * iblDiffuse;
        ambientSpecular = f0 * occlusion * (1.0 - clampedRoughness) * iblSpecular;
    }
    float3 ambientFloor = baseColor.rgb * max(IBLTextureParams.w, 0.0);
    float3 finalColor = ambientDiffuse + ambientSpecular + directLight + emissive + ambientFloor;

    return float4(finalColor, baseColor.a);
}
