/** @file CookedMaterialArtifact.cpp @brief Strict cooked material serializer */

#include "Resource/Cooked/CookedMaterialArtifact.h"

#include "CookedArtifactIO.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string_view>

namespace RVX::Resource
{
namespace
{
    using CookedArtifactIO::Reader;
    using CookedArtifactIO::Writer;

    constexpr uint32 MaxTextureBindingCount = 128;

    bool IsZeroDigest(const Hash::SHA256Digest& digest) noexcept
    {
        return std::all_of(
            digest.begin(), digest.end(),
            [](uint8 value) { return value == 0u; });
    }

    template <typename Enum>
    bool IsEnumInRange(Enum value, Enum last) noexcept
    {
        return static_cast<uint32>(value) <= static_cast<uint32>(last);
    }

    bool IsCanonicalPackageRelativePath(const std::string& value)
    {
        if (value.empty() || value.find('\0') != std::string::npos ||
            value.find('\\') != std::string::npos || value.front() == '/' ||
            (value.size() >= 2u && value[1] == ':' &&
             ((value[0] >= 'A' && value[0] <= 'Z') ||
              (value[0] >= 'a' && value[0] <= 'z'))))
        {
            return false;
        }
        const std::filesystem::path path(value);
        if (path.is_absolute() || path.has_root_name() || path.has_root_path() ||
            path.generic_string() != value)
        {
            return false;
        }
        for (const std::filesystem::path& component : path)
        {
            if (component.empty() || component == "." || component == "..")
                return false;
        }
        return path.extension() == ".rva";
    }

    bool SameSlotDefinition(
        const MaterialTextureSlotDefinition& left,
        const MaterialTextureSlotDefinition& right) noexcept
    {
        return left.slot == right.slot && left.uvSet == right.uvSet &&
            left.offset == right.offset && left.scale == right.scale &&
            left.rotation == right.rotation && left.wrapS == right.wrapS &&
            left.wrapT == right.wrapT && left.minFilter == right.minFilter &&
            left.magFilter == right.magFilter;
    }

    bool HasExpectedTextureSemantic(
        const CookedMaterialTextureBinding& binding) noexcept
    {
        switch (binding.definition.slot)
        {
            case MaterialTextureSlot::BaseColor:
            case MaterialTextureSlot::ToonRamp:
            case MaterialTextureSlot::SpecularColor:
                return binding.usage == TextureUsage::Color && binding.isSRGB &&
                    binding.fallbackSemantic == TextureFallbackSemantic::White;

            case MaterialTextureSlot::Emissive:
                return binding.usage == TextureUsage::Color && binding.isSRGB &&
                    binding.fallbackSemantic == TextureFallbackSemantic::Black;

            case MaterialTextureSlot::Normal:
            case MaterialTextureSlot::ClearcoatNormal:
                return binding.usage == TextureUsage::Normal && !binding.isSRGB &&
                    binding.fallbackSemantic == TextureFallbackSemantic::FlatNormal;

            case MaterialTextureSlot::MetallicRoughness:
            case MaterialTextureSlot::Occlusion:
            case MaterialTextureSlot::Transmission:
            case MaterialTextureSlot::Thickness:
            case MaterialTextureSlot::Specular:
            case MaterialTextureSlot::Clearcoat:
            case MaterialTextureSlot::ClearcoatRoughness:
                return binding.usage == TextureUsage::Data && !binding.isSRGB &&
                    binding.fallbackSemantic == TextureFallbackSemantic::White;
        }
        return false;
    }

    Hash::SHA256Digest ComputeDependencyHash(
        const std::vector<CookedMaterialTextureBinding>& bindings)
    {
        Writer writer;
        std::string ignored;
        writer.U32(static_cast<uint32>(bindings.size()));
        for (const CookedMaterialTextureBinding& binding : bindings)
        {
            writer.U8(static_cast<uint8>(binding.definition.slot));
            (void)writer.String(binding.artifactPath, ignored);
            writer.Bytes(binding.expectedContentHash);
            writer.U8(static_cast<uint8>(binding.usage));
            writer.U8(binding.isSRGB ? 1u : 0u);
            writer.U8(static_cast<uint8>(binding.fallbackSemantic));
        }
        return Hash::ComputeSHA256(
            writer.Data().data(), writer.Data().size());
    }

