/** @file AnimationRootMotionDeriveMain.cpp  @brief CLI for isolated root-motion derivation. */

#include "Tools/AnimationRootMotionDeriver.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using RVX::Tools::AnimationRootMotionDeriveRequest;
    using RVX::Tools::AnimationRootMotionPackagePublishRequest;

    struct Options
    {
        AnimationRootMotionDeriveRequest derivation;
        std::filesystem::path sourceRoot;
        std::filesystem::path packagePath;
        bool packageMode = false;
    };

    void PrintUsage(std::ostream& stream)
    {
        stream << "Usage: RVXAnimationDerive [--mode artifact] --input <artifact.rvxanim> "
               << "--output <derived.rvxanim> "
               << "--source-clip Walk --output-clip casual-female-walk-root-motion "
               << "--root CharacterArmature/Bone/Body/Hips --axis +Z --speed 1.0 "
               << "--duration-us 1250000 --parent-asset-id casual-female "
               << "(--parent-content-id rvx-asset-content-v1:sha256:<digest>:3595794:5 "
               << "| --parent-content-schema-version 1 --parent-content-algorithm sha256 "
               << "--parent-content-digest <digest> --parent-content-byte-count 3595794 "
               << "--parent-content-file-count 5)\n"
               << "   or: RVXAnimationDerive --mode package --input <parent.rvxanim> "
               << "--source-root <SamplesAssets> --package-output <child-package-dir> "
               << "--source-clip Walk --output-clip casual-female-walk-root-motion "
               << "--root CharacterArmature/Bone/Body/Hips --axis +Z --speed 1.0 "
               << "--duration-us 1250000 --parent-asset-id casual-female "
               << "--parent-content-id rvx-asset-content-v1:sha256:<digest>:3595794:5\n";
    }

    bool IsCanonicalUnsigned(std::string_view value)
    {
        if (value.empty() || (value.size() > 1 && value.front() == '0'))
            return false;
        for (const char character : value)
            if (character < '0' || character > '9')
                return false;
        return true;
    }

    bool ParseUInt32(std::string_view value, RVX::uint32& outValue)
    {
        if (!IsCanonicalUnsigned(value))
            return false;
        uint64_t parsed = 0;
        for (const char character : value)
        {
            parsed = parsed * 10u + static_cast<uint64_t>(character - '0');
            if (parsed > std::numeric_limits<RVX::uint32>::max())
                return false;
        }
        outValue = static_cast<RVX::uint32>(parsed);
        return true;
    }

    bool ParseUInt64(std::string_view value, RVX::uint64& outValue)
    {
        if (!IsCanonicalUnsigned(value))
            return false;
        uint64_t parsed = 0;
        for (const char character : value)
        {
            const uint64_t digit = static_cast<uint64_t>(character - '0');
            if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10u)
                return false;
            parsed = parsed * 10u + digit;
        }
        outValue = parsed;
        return true;
    }

    bool ParseContentId(std::string_view value,
                        RVX::Tools::AnimationRootMotionParentContentId& outContentId)
    {
        std::vector<std::string_view> fields;
        size_t begin = 0;
        while (begin <= value.size())
        {
            const size_t end = value.find(':', begin);
            fields.push_back(value.substr(begin, end - begin));
            if (end == std::string_view::npos)
                break;
            begin = end + 1;
        }
        if (fields.size() != 5 || fields[0] != "rvx-asset-content-v1" ||
            fields[1] != "sha256" || !ParseUInt64(fields[3], outContentId.byteCount) ||
            !ParseUInt32(fields[4], outContentId.fileCount))
        {
            return false;
        }
        outContentId.schemaVersion = 1;
        outContentId.algorithm = std::string(fields[1]);
        outContentId.digest = std::string(fields[2]);
        return RVX::Tools::FormatAnimationRootMotionParentContentId(outContentId) == value;
    }

    bool ReadRequired(const std::map<std::string, std::string>& options,
                      const char* name,
                      std::string& outValue,
                      std::string& outError)
    {
        const auto iterator = options.find(name);
        if (iterator == options.end() || iterator->second.empty())
        {
            outError = std::string("Missing required argument: ") + name;
            return false;
        }
        outValue = iterator->second;
        return true;
    }

    bool ParseOptions(int argc,
                      char** argv,
                      Options& outOptions,
                      bool& outShowHelp,
                      std::string& outError)
    {
        outShowHelp = false;
        std::map<std::string, std::string> options;
        const std::vector<std::string> allowed = {
            "--mode", "--input", "--output", "--source-root", "--package-output",
            "--source-clip", "--output-clip", "--root", "--axis",
            "--speed", "--duration-us", "--parent-asset-id", "--parent-content-id",
            "--parent-content-schema-version", "--parent-content-algorithm",
            "--parent-content-digest", "--parent-content-byte-count",
            "--parent-content-file-count"};

        for (int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index];
            if (argument == "--help")
            {
                if (argc != 2)
                {
                    outError = "--help cannot be combined with derivation arguments.";
                    return false;
                }
                outShowHelp = true;
                return true;
            }
            if (std::find(allowed.begin(), allowed.end(), argument) == allowed.end() ||
                index + 1 >= argc || options.contains(argument))
            {
                outError = "Unknown, duplicate, or valueless argument: " + argument;
                return false;
            }
            options.emplace(argument, argv[++index]);
        }

        std::string value;
        if (!ReadRequired(options, "--input", value, outError)) return false;
        outOptions.derivation.inputPath = value;

        const std::string mode = options.contains("--mode")
            ? options.at("--mode")
            : "artifact";
        if (mode == "artifact")
        {
            if (options.contains("--source-root") || options.contains("--package-output") ||
                !ReadRequired(options, "--output", value, outError))
            {
                if (outError.empty())
                    outError = "Artifact mode requires --output and rejects package arguments.";
                return false;
            }
            outOptions.derivation.outputPath = value;
        }
        else if (mode == "package")
        {
            if (options.contains("--output") ||
                !ReadRequired(options, "--source-root", value, outError))
            {
                if (outError.empty())
                    outError = "Package mode requires --source-root and rejects --output.";
                return false;
            }
            outOptions.sourceRoot = value;
            if (!ReadRequired(options, "--package-output", value, outError))
                return false;
            outOptions.packagePath = value;
            outOptions.packageMode = true;
        }
        else
        {
            outError = "Unsupported derivation mode: " + mode;
            return false;
        }

        if (!ReadRequired(options, "--source-clip", outOptions.derivation.sourceClip, outError) ||
            !ReadRequired(options, "--output-clip", outOptions.derivation.outputClip, outError) ||
            !ReadRequired(options, "--root", outOptions.derivation.rootBone, outError) ||
            !ReadRequired(options, "--axis", value, outError) || value != "+Z" ||
            !ReadRequired(options, "--speed", value, outError) || value != "1.0" ||
            !ReadRequired(options, "--duration-us", value, outError) ||
            !IsCanonicalUnsigned(value) || value != "1250000" ||
            !ReadRequired(options, "--parent-asset-id", outOptions.derivation.parentAssetId, outError))
        {
            if (outError.empty())
                outError = "Unsupported root-motion derivation argument value.";
            return false;
        }

        const bool hasCombinedId = options.contains("--parent-content-id");
        const std::vector<std::string> splitIdOptions = {
            "--parent-content-schema-version", "--parent-content-algorithm",
            "--parent-content-digest", "--parent-content-byte-count",
            "--parent-content-file-count"};
        const bool hasAnySplitId = std::any_of(
            splitIdOptions.begin(), splitIdOptions.end(),
            [&options](const std::string& option) { return options.contains(option); });
        if (hasCombinedId == hasAnySplitId)
        {
            outError = "Specify exactly one canonical parent content ID form.";
            return false;
        }

        if (hasCombinedId)
        {
            if (!ParseContentId(options.at("--parent-content-id"),
                                outOptions.derivation.parentContentId))
            {
                outError = "Parent content ID is not canonical.";
                return false;
            }
        }
        else
        {
            std::string schemaVersion;
            std::string algorithm;
            std::string digest;
            std::string byteCount;
            std::string fileCount;
            if (!ReadRequired(options, "--parent-content-schema-version", schemaVersion, outError) ||
                !ReadRequired(options, "--parent-content-algorithm", algorithm, outError) ||
                !ReadRequired(options, "--parent-content-digest", digest, outError) ||
                !ReadRequired(options, "--parent-content-byte-count", byteCount, outError) ||
                !ReadRequired(options, "--parent-content-file-count", fileCount, outError) ||
                !ParseUInt32(schemaVersion,
                             outOptions.derivation.parentContentId.schemaVersion) ||
                !ParseUInt64(byteCount,
                             outOptions.derivation.parentContentId.byteCount) ||
                !ParseUInt32(fileCount,
                             outOptions.derivation.parentContentId.fileCount))
            {
                if (outError.empty())
                    outError = "Parent content ID fields are not canonical.";
                return false;
            }
            outOptions.derivation.parentContentId.algorithm = algorithm;
            outOptions.derivation.parentContentId.digest = digest;
        }

        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    Options options;
    bool showHelp = false;
    std::string error;
    if (!ParseOptions(argc, argv, options, showHelp, error))
    {
        std::cerr << "RVXAnimationDerive: " << error << '\n';
        PrintUsage(std::cerr);
        return 2;
    }
    if (showHelp)
    {
        PrintUsage(std::cout);
        return 0;
    }

    if (options.packageMode)
    {
        AnimationRootMotionPackagePublishRequest request;
        request.sourceRoot = options.sourceRoot;
        request.packagePath = options.packagePath;
        request.derivation = std::move(options.derivation);
        RVX::Tools::AnimationRootMotionPackagePublishResult result;
        if (!RVX::Tools::PublishAnimationRootMotionPackage(request, result, error))
        {
            std::cerr << "RVXAnimationDerive: " << error << '\n';
            return 1;
        }
        std::cout << "input.sha256=" << result.derivation.inputSha256 << '\n'
                  << "artifact.sha256=" << result.artifactSha256 << '\n'
                  << "manifest.recipe.sha256=" << result.manifestRecipeHash << '\n'
                  << "cooked.sha256=" << result.cookedContentSha256 << '\n';
        return 0;
    }

    RVX::Tools::AnimationRootMotionDeriveResult result;
    if (!RVX::Tools::DeriveAnimationRootMotion(options.derivation, result, error))
    {
        std::cerr << "RVXAnimationDerive: " << error << '\n';
        return 1;
    }
    std::cout << "input.sha256=" << result.inputSha256 << '\n'
              << "recipe.sha256=" << result.recipeSha256 << '\n';
    return 0;
}
