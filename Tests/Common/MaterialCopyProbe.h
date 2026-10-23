#pragma once

#include "Render/Material/MaterialGPUData.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace RVX::Test
{
    inline bool IsMaterialCopyProbe(std::string_view entry) noexcept
    {
        return entry.starts_with("PSMaterialCopy");
    }

    inline void FillMaterialCopySentinels(MaterialGPUConstants& material)
    {
        static_assert(sizeof(material) % sizeof(float32) == 0);
        // Every word is nonzero and distinct. The probe only reconstructs the
        // ABI; these are deliberately not semantically valid authored flags.
        for (size_t word = 0; word < sizeof(material) / sizeof(float32); ++word)
        {
            const float32 value = 0.25f + static_cast<float32>(word + 1) / 256.0f;
            std::memcpy(reinterpret_cast<uint8*>(&material) + word * sizeof(value),
                        &value, sizeof(value));
        }
    }

    inline void AppendMaterialCopyProbe(std::string& productionSource,
                                       const std::filesystem::path& shaderDirectory)
    {
        std::ifstream stream(shaderDirectory / "Include/SurfaceMaterial.hlsli");
        std::string declaration{std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>()};
        declaration.erase(std::remove(declaration.begin(), declaration.end(), '\r'), declaration.end());
        const size_t begin = declaration.find("#define RVX_MATERIAL_PARAMETER_FIELDS");
        const size_t end = declaration.find("\n\n", begin);
        if (begin == std::string::npos || end == std::string::npos)
            throw std::runtime_error("Material declaration unavailable for native probe");
        declaration = declaration.substr(begin, end - begin) + ";";
        const std::regex field(R"(\b[A-Za-z_]\w*\s+(\w+)\s*;)");
        std::string comparisons;
        for (std::sregex_iterator it(declaration.begin(), declaration.end(), field), last;
             it != last; ++it)
        {
            const std::string name = (*it)[1].str();
            comparisons += "matching = matching && all(material." + name + " == " + name + ");\n";
        }
        if (comparisons.empty()) throw std::runtime_error("Material probe has zero fields");
        // Compile the on-disk production resolver, not a copied implementation.
        productionSource += R"(
float4 RVXMaterialCopyProbeResult(MaterialParameterData material)
{
    bool matching = true;
)" + comparisons + R"(
    return matching ? float4(1, 1, 1, 1) : float4(0, 1, 0, 1);
}
float4 PSMaterialCopyProbe(PSInput input) : SV_TARGET
{
    if (input.ReceivesShadowValue < 0.0f) discard;
    return RVXMaterialCopyProbeResult(ResolveMaterialParameters(input.MaterialParameterSlot));
}
float4 PSMaterialCopyMissingFieldProbe(PSInput input) : SV_TARGET
{
    if (input.ReceivesShadowValue < 0.0f) discard;
    MaterialParameterData material = ResolveMaterialParameters(input.MaterialParameterSlot);
    material.SpecularColor_Factor = 0.0f;
    return RVXMaterialCopyProbeResult(material);
}
)";
    }
}