    void WriteVec2(Writer& writer, const Vec2& value)
    {
        writer.Float(value.x);
        writer.Float(value.y);
    }

    void WriteVec3(Writer& writer, const Vec3& value)
    {
        writer.Float(value.x);
        writer.Float(value.y);
        writer.Float(value.z);
    }

    void WriteVec4(Writer& writer, const Vec4& value)
    {
        writer.Float(value.x);
        writer.Float(value.y);
        writer.Float(value.z);
        writer.Float(value.w);
    }

    bool ReadVec2(Reader& reader, Vec2& value)
    {
        return reader.Float(value.x) && reader.Float(value.y);
    }

    bool ReadVec3(Reader& reader, Vec3& value)
    {
        return reader.Float(value.x) && reader.Float(value.y) &&
            reader.Float(value.z);
    }

    bool ReadVec4(Reader& reader, Vec4& value)
    {
        return reader.Float(value.x) && reader.Float(value.y) &&
            reader.Float(value.z) && reader.Float(value.w);
    }

    bool WritePayload(
        const CookedMaterialArtifact& artifact,
        std::vector<uint8>& outPayload,
        std::string& outError)
    {
        Writer writer;
        if (!writer.String(RVX_MATERIAL_ARTIFACT_WRITER_ABI, outError) ||
            !writer.String(artifact.logicalMaterialId, outError))
        {
            return false;
        }
        const MaterialStaticDescriptor& descriptor = artifact.material.descriptor;
        writer.U8(static_cast<uint8>(descriptor.domain));
        writer.U8(static_cast<uint8>(descriptor.shadingModel));
        writer.U8(static_cast<uint8>(descriptor.blendMode));
        writer.U8(static_cast<uint8>(descriptor.cullMode));
        writer.U8(static_cast<uint8>(descriptor.shadowPolicy));
        writer.U8(static_cast<uint8>(descriptor.translucencyPhase));
        writer.U32(std::bit_cast<uint32>(descriptor.sortPriority));
        writer.U64(descriptor.featureMask);
        if (!writer.String(descriptor.programFamily, outError))
            return false;
        writer.U64(descriptor.parameterLayoutHash);
        writer.U64(artifact.permutation);

        const MaterialSourceData& value = artifact.material.parameters;
        WriteVec4(writer, value.baseColorFactor);
        writer.Float(value.metallicFactor);
        writer.Float(value.roughnessFactor);
        writer.Float(value.normalScale);
        writer.Float(value.occlusionStrength);
        WriteVec3(writer, value.emissiveColor);
        writer.Float(value.emissiveStrength);
        writer.Float(value.specularFactor);
        WriteVec3(writer, value.specularColor);
        writer.Float(value.clearcoatFactor);
        writer.Float(value.clearcoatRoughness);
        writer.Float(value.clearcoatNormalScale);
        writer.U32(value.textureFlags);
        writer.U8(static_cast<uint8>(value.alphaMode));
        writer.Float(value.alphaCutoff);
        writer.U8(static_cast<uint8>(value.workflow));
        writer.U8(value.doubleSided ? 1u : 0u);
        writer.Float(value.toonBands);
        writer.Float(value.toonSpecularThreshold);
        writer.Float(value.toonRimStrength);
        writer.Float(value.toonOutlineWidth);
        WriteVec3(writer, value.toonShadowTint);
        writer.Float(value.transmission);
        writer.Float(value.indexOfRefraction);
        writer.Float(value.thickness);
        WriteVec3(writer, value.attenuationColor);
        writer.Float(value.attenuationDistance);

        writer.U32(static_cast<uint32>(artifact.textureBindings.size()));
        for (const CookedMaterialTextureBinding& binding :
             artifact.textureBindings)
        {
            const MaterialTextureSlotDefinition& slot = binding.definition;
            writer.U8(static_cast<uint8>(slot.slot));
            if (!writer.String(binding.artifactPath, outError))
                return false;
            writer.Bytes(binding.expectedContentHash);
            writer.U8(static_cast<uint8>(binding.usage));
            writer.U8(binding.isSRGB ? 1u : 0u);
            writer.U8(static_cast<uint8>(binding.fallbackSemantic));
            writer.U8(slot.uvSet);
            WriteVec2(writer, slot.offset);
            WriteVec2(writer, slot.scale);
            writer.Float(slot.rotation);
            writer.U8(static_cast<uint8>(slot.wrapS));
            writer.U8(static_cast<uint8>(slot.wrapT));
            writer.U8(static_cast<uint8>(slot.minFilter));
            writer.U8(static_cast<uint8>(slot.magFilter));
        }
        writer.Bytes(artifact.dependencyHash);
        outPayload = writer.Take();
        return true;
    }

