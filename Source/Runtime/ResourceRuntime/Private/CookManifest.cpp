/** @file CookManifest.cpp  @brief Strict Resource-owned RVX cook manifest v2 admission. */

#include "Resource/CookManifest.h"

#include "Core/Hash/SHA256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace RVX::Resource
{
namespace
{
    constexpr const char* CookedClosureSchema = "RVX_COOKED_CONTENT_CLOSURE_V1";

    bool IsSha256(std::string_view value)
    {
        if (value.size() != 64u)
        {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [](const char character)
        {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
    }

    bool IsCanonicalRelativePath(const std::string& path)
    {
        if (path.empty() || path.find('\0') != std::string::npos ||
            path.find('\\') != std::string::npos)
        {
            return false;
        }

        const fs::path parsed(path);
        if (parsed.is_absolute() || parsed.has_root_name() ||
            parsed.generic_string() != path)
        {
            return false;
        }
        for (const fs::path& component : parsed)
        {
            if (component.empty() || component == "." || component == "..")
            {
                return false;
            }
        }
        return true;
    }

    bool IsStrictlySorted(const std::vector<CookFileIdentity>& identities)
    {
        return std::adjacent_find(identities.begin(), identities.end(),
            [](const CookFileIdentity& lhs, const CookFileIdentity& rhs)
            {
                return lhs.relativePath >= rhs.relativePath;
            }) == identities.end();
    }

    bool IsStrictlySorted(const std::vector<std::string>& values)
    {
        return std::adjacent_find(values.begin(), values.end(),
            [](const std::string& lhs, const std::string& rhs) { return lhs >= rhs; }) == values.end();
    }

    bool IsEntryBefore(const CookManifestEntry& lhs, const CookManifestEntry& rhs)
    {
        if (lhs.sourcePath != rhs.sourcePath)
        {
            return lhs.sourcePath < rhs.sourcePath;
        }
        if (lhs.outputPath != rhs.outputPath)
        {
            return lhs.outputPath < rhs.outputPath;
        }
        return static_cast<uint32>(lhs.type) < static_cast<uint32>(rhs.type);
    }

    std::string EscapeManifestValue(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char character : value)
        {
            switch (character)
            {
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped += character; break;
            }
        }
        return escaped;
    }

    bool UnescapeManifestValue(const std::string_view value,
                               std::string& outValue,
                               std::string& outError)
    {
        outValue.clear();
        outValue.reserve(value.size());
        for (size_t index = 0; index < value.size(); ++index)
        {
            const char character = value[index];
            if (character == '\0' || character == '\r' || character == '\n')
            {
                outError = "Cook manifest values may not contain raw control separators";
                return false;
            }
            if (character != '\\')
            {
                outValue += character;
                continue;
            }
            if (++index == value.size())
            {
                outError = "Cook manifest contains a truncated escape";
                return false;
            }
            switch (value[index])
            {
                case '\\': outValue += '\\'; break;
                case 'n': outValue += '\n'; break;
                case 'r': outValue += '\r'; break;
                case 't': outValue += '\t'; break;
                default:
                    outError = "Cook manifest contains an unknown escape";
                    return false;
            }
        }
        if (outValue.find('\0') != std::string::npos)
        {
            outError = "Cook manifest value contains NUL";
            return false;
        }
        return true;
    }

    bool ParseUnsigned(const std::string& text, uint64& outValue)
    {
        if (text.empty() || (text.size() > 1 && text.front() == '0') ||
            !std::all_of(text.begin(), text.end(), [](const char character)
            {
                return character >= '0' && character <= '9';
            }))
        {
            return false;
        }
        const auto result = std::from_chars(text.data(), text.data() + text.size(), outValue);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }

    bool ParseBool(const std::string& text, bool& outValue)
    {
        if (text == "0")
        {
            outValue = false;
            return true;
        }
        if (text == "1")
        {
            outValue = true;
            return true;
        }
        return false;
    }

    bool ParseAssetType(const std::string& value, CookAssetType& outType)
    {
        static constexpr std::pair<const char*, CookAssetType> Types[] = {
            { "Unknown", CookAssetType::Unknown }, { "Texture", CookAssetType::Texture },
            { "Mesh", CookAssetType::Mesh }, { "Material", CookAssetType::Material },
            { "Shader", CookAssetType::Shader }, { "Animation", CookAssetType::Animation },
            { "Audio", CookAssetType::Audio }, { "Font", CookAssetType::Font },
            { "Prefab", CookAssetType::Prefab }, { "Scene", CookAssetType::Scene },
            { "Script", CookAssetType::Script }, { "Model", CookAssetType::Model },
            { "Environment", CookAssetType::Environment },
        };
        for (const auto& [name, type] : Types)
        {
            if (value == name)
            {
                outType = type;
                return true;
            }
        }
        return false;
    }

    std::string ComputeStringSha256(const std::string_view value)
    {
        Hash::SHA256Hasher hasher;
        hasher.Update(value);
        return hasher.FinalizeHex();
    }

    std::string BuildRecipeHash(const CookManifestEntry& entry)
    {
        std::string recipe = "recipeSchema=RVX_COOK_RECIPE_V1\n";
        recipe += "toolName=" + std::string(CookManifest::DefaultToolName) + '\n';
        recipe += "toolVersion=" + std::string(CookManifest::DefaultToolVersion) + '\n';
        recipe += "importer=" + entry.importerName + '\n';
        recipe += "assetType=" + std::string(GetCookAssetTypeName(entry.type)) + '\n';
        recipe += "output.path=" + entry.outputPath + '\n';
        recipe += "source.path=" + entry.sourceContent.relativePath + '\n';
        recipe += "source.byteCount=" + std::to_string(entry.sourceContent.byteCount) + '\n';
        recipe += "source.sha256=" + entry.sourceContent.sha256 + '\n';
        if (entry.sourceDependencyClosureRecorded)
        {
            recipe += "sourceDependencyCount=" +
                      std::to_string(entry.sourceDependencies.size()) + '\n';
            for (const CookFileIdentity& dependency : entry.sourceDependencies)
            {
                recipe += "sourceDependency.path=" + dependency.relativePath + '\n';
                recipe += "sourceDependency.byteCount=" +
                          std::to_string(dependency.byteCount) + '\n';
                recipe += "sourceDependency.sha256=" + dependency.sha256 + '\n';
            }
        }
        recipe += "cookSettingsHash=" + entry.cookSettingsHash + '\n';
        recipe += "dependencyCount=" + std::to_string(entry.dependencies.size()) + '\n';
        for (const CookFileIdentity& dependency : entry.dependencies)
        {
            recipe += "dependency.path=" + dependency.relativePath + '\n';
            recipe += "dependency.byteCount=" + std::to_string(dependency.byteCount) + '\n';
            recipe += "dependency.sha256=" + dependency.sha256 + '\n';
        }
        return ComputeStringSha256(recipe);
    }

    ResourceContentIdentity MakeIdentity(const ResourceContentIdentityDomain domain,
                                         const ResourceContentIdentityScope scope,
                                         const std::string& digest,
                                         const uint64 byteCount,
                                         const uint32 fileCount)
    {
        ResourceContentIdentity identity;
        identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = domain;
        identity.scope = scope;
        identity.algorithm = ResourceContentHashAlgorithm::SHA256;
        identity.digest = digest;
        identity.byteCount = byteCount;
        identity.fileCount = fileCount;
        return identity;
    }

    int HexNibble(const char character)
    {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    }

    bool AppendSourceClosureRecord(Hash::SHA256Hasher& hasher,
                                   const std::string_view uri,
                                   const CookFileIdentity& identity,
                                   std::string& outError)
    {
        if (!identity.IsValid())
        {
            outError = "Cook manifest source closure has an invalid file identity.";
            return false;
        }
        hasher.Update(uri);
        constexpr uint8 separator = 0;
        hasher.Update(&separator, 1);
        std::array<uint8, 8> byteCountBytes{};
        for (size_t index = 0; index < byteCountBytes.size(); ++index)
        {
            const uint32 shift = static_cast<uint32>((byteCountBytes.size() - 1u - index) * 8u);
            byteCountBytes[index] = static_cast<uint8>(identity.byteCount >> shift);
        }
        hasher.Update(byteCountBytes.data(), byteCountBytes.size());
        std::array<uint8, 32> digest{};
        for (size_t index = 0; index < digest.size(); ++index)
        {
            const int high = HexNibble(identity.sha256[index * 2u]);
            const int low = HexNibble(identity.sha256[index * 2u + 1u]);
            if (high < 0 || low < 0)
            {
                outError = "Cook manifest source closure has a non-canonical SHA-256 digest.";
                return false;
            }
            digest[index] = static_cast<uint8>((high << 4) | low);
        }
        hasher.Update(digest.data(), digest.size());
        return true;
    }

    bool ComputeSourceContentIdentity(const CookManifestEntry& entry,
                                      ResourceContentIdentity& outIdentity,
                                      std::string& outError)
    {
        if (!entry.sourceContent.IsValid())
        {
            outError = "Cook manifest selected entry has an invalid root source identity.";
            return false;
        }
        if (!entry.sourceDependencyClosureRecorded || entry.sourceDependencies.empty())
        {
            outIdentity = MakeIdentity(ResourceContentIdentityDomain::Source,
                                       ResourceContentIdentityScope::SelfContainedArtifact,
                                       entry.sourceContent.sha256,
                                       entry.sourceContent.byteCount,
                                       1);
            return true;
        }
        if (entry.sourceDependencies.size() >= std::numeric_limits<uint32>::max())
        {
            outError = "Cook manifest source closure has too many files.";
            return false;
        }

        const fs::path sourceDirectory = fs::path(entry.sourcePath).parent_path();
        Hash::SHA256Hasher hasher;
        constexpr std::string_view closureDomain =
            "RVX.ResourceContentIdentity.DependencyClosure.v1";
        constexpr uint8 separator = 0;
        hasher.Update(closureDomain);
        hasher.Update(&separator, 1);
        if (!AppendSourceClosureRecord(hasher, "", entry.sourceContent, outError))
        {
            return false;
        }

        uint64 totalBytes = entry.sourceContent.byteCount;
        std::string previousUri;
        for (const CookFileIdentity& dependency : entry.sourceDependencies)
        {
            const fs::path uriPath = sourceDirectory.empty()
                ? fs::path(dependency.relativePath)
                : fs::path(dependency.relativePath).lexically_relative(sourceDirectory);
            const std::string uri = uriPath.generic_string();
            if (uri.empty() || uriPath.is_absolute() || uriPath.has_root_name() ||
                !IsCanonicalRelativePath(uri) || (!previousUri.empty() && previousUri >= uri))
            {
                outError = "Cook manifest source dependency is not a unique contained URI relative to its root glTF.";
                return false;
            }
            if (dependency.byteCount > std::numeric_limits<uint64>::max() - totalBytes)
            {
                outError = "Cook manifest source closure byte count overflow.";
                return false;
            }
            if (!AppendSourceClosureRecord(hasher, uri, dependency, outError))
            {
                return false;
            }
            totalBytes += dependency.byteCount;
            previousUri = uri;
        }
        outIdentity = MakeIdentity(ResourceContentIdentityDomain::Source,
                                   ResourceContentIdentityScope::DependencyClosure,
                                   hasher.FinalizeHex(),
                                   totalBytes,
                                   static_cast<uint32>(entry.sourceDependencies.size() + 1u));
        return true;
    }

    bool ResolveRoot(const fs::path& requestedRoot, fs::path& outRoot, std::string& outError)
    {
        std::error_code error;
        const fs::path absolute = fs::absolute(requestedRoot, error);
        if (error)
        {
            outError = "Unable to form an absolute mounted root: " + error.message();
            return false;
        }
        outRoot = fs::weakly_canonical(absolute, error);
        if (error || !fs::is_directory(outRoot, error) || error)
        {
            outError = "Mounted root is not an accessible directory: " + requestedRoot.string();
            return false;
        }
        return true;
    }

    bool PathComponentEquals(const fs::path& lhs, const fs::path& rhs)
    {
#if defined(_WIN32)
        std::string left = lhs.generic_string();
        std::string right = rhs.generic_string();
        std::transform(left.begin(), left.end(), left.begin(), [](const unsigned char c)
        { return static_cast<char>(std::tolower(c)); });
        std::transform(right.begin(), right.end(), right.begin(), [](const unsigned char c)
        { return static_cast<char>(std::tolower(c)); });
        return left == right;
#else
        return lhs == rhs;
#endif
    }

    bool IsContainedBy(const fs::path& path, const fs::path& root)
    {
        auto pathIt = path.begin();
        for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++pathIt)
        {
            if (pathIt == path.end() || !PathComponentEquals(*pathIt, *rootIt))
            {
                return false;
            }
        }
        return true;
    }

    bool CaptureMountedIdentity(const fs::path& root,
                                const CookFileIdentity& expected,
                                CookFileIdentity& outActual,
                                std::string& outError)
    {
        if (!expected.IsValid())
        {
            outError = "Manifest records an invalid file identity";
            return false;
        }
        std::error_code error;
        const fs::path candidate = root / fs::path(expected.relativePath);
        const fs::path canonical = fs::weakly_canonical(candidate, error);
        if (error || !IsContainedBy(canonical, root))
        {
            outError = "Mounted content path escapes its declared root: " + expected.relativePath;
            return false;
        }
        if (!fs::is_regular_file(canonical, error) || error)
        {
            outError = "Mounted content is missing or is not a regular file: " + expected.relativePath;
            return false;
        }

        const uintmax_t size = fs::file_size(canonical, error);
        if (error || size > static_cast<uintmax_t>(std::numeric_limits<uint64>::max()))
        {
            outError = "Unable to determine mounted content byte count: " + expected.relativePath;
            return false;
        }
        std::ifstream input(canonical, std::ios::binary);
        if (!input.is_open())
        {
            outError = "Unable to read mounted content: " + expected.relativePath;
            return false;
        }
        Hash::SHA256Hasher hasher;
        char buffer[64 * 1024];
        uint64 readBytes = 0;
        while (input.good())
        {
            input.read(buffer, sizeof(buffer));
            const std::streamsize count = input.gcount();
            if (count > 0)
            {
                hasher.Update(buffer, static_cast<size_t>(count));
                readBytes += static_cast<uint64>(count);
            }
        }
        if (!input.eof() || readBytes != static_cast<uint64>(size))
        {
            outError = "Mounted content changed while it was being hashed: " + expected.relativePath;
            return false;
        }

        outActual.relativePath = expected.relativePath;
        outActual.byteCount = readBytes;
        outActual.sha256 = hasher.FinalizeHex();
        if (!(outActual == expected))
        {
            outError = "Mounted content does not match its manifest identity: " + expected.relativePath;
            return false;
        }
        return true;
    }

    bool Take(std::map<std::string, std::string>& fields,
              const std::string& key,
              std::string& outValue,
              std::string& outError)
    {
        const auto found = fields.find(key);
        if (found == fields.end())
        {
            outError = "Cook manifest is missing required key: " + key;
            return false;
        }
        outValue = std::move(found->second);
        fields.erase(found);
        return true;
    }

    bool TakeUnsigned(std::map<std::string, std::string>& fields,
                      const std::string& key,
                      uint64& outValue,
                      std::string& outError)
    {
        std::string value;
        if (!Take(fields, key, value, outError) || !ParseUnsigned(value, outValue))
        {
            if (outError.empty()) outError = "Cook manifest has invalid unsigned value: " + key;
            return false;
        }
        return true;
    }

    bool TakeBool(std::map<std::string, std::string>& fields,
                  const std::string& key,
                  bool& outValue,
                  std::string& outError)
    {
        std::string value;
        if (!Take(fields, key, value, outError) || !ParseBool(value, outValue))
        {
            if (outError.empty()) outError = "Cook manifest has invalid boolean value: " + key;
            return false;
        }
        return true;
    }

    bool ParseFileIdentity(std::map<std::string, std::string>& fields,
                           const std::string& prefix,
                           CookFileIdentity& outIdentity,
                           std::string& outError)
    {
        return Take(fields, prefix + ".path", outIdentity.relativePath, outError) &&
               TakeUnsigned(fields, prefix + ".byteCount", outIdentity.byteCount, outError) &&
               Take(fields, prefix + ".sha256", outIdentity.sha256, outError);
    }

    bool MatchesSelector(const CookManifestEntry& entry, const CookAssetSelector& selector)
    {
        return (selector.sourcePath.empty() || selector.sourcePath == entry.sourcePath) &&
               (selector.outputPath.empty() || selector.outputPath == entry.outputPath) &&
               (selector.type == CookAssetType::Unknown || selector.type == entry.type);
    }
} // namespace

bool CookFileIdentity::IsValid() const noexcept
{
    return IsCanonicalRelativePath(relativePath) && IsSha256(sha256);
}

bool CookFileIdentity::operator==(const CookFileIdentity& other) const
{
    return relativePath == other.relativePath && byteCount == other.byteCount && sha256 == other.sha256;
}

bool CookSemanticMetrics::operator==(const CookSemanticMetrics& other) const
{
    return entryCount == other.entryCount && successCount == other.successCount &&
           failureCount == other.failureCount && dependencyCount == other.dependencyCount &&
           artifactCount == other.artifactCount && artifactByteCount == other.artifactByteCount;
}

bool CookAssetSelector::IsEmpty() const noexcept
{
    return sourcePath.empty() && outputPath.empty() && type == CookAssetType::Unknown;
}

const char* GetCookAssetTypeName(const CookAssetType type)
{
    switch (type)
    {
        case CookAssetType::Texture: return "Texture";
        case CookAssetType::Mesh: return "Mesh";
        case CookAssetType::Material: return "Material";
        case CookAssetType::Shader: return "Shader";
        case CookAssetType::Animation: return "Animation";
        case CookAssetType::Audio: return "Audio";
        case CookAssetType::Font: return "Font";
        case CookAssetType::Prefab: return "Prefab";
        case CookAssetType::Scene: return "Scene";
        case CookAssetType::Script: return "Script";
        case CookAssetType::Model: return "Model";
        case CookAssetType::Environment: return "Environment";
        default: return "Unknown";
    }
}

const char* GetCookManifestAdmissionCodeName(const CookManifestAdmissionCode code)
{
    switch (code)
    {
        case CookManifestAdmissionCode::Accepted: return "accepted";
        case CookManifestAdmissionCode::InvalidManifest: return "invalid-manifest";
        case CookManifestAdmissionCode::ToolMismatch: return "tool-mismatch";
        case CookManifestAdmissionCode::SelectorNotFound: return "selector-not-found";
        case CookManifestAdmissionCode::SelectorAmbiguous: return "selector-ambiguous";
        case CookManifestAdmissionCode::SourceIdentityMismatch: return "source-identity-mismatch";
        case CookManifestAdmissionCode::CookedIdentityMismatch: return "cooked-identity-mismatch";
        case CookManifestAdmissionCode::ManifestIdentityMismatch: return "manifest-identity-mismatch";
        case CookManifestAdmissionCode::CookSettingsMismatch: return "cook-settings-mismatch";
        case CookManifestAdmissionCode::RecipeMismatch: return "recipe-mismatch";
        case CookManifestAdmissionCode::MountedContentMismatch: return "mounted-content-mismatch";
        default: return "unknown";
    }
}

bool CookManifest::IsSemanticallyValid(std::string& outError) const
{
    if (schema != SchemaName || version != RVX_COOK_MANIFEST_SCHEMA_VERSION ||
        toolName.empty() || toolVersion.empty() || toolName.find('\0') != std::string::npos ||
        toolVersion.find('\0') != std::string::npos ||
        observedSourceRoot.find('\0') != std::string::npos ||
        observedOutputRoot.find('\0') != std::string::npos)
    {
        outError = "Cook manifest has invalid v2 header metadata";
        return false;
    }

    CookSemanticMetrics actual;
    actual.entryCount = entries.size();
    std::set<std::string> declaredArtifactPaths;
    for (size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex)
    {
        const CookManifestEntry& entry = entries[entryIndex];
        if (!entry.success)
        {
            outError = "Cook manifest contains a failed or ambiguous entry";
            return false;
        }
        if (entry.type == CookAssetType::Unknown || entry.error.size() != 0 || entry.importerName.empty() ||
            entry.canonicalCookSettings.empty() || !IsCanonicalRelativePath(entry.sourcePath) ||
            !IsCanonicalRelativePath(entry.outputPath) || entry.sourcePath != entry.sourceContent.relativePath ||
            !entry.sourceContent.IsValid() || !IsStrictlySorted(entry.warnings) ||
            !IsStrictlySorted(entry.sourceDependencies) || !IsStrictlySorted(entry.dependencies) ||
            !IsStrictlySorted(entry.artifacts) ||
            entry.artifacts.empty())
        {
            outError = "Cook manifest entry has invalid identity or ordering fields";
            return false;
        }
        if (entryIndex > 0 && !IsEntryBefore(entries[entryIndex - 1], entry))
        {
            outError = "Cook manifest entries are not in strict canonical order";
            return false;
        }
        if (entry.cookSettingsHash != ComputeStringSha256(entry.canonicalCookSettings) ||
            entry.recipeHash != BuildRecipeHash(entry))
        {
            outError = "Cook manifest settings or recipe identity does not match entry metadata";
            return false;
        }

        if (!entry.sourceDependencyClosureRecorded && !entry.sourceDependencies.empty())
        {
            outError = "Cook manifest entry has source dependencies without a source closure marker";
            return false;
        }
        for (const CookFileIdentity& dependency : entry.sourceDependencies)
        {
            if (!dependency.IsValid() || dependency.relativePath == entry.sourcePath)
            {
                outError = "Cook manifest contains invalid source dependency identity";
                return false;
            }
        }

        ResourceContentIdentity sourceIdentity;
        if (!ComputeSourceContentIdentity(entry, sourceIdentity, outError) || !sourceIdentity.IsValid())
        {
            if (outError.empty()) outError = "Cook manifest source closure identity is invalid";
            return false;
        }

        bool hasPrimaryArtifact = false;
        for (const CookFileIdentity& dependency : entry.dependencies)
        {
            if (!dependency.IsValid())
            {
                outError = "Cook manifest contains invalid dependency identity";
                return false;
            }
        }
        for (const CookFileIdentity& artifact : entry.artifacts)
        {
            if (!artifact.IsValid())
            {
                outError = "Cook manifest contains invalid artifact identity";
                return false;
            }
            if (!declaredArtifactPaths.insert(artifact.relativePath).second)
            {
                outError = "Cook manifest assigns one cooked artifact to more than one entry";
                return false;
            }
            hasPrimaryArtifact = hasPrimaryArtifact || artifact.relativePath == entry.outputPath;
            ++actual.artifactCount;
            if (artifact.byteCount > std::numeric_limits<uint64>::max() - actual.artifactByteCount)
            {
                outError = "Cook manifest artifact byte metrics overflow";
                return false;
            }
            actual.artifactByteCount += artifact.byteCount;
        }
        if (!hasPrimaryArtifact)
        {
            outError = "Cook manifest entry does not declare its primary output artifact";
            return false;
        }
        if (entry.dependencies.size() > std::numeric_limits<uint64>::max() - actual.dependencyCount)
        {
            outError = "Cook manifest dependency metrics overflow";
            return false;
        }
        actual.dependencyCount += entry.dependencies.size();
        ++actual.successCount;
    }
    if (actual.successCount > std::numeric_limits<uint64>::max() - actual.failureCount)
    {
        outError = "Cook manifest metrics overflow";
        return false;
    }
    if (actual.entryCount != declaredMetrics.entryCount ||
        actual.successCount != declaredMetrics.successCount ||
        actual.failureCount != declaredMetrics.failureCount)
    {
        outError = "Cook manifest declared counts do not match its entries";
        return false;
    }
    return true;
}

bool ParseCookManifest(const std::string_view text,
                       CookManifest& outManifest,
                       std::string& outError)
{
    outManifest = {};
    outError.clear();
    if (text.empty() || text.find('\0') != std::string_view::npos ||
        text.find('\r') != std::string_view::npos || text.back() != '\n')
    {
        outError = "Cook manifest must be non-empty LF-delimited text without NUL";
        return false;
    }

    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
        {
            outError = "Cook manifest is missing a final line delimiter";
            return false;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (lines.size() < 3 || lines.front() != CookManifest::Header || lines.back() != CookManifest::Footer)
    {
        outError = "Cook manifest has an invalid v2 header or footer";
        return false;
    }

    std::map<std::string, std::string> fields;
    for (size_t lineIndex = 1; lineIndex + 1 < lines.size(); ++lineIndex)
    {
        const std::string_view line = lines[lineIndex];
        const size_t separator = line.find('=');
        if (separator == std::string_view::npos || separator == 0)
        {
            outError = "Cook manifest contains a malformed key/value line";
            return false;
        }
        const std::string key(line.substr(0, separator));
        if (key.find_first_of("\\\r\n\t ") != std::string::npos)
        {
            outError = "Cook manifest contains an invalid key";
            return false;
        }
        std::string value;
        if (!UnescapeManifestValue(line.substr(separator + 1), value, outError))
        {
            return false;
        }
        if (!fields.emplace(key, std::move(value)).second)
        {
            outError = "Cook manifest contains a duplicate key: " + key;
            return false;
        }
    }

    CookManifest parsed;
    std::string value;
    uint64 version = 0;
    if (!Take(fields, "schema", parsed.schema, outError) ||
        !TakeUnsigned(fields, "version", version, outError) || version > std::numeric_limits<uint32>::max() ||
        !Take(fields, "toolName", parsed.toolName, outError) ||
        !Take(fields, "toolVersion", parsed.toolVersion, outError) ||
        !Take(fields, "sourceRoot", parsed.observedSourceRoot, outError) ||
        !Take(fields, "outputRoot", parsed.observedOutputRoot, outError) ||
        !TakeBool(fields, "recursive", parsed.recursive, outError) ||
        !TakeUnsigned(fields, "entryCount", parsed.declaredMetrics.entryCount, outError) ||
        !TakeUnsigned(fields, "successCount", parsed.declaredMetrics.successCount, outError) ||
        !TakeUnsigned(fields, "failureCount", parsed.declaredMetrics.failureCount, outError))
    {
        return false;
    }
    parsed.version = static_cast<uint32>(version);
    if (parsed.declaredMetrics.entryCount > static_cast<uint64>(std::numeric_limits<size_t>::max()))
    {
        outError = "Cook manifest entry count exceeds addressable range";
        return false;
    }

    parsed.entries.reserve(static_cast<size_t>(parsed.declaredMetrics.entryCount));
    for (uint64 entryIndex = 0; entryIndex < parsed.declaredMetrics.entryCount; ++entryIndex)
    {
        const std::string prefix = "entry." + std::to_string(entryIndex) + ".";
        CookManifestEntry entry;
        if (!Take(fields, prefix + "source", entry.sourcePath, outError) ||
            !Take(fields, prefix + "output", entry.outputPath, outError) ||
            !Take(fields, prefix + "type", value, outError) || !ParseAssetType(value, entry.type) ||
            !TakeBool(fields, prefix + "success", entry.success, outError) ||
            !Take(fields, prefix + "importer", entry.importerName, outError) ||
            !TakeUnsigned(fields, prefix + "source.byteCount", entry.sourceContent.byteCount, outError) ||
            !Take(fields, prefix + "source.sha256", entry.sourceContent.sha256, outError) ||
            !Take(fields, prefix + "cookSettings", entry.canonicalCookSettings, outError) ||
            !Take(fields, prefix + "cookSettingsHash", entry.cookSettingsHash, outError) ||
            !Take(fields, prefix + "recipeHash", entry.recipeHash, outError))
        {
            if (outError.empty()) outError = "Cook manifest entry has an unknown asset type";
            return false;
        }
        entry.sourceContent.relativePath = entry.sourcePath;

        // `sourceDependencyCount` was added as an optional v2 extension. Its
        // absence preserves parsing and recipe verification for manifests
        // produced before source closures were recorded.
        const auto sourceDependencyCountField = fields.find(prefix + "sourceDependencyCount");
        if (sourceDependencyCountField != fields.end())
        {
            uint64 sourceDependencyCount = 0;
            if (!TakeUnsigned(fields,
                              prefix + "sourceDependencyCount",
                              sourceDependencyCount,
                              outError) ||
                sourceDependencyCount > static_cast<uint64>(std::numeric_limits<size_t>::max()))
            {
                if (outError.empty()) outError = "Cook manifest source dependency count exceeds addressable range";
                return false;
            }
            entry.sourceDependencyClosureRecorded = true;
            entry.sourceDependencies.reserve(static_cast<size_t>(sourceDependencyCount));
            for (uint64 sourceDependencyIndex = 0;
                 sourceDependencyIndex < sourceDependencyCount;
                 ++sourceDependencyIndex)
            {
                CookFileIdentity dependency;
                if (!ParseFileIdentity(fields,
                                       prefix + "sourceDependency." +
                                           std::to_string(sourceDependencyIndex),
                                       dependency,
                                       outError))
                {
                    return false;
                }
                entry.sourceDependencies.push_back(std::move(dependency));
            }
        }

        uint64 warningCount = 0;
        if (!TakeUnsigned(fields, prefix + "warningCount", warningCount, outError) ||
            warningCount > static_cast<uint64>(std::numeric_limits<size_t>::max()))
        {
            if (outError.empty()) outError = "Cook manifest warning count exceeds addressable range";
            return false;
        }
        entry.warnings.reserve(static_cast<size_t>(warningCount));
        for (uint64 warningIndex = 0; warningIndex < warningCount; ++warningIndex)
        {
            if (!Take(fields, prefix + "warning." + std::to_string(warningIndex), value, outError)) return false;
            entry.warnings.push_back(std::move(value));
        }

        uint64 dependencyCount = 0;
        if (!TakeUnsigned(fields, prefix + "dependencyCount", dependencyCount, outError) ||
            dependencyCount > static_cast<uint64>(std::numeric_limits<size_t>::max()))
        {
            if (outError.empty()) outError = "Cook manifest dependency count exceeds addressable range";
            return false;
        }
        entry.dependencies.reserve(static_cast<size_t>(dependencyCount));
        for (uint64 dependencyIndex = 0; dependencyIndex < dependencyCount; ++dependencyIndex)
        {
            CookFileIdentity dependency;
            if (!ParseFileIdentity(fields, prefix + "dependency." + std::to_string(dependencyIndex), dependency, outError)) return false;
            entry.dependencies.push_back(std::move(dependency));
        }

        uint64 artifactCount = 0;
        if (!TakeUnsigned(fields, prefix + "artifactCount", artifactCount, outError) ||
            artifactCount > static_cast<uint64>(std::numeric_limits<size_t>::max()))
        {
            if (outError.empty()) outError = "Cook manifest artifact count exceeds addressable range";
            return false;
        }
        entry.artifacts.reserve(static_cast<size_t>(artifactCount));
        for (uint64 artifactIndex = 0; artifactIndex < artifactCount; ++artifactIndex)
        {
            CookFileIdentity artifact;
            if (!ParseFileIdentity(fields, prefix + "artifact." + std::to_string(artifactIndex), artifact, outError)) return false;
            entry.artifacts.push_back(std::move(artifact));
        }
        if (!Take(fields, prefix + "error", entry.error, outError)) return false;
        parsed.entries.push_back(std::move(entry));
    }

    if (!fields.empty())
    {
        outError = "Cook manifest contains an unknown or unconsumed key: " + fields.begin()->first;
        return false;
    }
    if (!parsed.IsSemanticallyValid(outError))
    {
        return false;
    }
    if (text.size() > std::numeric_limits<uint64>::max())
    {
        outError = "Cook manifest byte count exceeds identity range";
        return false;
    }
    parsed.manifestContentIdentity = MakeIdentity(ResourceContentIdentityDomain::CookManifest,
                                                   ResourceContentIdentityScope::SelfContainedArtifact,
                                                   ComputeStringSha256(text),
                                                   static_cast<uint64>(text.size()),
                                                   1);
    parsed.observedMetrics = parsed.declaredMetrics;
    parsed.observedMetrics.dependencyCount = 0;
    parsed.observedMetrics.artifactCount = 0;
    parsed.observedMetrics.artifactByteCount = 0;
    for (const CookManifestEntry& entry : parsed.entries)
    {
        if (entry.dependencies.size() > std::numeric_limits<uint64>::max() -
                                        parsed.observedMetrics.dependencyCount)
        {
            outError = "Cook manifest dependency metrics overflow";
            return false;
        }
        parsed.observedMetrics.dependencyCount += entry.dependencies.size();
        for (const CookFileIdentity& artifact : entry.artifacts)
        {
            if (artifact.byteCount > std::numeric_limits<uint64>::max() -
                                         parsed.observedMetrics.artifactByteCount)
            {
                outError = "Cook manifest artifact byte metrics overflow";
                return false;
            }
            ++parsed.observedMetrics.artifactCount;
            parsed.observedMetrics.artifactByteCount += artifact.byteCount;
        }
    }
    outManifest = std::move(parsed);
    return true;
}

bool LoadCookManifest(const fs::path& manifestPath,
                      CookManifest& outManifest,
                      std::string& outError)
{
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input.is_open())
    {
        outError = "Unable to open cook manifest: " + manifestPath.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof())
    {
        outError = "Unable to read cook manifest: " + manifestPath.string();
        return false;
    }
    return ParseCookManifest(buffer.str(), outManifest, outError);
}

bool SaveCookManifest(const fs::path& manifestPath,
                      const CookManifest& manifest,
                      std::string& outError)
{
    if (manifestPath.empty() || !manifest.IsSemanticallyValid(outError))
    {
        if (outError.empty()) outError = "Cook manifest output path is empty";
        return false;
    }
    std::ostringstream output;
    output << CookManifest::Header << '\n';
    output << "schema=" << CookManifest::SchemaName << '\n';
    output << "version=" << RVX_COOK_MANIFEST_SCHEMA_VERSION << '\n';
    output << "toolName=" << EscapeManifestValue(manifest.toolName) << '\n';
    output << "toolVersion=" << EscapeManifestValue(manifest.toolVersion) << '\n';
    // The Resource layer never serializes an authority-bearing machine path.
    output << "sourceRoot=.\n";
    output << "outputRoot=.\n";
    output << "recursive=" << (manifest.recursive ? 1 : 0) << '\n';
    output << "entryCount=" << manifest.declaredMetrics.entryCount << '\n';
    output << "successCount=" << manifest.declaredMetrics.successCount << '\n';
    output << "failureCount=" << manifest.declaredMetrics.failureCount << '\n';
    for (size_t entryIndex = 0; entryIndex < manifest.entries.size(); ++entryIndex)
    {
        const CookManifestEntry& entry = manifest.entries[entryIndex];
        const std::string prefix = "entry." + std::to_string(entryIndex) + ".";
        output << prefix << "source=" << EscapeManifestValue(entry.sourcePath) << '\n';
        output << prefix << "output=" << EscapeManifestValue(entry.outputPath) << '\n';
        output << prefix << "type=" << GetCookAssetTypeName(entry.type) << '\n';
        output << prefix << "success=1\n";
        output << prefix << "importer=" << EscapeManifestValue(entry.importerName) << '\n';
        output << prefix << "source.byteCount=" << entry.sourceContent.byteCount << '\n';
        output << prefix << "source.sha256=" << entry.sourceContent.sha256 << '\n';
        if (entry.sourceDependencyClosureRecorded)
        {
            output << prefix << "sourceDependencyCount=" << entry.sourceDependencies.size() << '\n';
            for (size_t sourceDependencyIndex = 0;
                 sourceDependencyIndex < entry.sourceDependencies.size();
                 ++sourceDependencyIndex)
            {
                const CookFileIdentity& dependency = entry.sourceDependencies[sourceDependencyIndex];
                const std::string dependencyPrefix = prefix + "sourceDependency." +
                    std::to_string(sourceDependencyIndex);
                output << dependencyPrefix << ".path="
                       << EscapeManifestValue(dependency.relativePath) << '\n';
                output << dependencyPrefix << ".byteCount=" << dependency.byteCount << '\n';
                output << dependencyPrefix << ".sha256=" << dependency.sha256 << '\n';
            }
        }
        output << prefix << "cookSettings=" << EscapeManifestValue(entry.canonicalCookSettings) << '\n';
        output << prefix << "cookSettingsHash=" << entry.cookSettingsHash << '\n';
        output << prefix << "recipeHash=" << entry.recipeHash << '\n';
        output << prefix << "warningCount=" << entry.warnings.size() << '\n';
        for (size_t warningIndex = 0; warningIndex < entry.warnings.size(); ++warningIndex)
        {
            output << prefix << "warning." << warningIndex << '=' << EscapeManifestValue(entry.warnings[warningIndex]) << '\n';
        }
        output << prefix << "dependencyCount=" << entry.dependencies.size() << '\n';
        for (size_t dependencyIndex = 0; dependencyIndex < entry.dependencies.size(); ++dependencyIndex)
        {
            const CookFileIdentity& dependency = entry.dependencies[dependencyIndex];
            const std::string dependencyPrefix = prefix + "dependency." + std::to_string(dependencyIndex);
            output << dependencyPrefix << ".path=" << EscapeManifestValue(dependency.relativePath) << '\n';
            output << dependencyPrefix << ".byteCount=" << dependency.byteCount << '\n';
            output << dependencyPrefix << ".sha256=" << dependency.sha256 << '\n';
        }
        output << prefix << "artifactCount=" << entry.artifacts.size() << '\n';
        for (size_t artifactIndex = 0; artifactIndex < entry.artifacts.size(); ++artifactIndex)
        {
            const CookFileIdentity& artifact = entry.artifacts[artifactIndex];
            const std::string artifactPrefix = prefix + "artifact." + std::to_string(artifactIndex);
            output << artifactPrefix << ".path=" << EscapeManifestValue(artifact.relativePath) << '\n';
            output << artifactPrefix << ".byteCount=" << artifact.byteCount << '\n';
            output << artifactPrefix << ".sha256=" << artifact.sha256 << '\n';
        }
        output << prefix << "error=\n";
    }
    output << CookManifest::Footer << '\n';

    std::error_code error;
    if (!manifestPath.parent_path().empty())
    {
        fs::create_directories(manifestPath.parent_path(), error);
        if (error)
        {
            outError = "Unable to create cook manifest directory: " + error.message();
            return false;
        }
    }
    const fs::path temporaryPath = manifestPath.string() + ".tmp";
    {
        std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            outError = "Unable to open temporary cook manifest for writing";
            return false;
        }
        file << output.str();
        if (!file.good())
        {
            outError = "Unable to write temporary cook manifest";
            return false;
        }
    }
    fs::remove(manifestPath, error);
    error.clear();
    fs::rename(temporaryPath, manifestPath, error);
    if (error)
    {
        fs::remove(temporaryPath);
        outError = "Unable to publish cook manifest: " + error.message();
        return false;
    }
    outError.clear();
    return true;
}

