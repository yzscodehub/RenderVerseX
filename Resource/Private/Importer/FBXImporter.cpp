#include "Resource/Importer/FBXImporter.h"
#include "Core/Log.h"

#include <filesystem>
#include <algorithm>
#include <cstring>
#include <unordered_map>

#ifdef HAS_UFBX
// Include ufbx - header-only library
#define UFBX_REAL_IS_FLOAT
#include <ufbx.h>
#endif

namespace RVX::Resource
{
    // =========================================================================
    // Public Interface
    // =========================================================================

    bool FBXImporter::CanImport(const std::string& path) const
    {
#ifndef HAS_UFBX
        (void)path;
        return false;
#else
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".fbx";
#endif
    }

    FBXImportResult FBXImporter::Import(const std::string& path, const FBXImportOptions& options)
    {
        FBXImportResult result;
        m_currentFilePath = path;

#ifndef HAS_UFBX
        (void)options;
        result.success = false;
        result.errorMessage = "FBX loading not available: ufbx library not found during build";
        RVX_CORE_WARN("FBXImporter: {}", result.errorMessage);
        return result;
#else
        ReportProgress(0.0f, "Loading FBX file");

        // Check if file exists
        if (!std::filesystem::exists(path))
        {
            result.success = false;
            result.errorMessage = "File not found: " + path;
            return result;
        }

        // Load FBX file using ufbx
        ufbx_load_opts loadOpts = {};
        loadOpts.target_axes = ufbx_axes_right_handed_y_up;
        loadOpts.target_unit_meters = options.scaleFactor;
        
        if (options.convertToLeftHanded)
        {
            loadOpts.target_axes = ufbx_axes_left_handed_y_up;
        }

        ufbx_error error;
        ufbx_scene* scene = ufbx_load_file(path.c_str(), &loadOpts, &error);

        if (!scene)
        {
            result.success = false;
            result.errorMessage = "Failed to load FBX: ";
            result.errorMessage += error.description.data;
            return result;
        }

        // Get base path for texture resolution
        std::filesystem::path filePath(path);
        std::string basePath = filePath.parent_path().string();

        ReportProgress(0.1f, "Extracting materials and textures");
        ParseMaterials(scene, basePath, result);

        ReportProgress(0.3f, "Parsing meshes");
        ParseMeshes(scene, result, options);

        ReportProgress(0.5f, "Building scene graph");
        ParseNodes(scene, result, options);

        if (options.importSkeleton)
        {
            ReportProgress(0.6f, "Parsing skeleton");
            ParseSkeleton(scene, result);
        }

        if (options.importAnimations)
        {
            ReportProgress(0.7f, "Parsing animations");
            ParseAnimations(scene, result, options);
        }

        ReportProgress(0.9f, "Computing bounds");
        if (result.model)
        {
            result.model->path = path;
            result.model->suffix = ".fbx";
            result.model->ComputeBoundingBox();
            result.model->SetHasAnimation(!result.animations.empty());
        }

        // Cleanup ufbx scene
        ufbx_free_scene(scene);

        ReportProgress(1.0f, "Complete");
        result.success = true;

        RVX_CORE_INFO("FBXImporter: Loaded {} meshes, {} materials, {} animations from {}",
            result.meshes.size(), result.materials.size(), result.animations.size(),
            filePath.filename().string());

        return result;
#endif
    }

#ifdef HAS_UFBX
    // =========================================================================
    // Material Parsing
    // =========================================================================

    void FBXImporter::ParseMaterials(ufbx_scene* scene, const std::string& basePath,
                                      FBXImportResult& result)
    {
        result.materials.reserve(scene->materials.count);

        for (size_t i = 0; i < scene->materials.count; ++i)
        {
            ufbx_material* mat = scene->materials.data[i];
            result.materials.push_back(ConvertMaterial(scene, mat, basePath, 
                                                        static_cast<int>(i), result));
        }

        // Add a default material if none exist
        if (result.materials.empty())
        {
            auto defaultMat = std::make_shared<Material>("DefaultMaterial");
            defaultMat->SetMaterialId(0);
            result.materials.push_back(defaultMat);
        }
    }

