#include "AssetImport/FBXImporter.h"

#include <ufbx.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace RVX::Resource
{
namespace
{
    namespace fs = std::filesystem;

    constexpr std::uint32_t kMaxNodeDepth = 512;

    struct SceneDeleter
    {
        void operator()(ufbx_scene* scene) const noexcept
        {
            ufbx_free_scene(scene);
        }
    };

    using ScenePtr = std::unique_ptr<ufbx_scene, SceneDeleter>;

    std::string ToString(ufbx_string value)
    {
        return value.data ? std::string(value.data, value.length) : std::string{};
    }

    bool IsFinite(float value)
    {
        return std::isfinite(value);
    }

    bool IsFinite(const Vec2& value)
    {
        return IsFinite(value.x) && IsFinite(value.y);
    }

    bool IsFinite(const Vec3& value)
    {
        return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
    }

    bool IsFinite(const Vec4& value)
    {
        return IsFinite(value.x) && IsFinite(value.y) &&
               IsFinite(value.z) && IsFinite(value.w);
    }

    bool IsFinite(const Quat& value)
    {
        return IsFinite(value.x) && IsFinite(value.y) &&
               IsFinite(value.z) && IsFinite(value.w);
    }

    bool IsContainedBy(const fs::path& root, const fs::path& candidate)
    {
        const fs::path relative = candidate.lexically_relative(root);
        if (relative.empty() || relative.is_absolute())
        {
            return candidate == root;
        }
        for (const fs::path& component : relative)
        {
            if (component == "..")
            {
                return false;
            }
        }
        return true;
    }

    bool LooksLikeAbsolutePortablePath(std::string_view path)
    {
        if (path.empty())
        {
            return false;
        }
        if (path.front() == '/' || path.front() == '\\')
        {
            return true;
        }
        return path.size() >= 2 &&
               ((path[0] >= 'A' && path[0] <= 'Z') ||
                (path[0] >= 'a' && path[0] <= 'z')) &&
               path[1] == ':';
    }

    bool ResolveExternalTexture(const fs::path& sourceRoot,
                                const ufbx_texture& texture,
                                fs::path& outPath,
                                std::shared_ptr<const std::vector<uint8_t>>& outPayload,
                                std::string& outError)
    {
        std::string reference = ToString(texture.relative_filename);
        if (reference.empty())
        {
            reference = ToString(texture.filename);
        }
        if (reference.empty())
        {
            outError = "FBX external texture has no source path.";
            return false;
        }

        std::replace(reference.begin(), reference.end(), '\\', '/');
        if (LooksLikeAbsolutePortablePath(reference))
        {
            outError = "Unsupported FBX external texture path outside the source root: " +
                       reference;
            return false;
        }

        const fs::path relative = fs::path(reference).lexically_normal();
        if (relative.empty() || relative == ".")
        {
            outError = "FBX external texture path is empty after normalization.";
            return false;
        }
        for (const fs::path& component : relative)
        {
            if (component == "..")
            {
                outError = "Unsupported FBX external texture path escapes the source root: " +
                           reference;
                return false;
            }
        }

        std::error_code error;
        fs::path candidate = fs::weakly_canonical(sourceRoot / relative, error);
        if (error)
        {
            candidate = (sourceRoot / relative).lexically_normal();
            error.clear();
        }
        if (!IsContainedBy(sourceRoot, candidate))
        {
            outError = "Unsupported FBX external texture path escapes the source root: " +
                       reference;
            return false;
        }
        if (!fs::is_regular_file(candidate, error) || error)
        {
            outError = "FBX external texture does not exist: " + candidate.string();
            return false;
        }

        std::ifstream input(candidate, std::ios::binary);
        if (!input.is_open())
        {
            outError = "FBX external texture could not be opened: " + candidate.string();
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
        if (!input.eof() && input.fail())
        {
            outError = "FBX external texture could not be read: " + candidate.string();
            return false;
        }
        if (bytes.empty())
        {
            outError = "FBX external texture is empty: " + candidate.string();
            return false;
        }

        outPath = candidate;
        outPayload = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        return true;
    }

    std::uint32_t FloatBits(float value)
    {
        if (value == 0.0f)
        {
            value = 0.0f;
        }
        return std::bit_cast<std::uint32_t>(value);
    }

    struct VertexKey
    {
        Vec3 position{0.0f};
        Vec3 normal{0.0f};
        Vec2 uv{0.0f};
        Vec4 tangent{0.0f};
        Vec4 color{1.0f};
        std::uint8_t attributeMask = 0;

        bool operator==(const VertexKey& rhs) const noexcept
        {
            const auto same = [](float lhs, float rhsValue)
            {
                return FloatBits(lhs) == FloatBits(rhsValue);
            };
            return attributeMask == rhs.attributeMask &&
                   same(position.x, rhs.position.x) &&
                   same(position.y, rhs.position.y) &&
                   same(position.z, rhs.position.z) &&
                   same(normal.x, rhs.normal.x) &&
                   same(normal.y, rhs.normal.y) &&
                   same(normal.z, rhs.normal.z) &&
                   same(uv.x, rhs.uv.x) && same(uv.y, rhs.uv.y) &&
                   same(tangent.x, rhs.tangent.x) &&
                   same(tangent.y, rhs.tangent.y) &&
                   same(tangent.z, rhs.tangent.z) &&
                   same(tangent.w, rhs.tangent.w) &&
                   same(color.x, rhs.color.x) &&
                   same(color.y, rhs.color.y) &&
                   same(color.z, rhs.color.z) &&
                   same(color.w, rhs.color.w);
        }
    };

    struct VertexKeyHash
    {
        size_t operator()(const VertexKey& key) const noexcept
        {
            size_t hash = key.attributeMask;
            const auto mix = [&](float value)
            {
                hash ^= static_cast<size_t>(FloatBits(value)) +
                        0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
            };
            mix(key.position.x); mix(key.position.y); mix(key.position.z);
            mix(key.normal.x); mix(key.normal.y); mix(key.normal.z);
            mix(key.uv.x); mix(key.uv.y);
            mix(key.tangent.x); mix(key.tangent.y);
            mix(key.tangent.z); mix(key.tangent.w);
            mix(key.color.x); mix(key.color.y); mix(key.color.z); mix(key.color.w);
            return hash;
        }
    };

    bool BuildVertex(const ufbx_mesh& mesh,
                     std::uint32_t cornerIndex,
                     const FBXImportOptions& options,
                     VertexKey& outVertex,
                     std::string& outError)
    {
        const ufbx_vec3 sourcePosition =
            ufbx_get_vertex_vec3(&mesh.vertex_position, cornerIndex);
        outVertex.position = Vec3(sourcePosition.x, sourcePosition.y, sourcePosition.z);
        if (!IsFinite(outVertex.position))
        {
            outError = "FBX mesh contains a non-finite position.";
            return false;
        }

        if (mesh.vertex_normal.exists)
        {
            const ufbx_vec3 sourceNormal =
                ufbx_get_vertex_vec3(&mesh.vertex_normal, cornerIndex);
            outVertex.normal = Vec3(sourceNormal.x, sourceNormal.y, sourceNormal.z);
            if (!IsFinite(outVertex.normal) ||
                glm::dot(outVertex.normal, outVertex.normal) <= 0.0f)
            {
                outError = "FBX mesh contains an invalid normal.";
                return false;
            }
            outVertex.normal = glm::normalize(outVertex.normal);
            outVertex.attributeMask |= 0x01u;
        }

        if (mesh.vertex_uv.exists)
        {
            const ufbx_vec2 sourceUv =
                ufbx_get_vertex_vec2(&mesh.vertex_uv, cornerIndex);
            outVertex.uv = Vec2(sourceUv.x,
                                options.flipUVs ? 1.0f - sourceUv.y : sourceUv.y);
            if (!IsFinite(outVertex.uv))
            {
                outError = "FBX mesh contains a non-finite texture coordinate.";
                return false;
            }
            outVertex.attributeMask |= 0x02u;
        }

        if (mesh.vertex_tangent.exists)
        {
            const ufbx_vec3 sourceTangent =
                ufbx_get_vertex_vec3(&mesh.vertex_tangent, cornerIndex);
            Vec3 tangent(sourceTangent.x, sourceTangent.y, sourceTangent.z);
            if (!IsFinite(tangent) || glm::dot(tangent, tangent) <= 0.0f)
            {
                outError = "FBX mesh contains an invalid tangent.";
                return false;
            }
            tangent = glm::normalize(tangent);
            float sign = 1.0f;
            if (mesh.vertex_bitangent.exists && mesh.vertex_normal.exists)
            {
                const ufbx_vec3 sourceBitangent =
                    ufbx_get_vertex_vec3(&mesh.vertex_bitangent, cornerIndex);
                const Vec3 bitangent(sourceBitangent.x,
                                     sourceBitangent.y,
                                     sourceBitangent.z);
                sign = glm::dot(glm::cross(outVertex.normal, tangent), bitangent) < 0.0f
                    ? -1.0f
                    : 1.0f;
            }
            outVertex.tangent = Vec4(tangent, sign);
            outVertex.attributeMask |= 0x04u;
        }

        if (mesh.vertex_color.exists)
        {
            const ufbx_vec4 sourceColor =
                ufbx_get_vertex_vec4(&mesh.vertex_color, cornerIndex);
            outVertex.color = Vec4(sourceColor.x,
                                   sourceColor.y,
                                   sourceColor.z,
                                   sourceColor.w);
            if (!IsFinite(outVertex.color))
            {
                outError = "FBX mesh contains a non-finite vertex color.";
                return false;
            }
            outVertex.attributeMask |= 0x08u;
        }
        return true;
    }

    std::uint32_t ResolveMaterialId(const ufbx_scene& scene,
                             const ufbx_mesh& mesh,
                             size_t partIndex)
    {
        if (partIndex >= mesh.materials.count || !mesh.materials.data[partIndex])
        {
            return 0;
        }
        const ufbx_material* material = mesh.materials.data[partIndex];
        for (size_t index = 0; index < scene.materials.count; ++index)
        {
            if (scene.materials.data[index] == material)
            {
                return static_cast<std::uint32_t>(index);
            }
        }
        return 0;
    }

    Mesh::Ptr ConvertMesh(const ufbx_scene& scene,
                          const ufbx_mesh& source,
                          const FBXImportOptions& options,
                          std::string& outError)
    {
        if (!source.vertex_position.exists || source.num_faces == 0)
        {
            outError = "FBX static mesh has no indexed polygon geometry.";
            return nullptr;
        }

        auto mesh = std::make_shared<Mesh>();
        mesh->name = ToString(source.name);
        if (mesh->name.empty())
        {
            mesh->name = "FBXMesh_" + std::to_string(source.typed_id);
        }
        mesh->SetPrimitiveType(PrimitiveType::Triangles);

        std::vector<VertexKey> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<SubMesh> subMeshes;
        std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> vertexMap;
        vertices.reserve(source.num_indices);
        indices.reserve(source.num_triangles * 3u);

        std::vector<std::uint32_t> triangulated(source.max_face_triangles * 3u);
        if (triangulated.empty())
        {
            outError = "FBX mesh cannot reserve a triangulation buffer.";
            return nullptr;
        }

        for (size_t partIndex = 0; partIndex < source.material_parts.count; ++partIndex)
        {
            const ufbx_mesh_part& part = source.material_parts.data[partIndex];
            if (part.num_triangles == 0)
            {
                continue;
            }
            if (indices.size() > std::numeric_limits<std::uint32_t>::max())
            {
                outError = "FBX mesh index offset exceeds the engine limit.";
                return nullptr;
            }

            SubMesh subMesh;
            subMesh.name = mesh->name + "_material_" + std::to_string(partIndex);
            subMesh.indexOffset = static_cast<std::uint32_t>(indices.size());
            subMesh.materialId = ResolveMaterialId(scene, source, partIndex);
            subMesh.primitive = PrimitiveType::Triangles;

            for (size_t faceListIndex = 0;
                 faceListIndex < part.face_indices.count;
                 ++faceListIndex)
            {
                const std::uint32_t faceIndex = part.face_indices.data[faceListIndex];
                if (faceIndex >= source.faces.count)
                {
                    outError = "FBX material part contains an out-of-range face index.";
                    return nullptr;
                }
                const ufbx_face face = source.faces.data[faceIndex];
                if (face.num_indices < 3)
                {
                    outError = "FBX static mesh contains a non-polygon face.";
                    return nullptr;
                }
                const std::uint32_t triangleCount = ufbx_triangulate_face(
                    triangulated.data(), triangulated.size(), &source, face);
                if (triangleCount == 0)
                {
                    outError = "FBX polygon triangulation failed.";
                    return nullptr;
                }
                for (std::uint32_t triangleIndex = 0;
                     triangleIndex < triangleCount * 3u;
                     ++triangleIndex)
                {
                    const std::uint32_t cornerIndex = triangulated[triangleIndex];
                    if (cornerIndex >= source.num_indices)
                    {
                        outError = "FBX triangulation produced an out-of-range corner index.";
                        return nullptr;
                    }
                    VertexKey vertex;
                    if (!BuildVertex(source, cornerIndex, options, vertex, outError))
                    {
                        return nullptr;
                    }
                    const auto [it, inserted] = vertexMap.emplace(
                        vertex, static_cast<std::uint32_t>(vertices.size()));
                    if (inserted)
                    {
                        if (vertices.size() >= std::numeric_limits<std::uint32_t>::max())
                        {
                            outError = "FBX mesh vertex count exceeds the engine limit.";
                            return nullptr;
                        }
                        vertices.push_back(vertex);
                    }
                    indices.push_back(it->second);
                }
            }

            const size_t partIndexCount = indices.size() - subMesh.indexOffset;
            if (partIndexCount != part.num_triangles * 3u ||
                partIndexCount > std::numeric_limits<std::uint32_t>::max())
            {
                outError = "FBX material part triangulation count is inconsistent.";
                return nullptr;
            }
            subMesh.indexCount = static_cast<std::uint32_t>(partIndexCount);
            subMeshes.push_back(std::move(subMesh));
        }

        if (indices.empty() || vertices.empty() || subMeshes.empty())
        {
            outError = "FBX static mesh produced no renderable triangles.";
            return nullptr;
        }

        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<Vec2> uvs;
        std::vector<Vec4> tangents;
        std::vector<Vec4> colors;
        positions.reserve(vertices.size());
        const std::uint8_t attributeMask = vertices.front().attributeMask;
        if ((attributeMask & 0x01u) != 0) normals.reserve(vertices.size());
        if ((attributeMask & 0x02u) != 0) uvs.reserve(vertices.size());
        if ((attributeMask & 0x04u) != 0) tangents.reserve(vertices.size());
        if ((attributeMask & 0x08u) != 0) colors.reserve(vertices.size());

        for (const VertexKey& vertex : vertices)
        {
            if (vertex.attributeMask != attributeMask)
            {
                outError = "FBX vertex attribute presence is inconsistent across polygon corners.";
                return nullptr;
            }
            positions.push_back(vertex.position);
            if ((attributeMask & 0x01u) != 0) normals.push_back(vertex.normal);
            if ((attributeMask & 0x02u) != 0) uvs.push_back(vertex.uv);
            if ((attributeMask & 0x04u) != 0) tangents.push_back(vertex.tangent);
            if ((attributeMask & 0x08u) != 0) colors.push_back(vertex.color);
        }

        mesh->SetPositions(positions);
        mesh->SetIndices(indices);
        if (!normals.empty()) mesh->SetNormals(normals);
        if (!uvs.empty()) mesh->SetUVs(uvs);
        if (!tangents.empty()) mesh->SetTangents(tangents);
        if (!colors.empty()) mesh->SetColors(colors);
        mesh->SetSubMeshes(std::move(subMeshes));

        if (normals.empty() && options.generateNormals && !mesh->GenerateNormals())
        {
            outError = "FBX mesh normal generation failed.";
            return nullptr;
        }
        if (tangents.empty() && options.generateTangents &&
            mesh->HasAttribute(VertexBufferNames::Normal) &&
            mesh->HasAttribute(VertexBufferNames::UV) &&
            !mesh->GenerateTangents())
        {
            outError = "FBX mesh tangent generation failed.";
            return nullptr;
        }
        if (!mesh->ComputeBoundingBox())
        {
            outError = "FBX mesh bounds could not be computed.";
            return nullptr;
        }
        mesh->ComputeSubMeshBounds();
        if (!mesh->ValidateSubMeshes() || !mesh->IsValid())
        {
            outError = "FBX mesh failed canonical engine validation.";
            return nullptr;
        }
        return mesh;
    }

    struct TextureReferenceKey
    {
        const ufbx_texture* texture = nullptr;
        TextureUsage usage = TextureUsage::Color;

        bool operator==(const TextureReferenceKey&) const noexcept = default;
    };

    struct TextureReferenceKeyHash
    {
        size_t operator()(const TextureReferenceKey& key) const noexcept
        {
            const size_t pointerHash =
                std::hash<const ufbx_texture*>{}(key.texture);
            const size_t usageHash = std::hash<int>{}(static_cast<int>(key.usage));
            return pointerHash ^ (usageHash + 0x9e3779b9u +
                                  (pointerHash << 6u) + (pointerHash >> 2u));
        }
    };

    using TextureReferenceIndex =
        std::unordered_map<TextureReferenceKey, int, TextureReferenceKeyHash>;

    bool AddTextureReference(const fs::path& sourceRoot,
                             const ufbx_texture* texture,
                             TextureUsage usage,
                             std::vector<TextureReference>& textures,
                             TextureReferenceIndex& textureIndices,
                             int& outIndex,
                             std::string& outError)
    {
        outIndex = -1;
        if (!texture)
        {
            return true;
        }
        if (texture->content.size > 0 ||
            (texture->video && texture->video->content.size > 0))
        {
            outError = "Unsupported FBX feature: embedded texture.";
            return false;
        }
        if (texture->type != UFBX_TEXTURE_FILE)
        {
            outError = "Unsupported FBX feature: non-file texture.";
            return false;
        }

        const TextureReferenceKey key{texture, usage};
        const auto existing = textureIndices.find(key);
        if (existing != textureIndices.end())
        {
            outIndex = existing->second;
            return true;
        }

        fs::path path;
        std::shared_ptr<const std::vector<uint8_t>> payload;
        if (!ResolveExternalTexture(sourceRoot, *texture, path, payload, outError))
        {
            return false;
        }

        TextureReference reference = TextureReference::CreateExternal(
            path.generic_string(), usage, usage == TextureUsage::Color);
        reference.capturedPayload = std::move(payload);
        reference.imageIndex = static_cast<int>(textures.size());
        reference.fallbackSemantic = usage == TextureUsage::Normal
            ? TextureFallbackSemantic::FlatNormal
            : (usage == TextureUsage::Color
                   ? TextureFallbackSemantic::White
                   : TextureFallbackSemantic::Black);
        outIndex = reference.imageIndex;
        textureIndices.emplace(key, outIndex);
        textures.push_back(std::move(reference));
        return true;
    }

    bool ConvertMaterials(const ufbx_scene& scene,
                          const fs::path& sourceRoot,
                          FBXImportResult& result)
    {
        TextureReferenceIndex textureIndices;
        result.materials.reserve(std::max<size_t>(scene.materials.count, 1u));

        for (size_t materialIndex = 0;
             materialIndex < scene.materials.count;
             ++materialIndex)
        {
            const ufbx_material& source = *scene.materials.data[materialIndex];
            std::string name = ToString(source.name);
            if (name.empty())
            {
                name = "FBXMaterial_" + std::to_string(materialIndex);
            }
            auto material = std::make_shared<Material>(name);
            material->SetMaterialId(static_cast<std::uint32_t>(materialIndex));

            if (source.pbr.base_color.has_value)
            {
                const ufbx_vec4 value = source.pbr.base_color.value_vec4;
                material->SetBaseColor(value.x, value.y, value.z, value.w);
            }
            else if (source.fbx.diffuse_color.has_value)
            {
                const ufbx_vec3 value = source.fbx.diffuse_color.value_vec3;
                material->SetBaseColor(value.x, value.y, value.z, 1.0f);
            }
            if (source.pbr.metalness.has_value)
            {
                material->SetMetallicFactor(source.pbr.metalness.value_real);
            }
            if (source.pbr.roughness.has_value)
            {
                material->SetRoughnessFactor(source.pbr.roughness.value_real);
            }
            else if (source.fbx.specular_exponent.has_value)
            {
                const float shininess = std::max(
                    0.0f, static_cast<float>(source.fbx.specular_exponent.value_real));
                material->SetRoughnessFactor(
                    std::clamp(1.0f - std::sqrt(shininess / 100.0f), 0.0f, 1.0f));
            }
            if (source.pbr.emission_color.has_value)
            {
                const ufbx_vec3 value = source.pbr.emission_color.value_vec3;
                material->SetEmissiveColor(Vec3(value.x, value.y, value.z));
            }
            else if (source.fbx.emission_color.has_value)
            {
                const ufbx_vec3 value = source.fbx.emission_color.value_vec3;
                material->SetEmissiveColor(Vec3(value.x, value.y, value.z));
            }

            const auto bindTexture = [&](const ufbx_texture* texture,
                                         TextureUsage usage,
                                         auto setter) -> bool
            {
                int index = -1;
                if (!AddTextureReference(sourceRoot,
                                         texture,
                                         usage,
                                         result.textures,
                                         textureIndices,
                                         index,
                                         result.errorMessage))
                {
                    return false;
                }
                if (index >= 0)
                {
                    TextureInfo info;
                    info.imageId = index;
                    info.texturePath = result.textures[static_cast<size_t>(index)].path;
                    setter(info);
                }
                return true;
            };

            const ufbx_texture* baseColor = source.pbr.base_color.texture
                ? source.pbr.base_color.texture
                : source.fbx.diffuse_color.texture;
            const ufbx_texture* normal = source.pbr.normal_map.texture
                ? source.pbr.normal_map.texture
                : source.fbx.normal_map.texture;
            if (!bindTexture(baseColor, TextureUsage::Color,
                             [&](const TextureInfo& info)
                             { material->SetBaseColorTexture(info); }) ||
                !bindTexture(normal, TextureUsage::Normal,
                             [&](const TextureInfo& info)
                             { material->SetNormalTexture(info); }) ||
                !bindTexture(source.pbr.metalness.texture, TextureUsage::Data,
                             [&](const TextureInfo& info)
                             { material->SetMetallicRoughnessTexture(info); }) ||
                !bindTexture(source.pbr.ambient_occlusion.texture, TextureUsage::Data,
                             [&](const TextureInfo& info)
                             { material->SetOcclusionTexture(info); }) ||
                !bindTexture(source.pbr.emission_color.texture, TextureUsage::Color,
                             [&](const TextureInfo& info)
                             { material->SetEmissiveTexture(info); }))
            {
                return false;
            }
            result.materials.push_back(std::move(material));
        }

        if (result.materials.empty())
        {
            auto material = std::make_shared<Material>("DefaultMaterial");
            material->SetMaterialId(0);
            result.materials.push_back(std::move(material));
        }
        return true;
    }

    bool ValidateInstanceMaterials(const ufbx_node& node, std::string& outError)
    {
        if (!node.mesh || node.materials.count == 0)
        {
            return true;
        }
        if (node.materials.count != node.mesh->materials.count)
        {
            outError = "Unsupported FBX feature: per-instance material override.";
            return false;
        }
        for (size_t index = 0; index < node.materials.count; ++index)
        {
            if (node.materials.data[index] != node.mesh->materials.data[index])
            {
                outError = "Unsupported FBX feature: per-instance material override.";
                return false;
            }
        }
        return true;
    }

    Node::Ptr ConvertNode(
        const ufbx_node& source,
        const std::unordered_map<const ufbx_mesh*, int>& meshIndices,
        const std::vector<Mesh::Ptr>& meshes,
        std::uint32_t depth,
        std::string& outError)
    {
        if (depth > kMaxNodeDepth)
        {
            outError = "FBX node hierarchy exceeds the supported depth.";
            return nullptr;
        }
        if (!ValidateInstanceMaterials(source, outError))
        {
            return nullptr;
        }

        std::string name = ToString(source.name);
        if (name.empty())
        {
            name = "FBXNode_" + std::to_string(source.typed_id);
        }
        auto node = std::make_shared<Node>(name);
        Transform& transform = node->GetLocalTransform();
        const ufbx_transform& local = source.local_transform;
        const Vec3 position(local.translation.x,
                            local.translation.y,
                            local.translation.z);
        const Quat rotation(local.rotation.w,
                            local.rotation.x,
                            local.rotation.y,
                            local.rotation.z);
        const Vec3 scale(local.scale.x, local.scale.y, local.scale.z);
        if (!IsFinite(position) || !IsFinite(rotation) || !IsFinite(scale))
        {
            outError = "FBX node contains a non-finite local transform.";
            return nullptr;
        }
        transform.SetPosition(position);
        transform.SetRotation(glm::normalize(rotation));
        transform.SetScale(scale);

        if (source.mesh)
        {
            const auto found = meshIndices.find(source.mesh);
            if (found == meshIndices.end() || found->second < 0 ||
                static_cast<size_t>(found->second) >= meshes.size())
            {
                outError = "FBX node references a mesh that was not converted.";
                return nullptr;
            }
            node->SetMeshIndex(found->second);
            std::vector<int> materialIndices;
            for (const SubMesh& subMesh : meshes[static_cast<size_t>(found->second)]->GetSubMeshes())
            {
                materialIndices.push_back(static_cast<int>(subMesh.materialId));
            }
            node->SetMaterialIndices(materialIndices);
        }

        for (size_t childIndex = 0; childIndex < source.children.count; ++childIndex)
        {
            Node::Ptr child = ConvertNode(*source.children.data[childIndex],
                                          meshIndices,
                                          meshes,
                                          depth + 1u,
                                          outError);
            if (!child)
            {
                return nullptr;
            }
            node->AddChild(std::move(child));
        }
        return node;
    }

    bool ValidateSupportedSubset(const ufbx_scene& scene, std::string& outError)
    {
        if (scene.skin_deformers.count > 0 || scene.skin_clusters.count > 0)
        {
            outError = "Unsupported FBX feature: skinning.";
            return false;
        }
        if (scene.anim_curves.count > 0 || scene.anim_values.count > 0)
        {
            outError = "Unsupported FBX feature: animation.";
            return false;
        }
        if (scene.blend_deformers.count > 0 || scene.blend_channels.count > 0 ||
            scene.blend_shapes.count > 0)
        {
            outError = "Unsupported FBX feature: blend shape.";
            return false;
        }
        for (size_t index = 0; index < scene.textures.count; ++index)
        {
            const ufbx_texture* texture = scene.textures.data[index];
            if (texture && (texture->content.size > 0 ||
                            (texture->video && texture->video->content.size > 0)))
            {
                outError = "Unsupported FBX feature: embedded texture.";
                return false;
            }
        }
        for (size_t index = 0; index < scene.videos.count; ++index)
        {
            if (scene.videos.data[index] && scene.videos.data[index]->content.size > 0)
            {
                outError = "Unsupported FBX feature: embedded texture.";
                return false;
            }
        }
        return true;
    }
} // namespace

bool FBXImporter::CanImport(const std::string& path) const
{
    std::string extension = fs::path(path).extension().string();
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char value)
                   { return static_cast<char>(std::tolower(value)); });
    return extension == ".fbx";
}

