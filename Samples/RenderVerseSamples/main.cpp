/** @file main.cpp @brief Unified RenderVerseX sample executable. */

#include "RegisterSamples.h"
#include "Samples/SampleManifest.h"
#include "Samples/SampleHostComposition.h"
#include "Samples/SampleRegistry.h"
#include "Samples/SampleRunner.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace
{
    [[nodiscard]] std::filesystem::path GetExecutablePath(
        const char* fallbackArgument)
    {
#if defined(_WIN32)
        std::vector<wchar_t> buffer(1024u);
        for (;;)
        {
            const DWORD count = GetModuleFileNameW(
                nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (count == 0)
            {
                break;
            }
            if (count < buffer.size() - 1u)
            {
                return std::filesystem::path(
                    std::wstring(buffer.data(), count));
            }
            buffer.resize(buffer.size() * 2u);
        }
#elif defined(__APPLE__)
        uint32_t size = 0;
        static_cast<void>(_NSGetExecutablePath(nullptr, &size));
        if (size != 0)
        {
            std::vector<char> buffer(size);
            if (_NSGetExecutablePath(buffer.data(), &size) == 0)
            {
                return std::filesystem::weakly_canonical(buffer.data());
            }
        }
#else
        std::vector<char> buffer(4096u);
        const ssize_t count = readlink(
            "/proc/self/exe", buffer.data(), buffer.size());
        if (count > 0 && static_cast<size_t>(count) < buffer.size())
        {
            return std::filesystem::path(
                std::string(buffer.data(), static_cast<size_t>(count)));
        }
#endif
        if (fallbackArgument == nullptr || *fallbackArgument == '\0')
        {
            return {};
        }
        std::error_code error;
        const std::filesystem::path fallback =
            std::filesystem::weakly_canonical(fallbackArgument, error);
        return error ? std::filesystem::path{} : fallback;
    }

    [[nodiscard]] bool RequestsHelp(int argc, char* argv[]) noexcept
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument =
                argv[index] != nullptr ? argv[index] : "";
            if (argument == "--help" || argument == "-h")
            {
                return true;
            }
        }
        return false;
    }
} // namespace

int main(int argc, char* argv[])
{
    if (RequestsHelp(argc, argv))
    {
        const RVX::SampleRegistry emptyRegistry;
        return RVX::SampleRunner(emptyRegistry).Run(argc, argv);
    }

    const std::filesystem::path executablePath = GetExecutablePath(
        argc > 0 ? argv[0] : nullptr);
    const std::filesystem::path executableDirectory =
        executablePath.parent_path();
    if (executableDirectory.empty())
    {
        std::cerr << "Failed to resolve the Sample host directory\n";
        return 1;
    }

    const std::filesystem::path manifestPath =
        executableDirectory / "Assets" / "Samples" /
        "renderverse-samples.json";
    RVX::SampleManifestCatalog manifest;
    std::string error;
    if (!RVX::SampleManifestParser::LoadFile(
            manifestPath, manifest, &error))
    {
        std::cerr << "Failed to load Sample manifest '"
                  << manifestPath.string() << "': " << error << "\n";
        return 1;
    }

    RVX::SampleRegistry registry;
    if (!RVX::RegisterRenderVerseSamples(manifest, registry, &error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    const std::unique_ptr<RVX::ISampleHostComposition> composition =
        RVX::CreateSampleDevelopmentComposition();
    if (!composition)
    {
        std::cerr << "Failed to create development Sample composition\n";
        return 1;
    }
    return RVX::SampleRunner(registry, *composition).Run(argc, argv);
}
