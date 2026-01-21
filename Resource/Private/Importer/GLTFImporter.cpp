#include "Resource/Importer/GLTFImporter.h"
#include "Core/Log.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#include <filesystem>
#include <algorithm>
#include <cstring>

namespace RVX::Resource
{
    // =========================================================================
    // Public Interface
    // =========================================================================

    bool GLTFImporter::CanImport(const std::string& path) const
    {
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".gltf" || ext == ".glb";
    }

    GLTFImportResult GLTFImporter::Import(const std::string& path, const GLTFImportOptions& options)
    {
        GLTFImportResult result;
        m_currentFilePath = path;

        ReportProgress(0.0f, "Loading file");

        // Load the glTF file
        tinygltf::Model gltfModel;
        std::string error, warning;
        
        if (!LoadFile(path, gltfModel, error, warning))
        {
            result.success = false;
            result.errorMessage = error;
            return result;
        }

        if (!warning.empty())
        {
            result.warnings.push_back(warning);
        }

        ReportProgress(0.1f, "Extracting textures");
        ExtractTextures(gltfModel, path, result);

        ReportProgress(0.3f, "Parsing materials");
        ParseMaterials(gltfModel, result);

        ReportProgress(0.5f, "Parsing meshes");
        ParseMeshes(gltfModel, result, options);

        ReportProgress(0.7f, "Building scene graph");
        ParseNodes(gltfModel, result);
        ParseScene(gltfModel, result);

        ReportProgress(0.9f, "Computing bounds");
        if (result.model)
        {
            result.model->path = path;
            std::filesystem::path filePath(path);
            result.model->suffix = filePath.extension().string();
            result.model->ComputeBoundingBox();
        }

        ReportProgress(1.0f, "Complete");
        result.success = true;
        return result;
    }

    // =========================================================================
    // File Loading
    // =========================================================================

    bool GLTFImporter::LoadFile(const std::string& path, tinygltf::Model& gltfModel,
                                 std::string& error, std::string& warning)
    {
        tinygltf::TinyGLTF loader;
        
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool success = false;
        if (ext == ".glb")
        {
            success = loader.LoadBinaryFromFile(&gltfModel, &error, &warning, path);
        }
        else
        {
            success = loader.LoadASCIIFromFile(&gltfModel, &error, &warning, path);
        }

        return success;
    }

    // =========================================================================
    // Texture Extraction
    // =========================================================================

    void GLTFImporter::ExtractTextures(const tinygltf::Model& gltf, const std::string& basePath,
                                        GLTFImportResult& result)
    {
        result.textures.resize(gltf.images.size());

        for (size_t i = 0; i < gltf.images.size(); ++i)
        {
            const auto& image = gltf.images[i];
            TextureReference& ref = result.textures[i];
            ref.imageIndex = static_cast<int>(i);

            if (!image.uri.empty() && image.uri.find("data:") != 0)
            {
                // External texture file
                ref.sourceType = TextureSourceType::External;
                ref.path = image.uri;
            }
            else
            {
                // Embedded texture (in GLB or base64)
                ref.sourceType = TextureSourceType::Embedded;
                ref.mimeType = image.mimeType;
                
                if (!image.image.empty())
                {
                    // Already decoded by tinygltf - raw RGBA pixels
                    ref.embeddedData.assign(image.image.begin(), image.image.end());
                    ref.isRawPixelData = true;
                    ref.rawWidth = static_cast<uint32_t>(image.width);
                    ref.rawHeight = static_cast<uint32_t>(image.height);
                }
                else if (image.bufferView >= 0)
                {
                    // Need to extract from buffer - still encoded (PNG/JPEG)
                    const auto& bufferView = gltf.bufferViews[image.bufferView];
                    const auto& buffer = gltf.buffers[bufferView.buffer];
                    ref.embeddedData.assign(
                        buffer.data.begin() + bufferView.byteOffset,
                        buffer.data.begin() + bufferView.byteOffset + bufferView.byteLength
                    );
                    ref.isRawPixelData = false;
                }
            }
        }
    }

    // =========================================================================
    // Material Parsing
    // =========================================================================

