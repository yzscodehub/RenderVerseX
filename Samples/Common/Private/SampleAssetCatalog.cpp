/** @file SampleAssetCatalog.cpp @brief Sample asset catalog implementation. */

#include "Samples/SampleAssetCatalog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
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
            return false;
        }
    } // namespace

    const char* GetSampleAssetKindName(SampleAssetKind kind) noexcept
    {
        switch (kind)
        {
            case SampleAssetKind::Model: return "model";
            case SampleAssetKind::Texture: return "texture";
            case SampleAssetKind::Environment: return "environment";
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
            version->get<uint32>() != RVX_SAMPLE_ASSET_CATALOG_SCHEMA_VERSION ||
            assets == document.end() || !assets->is_array() || assets->empty())
        {
            SetError(outError,
                     "Sample asset catalog schema/version/assets contract is invalid");
            return false;
        }

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

            entry.redistributable = redistributable->get<bool>();
            const auto attribution = asset.find("attribution");
            if (attribution != asset.end())
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

            entry.path = std::filesystem::path(path).lexically_normal();
            entry.license.file =
                std::filesystem::path(licenseFile).lexically_normal();
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
} // namespace RVX
