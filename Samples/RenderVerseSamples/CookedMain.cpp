/** @file CookedMain.cpp @brief Strict cooked-only RenderQualityLab host. */

#include "RegisterCookedSamples.h"

#include "Samples/SampleHostComposition.h"
#include "Samples/SampleManifest.h"
#include "Samples/SampleRegistry.h"
#include "Samples/SampleRunner.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    [[nodiscard]] bool IsProductOverride(std::string_view argument) noexcept
    {
        return argument == "--sample" || argument == "--catalog" ||
               argument == "--asset-root" || argument == "--model" ||
               argument == "--asset" || argument == "--environment" ||
               argument == "--environment-file" ||
               argument == "--runtime-only-composition";
    }

    [[nodiscard]] std::filesystem::path ResolveExecutableDirectory(
        const char* argument)
    {
        if (argument == nullptr || *argument == '\0')
        {
            return {};
        }
        std::error_code error;
        const std::filesystem::path executable =
            std::filesystem::absolute(argument, error);
        if (error)
        {
            return {};
        }
        return executable.parent_path();
    }
} // namespace

int main(int argc, char* argv[])
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index] != nullptr ? argv[index] : "";
        if (IsProductOverride(argument))
        {
            std::cerr << "RenderVerseSamplesCooked fixes " << argument
                      << " at its cooked product boundary\n";
            return 2;
        }
    }

    const std::filesystem::path executableDirectory =
        ResolveExecutableDirectory(argc > 0 ? argv[0] : nullptr);
    const std::filesystem::path assetRoot = executableDirectory / "Assets" /
        "RenderQualityLabCooked";
    const std::filesystem::path manifestPath =
        assetRoot / "renderverse-samples.json";
    const std::filesystem::path catalogPath = assetRoot / "catalog.json";
    if (executableDirectory.empty() || !std::filesystem::is_regular_file(manifestPath) ||
        !std::filesystem::is_regular_file(catalogPath))
    {
        std::cerr << "Cooked RenderQualityLab product assets are unavailable under '"
                  << assetRoot.string() << "'\n";
        return 1;
    }

    RVX::SampleManifestCatalog manifest;
    std::string error;
    if (!RVX::SampleManifestParser::LoadFile(manifestPath, manifest, &error))
    {
        std::cerr << "Failed to load cooked Sample manifest: " << error << "\n";
        return 1;
    }

    RVX::SampleRegistry registry;
    if (!RVX::RegisterRenderQualityLabCookedSample(manifest, registry, &error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    std::vector<std::string> ownedArguments;
    ownedArguments.reserve(static_cast<size_t>(argc) + 9u);
    for (int index = 0; index < argc; ++index)
    {
        ownedArguments.emplace_back(argv[index] != nullptr ? argv[index] : "");
    }
    ownedArguments.emplace_back("--sample");
    ownedArguments.emplace_back("render-quality-lab");
    ownedArguments.emplace_back("--catalog");
    ownedArguments.emplace_back(catalogPath.string());
    ownedArguments.emplace_back("--asset-root");
    ownedArguments.emplace_back(assetRoot.string());
    ownedArguments.emplace_back("--runtime-only-composition");

    std::vector<char*> forwarded;
    forwarded.reserve(ownedArguments.size());
    for (std::string& argument : ownedArguments)
    {
        forwarded.push_back(argument.data());
    }

    const std::unique_ptr<RVX::ISampleHostComposition> composition =
        RVX::CreateSampleCookedComposition();
    if (!composition)
    {
        std::cerr << "Failed to create cooked Sample composition\n";
        return 1;
    }
    return RVX::SampleRunner(registry, *composition).Run(
        static_cast<int>(forwarded.size()), forwarded.data());
}