    Material::Ptr FBXImporter::ConvertMaterial(ufbx_scene* scene, ufbx_material* mat,
                                                const std::string& basePath, int materialIndex,
                                                FBXImportResult& result)
    {
        std::string matName = mat->name.length > 0 ? 
            std::string(mat->name.data, mat->name.length) : 
            "Material_" + std::to_string(materialIndex);

        auto material = std::make_shared<Material>(matName);
        material->SetMaterialId(static_cast<uint32_t>(materialIndex));

        // PBR properties
        if (mat->pbr.base_color.has_value)
        {
            material->SetBaseColor(
                mat->pbr.base_color.value_vec4.x,
                mat->pbr.base_color.value_vec4.y,
                mat->pbr.base_color.value_vec4.z,
                mat->pbr.base_color.value_vec4.w
            );
        }
        else if (mat->fbx.diffuse_color.has_value)
        {
            // Fallback to FBX classic properties
            material->SetBaseColor(
                mat->fbx.diffuse_color.value_vec3.x,
                mat->fbx.diffuse_color.value_vec3.y,
                mat->fbx.diffuse_color.value_vec3.z,
                1.0f
            );
        }

        // Metallic
        if (mat->pbr.metalness.has_value)
        {
            material->SetMetallicFactor(mat->pbr.metalness.value_real);
        }

        // Roughness
        if (mat->pbr.roughness.has_value)
        {
            material->SetRoughnessFactor(mat->pbr.roughness.value_real);
        }
        else if (mat->fbx.specular_exponent.has_value)
        {
            // Convert shininess to roughness
            float shininess = mat->fbx.specular_exponent.value_real;
            float roughness = 1.0f - std::sqrt(shininess / 100.0f);
            material->SetRoughnessFactor(std::clamp(roughness, 0.0f, 1.0f));
        }

        // Emissive
        if (mat->pbr.emission_color.has_value)
        {
            material->SetEmissiveColor(Vec3(
                mat->pbr.emission_color.value_vec3.x,
                mat->pbr.emission_color.value_vec3.y,
                mat->pbr.emission_color.value_vec3.z
            ));
        }
        else if (mat->fbx.emission_color.has_value)
        {
            material->SetEmissiveColor(Vec3(
                mat->fbx.emission_color.value_vec3.x,
                mat->fbx.emission_color.value_vec3.y,
                mat->fbx.emission_color.value_vec3.z
            ));
        }

        // Textures
        // Base color / Diffuse texture
        if (mat->pbr.base_color.texture)
        {
            auto texRef = ExtractTexture(scene, mat->pbr.base_color.texture, basePath, TextureUsage::Color);
            if (texRef.IsValid())
            {
                int texIndex = static_cast<int>(result.textures.size());
                result.textures.push_back(texRef);
                
                TextureInfo info;
                info.imageId = texIndex;
                material->SetBaseColorTexture(info);
            }
        }
        else if (mat->fbx.diffuse_color.texture)
        {
            auto texRef = ExtractTexture(scene, mat->fbx.diffuse_color.texture, basePath, TextureUsage::Color);
            if (texRef.IsValid())
            {
                int texIndex = static_cast<int>(result.textures.size());
                result.textures.push_back(texRef);
                
                TextureInfo info;
                info.imageId = texIndex;
                material->SetBaseColorTexture(info);
            }
        }

        // Normal map
        if (mat->pbr.normal_map.texture)
        {
            auto texRef = ExtractTexture(scene, mat->pbr.normal_map.texture, basePath, TextureUsage::Normal);
            if (texRef.IsValid())
            {
                int texIndex = static_cast<int>(result.textures.size());
                result.textures.push_back(texRef);
                
                TextureInfo info;
                info.imageId = texIndex;
                material->SetNormalTexture(info);
            }
        }
        else if (mat->fbx.normal_map.texture)
        {
            auto texRef = ExtractTexture(scene, mat->fbx.normal_map.texture, basePath, TextureUsage::Normal);
            if (texRef.IsValid())
            {
                int texIndex = static_cast<int>(result.textures.size());
                result.textures.push_back(texRef);
                
                TextureInfo info;
                info.imageId = texIndex;
                material->SetNormalTexture(info);
            }
        }

        // Metallic-Roughness (packed texture)
        if (mat->pbr.metalness.texture)
        {
            auto texRef = ExtractTexture(scene, mat->pbr.metalness.texture, basePath, TextureUsage::Data);
            if (texRef.IsValid())
            {
                int texIndex = static_cast<int>(result.textures.size());
                result.textures.push_back(texRef);
                
                TextureInfo info;
                info.imageId = texIndex;
                material->SetMetallicRoughnessTexture(info);
            }
        }

        // Occlusion
        if (mat->pbr.ambient_occlusion.texture)
        {
            auto texRef = ExtractTexture(scene, mat->pbr.ambient_occlusion.texture, basePath, TextureUsage::Data);
            if (texRef.IsValid())
            {
                int texIndex = static_cast<int>(result.textures.size());
                result.textures.push_back(texRef);
                
                TextureInfo info;
                info.imageId = texIndex;
                material->SetOcclusionTexture(info);
            }
        }

        // Emissive texture
        if (mat->pbr.emission_color.texture)
        {
            auto texRef = ExtractTexture(scene, mat->pbr.emission_color.texture, basePath, TextureUsage::Color);
            if (texRef.IsValid())
            {
                int texIndex = static_cast<int>(result.textures.size());
                result.textures.push_back(texRef);
                
                TextureInfo info;
                info.imageId = texIndex;
                material->SetEmissiveTexture(info);
            }
        }

        return material;
    }