bool ComputeCookedContentIdentity(const CookManifest& manifest,
                                  ResourceContentIdentity& outIdentity,
                                  std::string& outError)
{
    if (!manifest.IsSemanticallyValid(outError))
    {
        return false;
    }
    std::string closure = std::string(CookedClosureSchema) + '\n';
    closure += "toolName=" + manifest.toolName + '\n';
    closure += "toolVersion=" + manifest.toolVersion + '\n';
    closure += "entryCount=" + std::to_string(manifest.entries.size()) + '\n';
    uint64 bytes = 0;
    uint64 fileCount = 0;
    for (size_t entryIndex = 0; entryIndex < manifest.entries.size(); ++entryIndex)
    {
        const CookManifestEntry& entry = manifest.entries[entryIndex];
        const std::string prefix = "entry." + std::to_string(entryIndex) + ".";
        closure += prefix + "source=" + entry.sourcePath + '\n';
        closure += prefix + "output=" + entry.outputPath + '\n';
        closure += prefix + "type=" + std::string(GetCookAssetTypeName(entry.type)) + '\n';
        closure += prefix + "importer=" + entry.importerName + '\n';
        closure += prefix + "cookSettingsHash=" + entry.cookSettingsHash + '\n';
        closure += prefix + "recipeHash=" + entry.recipeHash + '\n';
        closure += prefix + "artifactCount=" + std::to_string(entry.artifacts.size()) + '\n';
        for (const CookFileIdentity& artifact : entry.artifacts)
        {
            closure += prefix + "artifact.path=" + artifact.relativePath + '\n';
            closure += prefix + "artifact.byteCount=" + std::to_string(artifact.byteCount) + '\n';
            closure += prefix + "artifact.sha256=" + artifact.sha256 + '\n';
            if (artifact.byteCount > std::numeric_limits<uint64>::max() - bytes ||
                fileCount == std::numeric_limits<uint64>::max())
            {
                outError = "Cooked closure metrics overflow";
                return false;
            }
            bytes += artifact.byteCount;
            ++fileCount;
        }
    }
    if (fileCount == 0 || fileCount > std::numeric_limits<uint32>::max())
    {
        outError = "Cooked closure has an invalid artifact count";
        return false;
    }
    outIdentity = MakeIdentity(ResourceContentIdentityDomain::CookedArtifact,
                               fileCount == 1 ? ResourceContentIdentityScope::SelfContainedArtifact
                                              : ResourceContentIdentityScope::DependencyClosure,
                               ComputeStringSha256(closure),
                               bytes,
                               static_cast<uint32>(fileCount));
    return true;
}