    bool ReadPayload(
        std::span<const uint8> payload,
        CookedMaterialArtifact& outArtifact,
        std::string& outError)
    {
        Reader reader(payload);
        std::string writerAbi;
        if (!reader.String(writerAbi) ||
            writerAbi != RVX_MATERIAL_ARTIFACT_WRITER_ABI ||
            !reader.String(outArtifact.logicalMaterialId))
        {
            outError = "Material artifact identity or writer ABI is invalid";
            return false;
        }

        uint8 domain = 0;
        uint8 shadingModel = 0;
        uint8 blendMode = 0;
        uint8 cullMode = 0;
        uint8 shadowPolicy = 0;
        uint8 translucencyPhase = 0;
        uint32 sortPriority = 0;
        MaterialStaticDescriptor& descriptor = outArtifact.material.descriptor;
        if (!reader.U8(domain) || !reader.U8(shadingModel) ||
            !reader.U8(blendMode) || !reader.U8(cullMode) ||
            !reader.U8(shadowPolicy) || !reader.U8(translucencyPhase) ||
            !reader.U32(sortPriority) || !reader.U64(descriptor.featureMask) ||
            !reader.String(descriptor.programFamily) ||
            !reader.U64(descriptor.parameterLayoutHash) ||
            !reader.U64(outArtifact.permutation))
        {
            outError = "Material artifact static descriptor is truncated";
            return false;
        }
        descriptor.domain = static_cast<MaterialDomain>(domain);
        descriptor.shadingModel = static_cast<MaterialShadingModel>(shadingModel);
        descriptor.blendMode = static_cast<MaterialBlendMode>(blendMode);
        descriptor.cullMode = static_cast<MaterialCullMode>(cullMode);
        descriptor.shadowPolicy = static_cast<MaterialShadowPolicy>(shadowPolicy);
        descriptor.translucencyPhase =
            static_cast<MaterialTranslucencyPhase>(translucencyPhase);
        descriptor.sortPriority = std::bit_cast<int32>(sortPriority);

        MaterialSourceData& value = outArtifact.material.parameters;
        uint8 alphaMode = 0;
        uint8 workflow = 0;
        uint8 doubleSided = 0;
        if (!ReadVec4(reader, value.baseColorFactor) ||
            !reader.Float(value.metallicFactor) ||
            !reader.Float(value.roughnessFactor) ||
            !reader.Float(value.normalScale) ||
            !reader.Float(value.occlusionStrength) ||
            !ReadVec3(reader, value.emissiveColor) ||
            !reader.Float(value.emissiveStrength) ||
            !reader.Float(value.specularFactor) ||
            !ReadVec3(reader, value.specularColor) ||
            !reader.Float(value.clearcoatFactor) ||
            !reader.Float(value.clearcoatRoughness) ||
            !reader.Float(value.clearcoatNormalScale) ||
            !reader.U32(value.textureFlags) || !reader.U8(alphaMode) ||
            !reader.Float(value.alphaCutoff) || !reader.U8(workflow) ||
            !reader.U8(doubleSided) || !reader.Float(value.toonBands) ||
            !reader.Float(value.toonSpecularThreshold) ||
            !reader.Float(value.toonRimStrength) ||
            !reader.Float(value.toonOutlineWidth) ||
            !ReadVec3(reader, value.toonShadowTint) ||
            !reader.Float(value.transmission) ||
            !reader.Float(value.indexOfRefraction) ||
            !reader.Float(value.thickness) ||
            !ReadVec3(reader, value.attenuationColor) ||
            !reader.Float(value.attenuationDistance) || doubleSided > 1u)
        {
            outError = "Material artifact parameter block is truncated or invalid";
            return false;
        }
        value.alphaMode = static_cast<MaterialSourceAlphaMode>(alphaMode);
        value.workflow = static_cast<MaterialSourceWorkflow>(workflow);
        value.doubleSided = doubleSided != 0u;

        uint32 bindingCount = 0;
        if (!reader.U32(bindingCount) || bindingCount > MaxTextureBindingCount)
        {
            outError = "Material artifact texture binding count is invalid";
            return false;
        }
        outArtifact.textureBindings.reserve(bindingCount);
        outArtifact.material.textureSlots.reserve(bindingCount);
        for (uint32 index = 0; index < bindingCount; ++index)
        {
            CookedMaterialTextureBinding binding;
            uint8 slot = 0;
            uint8 usage = 0;
            uint8 srgb = 0;
            uint8 fallback = 0;
            uint8 wrapS = 0;
            uint8 wrapT = 0;
            uint8 minFilter = 0;
            uint8 magFilter = 0;
            std::span<const uint8> expectedContentHash;
            if (!reader.U8(slot) || !reader.String(binding.artifactPath) ||
                !reader.Bytes(
                    Hash::RVX_SHA256_DIGEST_SIZE, expectedContentHash) ||
                !reader.U8(usage) || !reader.U8(srgb) ||
                !reader.U8(fallback) ||
                !reader.U8(binding.definition.uvSet) ||
                !ReadVec2(reader, binding.definition.offset) ||
                !ReadVec2(reader, binding.definition.scale) ||
                !reader.Float(binding.definition.rotation) ||
                !reader.U8(wrapS) || !reader.U8(wrapT) ||
                !reader.U8(minFilter) || !reader.U8(magFilter) || srgb > 1u)
            {
                outError = "Material artifact texture binding is truncated";
                return false;
            }
            std::copy(
                expectedContentHash.begin(), expectedContentHash.end(),
                binding.expectedContentHash.begin());
            binding.definition.slot = static_cast<MaterialTextureSlot>(slot);
            binding.usage = static_cast<TextureUsage>(usage);
            binding.isSRGB = srgb != 0u;
            binding.fallbackSemantic =
                static_cast<TextureFallbackSemantic>(fallback);
            binding.definition.wrapS = static_cast<MaterialSamplerWrap>(wrapS);
            binding.definition.wrapT = static_cast<MaterialSamplerWrap>(wrapT);
            binding.definition.minFilter =
                static_cast<MaterialSamplerFilter>(minFilter);
            binding.definition.magFilter =
                static_cast<MaterialSamplerFilter>(magFilter);
            outArtifact.material.textureSlots.push_back(binding.definition);
            outArtifact.textureBindings.push_back(std::move(binding));
        }
        std::span<const uint8> dependencyHash;
        if (!reader.Bytes(Hash::RVX_SHA256_DIGEST_SIZE, dependencyHash) ||
            reader.Remaining() != 0)
        {
            outError = "Material artifact dependency hash or payload boundary is invalid";
            return false;
        }
        std::copy(dependencyHash.begin(), dependencyHash.end(),
                  outArtifact.dependencyHash.begin());
        return true;
    }
} // namespace

uint64 GetCookedMaterialPermutation(
    const MaterialStaticDescriptor& descriptor) noexcept
{
    return (static_cast<uint64>(descriptor.shadingModel) << 56u) |
        (static_cast<uint64>(descriptor.blendMode) << 48u) |
        (descriptor.featureMask & 0x0000ffffffffffffull);
}

bool FinalizeCookedMaterialArtifact(
    CookedMaterialArtifact& artifact,
    std::string& outError)
{
    artifact.material.descriptor.parameterLayoutHash =
        GetMaterialParameterLayoutHashV2();
    std::sort(
        artifact.textureBindings.begin(), artifact.textureBindings.end(),
        [](const CookedMaterialTextureBinding& left,
           const CookedMaterialTextureBinding& right)
        {
            return left.definition.slot < right.definition.slot;
        });
    artifact.material.textureSlots.clear();
    artifact.material.textureSlots.reserve(artifact.textureBindings.size());
    artifact.material.parameters.textureFlags = 0;
    for (const CookedMaterialTextureBinding& binding : artifact.textureBindings)
    {
        artifact.material.textureSlots.push_back(binding.definition);
        if (binding.definition.slot <= MaterialTextureSlot::ClearcoatNormal)
        {
            artifact.material.parameters.textureFlags |=
                1u << static_cast<uint8>(binding.definition.slot);
        }
    }
    artifact.material.descriptor.featureMask = GetMaterialFeatureMaskV2(
        artifact.material.parameters, artifact.material.textureSlots);
    artifact.permutation = GetCookedMaterialPermutation(
        artifact.material.descriptor);
    artifact.dependencyHash = ComputeDependencyHash(artifact.textureBindings);
    artifact.contentHash = {};
    return ValidateCookedMaterialArtifact(artifact, outError);
}

bool ValidateCookedMaterialArtifact(
    const CookedMaterialArtifact& artifact,
    std::string& outError)
{
    if (artifact.logicalMaterialId.empty() ||
        artifact.logicalMaterialId.find('\0') != std::string::npos ||
        artifact.textureBindings.size() > MaxTextureBindingCount ||
        artifact.material.textureSlots.size() != artifact.textureBindings.size())
    {
        outError = "Material artifact identity or binding count is invalid";
        return false;
    }
    if (!ValidateMaterialDefinition(artifact.material, outError))
        return false;
    if (artifact.permutation != GetCookedMaterialPermutation(
            artifact.material.descriptor))
    {
        outError = "Material artifact permutation does not match the static descriptor";
        return false;
    }

    for (size_t index = 0; index < artifact.textureBindings.size(); ++index)
    {
        const CookedMaterialTextureBinding& binding =
            artifact.textureBindings[index];
        if (index != 0 &&
            artifact.textureBindings[index - 1].definition.slot >=
                binding.definition.slot)
        {
            outError = "Material artifact texture bindings are not canonical or unique";
            return false;
        }
        if (!SameSlotDefinition(
                artifact.material.textureSlots[index], binding.definition) ||
            !IsCanonicalPackageRelativePath(binding.artifactPath) ||
            IsZeroDigest(binding.expectedContentHash) ||
            !IsEnumInRange(binding.usage, TextureUsage::Data) ||
            !IsEnumInRange(binding.fallbackSemantic,
                           TextureFallbackSemantic::Black) ||
            !HasExpectedTextureSemantic(binding))
        {
            outError = "Material artifact texture dependency is invalid";
            return false;
        }
    }
    if (artifact.dependencyHash !=
        ComputeDependencyHash(artifact.textureBindings))
    {
        outError = "Material artifact dependency hash mismatch";
        return false;
    }
    outError.clear();
    return true;
}

bool SerializeCookedMaterialArtifact(
    const CookedMaterialArtifact& artifact,
    std::vector<uint8>& outBytes,
    std::string& outError)
{
    outBytes.clear();
    if (!ValidateCookedMaterialArtifact(artifact, outError))
        return false;
    std::vector<uint8> payload;
    if (!WritePayload(artifact, payload, outError))
        return false;
    return CookedArtifactIO::WriteEnvelope(
        RVX_MATERIAL_ARTIFACT_MAGIC,
        RVX_MATERIAL_ARTIFACT_SCHEMA_VERSION,
        payload,
        outBytes,
        outError);
}

bool DeserializeCookedMaterialArtifact(
    std::span<const uint8> bytes,
    CookedMaterialArtifact& outArtifact,
    std::string& outError)
{
    outArtifact = {};
    std::span<const uint8> payload;
    Hash::SHA256Digest payloadHash{};
    if (!CookedArtifactIO::ReadEnvelope(
            bytes,
            RVX_MATERIAL_ARTIFACT_MAGIC,
            RVX_MATERIAL_ARTIFACT_SCHEMA_VERSION,
            payload,
            payloadHash,
            outError) ||
        !ReadPayload(payload, outArtifact, outError) ||
        !ValidateCookedMaterialArtifact(outArtifact, outError))
    {
        outArtifact = {};
        return false;
    }
    outArtifact.contentHash = payloadHash;
    outError.clear();
    return true;
}
} // namespace RVX::Resource