    TextureReference FBXImporter::ExtractTexture(ufbx_scene* scene, void* texturePtr,
                                                  const std::string& basePath, TextureUsage usage)
    {
        TextureReference ref;
        ref.usage = usage;

        ufbx_texture* texture = static_cast<ufbx_texture*>(texturePtr);
        if (!texture)
        {
            return ref;
        }

        // Check if texture has an external file
        if (texture->filename.length > 0)
        {
            std::string texturePath(texture->filename.data, texture->filename.length);
            
            // Try to resolve relative path
            std::filesystem::path fullPath;
            if (std::filesystem::path(texturePath).is_absolute())
            {
                fullPath = texturePath;
            }
            else
            {
                // Try relative to FBX file
                fullPath = std::filesystem::path(basePath) / texturePath;
                if (!std::filesystem::exists(fullPath))
                {
                    // Try just the filename
                    fullPath = std::filesystem::path(basePath) / 
                               std::filesystem::path(texturePath).filename();
                }
            }

            if (std::filesystem::exists(fullPath))
            {
                ref.sourceType = TextureSourceType::External;
                ref.path = fullPath.string();
                ref.isSRGB = (usage == TextureUsage::Color);
            }
            else
            {
                RVX_CORE_WARN("FBXImporter: Texture not found: {}", texturePath);
            }
        }
        else if (texture->content.size > 0)
        {
            // Embedded texture data
            ref.sourceType = TextureSourceType::Embedded;
            ref.embeddedData.assign(
                static_cast<const uint8_t*>(texture->content.data),
                static_cast<const uint8_t*>(texture->content.data) + texture->content.size
            );
            ref.isRawPixelData = false;
            ref.isSRGB = (usage == TextureUsage::Color);
        }

        return ref;
    }

    // =========================================================================
    // Mesh Parsing
    // =========================================================================

    void FBXImporter::ParseMeshes(ufbx_scene* scene, FBXImportResult& result,
                                   const FBXImportOptions& options)
    {
        result.meshes.reserve(scene->meshes.count);

        for (size_t i = 0; i < scene->meshes.count; ++i)
        {
            ufbx_mesh* mesh = scene->meshes.data[i];
            auto convertedMesh = ConvertMesh(scene, mesh, options, static_cast<int>(i));
            if (convertedMesh)
            {
                result.meshes.push_back(convertedMesh);
            }
        }
    }

