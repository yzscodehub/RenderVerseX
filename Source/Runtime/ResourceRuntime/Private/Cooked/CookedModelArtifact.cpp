#include "Resource/Cooked/CookedModelArtifact.h"

#include "Core/Diagnostics/ContentHash.h"

#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace RVX::Resource
{
namespace
{
    struct ParsedFields
    {
        std::unordered_map<std::string, std::string> values;
        std::unordered_set<std::string> consumed;
        uint64 decodedStringBytes = 0;
    };

    constexpr std::string_view PayloadBegin = "RVX_MODEL_PAYLOAD_BEGIN\n";
    constexpr std::string_view PayloadEnd = "RVX_MODEL_PAYLOAD_END\n";
    constexpr uint32 MaxNodePrimitiveMappings = 65536u;
    constexpr uint64 MaxArtifactBytes = 512ull * 1024ull * 1024ull;
    constexpr uint64 MaxSingleStringBytes = 1ull * 1024ull * 1024ull;
    constexpr uint64 MaxAggregateStringBytes = 64ull * 1024ull * 1024ull;
    constexpr uint32 MaxTextureCount = 16384u;
    constexpr uint32 MaxMaterialCount = 16384u;
    constexpr uint32 MaxNodeCount = 1048576u;
    constexpr uint32 MaxHierarchyDepth = 512u;
    constexpr uint32 FlagHasAnimation = 1u << 0u;
    constexpr uint32 KnownFlags = FlagHasAnimation;

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
        return true;
    }

    bool IsFiniteBounds(const AABB& bounds)
    {
        if (!bounds.IsValid())
            return false;
        const Vec3& min = bounds.GetMin();
        const Vec3& max = bounds.GetMax();
        return std::isfinite(min.x) && std::isfinite(min.y) &&
               std::isfinite(min.z) && std::isfinite(max.x) &&
               std::isfinite(max.y) && std::isfinite(max.z);
    }

    bool IsFiniteVector(const Vec2& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    bool IsFiniteVector(const Vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    bool IsFiniteVector(const Vec4& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z) && std::isfinite(value.w);
    }

    bool IsFiniteQuaternion(const Quat& value)
    {
        return std::isfinite(value.w) && std::isfinite(value.x) &&
               std::isfinite(value.y) && std::isfinite(value.z);
    }

    std::string HexEncode(std::string_view value)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string encoded;
        encoded.resize(value.size() * 2);
        for (size_t index = 0; index < value.size(); ++index)
        {
            const uint8 byte = static_cast<uint8>(value[index]);
            encoded[index * 2] = digits[byte >> 4u];
            encoded[index * 2 + 1] = digits[byte & 0x0fu];
        }
        return encoded;
    }

    bool HexDecode(std::string_view encoded, std::string& outValue)
    {
        auto nibble = [](char value) -> int
        {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        };

        if ((encoded.size() & 1u) != 0)
            return false;

        outValue.clear();
        outValue.reserve(encoded.size() / 2);
        for (size_t index = 0; index < encoded.size(); index += 2)
        {
            const int high = nibble(encoded[index]);
            const int low = nibble(encoded[index + 1]);
            if (high < 0 || low < 0)
                return false;
            outValue.push_back(static_cast<char>((high << 4) | low));
        }
        return true;
    }

    std::string HashPayload(std::string_view payload)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        for (const char value : payload)
        {
            hash ^= static_cast<uint8>(value);
            hash *= Diagnostics::RVX_DIAGNOSTICS_FNV1A64_PRIME;
        }
        return Diagnostics::FormatContentHash(hash);
    }

    bool ParseFields(std::string_view text,
                     ParsedFields& outFields,
                     std::string& outError)
    {
        outFields = {};
        std::istringstream stream{std::string(text)};
        stream.imbue(std::locale::classic());
        std::string line;
        while (std::getline(stream, line))
        {
            const size_t separator = line.find('=');
            if (separator == std::string::npos || separator == 0 ||
                !outFields.values.emplace(
                    line.substr(0, separator), line.substr(separator + 1)).second)
            {
                outError = separator == std::string::npos || separator == 0
                    ? "Model artifact contains a malformed field"
                    : "Model artifact contains a duplicate field";
                return false;
            }
        }
        return true;
    }

    template<typename T>
    bool ParseInteger(ParsedFields& fields, const std::string& key, T& outValue)
    {
        const auto it = fields.values.find(key);
        if (it == fields.values.end())
            return false;
        const char* begin = it->second.data();
        const char* end = begin + it->second.size();
        const auto [cursor, error] = std::from_chars(begin, end, outValue);
        if (error != std::errc{} || cursor != end)
            return false;
        fields.consumed.insert(key);
        return true;
    }

    bool ParseFloat(ParsedFields& fields, const std::string& key, float32& outValue)
    {
        const auto it = fields.values.find(key);
        if (it == fields.values.end())
            return false;
        try
        {
            size_t consumed = 0;
            outValue = std::stof(it->second, &consumed);
            if (consumed != it->second.size() || !std::isfinite(outValue))
                return false;
            fields.consumed.insert(key);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseString(ParsedFields& fields, const std::string& key, std::string& outValue)
    {
        const auto it = fields.values.find(key);
        if (it == fields.values.end() ||
            it->second.size() > MaxSingleStringBytes * 2ull ||
            !HexDecode(it->second, outValue) ||
            outValue.size() > MaxSingleStringBytes ||
            fields.decodedStringBytes > MaxAggregateStringBytes - outValue.size())
        {
            return false;
        }
        fields.decodedStringBytes += outValue.size();
        fields.consumed.insert(key);
        return true;
    }

    void WriteVec2(std::ostream& stream, const std::string& key, const Vec2& value)
    {
        stream << key << ".x=" << value.x << '\n'
               << key << ".y=" << value.y << '\n';
    }

    void WriteVec3(std::ostream& stream, const std::string& key, const Vec3& value)
    {
        stream << key << ".x=" << value.x << '\n'
               << key << ".y=" << value.y << '\n'
               << key << ".z=" << value.z << '\n';
    }

    void WriteVec4(std::ostream& stream, const std::string& key, const Vec4& value)
    {
        stream << key << ".x=" << value.x << '\n'
               << key << ".y=" << value.y << '\n'
               << key << ".z=" << value.z << '\n'
               << key << ".w=" << value.w << '\n';
    }

    bool ParseVec2(ParsedFields& fields, const std::string& key, Vec2& outValue)
    {
        return ParseFloat(fields, key + ".x", outValue.x) &&
               ParseFloat(fields, key + ".y", outValue.y);
    }

    bool ParseVec3(ParsedFields& fields, const std::string& key, Vec3& outValue)
    {
        return ParseFloat(fields, key + ".x", outValue.x) &&
               ParseFloat(fields, key + ".y", outValue.y) &&
               ParseFloat(fields, key + ".z", outValue.z);
    }

    bool ParseVec4(ParsedFields& fields, const std::string& key, Vec4& outValue)
    {
        return ParseFloat(fields, key + ".x", outValue.x) &&
               ParseFloat(fields, key + ".y", outValue.y) &&
               ParseFloat(fields, key + ".z", outValue.z) &&
               ParseFloat(fields, key + ".w", outValue.w);
    }

    const std::array<std::pair<const char*, const std::optional<TextureInfo>& (Material::*)() const>, 5>
        TextureSlots{{
            {"albedo", &Material::GetBaseColorTexture},
            {"normal", &Material::GetNormalTexture},
            {"metallicRoughness", &Material::GetMetallicRoughnessTexture},
            {"occlusion", &Material::GetOcclusionTexture},
            {"emissive", &Material::GetEmissiveTexture},
        }};

    void WriteTextureInfo(std::ostream& stream,
                          const std::string& key,
                          const std::optional<TextureInfo>& value)
    {
        stream << key << ".present=" << (value ? 1 : 0) << '\n';
        if (!value)
            return;
        stream << key << ".path=" << HexEncode(value->texturePath) << '\n'
               << key << ".imageId=" << value->imageId << '\n'
               << key << ".uvSet=" << value->uvSet << '\n'
               << key << ".rotation=" << value->rotation << '\n'
               << key << ".wrapS=" << static_cast<uint32>(value->wrapS) << '\n'
               << key << ".wrapT=" << static_cast<uint32>(value->wrapT) << '\n'
               << key << ".minFilter=" << static_cast<uint32>(value->minFilter) << '\n'
               << key << ".magFilter=" << static_cast<uint32>(value->magFilter) << '\n';
        WriteVec2(stream, key + ".offset", value->offset);
        WriteVec2(stream, key + ".scale", value->scale);
    }

    bool IsValidTextureInfo(const std::optional<TextureInfo>& value,
                            size_t textureCount)
    {
        if (!value)
            return true;
        return value->imageId >= 0 &&
               value->imageId < static_cast<int>(textureCount) &&
               std::isfinite(value->rotation) &&
               IsFiniteVector(value->offset) &&
               IsFiniteVector(value->scale) &&
               static_cast<uint32>(value->wrapS) <=
                   static_cast<uint32>(TextureInfo::WrapMode::ClampToBorder) &&
               static_cast<uint32>(value->wrapT) <=
                   static_cast<uint32>(TextureInfo::WrapMode::ClampToBorder) &&
               static_cast<uint32>(value->minFilter) <=
                   static_cast<uint32>(TextureInfo::FilterMode::LinearMipmapLinear) &&
               static_cast<uint32>(value->magFilter) <=
                   static_cast<uint32>(TextureInfo::FilterMode::LinearMipmapLinear);
    }

    bool ParseTextureInfo(ParsedFields& fields,
                          const std::string& key,
                          std::optional<TextureInfo>& outValue)
    {
        uint32 present = 0;
        if (!ParseInteger(fields, key + ".present", present))
            return false;
        if (present > 1)
            return false;
        if (present == 0)
        {
            outValue.reset();
            return true;
        }

        TextureInfo value;
        uint32 wrapS = 0;
        uint32 wrapT = 0;
        uint32 minFilter = 0;
        uint32 magFilter = 0;
        if (!ParseString(fields, key + ".path", value.texturePath) ||
            !ParseInteger(fields, key + ".imageId", value.imageId) ||
            !ParseInteger(fields, key + ".uvSet", value.uvSet) ||
            !ParseFloat(fields, key + ".rotation", value.rotation) ||
            !ParseInteger(fields, key + ".wrapS", wrapS) ||
            !ParseInteger(fields, key + ".wrapT", wrapT) ||
            !ParseInteger(fields, key + ".minFilter", minFilter) ||
            !ParseInteger(fields, key + ".magFilter", magFilter) ||
            !ParseVec2(fields, key + ".offset", value.offset) ||
            !ParseVec2(fields, key + ".scale", value.scale))
        {
            return false;
        }
        if (wrapS > static_cast<uint32>(TextureInfo::WrapMode::ClampToBorder) ||
            wrapT > static_cast<uint32>(TextureInfo::WrapMode::ClampToBorder) ||
            minFilter > static_cast<uint32>(TextureInfo::FilterMode::LinearMipmapLinear) ||
            magFilter > static_cast<uint32>(TextureInfo::FilterMode::LinearMipmapLinear))
        {
            return false;
        }
        value.wrapS = static_cast<TextureInfo::WrapMode>(wrapS);
        value.wrapT = static_cast<TextureInfo::WrapMode>(wrapT);
        value.minFilter = static_cast<TextureInfo::FilterMode>(minFilter);
        value.magFilter = static_cast<TextureInfo::FilterMode>(magFilter);
        outValue = std::move(value);
        return true;
    }

    struct FlatNode
    {
        const Node* node = nullptr;
        int32 parent = -1;
    };

    bool FlattenNode(const Node* node,
                     int32 parent,
                     uint32 depth,
                     std::unordered_set<const Node*>& visited,
                     std::vector<FlatNode>& outNodes,
                     std::string& outError)
    {
        if (!node)
        {
            outError = "Cooked model contains a null node";
            return false;
        }
        if (depth > MaxHierarchyDepth)
        {
            outError = "Cooked model hierarchy exceeds the depth limit";
            return false;
        }
        if (outNodes.size() >= MaxNodeCount)
        {
            outError = "Cooked model hierarchy exceeds the node limit";
            return false;
        }
        if (!visited.insert(node).second)
        {
            outError = "Cooked model hierarchy contains a cycle or duplicate node";
            return false;
        }
        const int32 current = static_cast<int32>(outNodes.size());
        outNodes.push_back({node, parent});
        for (const Node::Ptr& child : node->GetChildren())
        {
            if (!FlattenNode(child.get(), current, depth + 1u,
                             visited, outNodes, outError))
            {
                return false;
            }
        }
        return true;
    }

    bool ParseRawField(ParsedFields& fields,
                       const std::string& key,
                       std::string& outValue)
    {
        const auto it = fields.values.find(key);
        if (it == fields.values.end() || it->second.size() > MaxSingleStringBytes)
            return false;
        outValue = it->second;
        fields.consumed.insert(key);
        return true;
    }
} // namespace

bool SerializeCookedModelArtifact(const CookedModelArtifact& artifact,
                                  std::vector<uint8>& outBytes,
                                  std::string& outError)
{
    outBytes.clear();
    outError.clear();
    if (artifact.logicalAssetId.empty() || artifact.meshArtifactPath.empty() ||
        !IsCanonicalPackageRelativePath(artifact.meshArtifactPath) ||
        (artifact.animationArtifactPath.has_value() &&
         !IsCanonicalPackageRelativePath(*artifact.animationArtifactPath)) ||
        !artifact.rootNode || !IsFiniteBounds(artifact.bounds) ||
        artifact.textures.size() > MaxTextureCount ||
        artifact.materials.size() > MaxMaterialCount)
    {
        outError = "Cooked model requires a logical id, canonical dependencies, bounded counts, a root node, and finite bounds";
        return false;
    }
    uint64 aggregateStringBytes = 0;
    const auto accountString = [&aggregateStringBytes](std::string_view value)
    {
        if (value.size() > MaxSingleStringBytes ||
            aggregateStringBytes > MaxAggregateStringBytes - value.size())
        {
            return false;
        }
        aggregateStringBytes += value.size();
        return true;
    };
    if (!accountString(artifact.logicalAssetId) ||
        !accountString(artifact.meshArtifactPath) ||
        (artifact.animationArtifactPath.has_value() &&
         !accountString(*artifact.animationArtifactPath)))
    {
        outError = "Cooked model string metadata exceeds the configured limit";
        return false;
    }

    std::ostringstream payload;
    payload.imbue(std::locale::classic());
    payload << std::setprecision(std::numeric_limits<float32>::max_digits10);
    payload << "logicalAssetId=" << HexEncode(artifact.logicalAssetId) << '\n'
            << "meshArtifact=" << HexEncode(artifact.meshArtifactPath) << '\n';
    const bool hasAnimation = artifact.animationArtifactPath.has_value();
    payload << "animationPresent=" << (hasAnimation ? 1 : 0) << '\n';
    if (hasAnimation)
    {
        payload << "animationArtifact=" << HexEncode(*artifact.animationArtifactPath) << '\n';
    }
    payload << "boundsValid=1\n";
    WriteVec3(payload, "boundsMin", artifact.bounds.GetMin());
    WriteVec3(payload, "boundsMax", artifact.bounds.GetMax());

    payload << "textureCount=" << artifact.textures.size() << '\n';
    for (size_t index = 0; index < artifact.textures.size(); ++index)
    {
        const TextureReference& texture = artifact.textures[index];
        if (!texture.IsExternal() ||
            !IsCanonicalPackageRelativePath(texture.path) ||
            !accountString(texture.path) ||
            texture.imageIndex < 0 ||
            static_cast<uint32>(texture.usage) >
                static_cast<uint32>(TextureUsage::Data) ||
            static_cast<uint32>(texture.fallbackSemantic) >
                static_cast<uint32>(TextureFallbackSemantic::Black))
        {
            outError = "Cooked model texture dependency must be an external .rva path";
            return false;
        }
        const std::string key = "texture." + std::to_string(index);
        payload << key << ".path=" << HexEncode(texture.path) << '\n'
                << key << ".imageIndex=" << texture.imageIndex << '\n'
                << key << ".usage=" << static_cast<uint32>(texture.usage) << '\n'
                << key << ".srgb=" << (texture.isSRGB ? 1 : 0) << '\n'
                << key << ".fallback=" << static_cast<uint32>(texture.fallbackSemantic) << '\n';
    }

    payload << "materialCount=" << artifact.materials.size() << '\n';
    for (size_t index = 0; index < artifact.materials.size(); ++index)
    {
        const Material::Ptr& material = artifact.materials[index];
        if (!material)
        {
            outError = "Cooked model contains a null material";
            return false;
        }
        if (!accountString(material->GetName()))
        {
            outError = "Cooked model material names exceed the configured string limit";
            return false;
        }
        if (static_cast<uint32>(material->GetWorkflow()) >
                static_cast<uint32>(MaterialWorkflow::Unlit) ||
            static_cast<uint32>(material->GetAlphaMode()) >
                static_cast<uint32>(Material::AlphaMode::Blend) ||
            !std::isfinite(material->GetAlphaCutoff()) ||
            !std::isfinite(material->GetMetallicFactor()) ||
            !std::isfinite(material->GetRoughnessFactor()) ||
            !std::isfinite(material->GetNormalScale()) ||
            !std::isfinite(material->GetOcclusionStrength()) ||
            !std::isfinite(material->GetEmissiveStrength()) ||
            !IsFiniteVector(material->GetBaseColor()) ||
            !IsFiniteVector(material->GetEmissiveColor()))
        {
            outError = "Cooked model material contains invalid numeric metadata";
            return false;
        }
        for (const auto& [slotName, accessor] : TextureSlots)
        {
            (void)slotName;
            const std::optional<TextureInfo>& textureInfo = ((*material).*accessor)();
            if (!IsValidTextureInfo(textureInfo, artifact.textures.size()) ||
                (textureInfo && !accountString(textureInfo->texturePath)))
            {
                outError = "Cooked model material contains an invalid texture reference";
                return false;
            }
        }
        const std::string key = "material." + std::to_string(index);
        payload << key << ".name=" << HexEncode(material->GetName()) << '\n'
                << key << ".workflow=" << static_cast<uint32>(material->GetWorkflow()) << '\n'
                << key << ".doubleSided=" << (material->IsDoubleSided() ? 1 : 0) << '\n'
                << key << ".alphaMode=" << static_cast<uint32>(material->GetAlphaMode()) << '\n'
                << key << ".alphaCutoff=" << material->GetAlphaCutoff() << '\n'
                << key << ".metallic=" << material->GetMetallicFactor() << '\n'
                << key << ".roughness=" << material->GetRoughnessFactor() << '\n'
                << key << ".normalScale=" << material->GetNormalScale() << '\n'
                << key << ".occlusionStrength=" << material->GetOcclusionStrength() << '\n'
                << key << ".emissiveStrength=" << material->GetEmissiveStrength() << '\n';
        WriteVec4(payload, key + ".baseColor", material->GetBaseColor());
        WriteVec3(payload, key + ".emissive", material->GetEmissiveColor());
        for (const auto& [slotName, accessor] : TextureSlots)
            WriteTextureInfo(payload, key + ".slot." + slotName, ((*material).*accessor)());
    }

    std::vector<FlatNode> nodes;
    std::unordered_set<const Node*> visitedNodes;
    if (!FlattenNode(artifact.rootNode.get(), -1, 1u,
                     visitedNodes, nodes, outError))
    {
        return false;
    }
    payload << "nodeCount=" << nodes.size() << '\n';
    bool hasSkinnedNode = false;
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        const FlatNode& flat = nodes[index];
        const Node& node = *flat.node;
        const Transform& transform = node.GetLocalTransform();
        if (!accountString(node.GetName()) ||
            !IsFiniteVector(transform.GetPosition()) ||
            !IsFiniteQuaternion(transform.GetRotation()) ||
            !IsFiniteVector(transform.GetScale()))
        {
            outError = "Cooked model node contains a non-finite transform";
            return false;
        }
        if (node.GetSkinIndex() < -1 || node.GetSkinIndex() > 0)
        {
            outError = "Cooked model schema v2 supports at most one skin";
            return false;
        }
        hasSkinnedNode = hasSkinnedNode || node.HasSkin();
        const std::vector<int>& materialIndices = node.GetMaterialIndices();
        std::vector<int> resolvedMeshIndices;
        resolvedMeshIndices = node.GetMeshIndices();
        if (resolvedMeshIndices.empty() && node.GetMeshIndex() >= 0)
            resolvedMeshIndices.push_back(node.GetMeshIndex());
        if (resolvedMeshIndices.size() > MaxNodePrimitiveMappings ||
            (node.GetMeshIndex() < 0 &&
             (!resolvedMeshIndices.empty() || !materialIndices.empty())) ||
            (node.GetMeshIndex() >= 0 &&
             (resolvedMeshIndices.empty() || materialIndices.empty() ||
              resolvedMeshIndices.front() != node.GetMeshIndex())) ||
            resolvedMeshIndices.size() != materialIndices.size())
        {
            outError = "Cooked model primitive mesh and material mappings are inconsistent";
            return false;
        }
        std::unordered_set<int> uniqueMeshIndices;
        uniqueMeshIndices.reserve(resolvedMeshIndices.size());
        for (const int meshIndex : resolvedMeshIndices)
        {
            if (meshIndex < 0 || !uniqueMeshIndices.insert(meshIndex).second)
            {
                outError = "Cooked model primitive mesh indices must be unique and valid";
                return false;
            }
        }
        for (const int materialIndex : node.GetMaterialIndices())
        {
            if (materialIndex < 0 ||
                materialIndex >= static_cast<int>(artifact.materials.size()))
            {
                outError = "Cooked model node references an invalid material index";
                return false;
            }
        }
        const std::string key = "node." + std::to_string(index);
        payload << key << ".name=" << HexEncode(node.GetName()) << '\n'
                << key << ".parent=" << flat.parent << '\n'
                << key << ".active=" << (node.IsActive() ? 1 : 0) << '\n'
                << key << ".meshIndex=" << node.GetMeshIndex() << '\n'
                << key << ".skinIndex=" << node.GetSkinIndex() << '\n'
                << key << ".meshIndexCount=" << resolvedMeshIndices.size() << '\n';
        for (size_t meshIndex = 0;
             meshIndex < resolvedMeshIndices.size();
             ++meshIndex)
        {
            payload << key << ".meshIndex." << meshIndex << '='
                    << resolvedMeshIndices[meshIndex] << '\n';
        }
        payload
                << key << ".materialCount=" << node.GetMaterialIndices().size() << '\n';
        WriteVec3(payload, key + ".position", transform.GetPosition());
        const Quat& rotation = transform.GetRotation();
        payload << key << ".rotation.w=" << rotation.w << '\n'
                << key << ".rotation.x=" << rotation.x << '\n'
                << key << ".rotation.y=" << rotation.y << '\n'
                << key << ".rotation.z=" << rotation.z << '\n';
        WriteVec3(payload, key + ".scale", transform.GetScale());
        for (size_t materialIndex = 0; materialIndex < node.GetMaterialIndices().size(); ++materialIndex)
        {
            payload << key << ".material." << materialIndex << '='
                    << node.GetMaterialIndices()[materialIndex] << '\n';
        }
    }
    if (hasSkinnedNode != hasAnimation)
    {
        outError = "Cooked model skin nodes and .rvxanim dependency must be published together";
        return false;
    }

    const std::string payloadText = payload.str();
    const std::string contentHash = HashPayload(payloadText);
    std::ostringstream artifactStream;
    artifactStream.imbue(std::locale::classic());
    artifactStream << RVX_MODEL_ARTIFACT_MAGIC << '\n'
                   << "schemaVersion=" << RVX_MODEL_ARTIFACT_SCHEMA_VERSION << '\n'
                   << "writerAbi=" << RVX_MODEL_ARTIFACT_WRITER_ABI << '\n'
                   << "flags=" << (hasAnimation ? FlagHasAnimation : 0u)
                   << '\n'
                   << "contentHash=" << contentHash << '\n'
                   << "payloadSize=" << payloadText.size() << '\n'
                   << PayloadBegin << payloadText << PayloadEnd;
    const std::string serialized = artifactStream.str();
    if (serialized.size() > MaxArtifactBytes)
    {
        outError = "Cooked model artifact exceeds the size limit";
        return false;
    }
    outBytes.assign(serialized.begin(), serialized.end());
    return true;
}