FBXImportResult FBXImporter::Import(const std::string& path,
                                    const FBXImportOptions& options)
{
    FBXImportResult result;
    ReportProgress(0.0f, "Loading FBX file");

    std::error_code pathError;
    const fs::path sourcePath = fs::canonical(fs::absolute(fs::path(path)), pathError);
    if (pathError || !fs::is_regular_file(sourcePath, pathError))
    {
        result.errorMessage = "FBX source file does not exist: " + path;
        return result;
    }
    const fs::path sourceRoot = sourcePath.parent_path();

    ufbx_load_opts loadOptions{};
    loadOptions.strict = true;
    loadOptions.load_external_files = false;
    loadOptions.ignore_missing_external_files = true;
    loadOptions.generate_missing_normals = false;
    loadOptions.skip_skin_vertices = true;
    loadOptions.geometry_transform_handling =
        UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    loadOptions.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    loadOptions.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    loadOptions.target_axes = ufbx_axes_right_handed_y_up;
    loadOptions.target_unit_meters = 1.0f;
    loadOptions.node_depth_limit = kMaxNodeDepth;

    ufbx_error error{};
    ScenePtr scene(ufbx_load_file(sourcePath.string().c_str(), &loadOptions, &error));
    if (!scene)
    {
        result.errorMessage = "Failed to parse FBX source: " +
                              ToString(error.description);
        return result;
    }
    if (!ValidateSupportedSubset(*scene, result.errorMessage))
    {
        return result;
    }
    if (scene->meshes.count == 0 || !scene->root_node)
    {
        result.errorMessage = "FBX source contains no static model geometry.";
        return result;
    }

    ReportProgress(0.15f, "Validating FBX materials and textures");
    if (!ConvertMaterials(*scene, sourceRoot, result))
    {
        return result;
    }

    ReportProgress(0.35f, "Converting FBX polygon-corner geometry");
    result.meshes.reserve(scene->meshes.count);
    std::unordered_map<const ufbx_mesh*, int> meshIndices;
    for (size_t meshIndex = 0; meshIndex < scene->meshes.count; ++meshIndex)
    {
        Mesh::Ptr mesh = ConvertMesh(*scene,
                                     *scene->meshes.data[meshIndex],
                                     options,
                                     result.errorMessage);
        if (!mesh)
        {
            return result;
        }
        meshIndices.emplace(scene->meshes.data[meshIndex],
                            static_cast<int>(result.meshes.size()));
        result.meshes.push_back(std::move(mesh));
    }

    ReportProgress(0.75f, "Preserving FBX node hierarchy");
    Node::Ptr root = ConvertNode(*scene->root_node,
                                 meshIndices,
                                 result.meshes,
                                 0u,
                                 result.errorMessage);
    if (!root)
    {
        return result;
    }

    result.model = std::make_shared<Model>();
    result.model->path = sourcePath.generic_string();
    result.model->suffix = ".fbx";
    result.model->SetRootNode(std::move(root));
    result.model->SetHasAnimation(false);
    if (!result.model->ComputeBoundingBox(result.meshes))
    {
        result.errorMessage =
            "FBX model bounds require complete valid indexed mesh references.";
        result.model.reset();
        return result;
    }

    ReportProgress(0.95f, "Releasing FBX parser state");
    scene.reset();
    result.success = true;
    ReportProgress(1.0f, "Complete");
    return result;
}

void FBXImporter::ReportProgress(float progress, const std::string& stage) const
{
    if (m_progressCallback)
    {
        m_progressCallback(progress, stage);
    }
}
} // namespace RVX::Resource