    Mesh::Ptr FBXImporter::ConvertMesh(ufbx_scene* scene, ufbx_mesh* mesh,
                                        const FBXImportOptions& options, int meshIndex)
    {
        auto resultMesh = std::make_shared<Mesh>();
        
        std::string meshName = mesh->name.length > 0 ?
            std::string(mesh->name.data, mesh->name.length) :
            "Mesh_" + std::to_string(meshIndex);
        resultMesh->name = meshName;

        // Extract vertex positions
        std::vector<Vec3> positions;
        positions.reserve(mesh->num_vertices);
        
        for (size_t i = 0; i < mesh->num_vertices; ++i)
        {
            ufbx_vec3 pos = mesh->vertices[i];
            positions.push_back(ConvertPosition(pos.x, pos.y, pos.z, options));
        }
        resultMesh->AddAttribute(VertexBufferNames::Position, positions);

        // Triangulate and extract indices
        size_t numTriangles = mesh->num_triangles;
        std::vector<uint32_t> indices;
        indices.reserve(numTriangles * 3);

        // Create a triangulated mesh for easier processing
        size_t numTriIndices = mesh->max_face_triangles * 3;
        std::vector<uint32_t> triIndices(numTriIndices);

        for (size_t faceIdx = 0; faceIdx < mesh->num_faces; ++faceIdx)
        {
            ufbx_face face = mesh->faces[faceIdx];
            uint32_t numTris = ufbx_triangulate_face(triIndices.data(), numTriIndices, mesh, face);
            
            for (uint32_t i = 0; i < numTris * 3; ++i)
            {
                indices.push_back(triIndices[i]);
            }
        }
        resultMesh->SetIndices(indices);
        resultMesh->SetPrimitiveType(PrimitiveType::Triangles);

        // Extract normals
        if (mesh->vertex_normal.exists)
        {
            std::vector<Vec3> normals;
            normals.reserve(mesh->num_vertices);
            
            for (size_t i = 0; i < mesh->num_vertices; ++i)
            {
                ufbx_vec3 normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, i);
                Vec3 n = ConvertPosition(normal.x, normal.y, normal.z, options);
                normals.push_back(glm::normalize(n));
            }
            resultMesh->AddAttribute(VertexBufferNames::Normal, normals);
        }
        else if (options.generateNormals)
        {
            resultMesh->GenerateNormals();
        }

