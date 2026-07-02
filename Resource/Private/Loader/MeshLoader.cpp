#include "Resource/Loader/MeshLoader.h"

#include "Core/Log.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace RVX::Resource
{
namespace
{
    using FieldMap = std::unordered_map<std::string, std::string>;

    std::vector<uint8_t> ReadFileBytes(const std::string& path, std::string& outError)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            outError = "Cannot open mesh artifact: " + path;
            return {};
        }

        const std::streamsize size = file.tellg();
        if (size <= 0)
        {
            outError = "Mesh artifact is empty: " + path;
            return {};
        }

        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
        {
            outError = "Failed to read mesh artifact: " + path;
            return {};
        }

        return bytes;
    }

    FieldMap ParseFields(std::string_view text)
    {
        FieldMap fields;
        std::istringstream stream{std::string(text)};
        std::string line;
        while (std::getline(stream, line))
        {
            const size_t separator = line.find('=');
            if (separator == std::string::npos)
            {
                continue;
            }
            fields[line.substr(0, separator)] = line.substr(separator + 1);
        }
        return fields;
    }

    std::optional<uint32_t> ParseUint32(const FieldMap& fields, const std::string& key)
    {
        auto it = fields.find(key);
        if (it == fields.end())
        {
            return std::nullopt;
        }

        try
        {
            return static_cast<uint32_t>(std::stoul(it->second));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<int32> ParseInt32(const FieldMap& fields, const std::string& key)
    {
        auto it = fields.find(key);
        if (it == fields.end())
        {
            return std::nullopt;
        }

        try
        {
            return static_cast<int32>(std::stol(it->second));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<size_t> ParseSize(const FieldMap& fields, const std::string& key)
    {
        auto it = fields.find(key);
        if (it == fields.end())
        {
            return std::nullopt;
        }

        try
        {
            return static_cast<size_t>(std::stoull(it->second));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<bool> ParseBool(const FieldMap& fields, const std::string& key)
    {
        const std::optional<uint32_t> value = ParseUint32(fields, key);
        if (!value)
        {
            return std::nullopt;
        }
        return *value != 0;
    }

    std::optional<IndexType> ParseIndexType(const std::string& value)
    {
        if (value == "UInt8") return IndexType::UInt8;
        if (value == "UInt16") return IndexType::UInt16;
        if (value == "UInt32") return IndexType::UInt32;
        return std::nullopt;
    }

    std::optional<PrimitiveType> ParsePrimitiveType(const std::string& value)
    {
        if (value == "Triangles") return PrimitiveType::Triangles;
        if (value == "TriangleStrip") return PrimitiveType::TriangleStrip;
        if (value == "TriangleFan") return PrimitiveType::TriangleFan;
        if (value == "Lines") return PrimitiveType::Lines;
        if (value == "LineStrip") return PrimitiveType::LineStrip;
        if (value == "LineLoop") return PrimitiveType::LineLoop;
        if (value == "Points") return PrimitiveType::Points;
        return std::nullopt;
    }

    std::optional<AttributeType> ParseAttributeType(const std::string& value)
    {
        if (value == "Float") return AttributeType::Float;
        if (value == "Int") return AttributeType::Int;
        if (value == "UInt") return AttributeType::UInt;
        if (value == "Short") return AttributeType::Short;
        if (value == "UShort") return AttributeType::UShort;
        if (value == "Byte") return AttributeType::Byte;
        if (value == "UByte") return AttributeType::UByte;
        return std::nullopt;
    }

    bool SlicePayloadAt(std::string_view bytes,
                        size_t offset,
                        size_t byteSize,
                        const std::string& endMarker,
                        std::vector<uint8_t>& outPayload,
                        std::string& outError)
    {
        if (offset + byteSize > bytes.size())
        {
            outError = "Mesh artifact payload is truncated";
            return false;
        }

        const size_t endOffset = offset + byteSize;
        if (endOffset + endMarker.size() > bytes.size() ||
            bytes.substr(endOffset, endMarker.size()) != endMarker)
        {
            outError = "Mesh artifact marker mismatch near payload boundary";
            return false;
        }

        const auto* begin = reinterpret_cast<const uint8_t*>(bytes.data() + offset);
        outPayload.assign(begin, begin + byteSize);
        return true;
    }

    std::vector<uint32_t> DecodeUInt32Indices(const std::vector<uint8_t>& bytes)
    {
        std::vector<uint32_t> indices(bytes.size() / sizeof(uint32_t));
        if (!indices.empty())
        {
            std::memcpy(indices.data(), bytes.data(), indices.size() * sizeof(uint32_t));
        }
        return indices;
    }

    bool SetMeshIndices(Mesh& mesh,
                        IndexType indexType,
                        const std::vector<uint8_t>& indexBytes,
                        std::string& outError)
    {
        switch (indexType)
        {
            case IndexType::UInt8:
            {
                std::vector<uint8_t> indices(indexBytes.size());
                if (!indices.empty())
                {
                    std::memcpy(indices.data(), indexBytes.data(), indices.size());
                }
                mesh.SetIndices(indices);
                return true;
            }
            case IndexType::UInt16:
            {
                if (indexBytes.size() % sizeof(uint16_t) != 0)
                {
                    outError = "Mesh artifact UInt16 index payload has invalid size";
                    return false;
                }
                std::vector<uint16_t> indices(indexBytes.size() / sizeof(uint16_t));
                if (!indices.empty())
                {
                    std::memcpy(indices.data(), indexBytes.data(), indexBytes.size());
                }
                mesh.SetIndices(indices);
                return true;
            }
            case IndexType::UInt32:
            {
                if (indexBytes.size() % sizeof(uint32_t) != 0)
                {
                    outError = "Mesh artifact UInt32 index payload has invalid size";
                    return false;
                }
                mesh.SetIndices(DecodeUInt32Indices(indexBytes));
                return true;
            }
        }

        outError = "Mesh artifact contains unsupported index type";
        return false;
    }

    bool AddAttributesFromFields(Mesh& mesh,
                                 const FieldMap& fields,
                                 std::string_view bytes,
                                 size_t& cursor,
                                 uint32_t meshIndex,
                                 const std::string& metadataPrefix,
                                 const std::string& markerPrefix,
                                 size_t vertexCount,
                                 uint32_t attributeCount,
                                 std::string& outError)
    {
        for (uint32_t attributeIndex = 0; attributeIndex < attributeCount; ++attributeIndex)
        {
            const std::string attributeKey = metadataPrefix + ".attribute." + std::to_string(attributeIndex);
            auto nameIt = fields.find(attributeKey + ".name");
            auto typeIt = fields.find(attributeKey + ".type");
            const std::optional<uint32_t> components = ParseUint32(fields, attributeKey + ".components");
            const std::optional<bool> normalized = ParseBool(fields, attributeKey + ".normalized");
            const std::optional<size_t> byteSize = ParseSize(fields, attributeKey + ".byteSize");
            if (nameIt == fields.end() || typeIt == fields.end() || !components || !normalized || !byteSize)
            {
                outError = "Mesh artifact attribute metadata is incomplete";
                return false;
            }

            const std::optional<AttributeType> attributeType = ParseAttributeType(typeIt->second);
            if (!attributeType)
            {
                outError = "Mesh artifact attribute type is unsupported";
                return false;
            }

            const size_t expectedSize = vertexCount * (*components) * GetAttributeTypeSize(*attributeType);
            if (expectedSize != *byteSize)
            {
                outError = "Mesh artifact attribute payload size does not match metadata";
                return false;
            }

            const std::string beginMarker = markerPrefix + "_ATTRIBUTE_" +
                                            std::to_string(attributeIndex) +
                                            "_DATA_BEGIN name=" + nameIt->second + "\n";
            const std::string endMarker = "\n" + markerPrefix + "_ATTRIBUTE_" +
                                          std::to_string(attributeIndex) + "_DATA_END\n";
            const size_t beginOffset = bytes.find(beginMarker, cursor);
            if (beginOffset == std::string_view::npos)
            {
                outError = "Mesh artifact missing attribute data marker for mesh " + std::to_string(meshIndex);
                return false;
            }

            std::vector<uint8_t> payload;
            const size_t payloadOffset = beginOffset + beginMarker.size();
            if (!SlicePayloadAt(bytes, payloadOffset, *byteSize, endMarker, payload, outError))
            {
                return false;
            }
            cursor = payloadOffset + *byteSize + endMarker.size();

            mesh.AddAttribute(nameIt->second,
                              std::make_unique<VertexAttribute>(payload.data(),
                                                                 vertexCount,
                                                                 *components,
                                                                 *attributeType,
                                                                 *normalized));
        }

        return true;
    }

    std::shared_ptr<Mesh> ParseLodMesh(std::string_view bytes,
                                       size_t& cursor,
                                       uint32_t meshIndex,
                                       uint32_t lodLevel,
                                       const std::string& baseName,
                                       std::string& outError)
    {
        const std::string lodMarker = "RVX_MESH_" + std::to_string(meshIndex) +
                                      "_LOD_" + std::to_string(lodLevel);
        const std::string beginMarker = lodMarker + "_BEGIN\n";
        const size_t beginOffset = bytes.find(beginMarker, cursor);
        if (beginOffset == std::string_view::npos)
        {
            outError = "Mesh artifact missing LOD begin marker";
            return nullptr;
        }

        const std::string indexBeginMarker = lodMarker + "_INDEX_DATA_BEGIN\n";
        const size_t indexMarkerOffset = bytes.find(indexBeginMarker, beginOffset + beginMarker.size());
        if (indexMarkerOffset == std::string_view::npos)
        {
            outError = "Mesh artifact missing LOD index marker";
            return nullptr;
        }

        const FieldMap fields = ParseFields(bytes.substr(beginOffset + beginMarker.size(),
                                                         indexMarkerOffset - (beginOffset + beginMarker.size())));
        const std::string metadataPrefix = "mesh." + std::to_string(meshIndex) +
                                           ".lod." + std::to_string(lodLevel);
        const std::optional<uint32_t> vertexCount = ParseUint32(fields, metadataPrefix + ".vertexCount");
        const std::optional<uint32_t> indexCount = ParseUint32(fields, metadataPrefix + ".indexCount");
        const std::optional<uint32_t> attributeCount = ParseUint32(fields, metadataPrefix + ".attributeCount");
        if (!vertexCount || !indexCount || !attributeCount)
        {
            outError = "Mesh artifact LOD metadata is incomplete";
            return nullptr;
        }

        const size_t indexByteSize = static_cast<size_t>(*indexCount) * sizeof(uint32_t);
        const size_t indexPayloadOffset = indexMarkerOffset + indexBeginMarker.size();
        const std::string indexEndMarker = "\n" + lodMarker + "_INDEX_DATA_END\n";
        std::vector<uint8_t> indexBytes;
        if (!SlicePayloadAt(bytes, indexPayloadOffset, indexByteSize, indexEndMarker, indexBytes, outError))
        {
            return nullptr;
        }

        auto mesh = std::make_shared<Mesh>();
        mesh->name = baseName + "_LOD" + std::to_string(lodLevel);
        mesh->SetPrimitiveType(PrimitiveType::Triangles);
        mesh->SetIndices(DecodeUInt32Indices(indexBytes));

        cursor = indexPayloadOffset + indexByteSize + indexEndMarker.size();
        if (!AddAttributesFromFields(*mesh,
                                     fields,
                                     bytes,
                                     cursor,
                                     meshIndex,
                                     metadataPrefix,
                                     lodMarker,
                                     *vertexCount,
                                     *attributeCount,
                                     outError))
        {
            return nullptr;
        }

        mesh->ComputeBoundingBox();
        SubMesh subMesh;
        subMesh.indexOffset = 0;
        subMesh.indexCount = static_cast<uint32_t>(mesh->GetIndexCount());
        subMesh.name = mesh->name;
        subMesh.localBounds = mesh->GetBoundingBox();
        mesh->AddSubMesh(subMesh);
        return mesh;
    }

    std::shared_ptr<Mesh> ParseBaseMesh(std::string_view bytes,
                                        uint32_t meshIndex,
                                        uint32_t& outWrittenLodCount,
                                        size_t& outCursor,
                                        std::string& outError)
    {
        const std::string meshMarker = "RVX_MESH_" + std::to_string(meshIndex);
        const std::string beginMarker = meshMarker + "_BEGIN\n";
        const size_t beginOffset = bytes.find(beginMarker);
        if (beginOffset == std::string_view::npos)
        {
            outError = "Mesh artifact missing mesh begin marker";
            return nullptr;
        }

        const std::string indexBeginMarker = meshMarker + "_INDEX_DATA_BEGIN\n";
        const size_t indexMarkerOffset = bytes.find(indexBeginMarker, beginOffset + beginMarker.size());
        if (indexMarkerOffset == std::string_view::npos)
        {
            outError = "Mesh artifact missing base index marker";
            return nullptr;
        }

        const FieldMap fields = ParseFields(bytes.substr(beginOffset + beginMarker.size(),
                                                         indexMarkerOffset - (beginOffset + beginMarker.size())));
        const std::string metadataPrefix = "mesh." + std::to_string(meshIndex);
        auto nameIt = fields.find(metadataPrefix + ".name");
        auto indexTypeIt = fields.find(metadataPrefix + ".indexType");
        auto primitiveIt = fields.find(metadataPrefix + ".primitive");
        const std::optional<uint32_t> vertexCount = ParseUint32(fields, metadataPrefix + ".vertexCount");
        const std::optional<uint32_t> indexCount = ParseUint32(fields, metadataPrefix + ".indexCount");
        const std::optional<uint32_t> attributeCount = ParseUint32(fields, metadataPrefix + ".attributeCount");
        const std::optional<uint32_t> subMeshCount = ParseUint32(fields, metadataPrefix + ".subMeshCount");
        const std::optional<uint32_t> writtenLodCount = ParseUint32(fields, metadataPrefix + ".writtenLODCount");
        const std::optional<size_t> indexDataSize = ParseSize(fields, metadataPrefix + ".indexDataSize");
        if (nameIt == fields.end() || indexTypeIt == fields.end() || primitiveIt == fields.end() ||
            !vertexCount || !indexCount || !attributeCount || !subMeshCount || !writtenLodCount ||
            !indexDataSize)
        {
            outError = "Mesh artifact base mesh metadata is incomplete";
            return nullptr;
        }

        const std::optional<IndexType> indexType = ParseIndexType(indexTypeIt->second);
        const std::optional<PrimitiveType> primitiveType = ParsePrimitiveType(primitiveIt->second);
        if (!indexType || !primitiveType)
        {
            outError = "Mesh artifact contains unsupported base mesh type metadata";
            return nullptr;
        }

        const size_t indexPayloadOffset = indexMarkerOffset + indexBeginMarker.size();
        const std::string indexEndMarker = "\n" + meshMarker + "_INDEX_DATA_END\n";
        std::vector<uint8_t> indexBytes;
        if (!SlicePayloadAt(bytes, indexPayloadOffset, *indexDataSize, indexEndMarker, indexBytes, outError))
        {
            return nullptr;
        }

        auto mesh = std::make_shared<Mesh>();
        mesh->name = nameIt->second;
        mesh->SetPrimitiveType(*primitiveType);
        if (!SetMeshIndices(*mesh, *indexType, indexBytes, outError))
        {
            return nullptr;
        }

        size_t cursor = indexPayloadOffset + *indexDataSize + indexEndMarker.size();
        if (!AddAttributesFromFields(*mesh,
                                     fields,
                                     bytes,
                                     cursor,
                                     meshIndex,
                                     metadataPrefix,
                                     meshMarker,
                                     *vertexCount,
                                     *attributeCount,
                                     outError))
        {
            return nullptr;
        }

        mesh->ComputeBoundingBox();
        for (uint32_t subMeshIndex = 0; subMeshIndex < *subMeshCount; ++subMeshIndex)
        {
            const std::string key = metadataPrefix + ".subMesh." + std::to_string(subMeshIndex);
            const std::optional<uint32_t> offset = ParseUint32(fields, key + ".indexOffset");
            const std::optional<uint32_t> count = ParseUint32(fields, key + ".indexCount");
            const std::optional<int32> baseVertex = ParseInt32(fields, key + ".baseVertex");
            const std::optional<uint32_t> materialId = ParseUint32(fields, key + ".materialId");
            if (!offset || !count || !baseVertex || !materialId)
            {
                outError = "Mesh artifact submesh metadata is incomplete";
                return nullptr;
            }

            SubMesh subMesh;
            subMesh.indexOffset = *offset;
            subMesh.indexCount = *count;
            subMesh.baseVertex = *baseVertex;
            subMesh.materialId = *materialId;
            auto subMeshNameIt = fields.find(key + ".name");
            subMesh.name = subMeshNameIt != fields.end() ? subMeshNameIt->second : mesh->name;
            subMesh.localBounds = mesh->GetBoundingBox();
            mesh->AddSubMesh(subMesh);
        }

        outWrittenLodCount = *writtenLodCount;
        outCursor = cursor;
        return mesh;
    }

    std::unique_ptr<MeshResource> ParseCookedMeshArtifact(const std::vector<uint8_t>& fileData,
                                                          const std::string& path,
                                                          std::string& outError)
    {
        constexpr const char* magic = "RVX_MESH_PREBAKE_V1\n";
        const std::string_view bytes(reinterpret_cast<const char*>(fileData.data()), fileData.size());
        if (!bytes.starts_with(magic))
        {
            outError = "Mesh artifact missing RVX_MESH_PREBAKE_V1 magic";
            return nullptr;
        }

        const size_t firstMeshOffset = bytes.find("RVX_MESH_0_BEGIN\n");
        if (firstMeshOffset == std::string_view::npos)
        {
            outError = "Mesh artifact contains no mesh payload";
            return nullptr;
        }

        const FieldMap globalFields = ParseFields(bytes.substr(std::strlen(magic),
                                                               firstMeshOffset - std::strlen(magic)));
        const std::optional<uint32_t> meshCount = ParseUint32(globalFields, "meshCount");
        if (!meshCount)
        {
            outError = "Mesh artifact missing meshCount";
            return nullptr;
        }
        if (*meshCount != 1)
        {
            outError = "MeshLoader currently supports single-mesh artifacts only";
            return nullptr;
        }

        uint32_t writtenLodCount = 1;
        size_t cursor = firstMeshOffset;
        std::shared_ptr<Mesh> baseMesh = ParseBaseMesh(bytes, 0, writtenLodCount, cursor, outError);
        if (!baseMesh)
        {
            return nullptr;
        }

        std::vector<std::shared_ptr<Mesh>> lodMeshes;
        lodMeshes.push_back(baseMesh);
        for (uint32_t lodLevel = 1; lodLevel < writtenLodCount; ++lodLevel)
        {
            std::shared_ptr<Mesh> lodMesh = ParseLodMesh(bytes, cursor, 0, lodLevel, baseMesh->name, outError);
            if (!lodMesh)
            {
                return nullptr;
            }
            lodMeshes.push_back(lodMesh);
        }

        auto resource = std::make_unique<MeshResource>();
        resource->SetPath(path);
        resource->SetName(std::filesystem::path(path).stem().string());
        resource->SetId(GenerateResourceId(path));
        resource->SetLODMeshes(std::move(lodMeshes));
        return resource;
    }
} // namespace

MeshLoader::MeshLoader(ResourceManager* manager)
    : m_manager(manager)
{
}

std::vector<std::string> MeshLoader::GetSupportedExtensions() const
{
    return { ".rva" };
}

bool MeshLoader::CanLoad(const std::string& path) const
{
    std::filesystem::path filePath(path);
    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".rva";
}

IResource* MeshLoader::Load(const std::string& path)
{
    m_lastLoadError.clear();

    const std::filesystem::path absPath = std::filesystem::absolute(path);
    std::string readError;
    std::vector<uint8_t> fileData = ReadFileBytes(absPath.string(), readError);
    if (fileData.empty())
    {
        m_lastLoadError = readError;
        RVX_CORE_WARN("MeshLoader: {}", m_lastLoadError);
        return nullptr;
    }

    std::unique_ptr<MeshResource> resource = ParseCookedMeshArtifact(fileData, absPath.string(), m_lastLoadError);
    if (!resource)
    {
        RVX_CORE_WARN("MeshLoader: {}", m_lastLoadError);
        return nullptr;
    }

    (void)m_manager;
    resource->NotifyLoaded();
    return resource.release();
}
} // namespace RVX::Resource
