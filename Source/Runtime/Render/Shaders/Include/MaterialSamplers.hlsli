#ifndef RVX_MATERIAL_SAMPLERS_HLSLI
#define RVX_MATERIAL_SAMPLERS_HLSLI

// Material V2 has thirteen independent texture slots. DX11 has only sixteen
// pixel-stage sampler registers, eight of which are already reserved by the
// Forward+ frame ABI. The DX11 compact path therefore preserves all thirteen
// texture SRVs but aliases extension sampler registers to the corresponding
// base material sampler. MaterialSystem rejects an asset when an aliased pair
// uses different sampler policies; this shader include never creates an
// implicit sampler fallback.
#if defined(RVX_DX11_FLATTENED_REGISTER_SPACES)
SamplerState BaseColorSampler : register(s8);
SamplerState NormalSampler : register(s9);
SamplerState MetallicRoughnessSampler : register(s10);
SamplerState OcclusionSampler : register(s11);
SamplerState EmissiveSampler : register(s12);
SamplerState ToonRampSampler : register(s13);
SamplerState TransmissionSampler : register(s14);
SamplerState ThicknessSampler : register(s15);

#define SpecularSampler MetallicRoughnessSampler
#define SpecularColorSampler BaseColorSampler
#define ClearcoatSampler MetallicRoughnessSampler
#define ClearcoatRoughnessSampler MetallicRoughnessSampler
#define ClearcoatNormalSampler NormalSampler
#elif defined(RVX_COMPACT_MATERIAL_SAMPLERS)
// Metal preserves set-2 logical bindings through SPIR-V. Its direct ABI maps
// the eight primary samplers below to physical indices 0..7 and aliases the
// five extension samplers after MaterialSystem validates identical policies.
SamplerState BaseColorSampler : register(s14, space2);
SamplerState NormalSampler : register(s15, space2);
SamplerState MetallicRoughnessSampler : register(s16, space2);
SamplerState OcclusionSampler : register(s17, space2);
SamplerState EmissiveSampler : register(s18, space2);
SamplerState ToonRampSampler : register(s19, space2);
SamplerState TransmissionSampler : register(s20, space2);
SamplerState ThicknessSampler : register(s21, space2);

#define SpecularSampler MetallicRoughnessSampler
#define SpecularColorSampler BaseColorSampler
#define ClearcoatSampler MetallicRoughnessSampler
#define ClearcoatRoughnessSampler MetallicRoughnessSampler
#define ClearcoatNormalSampler NormalSampler
#else
SamplerState BaseColorSampler : register(s14, space2);
SamplerState NormalSampler : register(s15, space2);
SamplerState MetallicRoughnessSampler : register(s16, space2);
SamplerState OcclusionSampler : register(s17, space2);
SamplerState EmissiveSampler : register(s18, space2);
SamplerState ToonRampSampler : register(s19, space2);
SamplerState TransmissionSampler : register(s20, space2);
SamplerState ThicknessSampler : register(s21, space2);
SamplerState SpecularSampler : register(s22, space2);
SamplerState SpecularColorSampler : register(s23, space2);
SamplerState ClearcoatSampler : register(s24, space2);
SamplerState ClearcoatRoughnessSampler : register(s25, space2);
SamplerState ClearcoatNormalSampler : register(s26, space2);
#endif

#endif // RVX_MATERIAL_SAMPLERS_HLSLI