        // Extract UVs
        if (mesh->vertex_uv.exists)
        {
            std::vector<Vec2> uvs;
            uvs.reserve(mesh->num_vertices);
            
            for (size_t i = 0; i < mesh->num_vertices; ++i)
            {
                ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, i);
                float v = options.flipUVs ? 1.0f - uv.y : uv.y;
                uvs.push_back(Vec2(uv.x, v));
            }
            resultMesh->AddAttribute(VertexBufferNames::UV, uvs);
        }

        // Extract tangents
        if (mesh->vertex_tangent.exists)
        {
            std::vector<Vec4> tangents;
            tangents.reserve(mesh->num_vertices);
            
            for (size_t i = 0; i < mesh->num_vertices; ++i)
            {
                ufbx_vec3 tangent = ufbx_get_vertex_vec3(&mesh->vertex_tangent, i);
                // Bitangent sign (handedness)
                float sign = 1.0f;
                if (mesh->vertex_bitangent.exists)
                {
                    ufbx_vec3 bitangent = ufbx_get_vertex_vec3(&mesh->vertex_bitangent, i);
                    ufbx_vec3 normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, i);
                    // Cross product to determine handedness
                    ufbx_vec3 cross = {
                        normal.y * tangent.z - normal.z * tangent.y,
                        normal.z * tangent.x - normal.x * tangent.z,
                        normal.x * tangent.y - normal.y * tangent.x
                    };
                    float dot = cross.x * bitangent.x + cross.y * bitangent.y + cross.z * bitangent.z;
                    sign = dot < 0.0f ? -1.0f : 1.0f;
                }
                Vec3 t = ConvertPosition(tangent.x, tangent.y, tangent.z, options);
                tangents.push_back(Vec4(glm::normalize(t), sign));
            }
            resultMesh->AddAttribute(VertexBufferNames::Tangent, tangents);
        }
        else if (options.generateTangents && resultMesh->HasAttribute(VertexBufferNames::Normal) &&
                 resultMesh->HasAttribute(VertexBufferNames::UV))
        {
            resultMesh->GenerateTangents();
        }

        // Extract vertex colors
        if (mesh->vertex_color.exists)
        {
            std::vector<Vec4> colors;
            colors.reserve(mesh->num_vertices);
            
            for (size_t i = 0; i < mesh->num_vertices; ++i)
            {
                ufbx_vec4 color = ufbx_get_vertex_vec4(&mesh->vertex_color, i);
                colors.push_back(Vec4(color.x, color.y, color.z, color.w));
            }
            resultMesh->AddAttribute(VertexBufferNames::Color, colors);
        }

        // Extract bone weights if skinned
        if (mesh->skin_deformers.count > 0)
        {
            ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
            
            std::vector<IVec4> boneIndices(mesh->num_vertices, IVec4(0));
            std::vector<Vec4> boneWeights(mesh->num_vertices, Vec4(0.0f));

            for (size_t vertIdx = 0; vertIdx < mesh->num_vertices; ++vertIdx)
            {
                ufbx_skin_vertex skinVertex = skin->vertices[vertIdx];
                
                // Gather up to maxBonesPerVertex influences
                std::vector<std::pair<int, float>> influences;
                influences.reserve(skinVertex.num_weights);
                
                for (size_t w = 0; w < skinVertex.num_weights; ++w)
                {
                    ufbx_skin_weight weight = skin->weights[skinVertex.weight_begin + w];
                    influences.push_back({static_cast<int>(weight.cluster_index), 
                                         static_cast<float>(weight.weight)});
                }

                // Sort by weight descending
                std::sort(influences.begin(), influences.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; });

                // Take top influences
                float totalWeight = 0.0f;
                for (int i = 0; i < std::min(options.maxBonesPerVertex, 
                                              static_cast<int>(influences.size())); ++i)
                {
                    boneIndices[vertIdx][i] = influences[i].first;
                    boneWeights[vertIdx][i] = influences[i].second;
                    totalWeight += influences[i].second;
                }

                // Normalize weights
                if (totalWeight > 0.0f)
                {
                    boneWeights[vertIdx] /= totalWeight;
                }
            }

            resultMesh->AddAttribute(VertexBufferNames::BoneIndices, boneIndices);
            resultMesh->AddAttribute(VertexBufferNames::BoneWeights, boneWeights);
        }

        // Compute bounding box
        resultMesh->ComputeBoundingBox();

        // Create submeshes per material
        for (size_t matIdx = 0; matIdx < mesh->material_parts.count; ++matIdx)
        {
            ufbx_mesh_part* meshPart = &mesh->material_parts.data[matIdx];
            if (meshPart->num_triangles == 0) continue;

            SubMesh subMesh;
            subMesh.name = meshName + "_" + std::to_string(matIdx);
            
            // Find material index in scene
            if (matIdx < mesh->materials.count && mesh->materials.data[matIdx])
            {
                for (size_t i = 0; i < scene->materials.count; ++i)
                {
                    if (scene->materials.data[i] == mesh->materials.data[matIdx])
                    {
                        subMesh.materialId = static_cast<uint32_t>(i);
                        break;
                    }
                }
            }

            subMesh.indexOffset = 0;  // Would need proper offset tracking for multi-material
            subMesh.indexCount = static_cast<uint32_t>(meshPart->num_triangles * 3);
            subMesh.localBounds = resultMesh->GetBoundingBox();
            
            resultMesh->AddSubMesh(subMesh);
        }

        // If no submeshes created, create one with default material
        if (resultMesh->GetSubMeshes().empty())
        {
            SubMesh subMesh;
            subMesh.name = meshName;
            subMesh.materialId = 0;
            subMesh.indexOffset = 0;
            subMesh.indexCount = static_cast<uint32_t>(resultMesh->GetIndexCount());
            subMesh.localBounds = resultMesh->GetBoundingBox();
            resultMesh->AddSubMesh(subMesh);
        }

        return resultMesh;
    }

    // =========================================================================
    // Node Parsing
    // =========================================================================

    void FBXImporter::ParseNodes(ufbx_scene* scene, FBXImportResult& result,
                                  const FBXImportOptions& options)
    {
        // Build mesh index map
        std::unordered_map<ufbx_mesh*, int> meshIndexMap;
        for (size_t i = 0; i < scene->meshes.count; ++i)
        {
            meshIndexMap[scene->meshes.data[i]] = static_cast<int>(i);
        }

        // Convert root node
        if (scene->root_node)
        {
            result.model = std::make_shared<Model>();
            
            auto rootNode = ConvertNode(scene, scene->root_node, options, meshIndexMap);
            result.model->SetRootNode(rootNode);
        }
    }

    Node::Ptr FBXImporter::ConvertNode(ufbx_scene* scene, ufbx_node* node,
                                        const FBXImportOptions& options,
                                        const std::unordered_map<ufbx_mesh*, int>& meshIndexMap)
    {
        std::string nodeName = node->name.length > 0 ?
            std::string(node->name.data, node->name.length) : "Node";

        auto resultNode = std::make_shared<Node>(nodeName);

        // Set transform
        Transform& transform = resultNode->GetLocalTransform();

        // Extract local transform
        ufbx_transform localTransform = node->local_transform;
        
        transform.SetPosition(ConvertPosition(
            localTransform.translation.x,
            localTransform.translation.y,
            localTransform.translation.z,
            options));

        transform.SetRotation(ConvertRotation(
            localTransform.rotation.x,
            localTransform.rotation.y,
            localTransform.rotation.z,
            localTransform.rotation.w,
            options));

        transform.SetScale(Vec3(
            localTransform.scale.x,
            localTransform.scale.y,
            localTransform.scale.z));

        // Set mesh reference
        if (node->mesh)
        {
            auto it = meshIndexMap.find(node->mesh);
            if (it != meshIndexMap.end())
            {
                resultNode->SetMeshIndex(it->second);

                // Set material indices
                std::vector<int> materialIndices;
                for (size_t i = 0; i < node->mesh->materials.count; ++i)
                {
                    ufbx_material* mat = node->mesh->materials.data[i];
                    if (mat)
                    {
                        // Find material index
                        for (size_t matIdx = 0; matIdx < scene->materials.count; ++matIdx)
                        {
                            if (scene->materials.data[matIdx] == mat)
                            {
                                materialIndices.push_back(static_cast<int>(matIdx));
                                break;
                            }
                        }
                    }
                    else
                    {
                        materialIndices.push_back(0);
                    }
                }
                resultNode->SetMaterialIndices(materialIndices);
            }
        }

        // Process children
        for (size_t i = 0; i < node->children.count; ++i)
        {
            auto childNode = ConvertNode(scene, node->children.data[i], options, meshIndexMap);
            resultNode->AddChild(childNode);
        }

        return resultNode;
    }

    // =========================================================================
    // Skeleton Parsing
    // =========================================================================

    void FBXImporter::ParseSkeleton(ufbx_scene* scene, FBXImportResult& result)
    {
        if (scene->skin_deformers.count == 0)
        {
            return;
        }

        result.skeleton = std::make_unique<FBXSkeleton>();

        // Gather all bones from all skin deformers
        std::unordered_map<ufbx_node*, int> boneIndexMap;
        std::vector<ufbx_node*> boneNodes;

        for (size_t skinIdx = 0; skinIdx < scene->skin_deformers.count; ++skinIdx)
        {
            ufbx_skin_deformer* skin = scene->skin_deformers.data[skinIdx];
            
            for (size_t clusterIdx = 0; clusterIdx < skin->clusters.count; ++clusterIdx)
            {
                ufbx_skin_cluster* cluster = skin->clusters.data[clusterIdx];
                ufbx_node* boneNode = cluster->bone_node;
                
                if (boneNode && boneIndexMap.find(boneNode) == boneIndexMap.end())
                {
                    int index = static_cast<int>(boneNodes.size());
                    boneIndexMap[boneNode] = index;
                    boneNodes.push_back(boneNode);
                }
            }
        }

        // Build skeleton structure
        result.skeleton->bones.reserve(boneNodes.size());
        result.skeleton->boneNames.reserve(boneNodes.size());

        for (size_t i = 0; i < boneNodes.size(); ++i)
        {
            ufbx_node* boneNode = boneNodes[i];
            FBXSkeleton::Bone bone;
            
            bone.name = boneNode->name.length > 0 ?
                std::string(boneNode->name.data, boneNode->name.length) :
                "Bone_" + std::to_string(i);

            // Find parent bone
            if (boneNode->parent)
            {
                auto it = boneIndexMap.find(boneNode->parent);
                if (it != boneIndexMap.end())
                {
                    bone.parentIndex = it->second;
                }
            }

            // Get bind pose matrix from skin cluster
            for (size_t skinIdx = 0; skinIdx < scene->skin_deformers.count; ++skinIdx)
            {
                ufbx_skin_deformer* skin = scene->skin_deformers.data[skinIdx];
                for (size_t clusterIdx = 0; clusterIdx < skin->clusters.count; ++clusterIdx)
                {
                    ufbx_skin_cluster* cluster = skin->clusters.data[clusterIdx];
                    if (cluster->bone_node == boneNode)
                    {
                        // Store inverse bind pose
                        ufbx_matrix mat = cluster->geometry_to_bone;
                        bone.inverseBindPose = Mat4(
                            mat.m00, mat.m10, mat.m20, 0.0f,
                            mat.m01, mat.m11, mat.m21, 0.0f,
                            mat.m02, mat.m12, mat.m22, 0.0f,
                            mat.m03, mat.m13, mat.m23, 1.0f
                        );
                        bone.bindPose = glm::inverse(bone.inverseBindPose);
                        break;
                    }
                }
            }

            result.skeleton->bones.push_back(bone);
            result.skeleton->boneNames.push_back(bone.name);
        }

        RVX_CORE_INFO("FBXImporter: Extracted skeleton with {} bones", result.skeleton->bones.size());
    }

    // =========================================================================
    // Animation Parsing
    // =========================================================================

    void FBXImporter::ParseAnimations(ufbx_scene* scene, FBXImportResult& result,
                                       const FBXImportOptions& options)
    {
        if (scene->anim_stacks.count == 0 || !result.skeleton)
        {
            return;
        }

        result.animations.reserve(scene->anim_stacks.count);

        for (size_t i = 0; i < scene->anim_stacks.count; ++i)
        {
            ufbx_anim_stack* animStack = scene->anim_stacks.data[i];
            auto clip = ConvertAnimation(scene, animStack, options, result.skeleton.get());
            if (!clip.boneAnimations.empty())
            {
                result.animations.push_back(std::move(clip));
            }
        }

        RVX_CORE_INFO("FBXImporter: Extracted {} animation clips", result.animations.size());
    }

    FBXAnimationClip FBXImporter::ConvertAnimation(ufbx_scene* scene, ufbx_anim_stack* animStack,
                                                     const FBXImportOptions& options,
                                                     const FBXSkeleton* skeleton)
    {
        FBXAnimationClip clip;
        
        clip.name = animStack->name.length > 0 ?
            std::string(animStack->name.data, animStack->name.length) :
            "Animation";

        // Get animation time range
        double startTime = animStack->time_begin;
        double endTime = animStack->time_end;
        clip.duration = static_cast<float>(endTime - startTime);
        
        // Determine sample rate
        float sampleRate = options.animationSampleRate > 0.0f ?
            options.animationSampleRate : 30.0f;
        clip.framesPerSecond = sampleRate;

        // Sample animation for each bone
        clip.boneAnimations.reserve(skeleton->bones.size());

        // Create animation evaluator
        ufbx_anim* anim = animStack->anim;

        for (size_t boneIdx = 0; boneIdx < skeleton->bones.size(); ++boneIdx)
        {
            const auto& bone = skeleton->bones[boneIdx];
            
            // Find the node for this bone
            ufbx_node* boneNode = nullptr;
            for (size_t nodeIdx = 0; nodeIdx < scene->nodes.count; ++nodeIdx)
            {
                ufbx_node* node = scene->nodes.data[nodeIdx];
                std::string nodeName = node->name.length > 0 ?
                    std::string(node->name.data, node->name.length) : "";
                if (nodeName == bone.name)
                {
                    boneNode = node;
                    break;
                }
            }

            if (!boneNode) continue;

            FBXAnimationClip::BoneAnimation boneAnim;
            boneAnim.boneName = bone.name;
            boneAnim.boneIndex = static_cast<int>(boneIdx);

            // Sample the animation
            int numSamples = static_cast<int>(clip.duration * sampleRate) + 1;
            boneAnim.positionTimes.reserve(numSamples);
            boneAnim.rotationTimes.reserve(numSamples);
            boneAnim.scaleTimes.reserve(numSamples);
            boneAnim.positions.reserve(numSamples);
            boneAnim.rotations.reserve(numSamples);
            boneAnim.scales.reserve(numSamples);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                double time = startTime + (sample / sampleRate);
                float keyTime = static_cast<float>(time - startTime);

                // Evaluate transform at this time
                ufbx_transform transform = ufbx_evaluate_transform(anim, boneNode, time);

                boneAnim.positionTimes.push_back(keyTime);
                boneAnim.rotationTimes.push_back(keyTime);
                boneAnim.scaleTimes.push_back(keyTime);

                boneAnim.positions.push_back(ConvertPosition(
                    transform.translation.x,
                    transform.translation.y,
                    transform.translation.z,
                    options));

                boneAnim.rotations.push_back(ConvertRotation(
                    transform.rotation.x,
                    transform.rotation.y,
                    transform.rotation.z,
                    transform.rotation.w,
                    options));

                boneAnim.scales.push_back(Vec3(
                    transform.scale.x,
                    transform.scale.y,
                    transform.scale.z));
            }

            clip.boneAnimations.push_back(std::move(boneAnim));
        }

        return clip;
    }

    // =========================================================================
    // Coordinate System Conversion
    // =========================================================================

    Vec3 FBXImporter::ConvertPosition(float x, float y, float z, const FBXImportOptions& options) const
    {
        // ufbx handles coordinate system conversion via load options
        // Apply scale factor is handled by ufbx
        return Vec3(x, y, z);
    }

    Quat FBXImporter::ConvertRotation(float x, float y, float z, float w, const FBXImportOptions& options) const
    {
        // ufbx quaternion order: x, y, z, w
        // GLM quaternion order: w, x, y, z
        return Quat(w, x, y, z);
    }

    Mat4 FBXImporter::ConvertMatrix(const float* data, const FBXImportOptions& options) const
    {
        return Mat4(
            data[0], data[1], data[2], data[3],
            data[4], data[5], data[6], data[7],
            data[8], data[9], data[10], data[11],
            data[12], data[13], data[14], data[15]
        );
    }