bool DeserializeCookedModelArtifact(std::span<const uint8> bytes,
                                    CookedModelArtifact& outArtifact,
                                    std::string& outError)
{
    outArtifact = {};
    outError.clear();
    if (bytes.empty() || bytes.size() > MaxArtifactBytes)
    {
        outError = "Model artifact byte count is outside the supported limit";
        return false;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::string magicLine = std::string(RVX_MODEL_ARTIFACT_MAGIC) + '\n';
    if (!text.starts_with(magicLine))
    {
        outError = "Model artifact has an unsupported magic";
        return false;
    }

    const size_t payloadMarker = text.find(PayloadBegin);
    if (payloadMarker == std::string_view::npos)
    {
        outError = "Model artifact is missing its payload marker";
        return false;
    }
    ParsedFields header;
    if (!ParseFields(text.substr(magicLine.size(), payloadMarker - magicLine.size()),
                     header, outError))
    {
        return false;
    }
    uint32 schemaVersion = 0;
    uint32 flags = 0;
    uint64 payloadSize = 0;
    std::string writerAbi;
    std::string expectedHash;
    if (!ParseInteger(header, "schemaVersion", schemaVersion) ||
        !ParseRawField(header, "writerAbi", writerAbi) ||
        !ParseInteger(header, "flags", flags) ||
        !ParseRawField(header, "contentHash", expectedHash) ||
        !ParseInteger(header, "payloadSize", payloadSize) ||
        schemaVersion != RVX_MODEL_ARTIFACT_SCHEMA_VERSION ||
        writerAbi != RVX_MODEL_ARTIFACT_WRITER_ABI ||
        (flags & ~KnownFlags) != 0u ||
        header.consumed.size() != header.values.size())
    {
        outError = "Model artifact header, flags, or writer ABI is incompatible";
        return false;
    }

    const size_t payloadOffset = payloadMarker + PayloadBegin.size();
    if (payloadOffset > text.size() || text.size() < PayloadEnd.size() ||
        payloadSize > text.size() - payloadOffset ||
        payloadSize > std::numeric_limits<size_t>::max() - payloadOffset ||
        payloadOffset + static_cast<size_t>(payloadSize) >
            text.size() - PayloadEnd.size() ||
        payloadOffset + static_cast<size_t>(payloadSize) + PayloadEnd.size() != text.size() ||
        text.substr(payloadOffset + static_cast<size_t>(payloadSize), PayloadEnd.size()) != PayloadEnd)
    {
        outError = "Model artifact payload is truncated or has an invalid boundary";
        return false;
    }
    const std::string_view payload = text.substr(payloadOffset, static_cast<size_t>(payloadSize));
    if (HashPayload(payload) != expectedHash)
    {
        outError = "Model artifact content hash mismatch";
        return false;
    }

    ParsedFields fields;
    if (!ParseFields(payload, fields, outError))
        return false;
    if (!ParseString(fields, "logicalAssetId", outArtifact.logicalAssetId) ||
        !ParseString(fields, "meshArtifact", outArtifact.meshArtifactPath))
    {
        outError = "Model artifact dependency metadata is incomplete";
        return false;
    }
    uint32 animationPresent = 0;
    if (!ParseInteger(fields, "animationPresent", animationPresent) ||
        animationPresent > 1u ||
        ((flags & FlagHasAnimation) != 0u) != (animationPresent != 0u))
    {
        outError = "Model artifact animation-present flag is inconsistent";
        return false;
    }
    if (animationPresent != 0u)
    {
        std::string animationPath;
        if (!ParseString(fields, "animationArtifact", animationPath) ||
            !IsCanonicalPackageRelativePath(animationPath))
        {
            outError = "Model artifact animation dependency metadata is invalid";
            return false;
        }
        outArtifact.animationArtifactPath = std::move(animationPath);
    }
    else if (fields.values.contains("animationArtifact"))
    {
        outError = "Static model artifact may not contain an animation dependency";
        return false;
    }
    if (outArtifact.logicalAssetId.empty() ||
        !IsCanonicalPackageRelativePath(outArtifact.meshArtifactPath))
    {
        outError = "Model artifact logical id or mesh dependency is invalid";
        return false;
    }
    uint32 boundsValid = 0;
    if (!ParseInteger(fields, "boundsValid", boundsValid))
    {
        outError = "Model artifact is missing bounds metadata";
        return false;
    }
    if (boundsValid != 1)
    {
        outError = "Model artifact must contain finite model bounds";
        return false;
    }
    {
        Vec3 minValue;
        Vec3 maxValue;
        if (!ParseVec3(fields, "boundsMin", minValue) ||
            !ParseVec3(fields, "boundsMax", maxValue))
        {
            outError = "Model artifact bounds are incomplete";
            return false;
        }
        outArtifact.bounds = AABB(minValue, maxValue);
        if (!IsFiniteBounds(outArtifact.bounds))
        {
            outError = "Model artifact bounds are non-finite or inverted";
            return false;
        }
    }

    uint32 textureCount = 0;
    if (!ParseInteger(fields, "textureCount", textureCount))
    {
        outError = "Model artifact is missing textureCount";
        return false;
    }
    if (textureCount > MaxTextureCount)
    {
        outError = "Model artifact textureCount exceeds the configured limit";
        return false;
    }
    outArtifact.textures.reserve(textureCount);
    for (uint32 index = 0; index < textureCount; ++index)
    {
        TextureReference texture;
        uint32 usage = 0;
        uint32 srgb = 0;
        uint32 fallback = 0;
        const std::string key = "texture." + std::to_string(index);
        if (!ParseString(fields, key + ".path", texture.path) ||
            !ParseInteger(fields, key + ".imageIndex", texture.imageIndex) ||
            !ParseInteger(fields, key + ".usage", usage) ||
            !ParseInteger(fields, key + ".srgb", srgb) ||
            !ParseInteger(fields, key + ".fallback", fallback) ||
            usage > static_cast<uint32>(TextureUsage::Data) ||
            fallback > static_cast<uint32>(TextureFallbackSemantic::Black) ||
            srgb > 1 || texture.imageIndex < 0 ||
            !IsCanonicalPackageRelativePath(texture.path))
        {
            outError = "Model artifact texture metadata is invalid";
            return false;
        }
        texture.sourceType = TextureSourceType::External;
        texture.usage = static_cast<TextureUsage>(usage);
        texture.isSRGB = srgb != 0;
        texture.fallbackSemantic = static_cast<TextureFallbackSemantic>(fallback);
        outArtifact.textures.push_back(std::move(texture));
    }

    uint32 materialCount = 0;
    if (!ParseInteger(fields, "materialCount", materialCount))
    {
        outError = "Model artifact is missing materialCount";
        return false;
    }
    if (materialCount > MaxMaterialCount)
    {
        outError = "Model artifact materialCount exceeds the configured limit";
        return false;
    }
    outArtifact.materials.reserve(materialCount);
    for (uint32 index = 0; index < materialCount; ++index)
    {
        const std::string key = "material." + std::to_string(index);
        std::string name;
        uint32 workflow = 0;
        uint32 doubleSided = 0;
        uint32 alphaMode = 0;
        float32 alphaCutoff = 0.5f;
        float32 metallic = 1.0f;
        float32 roughness = 1.0f;
        float32 normalScale = 1.0f;
        float32 occlusionStrength = 1.0f;
        float32 emissiveStrength = 1.0f;
        Vec4 baseColor;
        Vec3 emissive;
        if (!ParseString(fields, key + ".name", name) ||
            !ParseInteger(fields, key + ".workflow", workflow) ||
            !ParseInteger(fields, key + ".doubleSided", doubleSided) ||
            !ParseInteger(fields, key + ".alphaMode", alphaMode) ||
            !ParseFloat(fields, key + ".alphaCutoff", alphaCutoff) ||
            !ParseFloat(fields, key + ".metallic", metallic) ||
            !ParseFloat(fields, key + ".roughness", roughness) ||
            !ParseFloat(fields, key + ".normalScale", normalScale) ||
            !ParseFloat(fields, key + ".occlusionStrength", occlusionStrength) ||
            !ParseFloat(fields, key + ".emissiveStrength", emissiveStrength) ||
            !ParseVec4(fields, key + ".baseColor", baseColor) ||
            !ParseVec3(fields, key + ".emissive", emissive) ||
            workflow > static_cast<uint32>(MaterialWorkflow::Unlit) ||
            alphaMode > static_cast<uint32>(Material::AlphaMode::Blend) ||
            doubleSided > 1)
        {
            outError = "Model artifact material metadata is invalid";
            return false;
        }

        auto material = std::make_shared<Material>(name);
        material->SetMaterialId(index);
        material->SetWorkflow(static_cast<MaterialWorkflow>(workflow));
        material->SetDoubleSided(doubleSided != 0);
        material->SetAlphaMode(static_cast<Material::AlphaMode>(alphaMode));
        material->SetAlphaCutoff(alphaCutoff);
        material->SetBaseColor(baseColor);
        material->SetMetallicFactor(metallic);
        material->SetRoughnessFactor(roughness);
        material->SetNormalScale(normalScale);
        material->SetOcclusionStrength(occlusionStrength);
        material->SetEmissiveColor(emissive);
        material->SetEmissiveStrength(emissiveStrength);

        std::array<std::optional<TextureInfo>, 5> textureInfos;
        for (size_t slotIndex = 0; slotIndex < TextureSlots.size(); ++slotIndex)
        {
            if (!ParseTextureInfo(fields,
                                  key + ".slot." + TextureSlots[slotIndex].first,
                                  textureInfos[slotIndex]))
            {
                outError = "Model artifact material texture metadata is invalid";
                return false;
            }
            if (textureInfos[slotIndex] &&
                (textureInfos[slotIndex]->imageId < 0 ||
                 textureInfos[slotIndex]->imageId >=
                     static_cast<int>(outArtifact.textures.size())))
            {
                outError = "Model artifact material references an invalid texture index";
                return false;
            }
        }
        if (textureInfos[0]) material->SetBaseColorTexture(*textureInfos[0]);
        if (textureInfos[1]) material->SetNormalTexture(*textureInfos[1]);
        if (textureInfos[2]) material->SetMetallicRoughnessTexture(*textureInfos[2]);
        if (textureInfos[3]) material->SetOcclusionTexture(*textureInfos[3]);
        if (textureInfos[4]) material->SetEmissiveTexture(*textureInfos[4]);
        outArtifact.materials.push_back(std::move(material));
    }

    uint32 nodeCount = 0;
    if (!ParseInteger(fields, "nodeCount", nodeCount) || nodeCount == 0)
    {
        outError = "Model artifact contains no nodes";
        return false;
    }
    if (nodeCount > MaxNodeCount)
    {
        outError = "Model artifact nodeCount exceeds the configured limit";
        return false;
    }
    std::vector<Node::Ptr> nodes;
    std::vector<int32> parents;
    std::vector<uint32> depths;
    nodes.reserve(nodeCount);
    parents.reserve(nodeCount);
    depths.reserve(nodeCount);
    for (uint32 index = 0; index < nodeCount; ++index)
    {
        const std::string key = "node." + std::to_string(index);
        std::string name;
        int32 parent = -1;
        uint32 active = 0;
        int32 meshIndex = -1;
        int32 skinIndex = -1;
        uint32 nodeMaterialCount = 0;
        Vec3 position;
        Vec3 scale;
        Quat rotation;
        if (!ParseString(fields, key + ".name", name) ||
            !ParseInteger(fields, key + ".parent", parent) ||
            !ParseInteger(fields, key + ".active", active) ||
            !ParseInteger(fields, key + ".meshIndex", meshIndex) ||
            !ParseInteger(fields, key + ".skinIndex", skinIndex) ||
            !ParseInteger(fields, key + ".materialCount", nodeMaterialCount) ||
            !ParseVec3(fields, key + ".position", position) ||
            !ParseFloat(fields, key + ".rotation.w", rotation.w) ||
            !ParseFloat(fields, key + ".rotation.x", rotation.x) ||
            !ParseFloat(fields, key + ".rotation.y", rotation.y) ||
            !ParseFloat(fields, key + ".rotation.z", rotation.z) ||
            !ParseVec3(fields, key + ".scale", scale) ||
            parent < -1 || parent >= static_cast<int32>(index) || active > 1 ||
            skinIndex < -1 || skinIndex > 0 ||
            nodeMaterialCount > MaxNodePrimitiveMappings)
        {
            outError = "Model artifact node hierarchy is invalid";
            return false;
        }

        uint32 meshIndexCount = 0;
        if (!ParseInteger(fields, key + ".meshIndexCount", meshIndexCount) ||
            meshIndexCount > MaxNodePrimitiveMappings ||
            meshIndexCount != nodeMaterialCount ||
            (meshIndex < 0 && meshIndexCount != 0) ||
            (meshIndex >= 0 && meshIndexCount == 0))
        {
            outError = "Model artifact primitive mesh index metadata is invalid";
            return false;
        }

        const uint32 depth = parent < 0
            ? 1u
            : depths[static_cast<size_t>(parent)] + 1u;
        if (depth > MaxHierarchyDepth)
        {
            outError = "Model artifact hierarchy exceeds the depth limit";
            return false;
        }

        auto node = std::make_shared<Node>(name);
        node->SetActive(active != 0);
        node->GetLocalTransform().SetPosition(position);
        node->GetLocalTransform().SetRotation(rotation);
        node->GetLocalTransform().SetScale(scale);
        node->SetMeshIndex(meshIndex);
        node->SetSkinIndex(skinIndex);
        std::vector<int> meshIndices;
        meshIndices.reserve(meshIndexCount);
        std::unordered_set<int32> uniqueMeshIndices;
        uniqueMeshIndices.reserve(meshIndexCount);
        for (uint32 meshOffset = 0; meshOffset < meshIndexCount; ++meshOffset)
        {
            int32 value = -1;
            if (!ParseInteger(fields,
                              key + ".meshIndex." + std::to_string(meshOffset),
                              value) ||
                value < 0 || !uniqueMeshIndices.insert(value).second)
            {
                outError = "Model artifact primitive mesh index list is incomplete or non-unique";
                return false;
            }
            meshIndices.push_back(value);
        }
        if (meshIndex >= 0 && meshIndices.front() != meshIndex)
        {
            outError = "Model artifact primary mesh index disagrees with primitive mesh indices";
            return false;
        }
        node->SetMeshIndices(std::move(meshIndices));
        std::vector<int> materialIndices;
        materialIndices.reserve(nodeMaterialCount);
        for (uint32 materialIndex = 0; materialIndex < nodeMaterialCount; ++materialIndex)
        {
            int32 value = -1;
            if (!ParseInteger(fields, key + ".material." + std::to_string(materialIndex), value))
            {
                outError = "Model artifact node material list is incomplete";
                return false;
            }
            if (value < 0 || value >= static_cast<int32>(outArtifact.materials.size()))
            {
                outError = "Model artifact node references an invalid material index";
                return false;
            }
            materialIndices.push_back(value);
        }
        node->SetMaterialIndices(materialIndices);
        nodes.push_back(std::move(node));
        parents.push_back(parent);
        depths.push_back(depth);
    }

    size_t rootCount = 0;
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        if (parents[index] < 0)
        {
            outArtifact.rootNode = nodes[index];
            ++rootCount;
        }
        else
        {
            nodes[static_cast<size_t>(parents[index])]->AddChild(nodes[index]);
        }
    }
    if (rootCount != 1)
    {
        outError = "Model artifact must contain exactly one root node";
        outArtifact = {};
        return false;
    }
    bool hasSkinnedNode = false;
    for (const Node::Ptr& node : nodes)
        hasSkinnedNode = hasSkinnedNode || (node && node->HasSkin());
    if (hasSkinnedNode != outArtifact.animationArtifactPath.has_value())
    {
        outError = "Model artifact skin nodes and animation dependency are inconsistent";
        outArtifact = {};
        return false;
    }
    if (fields.consumed.size() != fields.values.size())
    {
        outError = "Model artifact contains unknown or unconsumed fields";
        outArtifact = {};
        return false;
    }
    return true;
}
} // namespace RVX::Resource
