/** @file SampleAssetCatalog.cpp @brief Sample asset catalog implementation. */

#include "Samples/SampleAssetCatalog.h"

#include "Core/Hash/SHA256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

namespace RVX
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr uintmax_t MaximumCatalogBytes = 4u * 1024u * 1024u;

        void SetError(std::string* outError, std::string error)
        {
            if (outError)
            {
                *outError = std::move(error);
            }
        }

        bool ReadRequiredString(const Json& object,
                                const char* field,
                                std::string& output,
                                std::string& outError)
        {
            const auto it = object.find(field);
            if (it == object.end() || !it->is_string() || it->empty())
            {
                outError = std::string("Missing or invalid string field: ") + field;
                return false;
            }
            output = it->get<std::string>();
            return true;
        }

        bool IsValidAssetId(const std::string& id)
        {
            if (id.empty())
            {
                return false;
            }
            for (const unsigned char ch : id)
            {
                if (!std::islower(ch) && !std::isdigit(ch) && ch != '-' &&
                    ch != '_' && ch != '.')
                {
                    return false;
                }
            }
            return true;
        }

        bool IsSafeRelativePath(const std::filesystem::path& path)
        {
            if (path.empty() || path.is_absolute() || path.has_root_path())
            {
                return false;
            }
            for (const std::filesystem::path& component : path)
            {
                if (component == "..")
                {
                    return false;
                }
            }
            return true;
        }

        bool IsSamePathComponent(const std::filesystem::path& left,
                                 const std::filesystem::path& right)
        {
#if defined(_WIN32)
            const std::wstring leftText = left.native();
            const std::wstring rightText = right.native();
            return leftText.size() == rightText.size() &&
                   std::equal(leftText.begin(),
                              leftText.end(),
                              rightText.begin(),
                              [](wchar_t leftCharacter, wchar_t rightCharacter)
                              {
                                  return std::towlower(leftCharacter) ==
                                         std::towlower(rightCharacter);
                              });
#else
            return left == right;
#endif
        }

        bool IsContainedPath(const std::filesystem::path& root,
                             const std::filesystem::path& candidate)
        {
            auto rootComponent = root.begin();
            auto candidateComponent = candidate.begin();
            while (rootComponent != root.end())
            {
                if (candidateComponent == candidate.end() ||
                    !IsSamePathComponent(*rootComponent, *candidateComponent))
                {
                    return false;
                }
                ++rootComponent;
                ++candidateComponent;
            }
            return candidateComponent != candidate.end();
        }

        bool ResolveCatalogFile(const std::filesystem::path& root,
                                const std::filesystem::path& relative,
                                const char* label,
                                std::filesystem::path& output,
                                std::string& outError)
        {
            if (!IsSafeRelativePath(relative))
            {
                outError = std::string("Invalid ") + label +
                           " path (must be relative and traversal-free): " +
                           relative.string();
                return false;
            }

            std::error_code error;
            const std::filesystem::path unresolved =
                (root / relative).lexically_normal();
            if (!std::filesystem::is_regular_file(unresolved, error) || error)
            {
                outError = std::string("Missing ") + label + " file: " +
                           unresolved.string();
                return false;
            }

            output = std::filesystem::weakly_canonical(unresolved, error);
            if (error || !IsContainedPath(root, output))
            {
                outError = std::string("Resolved ") + label +
                           " path escapes asset root: " + relative.string() +
                           " (root=" + root.string() +
                           ", resolved=" + output.string() + ")";
                return false;
            }
            return true;
        }

        bool ResolveCatalogDirectory(const std::filesystem::path& root,
                                     const std::filesystem::path& relative,
                                     const char* label,
                                     std::filesystem::path& output,
                                     std::string& outError)
        {
            if (!IsSafeRelativePath(relative))
            {
                outError = std::string("Invalid ") + label +
                           " path (must be relative and traversal-free): " +
                           relative.string();
                return false;
            }

            std::error_code error;
            const std::filesystem::path unresolved =
                (root / relative).lexically_normal();
            if (!std::filesystem::is_directory(unresolved, error) || error)
            {
                outError = std::string("Missing ") + label + " directory: " +
                           unresolved.string();
                return false;
            }

            output = std::filesystem::weakly_canonical(unresolved, error);
            if (error || !IsContainedPath(root, output))
            {
                outError = std::string("Resolved ") + label +
                           " path escapes asset root: " + relative.string() +
                           " (root=" + root.string() +
                           ", resolved=" + output.string() + ")";
                return false;
            }
            return true;
        }

        bool ParseAssetKind(const std::string& text, SampleAssetKind& output)
        {
            if (text == "model")
            {
                output = SampleAssetKind::Model;
                return true;
            }
            if (text == "texture")
            {
                output = SampleAssetKind::Texture;
                return true;
            }
            if (text == "environment")
            {
                output = SampleAssetKind::Environment;
                return true;
            }
            if (text == "animation")
            {
                output = SampleAssetKind::Animation;
                return true;
            }
            return false;
        }

        bool ParseContentIdentity(const Json& object,
                                  Resource::ResourceContentIdentity& output,
                                  std::string& outError)
        {
            if (!object.is_object())
            {
                outError = "Sample asset contentIdentity must be an object";
                return false;
            }

            const auto schemaVersion = object.find("schemaVersion");
            const auto domain = object.find("domain");
            const auto scope = object.find("scope");
            const auto algorithm = object.find("algorithm");
            const auto digest = object.find("digest");
            const auto byteCount = object.find("byteCount");
            const auto fileCount = object.find("fileCount");
            if (schemaVersion == object.end() ||
                !schemaVersion->is_number_unsigned() ||
                domain == object.end() || !domain->is_string() ||
                scope == object.end() || !scope->is_string() ||
                algorithm == object.end() || !algorithm->is_string() ||
                digest == object.end() || !digest->is_string() ||
                byteCount == object.end() || !byteCount->is_number_unsigned() ||
                fileCount == object.end() || !fileCount->is_number_unsigned())
            {
                outError =
                    "Sample asset contentIdentity has missing or invalid fields";
                return false;
            }

            const uint64 parsedSchemaVersion = schemaVersion->get<uint64>();
            const uint64 parsedFileCount = fileCount->get<uint64>();
            if (parsedSchemaVersion > std::numeric_limits<uint32>::max() ||
                parsedFileCount > std::numeric_limits<uint32>::max())
            {
                outError = "Sample asset contentIdentity integer field is out of range";
                return false;
            }
            output.schemaVersion = static_cast<uint32>(parsedSchemaVersion);
            const std::string domainName = domain->get<std::string>();
            const std::string scopeName = scope->get<std::string>();
            const std::string algorithmName = algorithm->get<std::string>();
            if (domainName == "source")
            {
                output.domain = Resource::ResourceContentIdentityDomain::Source;
            }
            else if (domainName == "cooked-artifact")
            {
                output.domain =
                    Resource::ResourceContentIdentityDomain::CookedArtifact;
            }
            else if (domainName == "cook-manifest")
            {
                output.domain =
                    Resource::ResourceContentIdentityDomain::CookManifest;
            }
            else
            {
                output.domain = Resource::ResourceContentIdentityDomain::Unknown;
            }
            if (scopeName == "self-contained-artifact")
            {
                output.scope =
                    Resource::ResourceContentIdentityScope::SelfContainedArtifact;
            }
            else if (scopeName == "dependency-closure")
            {
                output.scope =
                    Resource::ResourceContentIdentityScope::DependencyClosure;
            }
            else
            {
                output.scope = Resource::ResourceContentIdentityScope::Unknown;
            }
            output.algorithm = algorithmName == "sha256"
                                   ? Resource::ResourceContentHashAlgorithm::SHA256
                                   : Resource::ResourceContentHashAlgorithm::None;
            output.digest = digest->get<std::string>();
            output.byteCount = byteCount->get<uint64>();
            output.fileCount = static_cast<uint32>(parsedFileCount);
            if (!output.IsValid())
            {
                outError = "Sample asset contentIdentity is invalid";
                return false;
            }
            return true;
        }

        bool IsLowerSHA256Digest(const std::string& digest);
        bool ParseStrictPosixRelativePath(const std::string& text,
                                          std::filesystem::path& output,
                                          std::string& outError);

        bool ParseCookAssetType(const std::string& text,
                                Resource::CookAssetType& output)
        {
            using Resource::CookAssetType;
            if (text == "Texture") { output = CookAssetType::Texture; return true; }
            if (text == "Mesh") { output = CookAssetType::Mesh; return true; }
            if (text == "Material") { output = CookAssetType::Material; return true; }
            if (text == "Shader") { output = CookAssetType::Shader; return true; }
            if (text == "Animation") { output = CookAssetType::Animation; return true; }
            if (text == "Audio") { output = CookAssetType::Audio; return true; }
            if (text == "Font") { output = CookAssetType::Font; return true; }
            if (text == "Prefab") { output = CookAssetType::Prefab; return true; }
            if (text == "Scene") { output = CookAssetType::Scene; return true; }
            if (text == "Script") { output = CookAssetType::Script; return true; }
            if (text == "Model") { output = CookAssetType::Model; return true; }
            if (text == "Environment") { output = CookAssetType::Environment; return true; }
            return false;
        }

        bool ParseCookSelector(const Json& object,
                               Resource::CookAssetSelector& output,
                               std::string& outError)
        {
            if (!object.is_object())
            {
                outError = "Sample asset cook selector must be an object";
                return false;
            }

            std::string sourcePath;
            std::string outputPath;
            std::string type;
            if (!ReadRequiredString(object, "sourcePath", sourcePath, outError) ||
                !ReadRequiredString(object, "outputPath", outputPath, outError) ||
                !ReadRequiredString(object, "type", type, outError))
            {
                outError = "Sample asset cook selector " + outError;
                return false;
            }

            std::filesystem::path normalizedSource;
            std::filesystem::path normalizedOutput;
            if (!ParseStrictPosixRelativePath(sourcePath, normalizedSource, outError) ||
                !ParseStrictPosixRelativePath(outputPath, normalizedOutput, outError) ||
                !ParseCookAssetType(type, output.type))
            {
                if (outError.empty())
                {
                    outError = "Sample asset cook selector has an unknown type";
                }
                else
                {
                    outError = "Sample asset cook selector " + outError;
                }
                return false;
            }

            output.sourcePath = normalizedSource.generic_string();
            output.outputPath = normalizedOutput.generic_string();
            return true;
        }

        bool ParseCookDeclaration(const Json& asset,
                                  SampleAssetCook& output,
                                  std::string& outError)
        {
            const auto cook = asset.find("cook");
            if (cook == asset.end())
            {
                return true;
            }
            if (!cook->is_object())
            {
                outError = "Sample asset cook must be an object";
                return false;
            }

            std::string manifestPath;
            std::string cookedRoot;
            std::string cookSettingsHash;
            std::string recipeHash;
            if (!ReadRequiredString(*cook, "manifestPath", manifestPath, outError) ||
                !ReadRequiredString(*cook, "cookedRoot", cookedRoot, outError) ||
                !ReadRequiredString(*cook, "cookSettingsHash", cookSettingsHash, outError) ||
                !ReadRequiredString(*cook, "recipeHash", recipeHash, outError))
            {
                outError = "Sample asset cook " + outError;
                return false;
            }
            if (!ParseStrictPosixRelativePath(manifestPath,
                                              output.manifestPath,
                                              outError) ||
                !ParseStrictPosixRelativePath(cookedRoot,
                                              output.cookedRoot,
                                              outError) ||
                !IsLowerSHA256Digest(cookSettingsHash) ||
                !IsLowerSHA256Digest(recipeHash))
            {
                if (outError.empty())
                {
                    outError = "Sample asset cook hashes must be lowercase SHA-256 digests";
                }
                else
                {
                    outError = "Sample asset cook " + outError;
                }
                return false;
            }

            const auto selector = cook->find("selector");
            const auto sourceIdentity = cook->find("sourceContentIdentity");
            const auto cookedIdentity = cook->find("cookedContentIdentity");
            const auto manifestIdentity = cook->find("manifestContentIdentity");
            const auto tool = cook->find("tool");
            if (selector == cook->end() || sourceIdentity == cook->end() ||
                cookedIdentity == cook->end() || manifestIdentity == cook->end() ||
                tool == cook->end() || !tool->is_object() ||
                !ParseCookSelector(*selector, output.selector, outError) ||
                !ParseContentIdentity(*sourceIdentity,
                                      output.sourceContentIdentity,
                                      outError) ||
                !ParseContentIdentity(*cookedIdentity,
                                      output.cookedContentIdentity,
                                      outError) ||
                !ParseContentIdentity(*manifestIdentity,
                                      output.manifestContentIdentity,
                                      outError) ||
                !ReadRequiredString(*tool, "name", output.toolName, outError) ||
                !ReadRequiredString(*tool, "version", output.toolVersion, outError))
            {
                outError = "Sample asset cook declaration is incomplete or invalid: " +
                           outError;
                return false;
            }
            if (output.sourceContentIdentity.domain !=
                    Resource::ResourceContentIdentityDomain::Source ||
                output.cookedContentIdentity.domain !=
                    Resource::ResourceContentIdentityDomain::CookedArtifact ||
                output.manifestContentIdentity.domain !=
                    Resource::ResourceContentIdentityDomain::CookManifest)
            {
                outError = "Sample asset cook identities use incorrect domains";
                return false;
            }

            output.cookSettingsHash = std::move(cookSettingsHash);
            output.recipeHash = std::move(recipeHash);
            output.declared = true;
            return true;
        }

        std::string MakeCanonicalPathKey(const std::filesystem::path& path)
        {
            std::string key = path.generic_string();
#if defined(_WIN32)
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char character)
                           {
                               return static_cast<char>(std::tolower(character));
                           });