#else // !HAS_UFBX
    // Stub implementations when ufbx is not available
    void FBXImporter::ParseMaterials(ufbx_scene*, const std::string&, FBXImportResult&) {}
    Material::Ptr FBXImporter::ConvertMaterial(ufbx_scene*, ufbx_material*, const std::string&, int, FBXImportResult&) { return nullptr; }
    TextureReference FBXImporter::ExtractTexture(ufbx_scene*, void*, const std::string&, TextureUsage) { return {}; }
    void FBXImporter::ParseMeshes(ufbx_scene*, FBXImportResult&, const FBXImportOptions&) {}
    Mesh::Ptr FBXImporter::ConvertMesh(ufbx_scene*, ufbx_mesh*, const FBXImportOptions&, int) { return nullptr; }
    void FBXImporter::ParseNodes(ufbx_scene*, FBXImportResult&, const FBXImportOptions&) {}
    Node::Ptr FBXImporter::ConvertNode(ufbx_scene*, ufbx_node*, const FBXImportOptions&, const std::unordered_map<ufbx_mesh*, int>&) { return nullptr; }
    void FBXImporter::ParseSkeleton(ufbx_scene*, FBXImportResult&) {}
    void FBXImporter::ParseAnimations(ufbx_scene*, FBXImportResult&, const FBXImportOptions&) {}
    FBXAnimationClip FBXImporter::ConvertAnimation(ufbx_scene*, ufbx_anim_stack*, const FBXImportOptions&, const FBXSkeleton*) { return {}; }
    Vec3 FBXImporter::ConvertPosition(float, float, float, const FBXImportOptions&) const { return {}; }
    Quat FBXImporter::ConvertRotation(float, float, float, float, const FBXImportOptions&) const { return {}; }
    Mat4 FBXImporter::ConvertMatrix(const float*, const FBXImportOptions&) const { return Mat4(1.0f); }
#endif // HAS_UFBX

    // =========================================================================
    // Utility
    // =========================================================================

    void FBXImporter::ReportProgress(float progress, const std::string& stage)
    {
        if (m_progressCallback)
        {
            m_progressCallback(progress, stage);
        }
    }

    PrimitiveType FBXImporter::GetPrimitiveType(int faceSize)
    {
        switch (faceSize)
        {
            case 1: return PrimitiveType::Points;
            case 2: return PrimitiveType::Lines;
            case 3: return PrimitiveType::Triangles;
            default: return PrimitiveType::Triangles;
        }
    }

} // namespace RVX::Resource