    void GLTFImporter::ParseMaterials(const tinygltf::Model& gltf, GLTFImportResult& result)
    {
        result.materials.reserve(gltf.materials.size());

        for (size_t i = 0; i < gltf.materials.size(); ++i)
        {
            result.materials.push_back(ConvertMaterial(gltf, gltf.materials[i], static_cast<int>(i)));
        }

        // Add a default material if none exist
        if (result.materials.empty())
        {
            auto defaultMat = std::make_shared<Material>("DefaultMaterial");
            defaultMat->SetMaterialId(0);
            result.materials.push_back(defaultMat);
        }
    }

    Material::Ptr GLTFImporter::ConvertMaterial(const tinygltf::Model& gltf,
                                                  const tinygltf::Material& mat,
                                                  int materialIndex)
    {
        auto material = std::make_shared<Material>(mat.name.empty() ? "Material_" + std::to_string(materialIndex) : mat.name);
        material->SetMaterialId(static_cast<uint32_t>(materialIndex));

        // PBR Metallic-Roughness
        const auto& pbr = mat.pbrMetallicRoughness;
        
        // Base color
        material->SetBaseColor(
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3])
        );

        if (pbr.baseColorTexture.index >= 0)
        {
            material->SetBaseColorTexture(ExtractTextureInfo(gltf, pbr.baseColorTexture.index, pbr.baseColorTexture.texCoord));
        }

        // Metallic and roughness
        material->SetMetallicFactor(static_cast<float>(pbr.metallicFactor));
        material->SetRoughnessFactor(static_cast<float>(pbr.roughnessFactor));

        if (pbr.metallicRoughnessTexture.index >= 0)
        {
            material->SetMetallicRoughnessTexture(ExtractTextureInfo(gltf, pbr.metallicRoughnessTexture.index, pbr.metallicRoughnessTexture.texCoord));
        }

        // Normal map
        if (mat.normalTexture.index >= 0)
        {
            material->SetNormalTexture(ExtractTextureInfo(gltf, mat.normalTexture.index, mat.normalTexture.texCoord));
            material->SetNormalScale(static_cast<float>(mat.normalTexture.scale));
        }

        // Occlusion
        if (mat.occlusionTexture.index >= 0)
        {
            material->SetOcclusionTexture(ExtractTextureInfo(gltf, mat.occlusionTexture.index, mat.occlusionTexture.texCoord));
            material->SetOcclusionStrength(static_cast<float>(mat.occlusionTexture.strength));
        }

        // Emissive
        material->SetEmissiveColor(Vec3(
            static_cast<float>(mat.emissiveFactor[0]),
            static_cast<float>(mat.emissiveFactor[1]),
            static_cast<float>(mat.emissiveFactor[2])
        ));

        if (mat.emissiveTexture.index >= 0)
        {
            material->SetEmissiveTexture(ExtractTextureInfo(gltf, mat.emissiveTexture.index, mat.emissiveTexture.texCoord));
        }

        // Alpha mode
        if (mat.alphaMode == "OPAQUE")
        {
            material->SetAlphaMode(Material::AlphaMode::Opaque);
        }
        else if (mat.alphaMode == "MASK")
        {
            material->SetAlphaMode(Material::AlphaMode::Mask);
            material->SetAlphaCutoff(static_cast<float>(mat.alphaCutoff));
        }
        else if (mat.alphaMode == "BLEND")
        {
            material->SetAlphaMode(Material::AlphaMode::Blend);
        }

        material->SetDoubleSided(mat.doubleSided);

        return material;
    }

    TextureInfo GLTFImporter::ExtractTextureInfo(const tinygltf::Model& gltf, int textureIndex, int uvSet)
    {
        TextureInfo info;
        info.uvSet = uvSet;

        if (textureIndex < 0 || textureIndex >= static_cast<int>(gltf.textures.size()))
        {
            return info;
        }

        const auto& texture = gltf.textures[textureIndex];
        info.imageId = texture.source;

        // Get the image path if it's an external file
        if (texture.source >= 0 && texture.source < static_cast<int>(gltf.images.size()))
        {
            const auto& image = gltf.images[texture.source];
            if (!image.uri.empty() && image.uri.find("data:") != 0)
            {
                info.texturePath = image.uri;
            }
        }

        // Sampler settings
        if (texture.sampler >= 0 && texture.sampler < static_cast<int>(gltf.samplers.size()))
        {
            const auto& sampler = gltf.samplers[texture.sampler];

            // Wrap mode
            auto convertWrap = [](int mode) -> TextureInfo::WrapMode {
                switch (mode)
                {
                    case TINYGLTF_TEXTURE_WRAP_REPEAT: return TextureInfo::WrapMode::Repeat;
                    case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE: return TextureInfo::WrapMode::ClampToEdge;
                    case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT: return TextureInfo::WrapMode::MirrorRepeat;
                    default: return TextureInfo::WrapMode::Repeat;
                }
            };

            info.wrapS = convertWrap(sampler.wrapS);
            info.wrapT = convertWrap(sampler.wrapT);

            // Filter mode
            auto convertMinFilter = [](int mode) -> TextureInfo::FilterMode {
                switch (mode)
                {
                    case TINYGLTF_TEXTURE_FILTER_NEAREST: return TextureInfo::FilterMode::Nearest;
                    case TINYGLTF_TEXTURE_FILTER_LINEAR: return TextureInfo::FilterMode::Linear;
                    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST: return TextureInfo::FilterMode::NearestMipmapNearest;
                    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST: return TextureInfo::FilterMode::LinearMipmapNearest;
                    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR: return TextureInfo::FilterMode::NearestMipmapLinear;
                    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR: return TextureInfo::FilterMode::LinearMipmapLinear;
                    default: return TextureInfo::FilterMode::LinearMipmapLinear;
                }
            };

            auto convertMagFilter = [](int mode) -> TextureInfo::FilterMode {
                switch (mode)
                {
                    case TINYGLTF_TEXTURE_FILTER_NEAREST: return TextureInfo::FilterMode::Nearest;
                    case TINYGLTF_TEXTURE_FILTER_LINEAR: return TextureInfo::FilterMode::Linear;
                    default: return TextureInfo::FilterMode::Linear;
                }
            };

            info.minFilter = convertMinFilter(sampler.minFilter);
            info.magFilter = convertMagFilter(sampler.magFilter);
        }

        return info;
    }

    // =========================================================================
    // Mesh Parsing
    // =========================================================================

    void GLTFImporter::ParseMeshes(const tinygltf::Model& gltf, GLTFImportResult& result,
                                    const GLTFImportOptions& options)
    {
        // In glTF, a "mesh" can have multiple primitives
        // We treat each primitive as a separate Mesh (SubMesh pattern)
        // But we can also combine primitives into a single Mesh with SubMeshes

        for (size_t meshIdx = 0; meshIdx < gltf.meshes.size(); ++meshIdx)
        {
            const auto& gltfMesh = gltf.meshes[meshIdx];
            std::string meshName = gltfMesh.name.empty() ? "Mesh_" + std::to_string(meshIdx) : gltfMesh.name;

            // For each primitive, create a Mesh
            for (size_t primIdx = 0; primIdx < gltfMesh.primitives.size(); ++primIdx)
            {
                auto mesh = ConvertPrimitive(gltf, gltfMesh.primitives[primIdx], meshName, static_cast<int>(primIdx), options);
                if (mesh)
                {
                    result.meshes.push_back(mesh);
                }
            }
        }
    }

    Mesh::Ptr GLTFImporter::ConvertPrimitive(const tinygltf::Model& gltf,
                                               const tinygltf::Primitive& primitive,
                                               const std::string& meshName,
                                               int primitiveIndex,
                                               const GLTFImportOptions& options)
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->name = meshName + (primitiveIndex > 0 ? "_" + std::to_string(primitiveIndex) : "");

        // Set primitive type
        mesh->SetPrimitiveType(ConvertPrimitiveMode(primitive.mode));

        // Extract vertex attributes
        for (const auto& [attrName, accessorIdx] : primitive.attributes)
        {
            const auto& accessor = gltf.accessors[accessorIdx];
            const auto& bufferView = gltf.bufferViews[accessor.bufferView];
            const auto& buffer = gltf.buffers[bufferView.buffer];

            size_t byteOffset = bufferView.byteOffset + accessor.byteOffset;
            size_t byteStride = bufferView.byteStride;
            size_t componentSize = tinygltf::GetComponentSizeInBytes(accessor.componentType);
            size_t numComponents = tinygltf::GetNumComponentsInType(accessor.type);

            if (byteStride == 0)
            {
                byteStride = componentSize * numComponents;
            }

            // Map glTF attribute names to our names
            std::string ourAttrName;
            if (attrName == "POSITION") ourAttrName = VertexBufferNames::Position;
            else if (attrName == "NORMAL") ourAttrName = VertexBufferNames::Normal;
            else if (attrName == "TANGENT") ourAttrName = VertexBufferNames::Tangent;
            else if (attrName == "TEXCOORD_0") ourAttrName = VertexBufferNames::UV;
            else if (attrName == "TEXCOORD_1") ourAttrName = VertexBufferNames::UV1;
            else if (attrName == "COLOR_0") ourAttrName = VertexBufferNames::Color;
            else if (attrName == "JOINTS_0") ourAttrName = VertexBufferNames::BoneIndices;
            else if (attrName == "WEIGHTS_0") ourAttrName = VertexBufferNames::BoneWeights;
            else ourAttrName = attrName;

            // Extract data based on component type
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                if (numComponents == 2)
                {
                    std::vector<Vec2> data(accessor.count);
                    for (size_t i = 0; i < accessor.count; ++i)
                    {
                        const float* ptr = reinterpret_cast<const float*>(buffer.data.data() + byteOffset + i * byteStride);
                        data[i] = Vec2(ptr[0], options.flipUVs && (ourAttrName == VertexBufferNames::UV || ourAttrName == VertexBufferNames::UV1) ? 1.0f - ptr[1] : ptr[1]);
                    }
                    mesh->AddAttribute(ourAttrName, data);
                }
                else if (numComponents == 3)
                {
                    std::vector<Vec3> data(accessor.count);
                    for (size_t i = 0; i < accessor.count; ++i)
                    {
                        const float* ptr = reinterpret_cast<const float*>(buffer.data.data() + byteOffset + i * byteStride);
                        data[i] = Vec3(ptr[0], ptr[1], ptr[2]) * options.scaleFactor;
                    }
                    // Don't scale normals
                    if (ourAttrName == VertexBufferNames::Normal)
                    {
                        for (auto& v : data) v = glm::normalize(v);
                    }
                    mesh->AddAttribute(ourAttrName, data);
                }
                else if (numComponents == 4)
                {
                    std::vector<Vec4> data(accessor.count);
                    for (size_t i = 0; i < accessor.count; ++i)
                    {
                        const float* ptr = reinterpret_cast<const float*>(buffer.data.data() + byteOffset + i * byteStride);
                        data[i] = Vec4(ptr[0], ptr[1], ptr[2], ptr[3]);
                    }
                    mesh->AddAttribute(ourAttrName, data);
                }
            }
            else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && numComponents == 4)
            {
                // Bone indices (JOINTS_0)
                std::vector<IVec4> data(accessor.count);
                for (size_t i = 0; i < accessor.count; ++i)
                {
                    const uint16_t* ptr = reinterpret_cast<const uint16_t*>(buffer.data.data() + byteOffset + i * byteStride);
                    data[i] = IVec4(ptr[0], ptr[1], ptr[2], ptr[3]);
                }
                mesh->AddAttribute(ourAttrName, data);
            }
            else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && numComponents == 4)
            {
                // Bone indices (JOINTS_0) as bytes
                std::vector<IVec4> data(accessor.count);
                for (size_t i = 0; i < accessor.count; ++i)
                {
                    const uint8_t* ptr = buffer.data.data() + byteOffset + i * byteStride;
                    data[i] = IVec4(ptr[0], ptr[1], ptr[2], ptr[3]);
                }
                mesh->AddAttribute(ourAttrName, data);
            }
        }

        // Extract indices
        if (primitive.indices >= 0)
        {
            std::vector<uint32_t> indices = ExtractIndices(gltf, primitive.indices);
            mesh->SetIndices(indices);
        }

        // Generate normals if needed
        if (options.generateNormals && !mesh->HasAttribute(VertexBufferNames::Normal))
        {
            mesh->GenerateNormals();
        }

        // Generate tangents if needed
        if (options.generateTangents && !mesh->HasAttribute(VertexBufferNames::Tangent) &&
            mesh->HasAttribute(VertexBufferNames::Normal) && mesh->HasAttribute(VertexBufferNames::UV))
        {
            mesh->GenerateTangents();
        }

        // Compute bounding box
        mesh->ComputeBoundingBox();

        // Create a single SubMesh referencing the material
        SubMesh subMesh;
        subMesh.indexOffset = 0;
        subMesh.indexCount = static_cast<uint32_t>(mesh->GetIndexCount());
        subMesh.materialId = primitive.material >= 0 ? static_cast<uint32_t>(primitive.material) : 0;
        subMesh.name = mesh->name;
        subMesh.localBounds = mesh->GetBoundingBox();
        mesh->AddSubMesh(subMesh);

        return mesh;
    }

    std::vector<uint32_t> GLTFImporter::ExtractIndices(const tinygltf::Model& gltf, int accessorIndex)
    {
        if (accessorIndex < 0 || accessorIndex >= static_cast<int>(gltf.accessors.size()))
        {
            return {};
        }

        const auto& accessor = gltf.accessors[accessorIndex];
        const auto& bufferView = gltf.bufferViews[accessor.bufferView];
        const auto& buffer = gltf.buffers[bufferView.buffer];

        size_t byteOffset = bufferView.byteOffset + accessor.byteOffset;
        std::vector<uint32_t> indices(accessor.count);

        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
        {
            const uint32_t* data = reinterpret_cast<const uint32_t*>(buffer.data.data() + byteOffset);
            std::memcpy(indices.data(), data, accessor.count * sizeof(uint32_t));
        }
        else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
        {
            const uint16_t* data = reinterpret_cast<const uint16_t*>(buffer.data.data() + byteOffset);
            for (size_t i = 0; i < accessor.count; ++i)
            {
                indices[i] = data[i];
            }
        }
        else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
        {
            const uint8_t* data = buffer.data.data() + byteOffset;
            for (size_t i = 0; i < accessor.count; ++i)
            {
                indices[i] = data[i];
            }
        }

        return indices;
    }

    PrimitiveType GLTFImporter::ConvertPrimitiveMode(int mode)
    {
        switch (mode)
        {
            case TINYGLTF_MODE_POINTS: return PrimitiveType::Points;
            case TINYGLTF_MODE_LINE: return PrimitiveType::Lines;
            case TINYGLTF_MODE_LINE_LOOP: return PrimitiveType::LineLoop;
            case TINYGLTF_MODE_LINE_STRIP: return PrimitiveType::LineStrip;
            case TINYGLTF_MODE_TRIANGLES: return PrimitiveType::Triangles;
            case TINYGLTF_MODE_TRIANGLE_STRIP: return PrimitiveType::TriangleStrip;
            case TINYGLTF_MODE_TRIANGLE_FAN: return PrimitiveType::TriangleFan;
            default: return PrimitiveType::Triangles;
        }
    }

    // =========================================================================
    // Node Parsing
    // =========================================================================

    void GLTFImporter::ParseNodes(const tinygltf::Model& gltf, GLTFImportResult& result)
    {
        // Build a mapping from glTF mesh index to our mesh array
        // Since we create one Mesh per primitive, we need to track the first primitive index for each mesh
        std::vector<int> meshToFirstPrimitiveIndex;
        int currentIndex = 0;
        for (const auto& gltfMesh : gltf.meshes)
        {
            meshToFirstPrimitiveIndex.push_back(currentIndex);
            currentIndex += static_cast<int>(gltfMesh.primitives.size());
        }

        // Create all nodes first (we'll connect hierarchy after)
        std::vector<Node::Ptr> allNodes;
        allNodes.reserve(gltf.nodes.size());

        for (size_t i = 0; i < gltf.nodes.size(); ++i)
        {
            allNodes.push_back(ConvertNode(gltf, static_cast<int>(i), meshToFirstPrimitiveIndex));
        }

        // Build hierarchy
        for (size_t i = 0; i < gltf.nodes.size(); ++i)
        {
            const auto& gltfNode = gltf.nodes[i];
            for (int childIdx : gltfNode.children)
            {
                if (childIdx >= 0 && childIdx < static_cast<int>(allNodes.size()))
                {
                    allNodes[i]->AddChild(allNodes[childIdx]);
                }
            }
        }

        // Store the nodes in result for scene building
        // We'll use them in ParseScene
        m_parsedNodes = std::move(allNodes);
    }

    Node::Ptr GLTFImporter::ConvertNode(const tinygltf::Model& gltf, int nodeIndex,
                                          const std::vector<int>& meshToFirstPrimitiveIndex)
    {
        const auto& gltfNode = gltf.nodes[nodeIndex];
        
        auto node = std::make_shared<Node>(gltfNode.name.empty() ? "Node_" + std::to_string(nodeIndex) : gltfNode.name);

        // Transform
        Transform& transform = node->GetLocalTransform();

        if (!gltfNode.matrix.empty())
        {
            // Use matrix directly
            Mat4 mat;
            for (int i = 0; i < 16; ++i)
            {
                mat[i / 4][i % 4] = static_cast<float>(gltfNode.matrix[i]);
            }
            transform.SetMatrix(mat);
        }
        else
        {
            // Use TRS
            if (!gltfNode.translation.empty())
            {
                transform.SetPosition(Vec3(
                    static_cast<float>(gltfNode.translation[0]),
                    static_cast<float>(gltfNode.translation[1]),
                    static_cast<float>(gltfNode.translation[2])
                ));
            }

            if (!gltfNode.rotation.empty())
            {
                // glTF uses xyzw quaternion order
                transform.SetRotation(Quat(
                    static_cast<float>(gltfNode.rotation[3]), // w
                    static_cast<float>(gltfNode.rotation[0]), // x
                    static_cast<float>(gltfNode.rotation[1]), // y
                    static_cast<float>(gltfNode.rotation[2])  // z
                ));
            }

            if (!gltfNode.scale.empty())
            {
                transform.SetScale(Vec3(
                    static_cast<float>(gltfNode.scale[0]),
                    static_cast<float>(gltfNode.scale[1]),
                    static_cast<float>(gltfNode.scale[2])
                ));
            }
        }

        // Set mesh index (use index-based mode instead of MeshComponent)
        if (gltfNode.mesh >= 0 && gltfNode.mesh < static_cast<int>(meshToFirstPrimitiveIndex.size()))
        {
            const auto& gltfMesh = gltf.meshes[gltfNode.mesh];
            
            // For multi-primitive meshes, we use the first primitive index
            node->SetMeshIndex(meshToFirstPrimitiveIndex[gltfNode.mesh]);

            // Set material indices for each primitive/submesh
            std::vector<int> materialIndices;
            for (const auto& prim : gltfMesh.primitives)
            {
                materialIndices.push_back(prim.material >= 0 ? prim.material : 0);
            }
            node->SetMaterialIndices(materialIndices);
        }

        return node;
    }

    void GLTFImporter::ParseScene(const tinygltf::Model& gltf, GLTFImportResult& result)
    {
        result.model = std::make_shared<Model>();

        // Find root nodes (nodes that are scene roots or have no parent)
        int sceneIdx = gltf.defaultScene >= 0 ? gltf.defaultScene : 0;
        
        if (sceneIdx < static_cast<int>(gltf.scenes.size()))
        {
            const auto& scene = gltf.scenes[sceneIdx];

            if (scene.nodes.size() == 1)
            {
                // Single root
                int rootIdx = scene.nodes[0];
                if (rootIdx >= 0 && rootIdx < static_cast<int>(m_parsedNodes.size()))
                {
                    result.model->SetRootNode(m_parsedNodes[rootIdx]);
                }
            }
            else if (scene.nodes.size() > 1)
            {
                // Multiple roots - create a container node
                auto containerRoot = std::make_shared<Node>("Root");
                for (int nodeIdx : scene.nodes)
                {
                    if (nodeIdx >= 0 && nodeIdx < static_cast<int>(m_parsedNodes.size()))
                    {
                        containerRoot->AddChild(m_parsedNodes[nodeIdx]);
                    }
                }
                result.model->SetRootNode(containerRoot);
            }
        }

        // Clear temporary storage
        m_parsedNodes.clear();
    }

    // =========================================================================
    // Utility
    // =========================================================================

    void GLTFImporter::ReportProgress(float progress, const std::string& stage)
    {
        if (m_progressCallback)
        {
            m_progressCallback(progress, stage);
        }
    }

} // namespace RVX::Resource
