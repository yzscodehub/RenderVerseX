#pragma once

/** @file SampleRegistry.h @brief Deterministic registry for sample scenes. */

#include "Samples/Sample.h"
#include "Samples/SampleManifest.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RVX
{
    using SampleFactory = std::function<std::unique_ptr<ISample>()>;

    class SampleRegistry final
    {
    public:
        bool Register(SampleInfo info,
                      SampleScenarioManifest manifest,
                      SampleFactory factory,
                      std::string* outError = nullptr);

        [[nodiscard]] std::unique_ptr<ISample> Create(
            std::string_view id) const;
        [[nodiscard]] const SampleInfo* Find(std::string_view id) const;
        [[nodiscard]] const SampleScenarioManifest* FindManifest(
            std::string_view id) const;
        [[nodiscard]] std::vector<SampleInfo> List() const;
        [[nodiscard]] bool ValidateManifestCoverage(
            const SampleManifestCatalog& catalog,
            std::string* outError = nullptr);
        /**
         * @brief Seal a product registry containing an explicit manifest subset.
         *
         * Every registered scenario must match the full manifest exactly, but
         * unrelated manifest scenarios may remain unregistered in a narrowed
         * cooked product executable.
         */
        [[nodiscard]] bool ValidateManifestSelection(
            const SampleManifestCatalog& catalog,
            std::string* outError = nullptr);
        [[nodiscard]] std::string_view GetManifestContentSha256() const noexcept
        {
            return m_manifestContentSha256;
        }
        [[nodiscard]] uint32 GetManifestSchemaVersion() const noexcept
        {
            return m_manifestSchemaVersion;
        }

    private:
        struct Entry
        {
            SampleInfo info;
            SampleScenarioManifest manifest;
            SampleFactory factory;
        };

        std::map<std::string, Entry, std::less<>> m_entries;
        std::string m_manifestContentSha256;
        uint32 m_manifestSchemaVersion = 0;
    };
} // namespace RVX
