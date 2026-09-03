/**
 * @file SampleCookedProductCatalogWriter.cpp
 * @brief Emit a selected, package-only Sample catalog for a cooked product.
 */

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    [[nodiscard]] bool IsCookedRelativePath(const nlohmann::json& value)
    {
        if (!value.is_string())
        {
            return false;
        }
        const std::filesystem::path path(value.get<std::string>());
        if (path.empty() || path.is_absolute() || path.has_root_path())
        {
            return false;
        }
        const std::string normalized = path.lexically_normal().generic_string();
        return normalized.starts_with("cooked/") &&
               normalized.find("..") == std::string::npos;
    }

    [[nodiscard]] bool IsAdmittedCookedEntry(
        const nlohmann::json& asset,
        std::string& outId,
        std::string& outError)
    {
        if (!asset.is_object() || !asset.contains("id") ||
            !asset.at("id").is_string())
        {
            outError = "Cooked product catalog entry is missing string id.";
            return false;
        }
        outId = asset.at("id").get<std::string>();
        if (!asset.contains("path") || !IsCookedRelativePath(asset.at("path")) ||
            !asset.contains("cook") || !asset.at("cook").is_object())
        {
            outError = "Cooked product entry '" + outId +
                "' lacks a package-relative cooked path or cook declaration.";
            return false;
        }
        const nlohmann::json& cook = asset.at("cook");
        // `declared` is a parsed SampleAssetCatalog state derived from the
        // presence of this cook object; it is not a serialized catalog field.
        // The package-only bit plus canonical CookManifest/root paths are the
        // machine-readable admission boundary in catalog schema v4.
        if (!cook.value("packageOnly", false) ||
            !cook.contains("manifestPath") || !
                IsCookedRelativePath(cook.at("manifestPath")) ||
            !cook.contains("cookedRoot") ||
                !IsCookedRelativePath(cook.at("cookedRoot")))
        {
            outError = "Cooked product entry '" + outId +
                "' is not an admitted package-only CookManifest product.";
            return false;
        }
        if (!asset.contains("usagePolicy") ||
            !asset.at("usagePolicy").is_string() ||
            !asset.contains("qualificationEligible") ||
            !asset.at("qualificationEligible").is_boolean())
        {
            outError = "Cooked product entry '" + outId +
                "' lacks schema-v4 usage policy metadata.";
            return false;
        }
        const std::string usagePolicy = asset.at("usagePolicy").get<std::string>();
        const bool qualificationEligible =
            asset.at("qualificationEligible").get<bool>();
        const bool validUsagePolicy =
            usagePolicy == "required-qualification" ||
            usagePolicy == "development-only" ||
            usagePolicy == "optional-nightly";
        if (!validUsagePolicy ||
            qualificationEligible !=
                (usagePolicy == "required-qualification"))
        {
            outError = "Cooked product entry '" + outId +
                "' has contradictory schema-v4 usage policy metadata.";
            return false;
        }
        return true;
    }

    void PrintUsage()
    {
        std::cerr << "Usage: RVXSampleCookedProductCatalogWriter"
                  << " --input-catalog <path> --output-catalog <path>"
                  << " --asset-id <id> [--asset-id <id> ...]\n";
    }
} // namespace

int main(int argc, char* argv[])
{
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    std::set<std::string, std::less<>> selectedIds;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index] != nullptr ? argv[index] : "";
        if ((argument == "--input-catalog" || argument == "--output-catalog" ||
             argument == "--asset-id") && index + 1 < argc)
        {
            const std::string value = argv[++index] != nullptr ? argv[index] : "";
            if (argument == "--input-catalog")
            {
                inputPath = value;
            }
            else if (argument == "--output-catalog")
            {
                outputPath = value;
            }
            else if (!value.empty())
            {
                selectedIds.insert(value);
            }
            continue;
        }
        PrintUsage();
        return 2;
    }

    if (inputPath.empty() || outputPath.empty() || selectedIds.empty())
    {
        PrintUsage();
        return 2;
    }

    std::ifstream input(inputPath, std::ios::binary);
    nlohmann::json catalog;
    try
    {
        input >> catalog;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Failed to parse input catalog: " << error.what() << "\n";
        return 1;
    }
    if (!input || !catalog.is_object() ||
        catalog.value("schemaId", std::string{}) != "RVX.SampleAssetCatalog" ||
        catalog.value("schemaVersion", 0u) != 4u ||
        !catalog.contains("assets") ||
        !catalog.at("assets").is_array())
    {
        std::cerr << "Input is not a schema-v4 Sample asset catalog with an assets array.\n";
        return 1;
    }

    nlohmann::json selected = nlohmann::json::array();
    std::set<std::string, std::less<>> found;
    for (const nlohmann::json& asset : catalog.at("assets"))
    {
        if (!asset.is_object() || !asset.contains("id") ||
            !asset.at("id").is_string())
        {
            continue;
        }
        const std::string id = asset.at("id").get<std::string>();
        if (!selectedIds.contains(id))
        {
            continue;
        }
        std::string checkedId;
        std::string error;
        if (!IsAdmittedCookedEntry(asset, checkedId, error))
        {
            std::cerr << error << "\n";
            return 1;
        }
        selected.push_back(asset);
        found.insert(std::move(checkedId));
    }
    if (found != selectedIds)
    {
        std::cerr << "Cooked product catalog did not find every requested asset id.\n";
        return 1;
    }

    catalog["assets"] = std::move(selected);
    std::error_code directoryError;
    std::filesystem::create_directories(outputPath.parent_path(), directoryError);
    if (directoryError)
    {
        std::cerr << "Failed to create output catalog directory: "
                  << directoryError.message() << "\n";
        return 1;
    }
    const std::filesystem::path temporaryPath = outputPath.string() + ".tmp";
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    output << catalog.dump(2) << '\n';
    output.close();
    if (!output)
    {
        std::cerr << "Failed to write output catalog.\n";
        return 1;
    }
    std::error_code renameError;
    std::filesystem::remove(outputPath, renameError);
    renameError.clear();
    std::filesystem::rename(temporaryPath, outputPath, renameError);
    if (renameError)
    {
        std::cerr << "Failed to publish output catalog: " << renameError.message() << "\n";
        return 1;
    }
    return 0;
}
