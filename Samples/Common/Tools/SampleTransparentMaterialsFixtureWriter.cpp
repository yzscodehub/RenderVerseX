/**
 * @file SampleTransparentMaterialsFixtureWriter.cpp
 * @brief Writes the deterministic authoring input for the GFX15 cook only.
 *
 * This program is intentionally a build-time tool.  StageSampleAssets invokes
 * it inside its transaction-only cook-input directory and deletes that
 * directory after RVXCook and the cooked catalog publisher have verified the
 * package.  RenderVerseSamples never receives this glTF at runtime.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    struct MaterialSpec
    {
        std::string_view name;
        std::array<float, 4> baseColor;
        std::array<float, 3> emissive;
        float metallic = 0.0f;
        float roughness = 1.0f;
        bool blend = false;
    };

    struct PanelSpec
    {
        std::string_view name;
        std::string_view role;
        uint32_t materialIndex = 0;
        std::array<float, 3> translation{};
        std::array<float, 3> scale{};
        std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
    };

    void AppendUInt16LE(std::vector<uint8_t>& bytes, uint16_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value & 0xffU));
        bytes.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    }

    void AppendFloat32LE(std::vector<uint8_t>& bytes, float value)
    {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        bytes.push_back(static_cast<uint8_t>(bits & 0xffU));
        bytes.push_back(static_cast<uint8_t>((bits >> 8U) & 0xffU));
        bytes.push_back(static_cast<uint8_t>((bits >> 16U) & 0xffU));
        bytes.push_back(static_cast<uint8_t>((bits >> 24U) & 0xffU));
    }

    [[nodiscard]] std::vector<uint8_t> BuildPanelBuffer()
    {
        // A single camera-facing unit panel.  Separate material meshes and
        // deterministic node transforms make each transparency class visible
        // without the Sample having to mutate any material at runtime.
        constexpr std::array<std::array<float, 3>, 4> positions = {{
            {{-0.5f, -0.5f, 0.0f}}, {{0.5f, -0.5f, 0.0f}},
            {{0.5f, 0.5f, 0.0f}}, {{-0.5f, 0.5f, 0.0f}},
        }};
        constexpr std::array<std::array<float, 3>, 4> normals = {{
            {{0.0f, 0.0f, 1.0f}}, {{0.0f, 0.0f, 1.0f}},
            {{0.0f, 0.0f, 1.0f}}, {{0.0f, 0.0f, 1.0f}},
        }};
        constexpr std::array<std::array<float, 2>, 4> texcoords = {{
            {{0.0f, 1.0f}}, {{1.0f, 1.0f}},
            {{1.0f, 0.0f}}, {{0.0f, 0.0f}},
        }};
        constexpr std::array<uint16_t, 6> indices = {{0, 1, 2, 0, 2, 3}};

        std::vector<uint8_t> bytes;
        bytes.reserve(4U * (3U + 3U + 2U) * sizeof(float) +
                      indices.size() * sizeof(uint16_t));
        for (const auto& value : positions)
        {
            for (const float component : value)
            {
                AppendFloat32LE(bytes, component);
            }
        }
        for (const auto& value : normals)
        {
            for (const float component : value)
            {
                AppendFloat32LE(bytes, component);
            }
        }
        for (const auto& value : texcoords)
        {
            for (const float component : value)
            {
                AppendFloat32LE(bytes, component);
            }
        }
        for (const uint16_t index : indices)
        {
            AppendUInt16LE(bytes, index);
        }
        return bytes;
    }

    [[nodiscard]] std::string EncodeBase64(const std::vector<uint8_t>& bytes)
    {
        constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve(((bytes.size() + 2U) / 3U) * 4U);
        for (size_t offset = 0; offset < bytes.size(); offset += 3U)
        {
            const size_t remaining = bytes.size() - offset;
            const uint32_t first = bytes[offset];
            const uint32_t second = remaining > 1U ? bytes[offset + 1U] : 0U;
            const uint32_t third = remaining > 2U ? bytes[offset + 2U] : 0U;
            const uint32_t value = (first << 16U) | (second << 8U) | third;
            result.push_back(alphabet[(value >> 18U) & 0x3fU]);
            result.push_back(alphabet[(value >> 12U) & 0x3fU]);
            result.push_back(remaining > 1U ? alphabet[(value >> 6U) & 0x3fU] : '=');
            result.push_back(remaining > 2U ? alphabet[value & 0x3fU] : '=');
        }
        return result;
    }

    [[nodiscard]] std::string FormatFloat(float value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(3) << value;
        return stream.str();
    }

    void AppendVec3(std::ostringstream& json, const std::array<float, 3>& value)
    {
        json << '[' << FormatFloat(value[0]) << ',' << FormatFloat(value[1])
             << ',' << FormatFloat(value[2]) << ']';
    }

    void AppendVec4(std::ostringstream& json, const std::array<float, 4>& value)
    {
        json << '[' << FormatFloat(value[0]) << ',' << FormatFloat(value[1])
             << ',' << FormatFloat(value[2]) << ',' << FormatFloat(value[3])
             << ']';
    }

    void AppendJsonString(std::ostringstream& json, std::string_view value)
    {
        json << '"';
        for (const char character : value)
        {
            if (character == '\\' || character == '"')
            {
                json << '\\';
            }
            json << character;
        }
        json << '"';
    }

    [[nodiscard]] const std::array<MaterialSpec, 12>& GetMaterials()
    {
        static const std::array<MaterialSpec, 12> materials = {{
            {"Opaque_Backdrop_Alpha", {0.08f, 0.16f, 0.32f, 1.0f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.85f, false},
            {"Opaque_Backdrop_Premultiplied", {0.36f, 0.72f, 0.16f, 1.0f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.85f, false},
            {"Opaque_Backdrop_AdditiveDark", {0.008f, 0.012f, 0.020f, 1.0f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.90f, false},
            {"Opaque_Backdrop_ThinStripeBlue", {0.04f, 0.14f, 0.75f, 1.0f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.78f, false},
            {"Opaque_Backdrop_ThinStripeGold", {0.95f, 0.54f, 0.06f, 1.0f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.78f, false},
            {"Opaque_Backdrop_ThinStripeMagenta", {0.70f, 0.05f, 0.42f, 1.0f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.78f, false},
            {"Transparent_AlphaBlend", {0.08f, 0.42f, 1.00f, 0.48f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.24f, true},
            {"Transparent_Premultiplied", {0.46f, 0.08f, 0.03f, 0.52f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.22f, true},
            {"Transparent_Additive", {1.00f, 0.22f, 0.01f, 0.58f},
             {1.0f, 0.22f, 0.01f}, 0.0f, 0.16f, true},
            // Full pane coverage: optical transmission controls transparency.
            // Fractional alpha would also mix in an unrefracted backdrop.
            {"Transparent_ThinTransmission", {0.10f, 0.82f, 0.72f, 1.0f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.08f, true},
            {"Opaque_Backdrop_PremultipliedBright", {0.96f, 0.80f, 0.08f, 1.0f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.72f, false},
            {"Transparent_ThinTransmissionRough", {0.10f, 0.82f, 0.72f, 1.0f},
             {0.0f, 0.0f, 0.0f}, 0.0f, 0.60f, true},
        }};
        return materials;
    }

    [[nodiscard]] const std::array<PanelSpec, 12>& GetPanels()
    {
        static const std::array<PanelSpec, 12> panels = {{
            {"Backdrop_Alpha", "opaque-alpha-backdrop", 0U,
             {-2.25f, 1.35f, -0.35f}, {1.25f, 1.75f, 1.0f}},
            {"Backdrop_Premultiplied", "opaque-premultiplied-backdrop", 1U,
             {-0.75f, 1.35f, -0.35f}, {1.25f, 1.75f, 1.0f}},
            {"Backdrop_AdditiveDark", "opaque-additive-backdrop", 2U,
             {0.75f, 1.35f, -0.35f}, {1.25f, 1.75f, 1.0f}},
            {"Backdrop_ThinStripeBlue", "opaque-thin-stripe-blue", 3U,
             {1.75f, 1.35f, -0.35f}, {0.50f, 1.75f, 1.0f}},
            {"Backdrop_ThinStripeGold", "opaque-thin-stripe-gold", 4U,
             {2.25f, 1.35f, -0.35f}, {0.50f, 1.75f, 1.0f}},
            {"Backdrop_ThinStripeMagenta", "opaque-thin-stripe-magenta", 5U,
             {2.75f, 1.35f, -0.35f}, {0.50f, 1.75f, 1.0f}},
            {"AlphaBlend_Panel", "alpha-blend", 6U,
             {-2.25f, 1.35f, 0.10f}, {1.05f, 1.55f, 1.0f}},
            {"Premultiplied_Panel", "premultiplied", 7U,
             {-0.75f, 1.35f, 0.16f}, {1.05f, 1.55f, 1.0f}},
            {"Additive_Panel", "additive", 8U,
             {0.75f, 1.35f, 0.22f}, {1.05f, 1.55f, 1.0f}},
            {"ThinTransmission_Panel", "thin-transmission", 9U,
             {2.25f, 1.77f, 0.28f}, {1.55f, 0.70f, 1.0f},
             {0.0f, 0.207912f, 0.0f, 0.978148f}},
            {"Backdrop_PremultipliedBright", "opaque-premultiplied-bright-boundary", 10U,
             {-0.75f, 1.35f, -0.34f}, {0.36f, 1.75f, 1.0f}},
            // Identical tilt and optical parameters isolate roughness filtering.
            {"ThinTransmissionRough_Panel", "thin-transmission-rough", 11U,
             {2.25f, 0.93f, 0.28f}, {1.55f, 0.70f, 1.0f},
             {0.0f, 0.207912f, 0.0f, 0.978148f}},
        }};
        return panels;
    }

    [[nodiscard]] std::string BuildGltfJson(const std::vector<uint8_t>& binary)
    {
        const auto& materials = GetMaterials();
        const auto& panels = GetPanels();
        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << "{\n"
             << "  \"asset\": {\"version\": \"2.0\", \"generator\": \"RenderVerseX deterministic transparent-material fixture writer\"},\n"
             << "  \"scene\": 0,\n"
             << "  \"scenes\": [{\"name\": \"TransparentMaterialsQualification\",\"nodes\": [";
        for (size_t index = 0; index < panels.size(); ++index)
        {
            if (index != 0U)
            {
                json << ',';
            }
            json << index;
        }
        json << "]}],\n  \"nodes\": [\n";
        for (size_t index = 0; index < panels.size(); ++index)
        {
            const PanelSpec& panel = panels[index];
            json << "    {\"name\": ";
            AppendJsonString(json, panel.name);
            json << ",\"mesh\": " << panel.materialIndex << ",\"translation\": ";
            AppendVec3(json, panel.translation);
            json << ",\"scale\": ";
            AppendVec3(json, panel.scale);
            json << ",\"rotation\": ";
            AppendVec4(json, panel.rotation);
            json << ",\"extras\": {\"fixtureRole\": ";
            AppendJsonString(json, panel.role);
            json << ",\"renderable\": true}}";
            json << (index + 1U == panels.size() ? "\n" : ",\n");
        }
        json << "  ],\n  \"meshes\": [\n";
        for (size_t index = 0; index < materials.size(); ++index)
        {
            json << "    {\"name\": \"PanelMesh_" << index
                 << "\",\"primitives\": [{\"attributes\": {\"POSITION\": 0,\"NORMAL\": 1,\"TEXCOORD_0\": 2},\"indices\": 3,\"material\": "
                 << index << ",\"mode\": 4}]}";
            json << (index + 1U == materials.size() ? "\n" : ",\n");
        }
        json << "  ],\n  \"materials\": [\n";
        for (size_t index = 0; index < materials.size(); ++index)
        {
            const MaterialSpec& material = materials[index];
            json << "    {\"name\": ";
            AppendJsonString(json, material.name);
            json << ",\"pbrMetallicRoughness\": {\"baseColorFactor\": ";
            AppendVec4(json, material.baseColor);
            json << ",\"metallicFactor\": " << FormatFloat(material.metallic)
                 << ",\"roughnessFactor\": " << FormatFloat(material.roughness)
                 << "},\"emissiveFactor\": ";
            AppendVec3(json, material.emissive);
            json << ",\"alphaMode\": \"" << (material.blend ? "BLEND" : "OPAQUE")
                 << "\",\"doubleSided\": true}";
            json << (index + 1U == materials.size() ? "\n" : ",\n");
        }
        const std::string base64 = EncodeBase64(binary);
        json << "  ],\n"
             << "  \"buffers\": [{\"byteLength\": " << binary.size()
             << ",\"uri\": \"data:application/octet-stream;base64," << base64 << "\"}],\n"
             << "  \"bufferViews\": ["
             << "{\"buffer\": 0,\"byteOffset\": 0,\"byteLength\": 48,\"target\": 34962},"
             << "{\"buffer\": 0,\"byteOffset\": 48,\"byteLength\": 48,\"target\": 34962},"
             << "{\"buffer\": 0,\"byteOffset\": 96,\"byteLength\": 32,\"target\": 34962},"
             << "{\"buffer\": 0,\"byteOffset\": 128,\"byteLength\": 12,\"target\": 34963}],\n"
             << "  \"accessors\": ["
             << "{\"bufferView\": 0,\"componentType\": 5126,\"count\": 4,\"type\": \"VEC3\",\"min\": [-0.500,-0.500,0.000],\"max\": [0.500,0.500,0.000]},"
             << "{\"bufferView\": 1,\"componentType\": 5126,\"count\": 4,\"type\": \"VEC3\"},"
             << "{\"bufferView\": 2,\"componentType\": 5126,\"count\": 4,\"type\": \"VEC2\"},"
             << "{\"bufferView\": 3,\"componentType\": 5123,\"count\": 6,\"type\": \"SCALAR\",\"min\": [0],\"max\": [3]}],\n"
             << "  \"extras\": {\n"
             << "    \"fixtureSchema\": \"RVX.TransparentMaterialsFixture.v2\",\n"
             << "    \"fixtureSchemaVersion\": 2,\n"
             << "    \"fixtureKind\": \"CookOnlyTransparencyQualification\",\n"
             << "    \"materialCount\": " << materials.size() << ",\n"
             << "    \"transparentMaterialCount\": "
             << std::count_if(materials.begin(), materials.end(),
                    [](const MaterialSpec& material) { return material.blend; }) << ",\n"
             << "    \"transparentClasses\": [\"alpha-blend\",\"premultiplied\",\"additive\",\"thin-transmission\"],\n"
             << "    \"thinTransmissionBackground\": \"high-contrast-blue-gold-magenta-stripes\",\n"
             << "    \"premultipliedBackground\": \"green-gold-contrast-boundary\",\n"
             << "    \"runtimeSourceForbidden\": true\n"
             << "  }\n}\n";
        return json.str();
    }

    [[nodiscard]] bool WriteFixture(const std::filesystem::path& outputPath)
    {
        std::error_code error;
        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(outputPath.parent_path(), error);
            if (error)
            {
                return false;
            }
        }

        const std::vector<uint8_t> binary = BuildPanelBuffer();
        const std::string json = BuildGltfJson(binary);
        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return false;
        }
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        output.flush();
        return output.good();
    }
} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3 || std::string_view(argv[1]) != "--output")
    {
        std::cerr << "Usage: RVXSampleTransparentMaterialsFixtureWriter --output <scene.gltf>\n";
        return 2;
    }
    if (!WriteFixture(argv[2]))
    {
        std::cerr << "Failed to write deterministic transparent-material fixture\n";
        return 1;
    }
    return 0;
}
