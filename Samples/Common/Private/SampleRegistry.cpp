/** @file SampleRegistry.cpp @brief SampleRegistry implementation. */

#include "Samples/SampleRegistry.h"

#include <algorithm>
#include <set>
#include <utility>

namespace RVX
{
    bool SampleRegistry::Register(SampleInfo info,
                                  SampleScenarioManifest manifest,
                                  SampleFactory factory,
                                  std::string* outError)
    {
        if (!m_manifestContentSha256.empty())
        {
            if (outError)
            {
                *outError =
                    "Sample registry is sealed after manifest coverage validation";
            }
            return false;
        }
        if (info.id.empty())
        {
            if (outError)
            {
                *outError = "Sample id must not be empty";
            }
            return false;
        }
        if (manifest.id != info.id)
        {
            if (outError)
            {
                *outError = "Sample manifest id does not match registry id: " +
                            info.id + " != " + manifest.id;
            }
            return false;
        }
        if (manifest.visualContractVersion == 0)
        {
            if (outError)
            {
                *outError = "Sample visual contract version must be non-zero: " +
                            info.id;
            }
            return false;
        }
        if (!factory)
        {
            if (outError)
            {
                *outError = "Sample factory must not be empty: " + info.id;
            }
            return false;
        }
        if (m_entries.contains(info.id))
        {
            if (outError)
            {
                *outError = "Duplicate sample id: " + info.id;
            }
            return false;
        }

        std::set<std::string, std::less<>> declaredAssets(
            manifest.cookedAssetIds.begin(), manifest.cookedAssetIds.end());
        const auto requireDeclaredAsset =
            [&declaredAssets, &info, outError](const std::string& assetId)
        {
            if (assetId.empty() || declaredAssets.contains(assetId))
            {
                return true;
            }
            if (outError)
            {
                *outError = "Sample manifest omits catalog asset '" + assetId +
                            "' required by " + info.id;
            }
            return false;
        };
        if (!requireDeclaredAsset(info.defaultAssetId) ||
            !requireDeclaredAsset(info.defaultEnvironmentId))
        {
            return false;
        }
        for (const std::string& assetId : info.additionalDefaultAssetIds)
        {
            if (!requireDeclaredAsset(assetId))
            {
                return false;
            }
        }
        for (const std::string& assetId : info.requiredAnimationAssetIds)
        {
            if (!requireDeclaredAsset(assetId))
            {
                return false;
            }
        }
        for (const std::string& assetId : info.requiredCookedAssetIds)
        {
            if (!requireDeclaredAsset(assetId))
            {
                return false;
            }
        }

        info.domain = manifest.domain;
        info.intent = manifest.intent;
        info.visualContractId = manifest.id;
        info.deterministicQualification =
            manifest.deterministicQualification;

        const std::string id = info.id;
        m_entries.emplace(
            id,
            Entry{std::move(info), std::move(manifest), std::move(factory)});
        return true;
    }

    std::unique_ptr<ISample> SampleRegistry::Create(std::string_view id) const
    {
        const auto it = m_entries.find(id);
        return it == m_entries.end() ? nullptr : it->second.factory();
    }

    const SampleInfo* SampleRegistry::Find(std::string_view id) const
    {
        const auto it = m_entries.find(id);
        return it == m_entries.end() ? nullptr : &it->second.info;
    }

    const SampleScenarioManifest* SampleRegistry::FindManifest(
        std::string_view id) const
    {
        const auto it = m_entries.find(id);
        return it == m_entries.end() ? nullptr : &it->second.manifest;
    }

    std::vector<SampleInfo> SampleRegistry::List() const
    {
        std::vector<SampleInfo> result;
        result.reserve(m_entries.size());
        for (const auto& [id, entry] : m_entries)
        {
            static_cast<void>(id);
            result.push_back(entry.info);
        }
        return result;
    }

    bool SampleRegistry::ValidateManifestCoverage(
        const SampleManifestCatalog& catalog,
        std::string* outError)
    {
        const bool validContentHash = catalog.contentSha256.size() == 64u &&
            std::all_of(catalog.contentSha256.begin(),
                        catalog.contentSha256.end(),
                        [](char character)
                        {
                            return (character >= '0' && character <= '9') ||
                                   (character >= 'a' && character <= 'f');
                        });
        if (catalog.schema != RVX_SAMPLE_MANIFEST_SCHEMA ||
            catalog.schemaVersion != RVX_SAMPLE_MANIFEST_SCHEMA_VERSION ||
            !validContentHash)
        {
            if (outError)
            {
                *outError =
                    "Sample manifest identity is incomplete or non-canonical";
            }
            return false;
        }
        if (catalog.samples.size() != m_entries.size())
        {
            if (outError)
            {
                *outError = "Sample manifest/registry count mismatch: manifest=" +
                            std::to_string(catalog.samples.size()) +
                            ", registry=" + std::to_string(m_entries.size());
            }
            return false;
        }
        for (const auto& [id, manifest] : catalog.samples)
        {
            const auto entry = m_entries.find(id);
            if (entry == m_entries.end())
            {
                if (outError)
                {
                    *outError = "Sample manifest has no registered scenario: " + id;
                }
                return false;
            }
            if (entry->second.manifest != manifest)
            {
                if (outError)
                {
                    *outError = "Registered sample manifest changed during registration: " +
                                id;
                }
                return false;
            }
        }
        m_manifestContentSha256 = catalog.contentSha256;
        m_manifestSchemaVersion = catalog.schemaVersion;
        return true;
    }
} // namespace RVX
