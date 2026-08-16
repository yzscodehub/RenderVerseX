/**
 * @file SampleInteriorFixtureWriter.cpp
 * @brief Writes a deterministic, self-contained Sponza-like glTF fixture.
 *
 * The fixture intentionally uses no external assets.  A single unit cube
 * geometry (one POSITION/NORMAL/index accessor set) is instanced through a
 * small set of material-specific meshes and many room nodes.  Keeping the
 * source text and binary packing explicit makes the generated glTF byte
 * stable across repeated invocations.
 */

#include <algorithm>
#include <array>
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
        float metallic;
        float roughness;
        bool transparent;
    };

    struct NodeSpec
    {
        std::string name;
        std::string role;
        std::uint32_t mesh;
        std::array<float, 3> translation;
        std::array<float, 3> scale;
    };

    void AppendUInt16LE(std::vector<std::uint8_t>& bytes,
                        std::uint16_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    }

    void AppendUInt32LE(std::vector<std::uint8_t>& bytes,
                        std::uint32_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
        bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    }

    void AppendFloat32LE(std::vector<std::uint8_t>& bytes, float value)
    {
        static_assert(sizeof(float) == sizeof(std::uint32_t));
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        AppendUInt32LE(bytes, bits);
    }

    std::vector<std::uint8_t> BuildUnitCubeBuffer()
    {
        // Twenty-four vertices (four per face) keep each face's normal flat.
        // The order is fixed: +X, -X, +Y, -Y, +Z, -Z.
        constexpr std::array<std::array<float, 3>, 24> positions = {{
            {{0.5f, -0.5f, -0.5f}}, {{0.5f, 0.5f, -0.5f}},
            {{0.5f, 0.5f, 0.5f}},   {{0.5f, -0.5f, 0.5f}},
            {{-0.5f, -0.5f, 0.5f}}, {{-0.5f, 0.5f, 0.5f}},
            {{-0.5f, 0.5f, -0.5f}},  {{-0.5f, -0.5f, -0.5f}},
            {{-0.5f, 0.5f, -0.5f}},  {{-0.5f, 0.5f, 0.5f}},
            {{0.5f, 0.5f, 0.5f}},    {{0.5f, 0.5f, -0.5f}},
            {{-0.5f, -0.5f, 0.5f}},  {{-0.5f, -0.5f, -0.5f}},
            {{0.5f, -0.5f, -0.5f}},   {{0.5f, -0.5f, 0.5f}},
            {{-0.5f, -0.5f, 0.5f}},  {{0.5f, -0.5f, 0.5f}},
            {{0.5f, 0.5f, 0.5f}},    {{-0.5f, 0.5f, 0.5f}},
            {{0.5f, -0.5f, -0.5f}},   {{-0.5f, -0.5f, -0.5f}},
            {{-0.5f, 0.5f, -0.5f}},  {{0.5f, 0.5f, -0.5f}},
        }};
        constexpr std::array<std::array<float, 3>, 6> normals = {{
            {{1.0f, 0.0f, 0.0f}},  {{-1.0f, 0.0f, 0.0f}},
            {{0.0f, 1.0f, 0.0f}},  {{0.0f, -1.0f, 0.0f}},
            {{0.0f, 0.0f, 1.0f}},  {{0.0f, 0.0f, -1.0f}},
        }};
        constexpr std::array<std::array<float, 2>, 4> texcoords = {{
            {{0.0f, 0.0f}}, {{0.0f, 1.0f}}, {{1.0f, 1.0f}}, {{1.0f, 0.0f}},
        }};
        constexpr std::array<std::uint16_t, 36> indices = {
            0, 1, 2, 0, 2, 3,
            4, 5, 6, 4, 6, 7,
            8, 9, 10, 8, 10, 11,
            12, 13, 14, 12, 14, 15,
            16, 17, 18, 16, 18, 19,
            20, 21, 22, 20, 22, 23,
        };

        std::vector<std::uint8_t> bytes;
        bytes.reserve(24U * 3U * sizeof(float) * 2U +
                      24U * 2U * sizeof(float) +
                      indices.size() * sizeof(std::uint16_t));
        for (const auto& position : positions)
        {
            for (const float component : position)
            {
                AppendFloat32LE(bytes, component);
            }
        }
        for (std::size_t face = 0; face < 6U; ++face)
        {
            for (std::size_t vertex = 0; vertex < 4U; ++vertex)
            {
                for (const float component : normals[face])
                {
                    AppendFloat32LE(bytes, component);
                }
            }
        }
        for (std::size_t face = 0; face < 6U; ++face)
        {
            (void)face;
            for (const auto& texcoord : texcoords)
            {
                for (const float component : texcoord)
                {
                    AppendFloat32LE(bytes, component);
                }
            }
        }
        for (const std::uint16_t index : indices)
        {
            AppendUInt16LE(bytes, index);
        }
        return bytes;
    }

    std::string EncodeBase64(const std::vector<std::uint8_t>& bytes)
    {
        constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);

        for (std::size_t offset = 0; offset < bytes.size(); offset += 3U)
        {
            const std::size_t remaining = bytes.size() - offset;
            const std::uint32_t first = bytes[offset];
            const std::uint32_t second = remaining > 1U ? bytes[offset + 1U] : 0U;
            const std::uint32_t third = remaining > 2U ? bytes[offset + 2U] : 0U;
            const std::uint32_t value = (first << 16U) | (second << 8U) | third;

            encoded.push_back(alphabet[(value >> 18U) & 0x3fU]);
            encoded.push_back(alphabet[(value >> 12U) & 0x3fU]);
            encoded.push_back(remaining > 1U ? alphabet[(value >> 6U) & 0x3fU] : '=');
            encoded.push_back(remaining > 2U ? alphabet[value & 0x3fU] : '=');
        }
        return encoded;
    }

    std::string FormatFloat(float value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(3) << value;
        return stream.str();
    }

    void AppendFloatArray(std::ostringstream& json,
                          const std::array<float, 3>& values)
    {
        json << '[' << FormatFloat(values[0]) << ',' << FormatFloat(values[1])
             << ',' << FormatFloat(values[2]) << ']';
    }

    void AppendColorArray(std::ostringstream& json,
                          const std::array<float, 4>& values)
    {
        json << '[' << FormatFloat(values[0]) << ',' << FormatFloat(values[1])
             << ',' << FormatFloat(values[2]) << ',' << FormatFloat(values[3])
             << ']';
    }

    std::vector<MaterialSpec> BuildMaterials()
    {
        return {
            {"Opaque_Limestone", {0.72f, 0.69f, 0.62f, 1.0f}, 0.05f, 0.82f, false},
            {"Opaque_Sandstone", {0.56f, 0.35f, 0.19f, 1.0f}, 0.0f, 0.92f, false},
            {"Opaque_Wood", {0.24f, 0.09f, 0.035f, 1.0f}, 0.0f, 0.62f, false},
            {"Opaque_PaintedBlue", {0.035f, 0.18f, 0.34f, 1.0f}, 0.12f, 0.46f, false},
            {"Opaque_PaintedGold", {0.68f, 0.38f, 0.065f, 1.0f}, 0.68f, 0.31f, false},
            {"Opaque_Iron", {0.075f, 0.085f, 0.095f, 1.0f}, 0.86f, 0.28f, false},
            {"Transparent_GlassA", {0.12f, 0.64f, 0.92f, 0.38f}, 0.08f, 0.14f, true},
            {"Transparent_GlassB", {0.92f, 0.36f, 0.14f, 0.46f}, 0.05f, 0.2f, true},
        };
    }

    std::vector<NodeSpec> BuildNodes()
    {
        std::vector<NodeSpec> nodes;
        nodes.reserve(52U);
        const auto Add = [&nodes](std::string_view name,
                                  std::string_view role,
                                  std::uint32_t mesh,
                                  std::array<float, 3> translation,
                                  std::array<float, 3> scale) {
            nodes.push_back({std::string(name), std::string(role), mesh,
                             translation, scale});
        };

        // Room shell and a stepped ceiling establish the camera's interior view.
        Add("Floor_Main", "floor", 0U, {0.0f, -0.25f, 0.0f}, {24.0f, 0.5f, 16.0f});
        Add("Floor_Aisle_North", "floor", 1U, {0.0f, 0.02f, -3.2f}, {9.0f, 0.07f, 0.7f});
        Add("Floor_Aisle_South", "floor", 1U, {0.0f, 0.02f, 3.2f}, {9.0f, 0.07f, 0.7f});
        Add("Ceiling_Main", "ceiling", 0U, {0.0f, 6.25f, 0.0f}, {24.0f, 0.5f, 16.0f});
        Add("Ceiling_Beam_Center", "ceiling", 2U, {0.0f, 5.75f, 0.0f}, {0.55f, 0.5f, 16.0f});
        Add("Ceiling_Beam_Left", "ceiling", 2U, {-6.0f, 5.75f, 0.0f}, {0.35f, 0.5f, 16.0f});
        Add("Ceiling_Beam_Right", "ceiling", 2U, {6.0f, 5.75f, 0.0f}, {0.35f, 0.5f, 16.0f});

        Add("Wall_West", "wall", 2U, {-11.75f, 3.0f, 0.0f}, {0.5f, 6.0f, 16.0f});
        Add("Wall_East", "wall", 2U, {11.75f, 3.0f, 0.0f}, {0.5f, 6.0f, 16.0f});
        Add("Wall_Back", "wall", 3U, {0.0f, 3.0f, -7.75f}, {24.0f, 6.0f, 0.5f});
        Add("Wall_Front", "wall", 3U, {0.0f, 3.0f, 7.75f}, {24.0f, 6.0f, 0.5f});
        Add("Wall_Back_Upper", "wall", 4U, {0.0f, 5.15f, -7.4f}, {18.0f, 0.65f, 0.35f});
        Add("Wall_Front_Upper", "wall", 4U, {0.0f, 5.15f, 7.4f}, {18.0f, 0.65f, 0.35f});

        // Sixteen columns create repeated occlusion and several depth planes.
        constexpr std::array<float, 4> columnX = {-8.5f, -3.0f, 3.0f, 8.5f};
        constexpr std::array<float, 4> columnZ = {-5.25f, -1.75f, 1.75f, 5.25f};
        std::uint32_t columnIndex = 0U;
        for (const float z : columnZ)
        {
            for (const float x : columnX)
            {
                const std::string name =
                    "Column_" + std::to_string(columnIndex++);
                Add(name, "column", columnIndex % 3U,
                    {x, 2.65f, z}, {0.52f, 5.30f, 0.52f});
            }
        }

        // Low blocks and furniture-like occluders are deliberately distributed
        // at near, mid, and far z values to exercise visibility ordering.
        const std::array<NodeSpec, 12> occluderTemplates = {{
            {"Occluder_Near_Left", "occluder", 2U, {-5.6f, 0.85f, 3.6f}, {1.25f, 0.85f, 0.8f}},
            {"Occluder_Near_Right", "occluder", 3U, {5.6f, 0.85f, 3.6f}, {1.25f, 0.85f, 0.8f}},
            {"Occluder_Mid_Left", "occluder", 4U, {-5.4f, 1.0f, 0.2f}, {1.1f, 1.0f, 0.75f}},
            {"Occluder_Mid_Right", "occluder", 5U, {5.4f, 1.0f, 0.2f}, {1.1f, 1.0f, 0.75f}},
            {"Occluder_Mid_Center", "occluder", 1U, {0.0f, 0.72f, -0.4f}, {1.6f, 0.72f, 0.65f}},
            {"Occluder_Far_Left", "occluder", 2U, {-5.0f, 1.1f, -3.8f}, {1.2f, 1.1f, 0.7f}},
            {"Occluder_Far_Right", "occluder", 3U, {5.0f, 1.1f, -3.8f}, {1.2f, 1.1f, 0.7f}},
            {"Occluder_Far_Center", "occluder", 4U, {0.0f, 1.3f, -5.5f}, {1.8f, 1.3f, 0.6f}},
            {"Occluder_Platform_Left", "occluder", 5U, {-8.0f, 0.42f, 0.0f}, {1.4f, 0.42f, 1.2f}},
            {"Occluder_Platform_Right", "occluder", 5U, {8.0f, 0.42f, 0.0f}, {1.4f, 0.42f, 1.2f}},
            {"Occluder_Back_Left", "occluder", 1U, {-7.0f, 1.4f, -6.0f}, {1.1f, 1.4f, 0.45f}},
            {"Occluder_Back_Right", "occluder", 1U, {7.0f, 1.4f, -6.0f}, {1.1f, 1.4f, 0.45f}},
        }};
        nodes.insert(nodes.end(), occluderTemplates.begin(), occluderTemplates.end());

        // Eight arches/beams add small silhouettes behind the columns.
        Add("Arch_North_Left", "occluder", 4U, {-7.0f, 3.9f, -6.55f}, {2.0f, 0.45f, 0.35f});
        Add("Arch_North_Right", "occluder", 4U, {7.0f, 3.9f, -6.55f}, {2.0f, 0.45f, 0.35f});
        Add("Arch_South_Left", "occluder", 4U, {-7.0f, 3.9f, 6.55f}, {2.0f, 0.45f, 0.35f});
        Add("Arch_South_Right", "occluder", 4U, {7.0f, 3.9f, 6.55f}, {2.0f, 0.45f, 0.35f});
        Add("Balcony_Left", "occluder", 2U, {-8.5f, 3.2f, -6.4f}, {2.3f, 0.2f, 0.8f});
        Add("Balcony_Right", "occluder", 2U, {8.5f, 3.2f, -6.4f}, {2.3f, 0.2f, 0.8f});
        Add("Balcony_Rail_Left", "occluder", 5U, {-8.5f, 3.8f, -5.8f}, {2.3f, 0.35f, 0.15f});
        Add("Balcony_Rail_Right", "occluder", 5U, {8.5f, 3.8f, -5.8f}, {2.3f, 0.35f, 0.15f});

        // The transparent pair is equidistant from the room center and uses
        // separate BLEND materials so sorting/pipeline binding is observable.
        Add("Transparent_Portal_Left", "transparent", 6U, {-3.0f, 2.3f, -4.0f}, {1.35f, 2.1f, 0.08f});
        Add("Transparent_Portal_Right", "transparent", 7U, {3.0f, 2.3f, -4.0f}, {1.35f, 2.1f, 0.08f});

        return nodes;
    }

    void AppendJsonString(std::ostringstream& json, std::string_view value)
    {
        json << '"';
        for (const char character : value)
        {
            switch (character)
            {
            case '\\':
                json << "\\\\";
                break;
            case '"':
                json << "\\\"";
                break;
            case '\n':
                json << "\\n";
                break;
            case '\r':
                json << "\\r";
                break;
            case '\t':
                json << "\\t";
                break;
            default:
                json << character;
                break;
            }
        }
        json << '"';
    }

    std::string BuildGltfJson(const std::vector<std::uint8_t>& binary,
                              const std::vector<MaterialSpec>& materials,
                              const std::vector<NodeSpec>& nodes)
    {
        const std::string base64 = EncodeBase64(binary);
        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << "{\n"
             << "  \"asset\": {\"version\": \"2.0\", \"generator\": \"RenderVerseX deterministic interior fixture writer\"},\n"
             << "  \"scene\": 0,\n"
             << "  \"scenes\": [{\"name\": \"SponzaLikeInteriorScene\", \"nodes\": [";
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            if (index != 0U)
            {
                json << ',';
            }
            json << index;
        }
        json << "]}],\n"
             << "  \"nodes\": [\n";
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            const NodeSpec& node = nodes[index];
            json << "    {\"name\": ";
            AppendJsonString(json, node.name);
            json << ",\"mesh\": " << node.mesh << ",\"translation\": ";
            AppendFloatArray(json, node.translation);
            json << ",\"scale\": ";
            AppendFloatArray(json, node.scale);
            json << ",\"extras\": {\"fixtureRole\": ";
            AppendJsonString(json, node.role);
            json << ",\"renderable\": true}";
            json << (index + 1U == nodes.size() ? "}\n" : "},\n");
        }
        json << "  ],\n"
             << "  \"meshes\": [\n";
        for (std::size_t index = 0; index < materials.size(); ++index)
        {
            json << "    {\"name\": \"UnitCubeMesh_" << index
                 << "\",\"primitives\": [{\"attributes\": {\"POSITION\": 0,\"NORMAL\": 1,\"TEXCOORD_0\": 2},\"indices\": 3,\"material\": "
                 << index << ",\"mode\": 4}]}";
            json << (index + 1U == materials.size() ? "\n" : ",\n");
        }
        json << "  ],\n"
             << "  \"materials\": [\n";
        for (std::size_t index = 0; index < materials.size(); ++index)
        {
            const MaterialSpec& material = materials[index];
            json << "    {\"name\": ";
            AppendJsonString(json, material.name);
            json << ",\"pbrMetallicRoughness\": {\"baseColorFactor\": ";
            AppendColorArray(json, material.baseColor);
            json << ",\"metallicFactor\": " << FormatFloat(material.metallic)
                 << ",\"roughnessFactor\": " << FormatFloat(material.roughness)
                 << "},\"alphaMode\": \"" << (material.transparent ? "BLEND" : "OPAQUE")
                 << "\",\"doubleSided\": true}";
            json << (index + 1U == materials.size() ? "\n" : ",\n");
        }
        json << "  ],\n"
             << "  \"buffers\": [{\"byteLength\": " << binary.size()
             << ",\"uri\": \"data:application/octet-stream;base64," << base64
             << "\"}],\n"
             << "  \"bufferViews\": ["
             << "{\"buffer\": 0,\"byteOffset\": 0,\"byteLength\": 288,\"target\": 34962},"
             << "{\"buffer\": 0,\"byteOffset\": 288,\"byteLength\": 288,\"target\": 34962},"
             << "{\"buffer\": 0,\"byteOffset\": 576,\"byteLength\": 192,\"target\": 34962},"
             << "{\"buffer\": 0,\"byteOffset\": 768,\"byteLength\": 72,\"target\": 34963}],\n"
             << "  \"accessors\": ["
             << "{\"bufferView\": 0,\"byteOffset\": 0,\"componentType\": 5126,\"count\": 24,\"type\": \"VEC3\",\"min\": [-0.500,-0.500,-0.500],\"max\": [0.500,0.500,0.500]},"
             << "{\"bufferView\": 1,\"byteOffset\": 0,\"componentType\": 5126,\"count\": 24,\"type\": \"VEC3\"},"
             << "{\"bufferView\": 2,\"byteOffset\": 0,\"componentType\": 5126,\"count\": 24,\"type\": \"VEC2\",\"min\": [0.000,0.000],\"max\": [1.000,1.000]},"
             << "{\"bufferView\": 3,\"byteOffset\": 0,\"componentType\": 5123,\"count\": 36,\"type\": \"SCALAR\",\"min\": [0],\"max\": [23]}],\n"
             << "  \"extras\": {\n"
             << "    \"fixtureSchema\": \"RVX.InteriorFixture.v1\",\n"
             << "    \"fixtureSchemaVersion\": 1,\n"
             << "    \"fixtureKind\": \"SponzaLikeInterior\",\n"
             << "    \"geometryKind\": \"UnitCube\",\n"
             << "    \"unitCubeVertexCount\": 24,\n"
             << "    \"unitCubeTexcoordCount\": 24,\n"
             << "    \"unitCubeIndexCount\": 36,\n"
             << "    \"meshCount\": " << materials.size() << ",\n"
             << "    \"materialCount\": " << materials.size() << ",\n"
             << "    \"nodeCount\": " << nodes.size() << ",\n"
             << "    \"renderableNodeCount\": " << nodes.size() << ",\n"
             << "    \"opaqueMaterialCount\": 6,\n"
             << "    \"transparentMaterialCount\": 2,\n"
             << "    \"transparentNodeCount\": 2,\n"
             << "    \"counts\": {\"meshes\": " << materials.size()
             << ",\"materials\": " << materials.size() << ",\"nodes\": "
             << nodes.size() << ",\"renderableNodes\": " << nodes.size()
             << ",\"opaqueMaterials\": 6,\"transparentMaterials\": 2,\"transparentNodes\": 2}\n"
             << "  }\n"
             << "}\n";
        return json.str();
    }

    bool WriteInteriorFixture(const std::filesystem::path& outputPath)
    {
        std::error_code error;
        const std::filesystem::path parentPath = outputPath.parent_path();
        if (!parentPath.empty())
        {
            std::filesystem::create_directories(parentPath, error);
            if (error)
            {
                return false;
            }
        }

        const std::vector<MaterialSpec> materials = BuildMaterials();
        const std::vector<NodeSpec> nodes = BuildNodes();
        const std::vector<std::uint8_t> binary = BuildUnitCubeBuffer();
        const std::string json = BuildGltfJson(binary, materials, nodes);

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
    std::filesystem::path outputPath;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--output" && index + 1 < argc)
        {
            outputPath = argv[++index];
        }
        else
        {
            std::cerr << "Usage: RVXSampleInteriorFixtureWriter --output <path>\n";
            return 2;
        }
    }

    if (outputPath.empty())
    {
        std::cerr << "Missing --output path\n";
        return 2;
    }
    if (!WriteInteriorFixture(outputPath))
    {
        std::cerr << "Failed to write interior glTF fixture: " << outputPath << "\n";
        return 1;
    }

    std::cout << "Wrote interior glTF fixture: " << outputPath << "\n";
    return 0;
}