CookManifestAdmissionReceipt VerifyCookedAssetAdmission(const CookManifest& manifest,
                                                        const fs::path& mountedSourceRoot,
                                                        const fs::path& mountedCookedRoot,
                                                        const CookManifestExpectation& expectation)
{
    CookManifestAdmissionReceipt receipt;
    std::string error;
    if (!manifest.IsSemanticallyValid(error))
    {
        receipt.detail = std::move(error);
        return receipt;
    }
    receipt.observedManifestContentIdentity = manifest.manifestContentIdentity;
    if ((!expectation.requiredToolName.empty() && manifest.toolName != expectation.requiredToolName) ||
        (!expectation.requiredToolVersion.empty() && manifest.toolVersion != expectation.requiredToolVersion))
    {
        receipt.code = CookManifestAdmissionCode::ToolMismatch;
        receipt.detail = "Cook manifest tool identity does not satisfy the runtime expectation";
        return receipt;
    }
    if (!expectation.expectedManifestContentIdentity.IsEmpty())
    {
        if (!expectation.expectedManifestContentIdentity.IsValid() ||
            expectation.expectedManifestContentIdentity.domain != ResourceContentIdentityDomain::CookManifest ||
            receipt.observedManifestContentIdentity != expectation.expectedManifestContentIdentity)
        {
            receipt.code = CookManifestAdmissionCode::ManifestIdentityMismatch;
            receipt.detail = "Cook manifest bytes do not satisfy the expected content identity";
            return receipt;
        }
    }

    std::vector<uint32> matches;
    for (uint32 index = 0; index < manifest.entries.size(); ++index)
    {
        if (MatchesSelector(manifest.entries[index], expectation.selector)) matches.push_back(index);
    }
    if (matches.empty())
    {
        receipt.code = CookManifestAdmissionCode::SelectorNotFound;
        receipt.detail = "Cook manifest selector did not match an entry";
        return receipt;
    }
    if (matches.size() != 1)
    {
        receipt.code = CookManifestAdmissionCode::SelectorAmbiguous;
        receipt.detail = "Cook manifest selector matched more than one entry";
        return receipt;
    }
    receipt.selectedEntryIndex = matches.front();
    const CookManifestEntry& selected = manifest.entries[receipt.selectedEntryIndex];
    if (!ComputeSourceContentIdentity(selected, receipt.observedSourceContentIdentity, error))
    {
        receipt.code = CookManifestAdmissionCode::InvalidManifest;
        receipt.detail = std::move(error);
        return receipt;
    }
    if (!expectation.expectedSourceContentIdentity.IsEmpty() &&
        (!expectation.expectedSourceContentIdentity.IsValid() ||
         expectation.expectedSourceContentIdentity.domain != ResourceContentIdentityDomain::Source ||
         receipt.observedSourceContentIdentity != expectation.expectedSourceContentIdentity))
    {
        receipt.code = CookManifestAdmissionCode::SourceIdentityMismatch;
        receipt.detail = "Selected source does not satisfy the expected content identity";
        return receipt;
    }
    if (!expectation.expectedCookSettingsHash.empty() &&
        selected.cookSettingsHash != expectation.expectedCookSettingsHash)
    {
        receipt.code = CookManifestAdmissionCode::CookSettingsMismatch;
        receipt.detail = "Selected entry cook settings hash does not match expectation";
        return receipt;
    }
    if (!expectation.expectedRecipeHash.empty() && selected.recipeHash != expectation.expectedRecipeHash)
    {
        receipt.code = CookManifestAdmissionCode::RecipeMismatch;
        receipt.detail = "Selected entry recipe hash does not match expectation";
        return receipt;
    }
    if (!ComputeCookedContentIdentity(manifest, receipt.observedCookedContentIdentity, error))
    {
        receipt.code = CookManifestAdmissionCode::InvalidManifest;
        receipt.detail = std::move(error);
        return receipt;
    }
    if (!expectation.expectedCookedContentIdentity.IsEmpty() &&
        (!expectation.expectedCookedContentIdentity.IsValid() ||
         expectation.expectedCookedContentIdentity.domain != ResourceContentIdentityDomain::CookedArtifact ||
         receipt.observedCookedContentIdentity != expectation.expectedCookedContentIdentity))
    {
        receipt.code = CookManifestAdmissionCode::CookedIdentityMismatch;
        receipt.detail = "Cooked closure does not satisfy the expected content identity";
        return receipt;
    }

    fs::path sourceRoot;
    fs::path cookedRoot;
    if (!ResolveRoot(mountedSourceRoot, sourceRoot, error) || !ResolveRoot(mountedCookedRoot, cookedRoot, error))
    {
        receipt.code = CookManifestAdmissionCode::MountedContentMismatch;
        receipt.detail = std::move(error);
        return receipt;
    }
    for (const CookManifestEntry& entry : manifest.entries)
    {
        CookFileIdentity actual;
        if (!CaptureMountedIdentity(sourceRoot, entry.sourceContent, actual, error))
        {
            receipt.code = CookManifestAdmissionCode::MountedContentMismatch;
            receipt.detail = std::move(error);
            return receipt;
        }
        for (const CookFileIdentity& dependency : entry.sourceDependencies)
        {
            if (!CaptureMountedIdentity(sourceRoot, dependency, actual, error))
            {
                receipt.code = CookManifestAdmissionCode::MountedContentMismatch;
                receipt.detail = std::move(error);
                return receipt;
            }
        }
        for (const CookFileIdentity& dependency : entry.dependencies)
        {
            if (!CaptureMountedIdentity(cookedRoot, dependency, actual, error))
            {
                receipt.code = CookManifestAdmissionCode::MountedContentMismatch;
                receipt.detail = std::move(error);
                return receipt;
            }
        }
        for (const CookFileIdentity& artifact : entry.artifacts)
        {
            if (!CaptureMountedIdentity(cookedRoot, artifact, actual, error))
            {
                receipt.code = CookManifestAdmissionCode::MountedContentMismatch;
                receipt.detail = std::move(error);
                return receipt;
            }
        }
    }
    receipt.code = CookManifestAdmissionCode::Accepted;
    receipt.detail.clear();
    return receipt;
}
} // namespace RVX::Resource