#endif
            return key;
        }

        bool IsLowerSHA256Digest(const std::string& digest)
        {
            if (digest.size() != 64)
            {
                return false;
            }
            for (const unsigned char character : digest)
            {
                if (!std::isdigit(character) &&
                    (character < 'a' || character > 'f'))
                {
                    return false;
                }
            }
            return true;
        }

        bool ParseStrictPosixRelativePath(const std::string& text,
                                          std::filesystem::path& output,
                                          std::string& outError)
        {
            if (text.empty() || text.find('\\') != std::string::npos)
            {
                outError = "Path must be a non-empty canonical POSIX path";
                return false;
            }

            const std::filesystem::path candidate(text);
            if (!IsSafeRelativePath(candidate))
            {
                outError = "Path must be relative and traversal-free";
                return false;
            }
            for (const std::filesystem::path& component : candidate)
            {
                if (component == "." || component == "..")
                {
                    outError = "Path must not contain '.' or '..' components";
                    return false;
                }
            }

            const std::string normalized = candidate.lexically_normal().generic_string();
            if (normalized != text || candidate.generic_string() != text)
            {
                outError = "Path must use canonical POSIX separators and ordering";
                return false;
            }
            output = candidate;
            return true;
        }

        bool HashFileStreaming(const std::filesystem::path& path,
                               uint64& outByteCount,
                               std::string& outDigest,
                               std::string& outError)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                outError = "Failed to open asset package file for hashing: " +
                           path.string();
                return false;
            }

            Hash::SHA256Hasher hasher;
            std::array<char, 64u * 1024u> buffer{};
            uint64 byteCount = 0;
            while (stream)
            {
                stream.read(buffer.data(),
                            static_cast<std::streamsize>(buffer.size()));
                const std::streamsize readCount = stream.gcount();
                if (readCount > 0)
                {
                    const uint64 count = static_cast<uint64>(readCount);
                    if (byteCount > std::numeric_limits<uint64>::max() - count)
                    {
                        outError = "Asset package file is too large to hash: " +
                                   path.string();
                        return false;
                    }
                    hasher.Update(buffer.data(), static_cast<size_t>(readCount));
                    byteCount += count;
                }
            }
            if (!stream.eof())
            {
                outError = "Failed while hashing asset package file: " +
                           path.string();
                return false;
            }

            outByteCount = byteCount;
            outDigest = hasher.FinalizeHex();
            return true;
        }

        bool ParseAssetContentId(const Json& object,
                                 AssetContentId& output,
                                 std::string& outError)
        {
            if (!object.is_object())
            {
                outError = "Sample asset assetContentId must be an object";
                return false;
            }

            const auto schemaVersion = object.find("schemaVersion");
            const auto algorithm = object.find("algorithm");
            const auto digest = object.find("digest");
            const auto byteCount = object.find("byteCount");
            const auto fileCount = object.find("fileCount");
            if (schemaVersion == object.end() || !schemaVersion->is_number_unsigned() ||
                algorithm == object.end() || !algorithm->is_string() ||
                digest == object.end() || !digest->is_string() ||
                byteCount == object.end() || !byteCount->is_number_unsigned() ||
                fileCount == object.end() || !fileCount->is_number_unsigned())
            {
                outError = "Sample asset assetContentId has missing or invalid fields";
                return false;
            }

            const uint64 parsedSchemaVersion = schemaVersion->get<uint64>();
            const uint64 parsedFileCount = fileCount->get<uint64>();
            if (parsedSchemaVersion > std::numeric_limits<uint32>::max() ||
                parsedFileCount > std::numeric_limits<uint32>::max())
            {
                outError = "Sample asset assetContentId integer field is out of range";
                return false;
            }

            output.schemaVersion = static_cast<uint32>(parsedSchemaVersion);
            output.algorithm = algorithm->get<std::string>();
            output.digest = digest->get<std::string>();
            output.byteCount = byteCount->get<uint64>();
            output.fileCount = static_cast<uint32>(parsedFileCount);
            if (!output.IsValid())
            {
                outError = "Sample asset assetContentId is invalid";
                return false;
            }
            return true;
        }

        bool DecodePercentEncodedUri(const std::string& uri,
                                     std::string& output,
                                     std::string& outError)
        {
            const auto hexValue = [](unsigned char character) -> int
            {
                if (character >= '0' && character <= '9')
                {
                    return character - '0';
                }
                if (character >= 'a' && character <= 'f')
                {
                    return character - 'a' + 10;
                }
                if (character >= 'A' && character <= 'F')
                {
                    return character - 'A' + 10;
                }
                return -1;
            };

            output.clear();
            output.reserve(uri.size());
            for (size_t index = 0; index < uri.size(); ++index)
            {
                if (uri[index] != '%')
                {
                    output.push_back(uri[index]);
                    continue;
                }
                if (index + 2 >= uri.size())
                {
                    outError = "glTF dependency URI has incomplete percent encoding";
                    return false;
                }
                const int high = hexValue(static_cast<unsigned char>(uri[index + 1]));
                const int low = hexValue(static_cast<unsigned char>(uri[index + 2]));
                if (high < 0 || low < 0)
                {
                    outError = "glTF dependency URI has invalid percent encoding";
                    return false;
                }
                const char decoded = static_cast<char>((high << 4) | low);
                if (decoded == '\0')
                {
                    outError = "glTF dependency URI contains a NUL character";
                    return false;
                }
                output.push_back(decoded);
                index += 2;
            }
            return true;
        }

        bool HasUriScheme(const std::string& uri)
        {
            if (uri.empty() || !std::isalpha(static_cast<unsigned char>(uri.front())))
            {
                return false;
            }
            for (size_t index = 1; index < uri.size(); ++index)
            {
                const unsigned char character = static_cast<unsigned char>(uri[index]);
                if (uri[index] == ':')
                {
                    return true;
                }
                if (!std::isalnum(character) && uri[index] != '+' &&
                    uri[index] != '-' && uri[index] != '.')
                {
                    return false;
                }
            }
            return false;
        }

        bool StartsWithDataUri(const std::string& uri)
        {
            constexpr std::string_view DataPrefix = "data:";
            if (uri.size() < DataPrefix.size())
            {
                return false;
            }
            for (size_t index = 0; index < DataPrefix.size(); ++index)
            {
                if (std::tolower(static_cast<unsigned char>(uri[index])) !=
                    DataPrefix[index])
                {
                    return false;
                }
            }
            return true;
        }

        bool IsGltfPath(const std::filesystem::path& path)
        {
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char character)
                           {
                               return static_cast<char>(std::tolower(character));
                           });
            return extension == ".gltf";
        }

        bool ParseGltfLocalUri(const std::string& uri,
                               std::string& output,
                               std::string& outError)
        {
            if (uri.empty() || uri.front() == '/' || uri.front() == '\\' ||
                uri.rfind("//", 0) == 0 || uri.find('\\') != std::string::npos ||
                uri.find('?') != std::string::npos || uri.find('#') != std::string::npos ||
                HasUriScheme(uri))
            {
                outError = "glTF dependency URI must be a local relative path";
                return false;
            }

            std::string decoded;
            if (!DecodePercentEncodedUri(uri, decoded, outError))
            {
                return false;
            }
            if (decoded.empty() || decoded.front() == '/' || decoded.front() == '\\' ||
                decoded.rfind("//", 0) == 0 ||
                decoded.find('\\') != std::string::npos || HasUriScheme(decoded))
            {
                outError = "glTF dependency URI escapes the local asset package";
                return false;
            }

            std::filesystem::path parsed;
            if (!ParseStrictPosixRelativePath(decoded, parsed, outError))
            {
                outError = "glTF dependency URI is not a canonical local path: " +
                           outError;
                return false;
            }
            output = parsed.generic_string();
            return true;
        }

        bool ValidateGltfDependencies(const SampleAssetEntry& entry,
                                      std::string& outError)
        {
            std::ifstream stream(entry.resolvedPath, std::ios::binary);
            const Json document = Json::parse(stream, nullptr, false);
            if (!stream || document.is_discarded() || !document.is_object())
            {
                outError = "Schema-v3 glTF root is not valid JSON";
                return false;
            }

            std::set<std::string, std::less<>> listedFiles;
            for (const SampleAssetFile& file : entry.files)
            {
                listedFiles.insert(file.path.generic_string());
            }

            const auto validateCollection = [&document,
                                             &entry,
                                             &listedFiles,
                                             &outError](const char* collection) -> bool
            {
                const auto entries = document.find(collection);
                if (entries == document.end())
                {
                    return true;
                }
                if (!entries->is_array())
                {
                    outError = std::string("glTF ") + collection +
                               " must be an array";
                    return false;
                }
                for (const Json& value : *entries)
                {
                    if (!value.is_object())
                    {
                        outError = std::string("glTF ") + collection +
                                   " entry must be an object";
                        return false;
                    }
                    const auto uri = value.find("uri");
                    if (uri == value.end())
                    {
                        continue;
                    }
                    if (!uri->is_string())
                    {
                        outError = std::string("glTF ") + collection +
                                   " URI must be a string";
                        return false;
                    }
                    const std::string uriText = uri->get<std::string>();
                    if (StartsWithDataUri(uriText))
                    {
                        continue;
                    }

                    std::string localPath;
                    if (!ParseGltfLocalUri(uriText, localPath, outError))
                    {
                        return false;
                    }
                    const std::string packagePath =
                        (entry.path.parent_path() / localPath).generic_string();
                    if (!listedFiles.contains(packagePath))
                    {
                        outError = "glTF dependency is not listed in files[]: " +
                                   packagePath;
                        return false;
                    }
                }
                return true;
            };

            return validateCollection("buffers") && validateCollection("images");
        }

        bool ParseAndVerifyV3Manifest(const Json& asset,
                                      SampleAssetEntry& entry,
                                      const std::filesystem::path& assetRoot,
                                      std::string& outError)
        {
            const auto assetContentId = asset.find("assetContentId");
            const auto files = asset.find("files");
            if (assetContentId == asset.end() || files == asset.end() ||
                !files->is_array() || files->empty())
            {
                outError = "Schema-v3 asset requires assetContentId and non-empty files[]";
                return false;
            }
            if (!ParseAssetContentId(*assetContentId, entry.assetContentId, outError))
            {
                return false;
            }

            Hash::SHA256Hasher aggregateHasher;
            std::string previousPath;
            bool rootListed = false;
            uint64 aggregateByteCount = 0;
            std::set<std::string, std::less<>> canonicalFilePaths;
            entry.files.clear();
            entry.files.reserve(files->size());
            for (const Json& file : *files)
            {
                if (!file.is_object())
                {
                    outError = "Schema-v3 asset files[] entry must be an object";
                    return false;
                }

                SampleAssetFile parsedFile;
                std::string pathText;
                if (!ReadRequiredString(file, "path", pathText, outError) ||
                    !ReadRequiredString(file, "purpose", parsedFile.purpose, outError) ||
                    !ReadRequiredString(file, "sha256", parsedFile.sha256, outError))
                {
                    outError = "Schema-v3 files[] " + outError;
                    return false;
                }
                const auto byteCount = file.find("byteCount");
                if (byteCount == file.end() || !byteCount->is_number_unsigned())
                {
                    outError = "Schema-v3 files[] byteCount must be an unsigned integer";
                    return false;
                }
                if (!IsLowerSHA256Digest(parsedFile.sha256))
                {
                    outError = "Schema-v3 files[] sha256 must be a lowercase SHA-256 digest";
                    return false;
                }
                if (!ParseStrictPosixRelativePath(pathText, parsedFile.path, outError))
                {
                    outError = "Schema-v3 files[] path is invalid: " + outError;
                    return false;
                }

                const std::string canonicalPath = parsedFile.path.generic_string();
                if (!previousPath.empty() && canonicalPath <= previousPath)
                {
                    outError = "Schema-v3 files[] paths must be strictly increasing canonical POSIX paths";
                    return false;
                }
                previousPath = canonicalPath;
                if (!ResolveCatalogFile(assetRoot,
                                        parsedFile.path,
                                        "asset package",
                                        parsedFile.resolvedPath,
                                        outError))
                {
                    return false;
                }
                if (!canonicalFilePaths.emplace(
                        MakeCanonicalPathKey(parsedFile.resolvedPath)).second)
                {
                    outError = "Schema-v3 files[] contains duplicate canonical paths";
                    return false;
                }

                uint64 observedByteCount = 0;
                std::string observedDigest;
                if (!HashFileStreaming(parsedFile.resolvedPath,
                                       observedByteCount,
                                       observedDigest,
                                       outError))
                {
                    return false;
                }
                parsedFile.byteCount = byteCount->get<uint64>();
                if (parsedFile.byteCount != observedByteCount ||
                    parsedFile.sha256 != observedDigest)
                {
                    outError = "Schema-v3 asset package file hash or byte count drift: " +
                               canonicalPath;
                    return false;
                }
                if (aggregateByteCount >
                    std::numeric_limits<uint64>::max() - parsedFile.byteCount)
                {
                    outError = "Schema-v3 asset package byte count overflows uint64";
                    return false;
                }
                aggregateByteCount += parsedFile.byteCount;
                aggregateHasher.Update(parsedFile.sha256);
                aggregateHasher.Update(" ");
                aggregateHasher.Update(std::to_string(parsedFile.byteCount));
                aggregateHasher.Update(" ");
                aggregateHasher.Update(canonicalPath);
                aggregateHasher.Update("\n");
                rootListed = rootListed || parsedFile.path == entry.path;
                entry.files.push_back(std::move(parsedFile));
            }

            if (!rootListed)
            {
                outError = "Schema-v3 files[] must list the entry path";
                return false;
            }
            if (entry.assetContentId.fileCount != entry.files.size() ||
                entry.assetContentId.byteCount != aggregateByteCount ||
                entry.assetContentId.digest != aggregateHasher.FinalizeHex())
            {
                outError = "Schema-v3 assetContentId does not match the verified files[] package";
                return false;
            }
            if (IsGltfPath(entry.path) &&
                !ValidateGltfDependencies(entry, outError))
            {
                return false;
            }
            return true;
        }
    } // namespace

    bool AssetContentId::IsEmpty() const noexcept
    {
        return schemaVersion == 0 && algorithm.empty() && digest.empty() &&
               byteCount == 0 && fileCount == 0;
    }

    bool AssetContentId::IsValid() const noexcept
    {
        return schemaVersion == RVX_SAMPLE_ASSET_CONTENT_ID_SCHEMA_VERSION &&
               algorithm == "sha256" && IsLowerSHA256Digest(digest) &&
               fileCount > 0;
    }

    const char* GetSampleAssetKindName(SampleAssetKind kind) noexcept
    {
        switch (kind)
        {
            case SampleAssetKind::Model: return "model";
            case SampleAssetKind::Texture: return "texture";
            case SampleAssetKind::Environment: return "environment";
            case SampleAssetKind::Animation: return "animation";
            default: return "unknown";
        }
    }

    bool SampleAssetCatalog::Load(const std::filesystem::path& catalogPath,
                                  const std::filesystem::path& assetRoot,
                                  std::string* outError)
    {
        m_catalogPath.clear();
        m_assetRoot.clear();
        m_entries.clear();

        std::error_code error;
        m_catalogPath = std::filesystem::weakly_canonical(catalogPath, error);
        if (error || !std::filesystem::is_regular_file(m_catalogPath, error) || error)
        {
            SetError(outError, "Sample asset catalog does not exist: " +
                                   catalogPath.string());
            return false;
        }
        const uintmax_t catalogBytes =
            std::filesystem::file_size(m_catalogPath, error);
        if (error || catalogBytes == 0 || catalogBytes > MaximumCatalogBytes)
        {
            SetError(outError, "Sample asset catalog has an invalid size: " +
                                   m_catalogPath.string());
            return false;
        }

        const std::filesystem::path requestedRoot =
            assetRoot.empty() ? m_catalogPath.parent_path() : assetRoot;
        m_assetRoot = std::filesystem::weakly_canonical(requestedRoot, error);
        if (error || !std::filesystem::is_directory(m_assetRoot, error) || error)
        {
            SetError(outError, "Sample asset root does not exist: " +
                                   requestedRoot.string());
            return false;
        }

        std::ifstream stream(m_catalogPath, std::ios::binary);
        const Json document = Json::parse(stream, nullptr, false);
        if (!stream || document.is_discarded() || !document.is_object())
        {
            SetError(outError, "Sample asset catalog is not valid JSON: " +
                                   m_catalogPath.string());
            return false;
        }

        const auto schema = document.find("schemaId");
        const auto version = document.find("schemaVersion");
        const auto assets = document.find("assets");
        if (schema == document.end() || !schema->is_string() ||
            schema->get<std::string>() != RVX_SAMPLE_ASSET_CATALOG_SCHEMA_ID ||
            version == document.end() || !version->is_number_unsigned() ||
            (version->get<uint64>() != 1 &&
             version->get<uint64>() != 2 &&
             version->get<uint64>() != RVX_SAMPLE_ASSET_CATALOG_SCHEMA_VERSION) ||
            assets == document.end() || !assets->is_array() || assets->empty())
        {
            SetError(outError,
                     "Sample asset catalog schema/version/assets contract is invalid");
            return false;
        }

        const uint32 catalogSchemaVersion =
            static_cast<uint32>(version->get<uint64>());
        std::map<std::string, std::string, std::less<>> canonicalPathOwners;
        for (const Json& asset : *assets)
        {
            if (!asset.is_object())
            {
                SetError(outError, "Sample asset entry must be an object");
                return false;
            }

            SampleAssetEntry entry;
            std::string kind;
            std::string path;
            std::string licenseFile;
            std::string parseError;
            if (!ReadRequiredString(asset, "id", entry.id, parseError) ||
                !ReadRequiredString(asset, "kind", kind, parseError) ||
                !ReadRequiredString(asset, "path", path, parseError))
            {
                SetError(outError, parseError);
                return false;
            }
            if (!IsValidAssetId(entry.id) || !ParseAssetKind(kind, entry.kind))
            {
                SetError(outError, "Invalid sample asset id or kind: " + entry.id);
                return false;
            }
            if (m_entries.contains(entry.id))
            {
                SetError(outError, "Duplicate sample asset id: " + entry.id);
                return false;
            }

            const auto license = asset.find("license");
            const auto source = asset.find("source");
            const auto redistributable = asset.find("redistributable");
            if (license == asset.end() || !license->is_object() ||
                source == asset.end() || !source->is_object() ||
                redistributable == asset.end() || !redistributable->is_boolean())
            {
                SetError(outError,
                         "Sample asset requires license, source, and redistributable metadata: " +
                             entry.id);
                return false;
            }
            if (!ReadRequiredString(*license,
                                    "spdxId",
                                    entry.license.spdxId,
                                    parseError) ||
                !ReadRequiredString(*license,
                                    "file",
                                    licenseFile,
                                    parseError) ||
                !ReadRequiredString(*source,
                                    "name",
                                    entry.source.name,
                                    parseError) ||
                !ReadRequiredString(*source,
                                    "uri",
                                    entry.source.uri,
                                    parseError) ||
                !ReadRequiredString(*source,
                                    "author",
                                    entry.source.author,
                                    parseError))
            {
                SetError(outError, parseError + " for asset " + entry.id);
                return false;
            }

            if (catalogSchemaVersion >= 3)
            {
                if (!ReadRequiredString(*license,
                                        "coverage",
                                        entry.license.coverage,
                                        parseError) ||
                    !ReadRequiredString(*source,
                                        "version",
                                        entry.source.version,
                                        parseError))
                {
                    SetError(outError, parseError + " for asset " + entry.id);
                    return false;
                }

                const auto archiveSha256 = source->find("archiveSha256");
                if (archiveSha256 != source->end())
                {
                    if (!archiveSha256->is_string() ||
                        !IsLowerSHA256Digest(archiveSha256->get<std::string>()))
                    {
                        SetError(outError,
                                 "Sample asset source archiveSha256 must be a lowercase "
                                 "SHA-256 digest: " + entry.id);
                        return false;
                    }
                    entry.source.archiveSha256 = archiveSha256->get<std::string>();
                }
            }

            entry.redistributable = redistributable->get<bool>();
            const auto attribution = asset.find("attribution");
            if (catalogSchemaVersion >= 3)
            {
                if (attribution == asset.end() || !attribution->is_string() ||
                    attribution->get<std::string>().empty())
                {
                    SetError(outError,
                             "Sample asset attribution must be a non-empty string: " +
                             entry.id);
                    return false;
                }
                entry.attribution = attribution->get<std::string>();
            }
            else if (attribution != asset.end())
            {
                if (!attribution->is_string())
                {
                    SetError(outError,
                             "Sample asset attribution must be a string: " +
                             entry.id);
                    return false;
                }
                entry.attribution = attribution->get<std::string>();
            }

            const auto modificationNotice = asset.find("modificationNotice");
            if (catalogSchemaVersion >= 3)
            {
                if (modificationNotice == asset.end() ||
                    !modificationNotice->is_string() ||
                    modificationNotice->get<std::string>().empty())
                {
                    SetError(outError,
                             "Sample asset modificationNotice must be a non-empty string: " +
                                 entry.id);
                    return false;
                }
                entry.modificationNotice = modificationNotice->get<std::string>();
            }
            else if (modificationNotice != asset.end())
            {
                if (!modificationNotice->is_string())
                {
                    SetError(outError,
                             "Sample asset modificationNotice must be a string: " +
                                 entry.id);
                    return false;
                }
                entry.modificationNotice = modificationNotice->get<std::string>();
            }

            if (catalogSchemaVersion >= 3)
            {
                const auto derivedFrom = asset.find("derivedFrom");
                const auto generation = asset.find("generation");
                if (derivedFrom != asset.end() || generation != asset.end())
                {
                    if (derivedFrom == asset.end() || generation == asset.end() ||
                        !derivedFrom->is_array() || derivedFrom->empty() ||
                        !generation->is_object())
                    {
                        SetError(outError,
                                 "Sample asset derivedFrom and generation must be complete: " +
                                     entry.id);
                        return false;
                    }
                    for (const Json& parent : *derivedFrom)
                    {
                        if (!parent.is_string() || parent.get<std::string>().empty())
                        {
                            SetError(outError,
                                     "Sample asset derivedFrom must contain non-empty strings: " +
                                         entry.id);
                            return false;
                        }
                        entry.derivedFrom.push_back(parent.get<std::string>());
                    }
                    if (!ReadRequiredString(*generation,
                                            "recipe",
                                            entry.generation.recipe,
                                            parseError) ||
                        !ReadRequiredString(*generation,
                                            "toolVersion",
                                            entry.generation.toolVersion,
                                            parseError) ||
                        !ReadRequiredString(*generation,
                                            "recipeHash",
                                            entry.generation.recipeHash,
                                            parseError) ||
                        !IsLowerSHA256Digest(entry.generation.recipeHash))
                    {
                        SetError(outError,
                                 "Sample asset generation metadata is incomplete or invalid: " +
                                     entry.id);
                        return false;
                    }
                }
            }

            if (catalogSchemaVersion >= 3)
            {
                if (!ParseStrictPosixRelativePath(path, entry.path, parseError) ||
                    !ParseStrictPosixRelativePath(licenseFile,
                                                  entry.license.file,
                                                  parseError))
                {
                    SetError(outError, "Schema-v3 " + parseError + " for asset " + entry.id);
                    return false;
                }
            }
            else
            {
                entry.path = std::filesystem::path(path).lexically_normal();
                entry.license.file =
                    std::filesystem::path(licenseFile).lexically_normal();
            }
            if (!ResolveCatalogFile(m_assetRoot,
                                    entry.path,
                                    "asset",
                                    entry.resolvedPath,
                                    parseError) ||
                !ResolveCatalogFile(m_assetRoot,
                                    entry.license.file,
                                    "license",
                                    entry.license.resolvedFile,
                                    parseError))
            {
                SetError(outError, parseError + " for asset " + entry.id);
                return false;
            }

            const auto contentIdentity = asset.find("contentIdentity");
            if (contentIdentity != asset.end())
            {
                if (catalogSchemaVersion < 2)
                {
                    SetError(outError,
                             "Sample asset contentIdentity requires catalog schema v2: " +
                                 entry.id);
                    return false;
                }
                if (!ParseContentIdentity(*contentIdentity,
                                          entry.contentIdentity,
                                          parseError))
                {
                    SetError(outError, parseError + " for asset " + entry.id);
                    return false;
                }
            }

            if (catalogSchemaVersion >= 3 &&
                !ParseAndVerifyV3Manifest(asset,
                                          entry,
                                          m_assetRoot,
                                          parseError))
            {
                SetError(outError, parseError + " for asset " + entry.id);
                return false;
            }

            const auto cook = asset.find("cook");
            if (cook != asset.end() && catalogSchemaVersion < 3)
            {
                SetError(outError,
                         "Sample asset cook requires catalog schema v3: " +
                             entry.id);
                return false;
            }
            if (catalogSchemaVersion >= 3 &&
                !ParseCookDeclaration(asset, entry.cook, parseError))
            {
                SetError(outError, parseError + " for asset " + entry.id);
                return false;
            }

            const std::string canonicalPathKey =
                MakeCanonicalPathKey(entry.resolvedPath);
            const auto existingPath = canonicalPathOwners.find(canonicalPathKey);
            if (existingPath != canonicalPathOwners.end() &&
                existingPath->second != entry.id)
            {
                SetError(outError,
                         "Duplicate canonical sample asset path for ids '" +
                             existingPath->second + "' and '" + entry.id + "'");
                return false;
            }
            canonicalPathOwners.emplace(canonicalPathKey, entry.id);

            m_entries.emplace(entry.id, std::move(entry));
        }

        return true;
    }

    const SampleAssetEntry* SampleAssetCatalog::Find(std::string_view id) const
    {
        const auto it = m_entries.find(id);
        return it == m_entries.end() ? nullptr : &it->second;
    }

    std::vector<SampleAssetEntry> SampleAssetCatalog::List() const
    {
        std::vector<SampleAssetEntry> result;
        result.reserve(m_entries.size());
        for (const auto& [id, entry] : m_entries)
        {
            static_cast<void>(id);
            result.push_back(entry);
        }
        return result;
    }

    const char* GetSampleCookedAssetAdmissionCodeName(
        SampleCookedAssetAdmissionCode code) noexcept
    {
        switch (code)
        {
            case SampleCookedAssetAdmissionCode::Accepted: return "accepted";
            case SampleCookedAssetAdmissionCode::AssetNotFound: return "asset-not-found";
            case SampleCookedAssetAdmissionCode::CookDataMissing: return "cook-data-missing";
            case SampleCookedAssetAdmissionCode::CookDataInvalid: return "cook-data-invalid";
            case SampleCookedAssetAdmissionCode::ManifestLoadFailed: return "manifest-load-failed";
            case SampleCookedAssetAdmissionCode::ResourceAdmissionFailed:
                return "resource-admission-failed";
            default: return "unknown";
        }
    }

    SampleCookedAssetAdmissionReceipt
    SampleAssetCatalog::RequireCookedAdmission(std::string_view id) const
    {
        SampleCookedAssetAdmissionReceipt result;
        result.assetId = std::string(id);

        const SampleAssetEntry* entry = Find(id);
        if (!entry)
        {
            result.code = SampleCookedAssetAdmissionCode::AssetNotFound;
            result.detail = "Catalog does not contain the requested asset id";
            return result;
        }
        if (!entry->cook.declared)
        {
            result.code = SampleCookedAssetAdmissionCode::CookDataMissing;
            result.detail =
                "Catalog asset has no cook declaration and cannot be admitted for qualification";
            return result;
        }

        std::string error;
        std::filesystem::path manifestPath;
        std::filesystem::path cookedRoot;
        if (!ResolveCatalogFile(m_assetRoot,
                                entry->cook.manifestPath,
                                "cook manifest",
                                manifestPath,
                                error) ||
            !ResolveCatalogDirectory(m_assetRoot,
                                     entry->cook.cookedRoot,
                                     "cooked asset root",
                                     cookedRoot,
                                     error))
        {
            result.code = SampleCookedAssetAdmissionCode::CookDataInvalid;
            result.detail = std::move(error);
            return result;
        }

        Resource::CookManifest manifest;
        if (!Resource::LoadCookManifest(manifestPath, manifest, error))
        {
            result.code = SampleCookedAssetAdmissionCode::ManifestLoadFailed;
            result.detail = std::move(error);
            return result;
        }

        Resource::CookManifestExpectation expectation;
        expectation.selector = entry->cook.selector;
        expectation.requiredToolName = entry->cook.toolName;
        expectation.requiredToolVersion = entry->cook.toolVersion;
        expectation.expectedSourceContentIdentity =
            entry->cook.sourceContentIdentity;
        expectation.expectedCookedContentIdentity =
            entry->cook.cookedContentIdentity;
        expectation.expectedManifestContentIdentity =
            entry->cook.manifestContentIdentity;
        expectation.expectedCookSettingsHash = entry->cook.cookSettingsHash;
        expectation.expectedRecipeHash = entry->cook.recipeHash;
        result.resourceReceipt = Resource::VerifyCookedAssetAdmission(
            manifest,
            m_assetRoot,
            cookedRoot,
            expectation);
        if (!result.resourceReceipt.IsAccepted())
        {
            result.code = SampleCookedAssetAdmissionCode::ResourceAdmissionFailed;
            result.detail = result.resourceReceipt.detail;
            return result;
        }

        result.code = SampleCookedAssetAdmissionCode::Accepted;
        result.detail.clear();
        return result;
    }
} // namespace RVX
