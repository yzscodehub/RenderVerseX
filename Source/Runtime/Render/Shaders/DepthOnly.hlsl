// =============================================================================
// DepthOnly.hlsl - Minimal depth-only vertex shader
// =============================================================================
//
// Descriptor sets:
//   set 0 / space0: frame data
//   set 1 / space1: object data
//
// Vertex inputs:
//   Slot 0: Position buffer (float3)
//   Slot 4: Bone indices buffer (uint4)
//   Slot 5: Bone weights buffer (float4)
//   Slot 6: GPU-driven global instance index buffer (uint)
//   Slot 2: Masked-depth UVs (float2)
// =============================================================================

// GPU-scene's private compatibility cbuffer retains this fixed tail. Direct
// raster skinning uses Include/Skinning.hlsli and never indexes that tail.
#define RVX_MAX_OBJECT_SKINNING_MATRICES 128

#include "Include/GPUInstanceData.hlsli"
#include "Include/SurfaceMaterial.hlsli"
#include "GPUDriven/GPUSceneRaster.hlsli"

cbuffer ViewConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float4 CameraPosition_Time;
    float4 LightDirection_Padding;
};

#define CameraPosition CameraPosition_Time.xyz
#define Time CameraPosition_Time.w
#define LightDirection LightDirection_Padding.xyz
#define Padding LightDirection_Padding.w

#if !defined(RVX_GPU_SCENE_RASTER)
cbuffer ObjectConstants : register(b0, space1)
{
    float4x4 World;
    float4x4 NormalMatrix;
    float4x4 PreviousWorldViewProjection;
    float4 ObjectVelocityParams;
    float4 SkinningParams; // x: enabled, y: matrix count, z/w: current/previous offsets
};
#endif

#if !defined(RVX_GPU_SCENE_RASTER)
StructuredBuffer<GPUInstanceData> GPUDrivenInstances : register(t1, space1);
#endif

#include "Include/Skinning.hlsli"

struct VSInput
{
    float3 Position : POSITION;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
};

struct RigidDirectVSInput
{
    float3 Position : POSITION;
};

struct RigidVSInput
{
    float3 Position : POSITION;
    uint InstanceIndex : INSTANCE_INDEX;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
};

struct MaskedVSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float2 TexCoord1 : TEXCOORD1;
    uint4 BoneIndices : BLENDINDICES;
    float4 BoneWeights : BLENDWEIGHT;
};

struct MaskedRigidVSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float2 TexCoord1 : TEXCOORD1;
};

struct MaskedVSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
    float2 TexCoord1 : TEXCOORD1;
};

cbuffer MaterialConstants : register(b0, space2)
{
    RVX_MATERIAL_PARAMETER_FIELDS;
};

Texture2D BaseColorTexture : register(t1, space2);
SamplerState MaterialSampler : register(s9, space2);

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 worldPosition = mul(
        World,
        RVXResolveCurrentSkinningPosition(
            input.Position, input.BoneIndices, input.BoneWeights));
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}

VSOutput VSMainRigid(RigidDirectVSInput input)
{
    VSOutput output;
    const float4 worldPosition = RVXTransformRigidAffinePosition(
        World[0], World[1], World[2], input.Position);
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}

#if !defined(RVX_GPU_SCENE_RASTER)
VSOutput VSMainGPUDriven(
    RigidVSInput input)
{
    VSOutput output;
    const float4x4 world = GPUDrivenInstances[input.InstanceIndex].worldMatrix;
    const float4 worldPosition = RVXTransformRigidAffinePosition(
        world[0], world[1], world[2], input.Position);
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}
#endif

#if defined(RVX_GPU_SCENE_RASTER)
VSOutput VSMainGPUScene(RigidVSInput input)
{
    VSOutput output;
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
        return output;
    }

    const float4 worldPosition = GPUSceneTransformPosition(transform, input.Position);
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}

#endif

MaskedVSOutput VSMainMasked(MaskedVSInput input)
{
    MaskedVSOutput output;
    float4 worldPosition = mul(
        World,
        RVXResolveCurrentSkinningPosition(
            input.Position, input.BoneIndices, input.BoneWeights));
    output.Position = mul(ViewProjection, worldPosition);
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    return output;
}

MaskedVSOutput VSMainMaskedRigid(MaskedRigidVSInput input)
{
    MaskedVSOutput output;
    const float4 worldPosition = RVXTransformRigidAffinePosition(
        World[0], World[1], World[2], input.Position);
    output.Position = mul(ViewProjection, worldPosition);
    output.TexCoord = input.TexCoord;
    output.TexCoord1 = input.TexCoord1;
    return output;
}

MaterialParameterData ResolveMaterialParameters()
{
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
    material.MetallicRoughnessUVScaleOffset = MetallicRoughnessUVScaleOffset;
    material.MetallicRoughnessUVRotationSet = MetallicRoughnessUVRotationSet;
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

void PSMainMasked(
    MaskedVSOutput input,
    bool isFrontFace : SV_IsFrontFace)
{
    const MaterialParameterData material = ResolveMaterialParameters();
    const float2 baseColorUV = RVXResolveMaterialUV(
        material,
        MATERIAL_TEXTURE_SLOT_BASE_COLOR,
        input.TexCoord,
        input.TexCoord1);
    const float sampledAlpha =
        (material.TextureFlags & MATERIAL_TEXTURE_BASE_COLOR) != 0u
            ? BaseColorTexture.Sample(MaterialSampler, baseColorUV).a
            : 1.0f;

    // Keep the face-orientation contract identical to the color path even
    // though depth-only shading does not consume the resulting basis.
    float3 faceNormal = float3(0.0f, 0.0f, 1.0f);
    float4 faceTangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
    RVXResolveFrontFaceBasis(
        isFrontFace,
        material.DoubleSided,
        faceNormal,
        faceTangent);
    RVXApplyAlphaMask(
        material,
        RVXResolveBaseColorAlpha(material, sampledAlpha));
}
