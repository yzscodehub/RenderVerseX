#include "Tools/AssetPipeline.h"

#include "Core/Log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace
{
    struct Options
    {
        std::filesystem::path sourceRoot;
        std::filesystem::path outputRoot;
        std::filesystem::path manifestPath;
        std::filesystem::path profilePath;
        RVX::Tools::TextureImportOptions textureOptions;
        bool hasTextureOptions = false;
        bool recursive = true;
        bool failOnErrors = false;
        bool rewriteGltfTextureUris = false;
        bool showHelp = false;
    };

    struct TextureCompressionRule
    {
        std::string pattern;
        RVX::Tools::TextureCompressionMode mode = RVX::Tools::TextureCompressionMode::Auto;
    };

    struct CookProfile
    {
        bool hasDefaultTextureCompression = false;
        RVX::Tools::TextureCompressionMode defaultTextureCompression = RVX::Tools::TextureCompressionMode::Auto;
        std::vector<TextureCompressionRule> textureCompressionRules;
        bool hasMeshOptions = false;
        RVX::Tools::MeshImportOptions meshOptions;
    };

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(),
                       value.end(),
                       value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    std::string Trim(std::string value)
    {
        auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch)
        {
            return !isSpace(static_cast<unsigned char>(ch));
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch)
        {
            return !isSpace(static_cast<unsigned char>(ch));
        }).base(), value.end());
        return value;
    }

    std::string NormalizeProfilePath(std::string value)
    {
        std::replace(value.begin(), value.end(), '\\', '/');
        return ToLower(value);
    }

    bool WildcardMatch(const std::string& pattern, const std::string& value)
    {
        size_t patternIndex = 0;
        size_t valueIndex = 0;
        size_t starIndex = std::string::npos;
        size_t retryValueIndex = 0;

        while (valueIndex < value.size())
        {
            if (patternIndex < pattern.size() &&
                (pattern[patternIndex] == '?' || pattern[patternIndex] == value[valueIndex]))
            {
                ++patternIndex;
                ++valueIndex;
            }
            else if (patternIndex < pattern.size() && pattern[patternIndex] == '*')
            {
                starIndex = patternIndex++;
                retryValueIndex = valueIndex;
            }
            else if (starIndex != std::string::npos)
            {
                patternIndex = starIndex + 1;
                valueIndex = ++retryValueIndex;
            }
            else
            {
                return false;
            }
        }

        while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
        {
            ++patternIndex;
        }

        return patternIndex == pattern.size();
    }

    void SetTextureCompressionMode(RVX::Tools::TextureImportOptions& options,
                                   RVX::Tools::TextureCompressionMode mode)
    {
        options.compressionMode = mode;
        options.compress = mode != RVX::Tools::TextureCompressionMode::None;
    }

    bool ParseTextureCompressionMode(const std::string& value,
                                     RVX::Tools::TextureCompressionMode& outMode)
    {
        const std::string mode = ToLower(value);
        if (mode == "auto")
        {
            outMode = RVX::Tools::TextureCompressionMode::Auto;
            return true;
        }
        if (mode == "none" || mode == "off" || mode == "uncompressed")
        {
            outMode = RVX::Tools::TextureCompressionMode::None;
            return true;
        }
        if (mode == "bc1")
        {
            outMode = RVX::Tools::TextureCompressionMode::BC1;
            return true;
        }
        if (mode == "bc3")
        {
            outMode = RVX::Tools::TextureCompressionMode::BC3;
            return true;
        }
        if (mode == "bc5")
        {
            outMode = RVX::Tools::TextureCompressionMode::BC5;
            return true;
        }
        if (mode == "bc7")
        {
            outMode = RVX::Tools::TextureCompressionMode::BC7;
            return true;
        }

        return false;
    }

    bool ParseBoolValue(const std::string& value, bool& outValue)
    {
        const std::string normalized = ToLower(value);
        if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on")
        {
            outValue = true;
            return true;
        }
        if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off")
        {
            outValue = false;
            return true;
        }
        return false;
    }

    bool ParseIntValue(const std::string& value, int& outValue)
    {
        try
        {
            size_t parsed = 0;
            const int result = std::stoi(value, &parsed, 10);
            if (parsed != value.size())
            {
                return false;
            }
            outValue = result;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseFloatValue(const std::string& value, float& outValue)
    {
        try
        {
            size_t parsed = 0;
            const float result = std::stof(value, &parsed);
            if (parsed != value.size())
            {
                return false;
            }
            outValue = result;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool LoadCookProfile(const std::filesystem::path& profilePath,
                         CookProfile& profile,
                         std::string& outError)
    {
        std::ifstream file(profilePath);
        if (!file.is_open())
        {
            outError = "Failed to open cook profile: " + profilePath.string();
            return false;
        }

        bool sawHeader = false;
        std::string line;
        size_t lineNumber = 0;
        while (std::getline(file, line))
        {
            ++lineNumber;
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
            {
                continue;
            }

            if (!sawHeader)
            {
                if (line != "RVX_COOK_PROFILE_V1")
                {
                    outError = "Cook profile line " + std::to_string(lineNumber) +
                               " must be RVX_COOK_PROFILE_V1";
                    return false;
                }
                sawHeader = true;
                continue;
            }

            const size_t separator = line.find('=');
            if (separator == std::string::npos)
            {
                outError = "Cook profile line " + std::to_string(lineNumber) +
                           " is missing '='";
                return false;
            }

            const std::string key = Trim(line.substr(0, separator));
            const std::string value = Trim(line.substr(separator + 1));
            const std::string keyLower = ToLower(key);

            if (keyLower == "texture.compression" || keyLower == "texture.compression.default")
            {
                RVX::Tools::TextureCompressionMode mode = RVX::Tools::TextureCompressionMode::Auto;
                if (!ParseTextureCompressionMode(value, mode))
                {
                    outError = "Cook profile line " + std::to_string(lineNumber) +
                               " has unsupported texture compression mode: " + value;
                    return false;
                }

                profile.defaultTextureCompression = mode;
                profile.hasDefaultTextureCompression = true;
                continue;
            }

            constexpr const char* prefix = "texture.compression[";
            constexpr const char* suffix = "]";
            if (keyLower.rfind(prefix, 0) == 0 &&
                keyLower.size() > std::char_traits<char>::length(prefix) + std::char_traits<char>::length(suffix) &&
                keyLower.back() == ']')
            {
                RVX::Tools::TextureCompressionMode mode = RVX::Tools::TextureCompressionMode::Auto;
                if (!ParseTextureCompressionMode(value, mode))
                {
                    outError = "Cook profile line " + std::to_string(lineNumber) +
                               " has unsupported texture compression mode: " + value;
                    return false;
                }

                const size_t patternBegin = std::char_traits<char>::length(prefix);
                const size_t patternLength = key.size() - patternBegin - std::char_traits<char>::length(suffix);
                std::string pattern = NormalizeProfilePath(Trim(key.substr(patternBegin, patternLength)));
                if (pattern.empty())
                {
                    outError = "Cook profile line " + std::to_string(lineNumber) +
                               " has an empty texture compression pattern";
                    return false;
                }

                profile.textureCompressionRules.push_back({std::move(pattern), mode});
                continue;
            }

            if (keyLower == "mesh.generatetangents")
            {
                bool parsed = false;
                if (!ParseBoolValue(value, parsed))
                {
                    outError = "Cook profile line " + std::to_string(lineNumber) +
                               " has unsupported mesh.generateTangents value: " + value;
                    return false;
                }
                profile.meshOptions.generateTangents = parsed;
                profile.hasMeshOptions = true;
                continue;
            }

            if (keyLower == "mesh.optimizemesh")
            {
                bool parsed = false;
                if (!ParseBoolValue(value, parsed))
                {
                    outError = "Cook profile line " + std::to_string(lineNumber) +
                               " has unsupported mesh.optimizeMesh value: " + value;
                    return false;
                }
                profile.meshOptions.optimizeMesh = parsed;
                profile.hasMeshOptions = true;
                continue;
            }

            if (keyLower == "mesh.generatelods")
            {
                bool parsed = false;
                if (!ParseBoolValue(value, parsed))
                {
                    outError = "Cook profile line " + std::to_string(lineNumber) +
                               " has unsupported mesh.generateLODs value: " + value;
                    return false;
                }
                profile.meshOptions.generateLODs = parsed;
                profile.hasMeshOptions = true;
                continue;
            }

            if (keyLower == "mesh.lodcount")
            {
                int parsed = 0;
                if (!ParseIntValue(value, parsed) || parsed < 1 || parsed > 16)
                {
                    outError = "Cook profile line " + std::to_string(lineNumber) +
                               " has unsupported mesh.lodCount value: " + value;
                    return false;
                }
                profile.meshOptions.lodCount = parsed;
                profile.hasMeshOptions = true;
                continue;
            }

            if (keyLower == "mesh.lodreductionfactor")
            {
                float parsed = 0.0f;
                if (!ParseFloatValue(value, parsed) || !std::isfinite(parsed) ||
                    parsed <= 0.0f || parsed >= 1.0f)
                {
                    outError = "Cook profile line " + std::to_string(lineNumber) +
                               " has unsupported mesh.lodReductionFactor value: " + value;
                    return false;
                }
                profile.meshOptions.lodReductionFactor = parsed;
                profile.hasMeshOptions = true;
                continue;
            }

            outError = "Cook profile line " + std::to_string(lineNumber) +
                       " has unsupported key: " + key;
            return false;
        }

        if (!sawHeader)
        {
            outError = "Cook profile is empty or missing RVX_COOK_PROFILE_V1: " + profilePath.string();
            return false;
        }

        outError.clear();
        return true;
    }

    std::optional<RVX::Tools::TextureCompressionMode> MatchTextureCompressionRule(
        const CookProfile& profile,
        const std::filesystem::path& sourceRoot,
        const std::filesystem::path& sourcePath)
    {
        std::error_code ec;
        std::filesystem::path relativePath = std::filesystem::relative(sourcePath, sourceRoot, ec);
        const std::string normalizedPath = NormalizeProfilePath(
            ec ? sourcePath.generic_string() : relativePath.generic_string());

        std::optional<RVX::Tools::TextureCompressionMode> result;
        for (const TextureCompressionRule& rule : profile.textureCompressionRules)
        {
            if (WildcardMatch(rule.pattern, normalizedPath))
            {
                result = rule.mode;
            }
        }

        return result;
    }

    uint64_t GetFileWriteTimeTicks(const std::filesystem::path& path)
    {
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(path, ec);
        if (ec)
        {
            return 0;
        }
        return static_cast<uint64_t>(time.time_since_epoch().count());
    }

    uint64_t GetFileSizeBytes(const std::filesystem::path& path)
    {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            return 0;
        }
        return static_cast<uint64_t>(size);
    }

    std::string ToGenericPathString(const std::filesystem::path& path)
    {
        return path.generic_string();
    }

    bool IsGltfSourceFile(const std::filesystem::path& path)
    {
        return ToLower(path.extension().string()) == ".gltf";
    }

    bool HasUriScheme(const std::string& uri)
    {
        const size_t colon = uri.find(':');
        if (colon == std::string::npos || colon == 0)
        {
            return false;
        }

        const size_t slash = uri.find_first_of("/\\");
        if (slash != std::string::npos && slash < colon)
        {
            return false;
        }

        for (size_t i = 0; i < colon; ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(uri[i]);
            if (!(std::isalnum(ch) || ch == '+' || ch == '-' || ch == '.'))
            {
                return false;
            }
        }

        return true;
    }

    bool IsCookedTextureArtifactUri(const std::string& uri)
    {
        return ToLower(std::filesystem::path(uri).extension().string()) == ".rva";
    }

    bool IsJsonWhitespace(char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    }

    size_t SkipJsonWhitespace(const std::string& text, size_t offset)
    {
        while (offset < text.size() && IsJsonWhitespace(text[offset]))
        {
            ++offset;
        }
        return offset;
    }

    std::optional<std::string> DecodeJsonString(const std::string& text,
                                                size_t quoteOffset,
                                                size_t& outEndQuote)
    {
        if (quoteOffset >= text.size() || text[quoteOffset] != '"')
        {
            return std::nullopt;
        }

        std::string decoded;
        for (size_t i = quoteOffset + 1; i < text.size(); ++i)
        {
            const char ch = text[i];
            if (ch == '"')
            {
                outEndQuote = i;
                return decoded;
            }

            if (ch != '\\')
            {
                decoded.push_back(ch);
                continue;
            }

            if (i + 1 >= text.size())
            {
                return std::nullopt;
            }

            const char escaped = text[++i];
            switch (escaped)
            {
                case '"': decoded.push_back('"'); break;
                case '\\': decoded.push_back('\\'); break;
                case '/': decoded.push_back('/'); break;
                case 'b': decoded.push_back('\b'); break;
                case 'f': decoded.push_back('\f'); break;
                case 'n': decoded.push_back('\n'); break;
                case 'r': decoded.push_back('\r'); break;
                case 't': decoded.push_back('\t'); break;
                case 'u':
                    if (i + 4 >= text.size())
                    {
                        return std::nullopt;
                    }
                    decoded.push_back('?');
                    i += 4;
                    break;
                default:
                    return std::nullopt;
            }
        }

        return std::nullopt;
    }

    std::string EncodeJsonString(const std::string& value)
    {
        std::string encoded;
        encoded.reserve(value.size() + 2);
        encoded.push_back('"');
        for (char ch : value)
        {
            switch (ch)
            {
                case '"': encoded += "\\\""; break;
                case '\\': encoded += "\\\\"; break;
                case '\b': encoded += "\\b"; break;
                case '\f': encoded += "\\f"; break;
                case '\n': encoded += "\\n"; break;
                case '\r': encoded += "\\r"; break;
                case '\t': encoded += "\\t"; break;
                default: encoded.push_back(ch); break;
            }
        }
        encoded.push_back('"');
        return encoded;
    }

    bool FindMatchingJsonArrayEnd(const std::string& text, size_t arrayStart, size_t& outArrayEnd)
    {
        if (arrayStart >= text.size() || text[arrayStart] != '[')
        {
            return false;
        }

        size_t depth = 0;
        for (size_t i = arrayStart; i < text.size(); ++i)
        {
            if (text[i] == '"')
            {
                size_t endQuote = 0;
                if (!DecodeJsonString(text, i, endQuote))
                {
                    return false;
                }
                i = endQuote;
                continue;
            }

            if (text[i] == '[')
            {
                ++depth;
            }
            else if (text[i] == ']')
            {
                if (depth == 0)
                {
                    return false;
                }
                --depth;
                if (depth == 0)
                {
                    outArrayEnd = i;
                    return true;
                }
            }
        }

        return false;
    }

    bool FindJsonArrayProperty(const std::string& text,
                               const char* propertyName,
                               size_t& outArrayStart,
                               size_t& outArrayEnd)
    {
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '"')
            {
                continue;
            }

            size_t endQuote = 0;
            std::optional<std::string> property = DecodeJsonString(text, i, endQuote);
            if (!property)
            {
                return false;
            }

            i = endQuote;
            if (*property != propertyName)
            {
                continue;
            }

            size_t valueOffset = SkipJsonWhitespace(text, endQuote + 1);
            if (valueOffset >= text.size() || text[valueOffset] != ':')
            {
                continue;
            }

            valueOffset = SkipJsonWhitespace(text, valueOffset + 1);
            if (valueOffset >= text.size() || text[valueOffset] != '[')
            {
                continue;
            }

            outArrayStart = valueOffset;
            return FindMatchingJsonArrayEnd(text, outArrayStart, outArrayEnd);
        }

        return false;
    }

    std::optional<std::string> ResolveCookedTextureUri(
        const std::string& uri,
        const std::filesystem::path& sourceRoot,
        const std::filesystem::path& outputRoot,
        const std::filesystem::path& sourceGltfPath,
        const std::filesystem::path& outputGltfPath,
        const std::unordered_map<std::string, std::string>& textureOutputBySource)
    {
        const std::string lowerUri = ToLower(uri);
        if (lowerUri.rfind("data:", 0) == 0 ||
            HasUriScheme(uri) ||
            IsCookedTextureArtifactUri(uri))
        {
            return std::nullopt;
        }

        std::filesystem::path textureSourcePath = sourceGltfPath.parent_path() / std::filesystem::path(uri);
        textureSourcePath = textureSourcePath.lexically_normal();

        std::error_code ec;
        const std::filesystem::path relativeSourcePath =
            std::filesystem::relative(textureSourcePath, sourceRoot, ec);
        if (ec)
        {
            return std::nullopt;
        }

        const std::string sourceKey = NormalizeProfilePath(relativeSourcePath.generic_string());
        auto it = textureOutputBySource.find(sourceKey);
        if (it == textureOutputBySource.end())
        {
            return std::nullopt;
        }

        const std::filesystem::path cookedTexturePath =
            (outputRoot / std::filesystem::path(it->second)).lexically_normal();
        std::filesystem::path relativeCookedUri =
            std::filesystem::relative(cookedTexturePath, outputGltfPath.parent_path(), ec);
        if (ec)
        {
            relativeCookedUri = cookedTexturePath;
        }

        return relativeCookedUri.generic_string();
    }

    bool RewriteGltfImageUris(const std::string& sourceText,
                              const std::filesystem::path& sourceRoot,
                              const std::filesystem::path& outputRoot,
                              const std::filesystem::path& sourceGltfPath,
                              const std::filesystem::path& outputGltfPath,
                              const std::unordered_map<std::string, std::string>& textureOutputBySource,
                              std::string& outText,
                              uint32_t& outRewriteCount,
                              std::string& outError)
    {
        size_t imagesStart = 0;
        size_t imagesEnd = 0;
        if (!FindJsonArrayProperty(sourceText, "images", imagesStart, imagesEnd))
        {
            outText = sourceText;
            outRewriteCount = 0;
            outError.clear();
            return true;
        }

        struct Replacement
        {
            size_t begin = 0;
            size_t end = 0;
            std::string value;
        };

        std::vector<Replacement> replacements;
        for (size_t i = imagesStart + 1; i < imagesEnd; ++i)
        {
            if (sourceText[i] != '"')
            {
                continue;
            }

            size_t keyEndQuote = 0;
            std::optional<std::string> key = DecodeJsonString(sourceText, i, keyEndQuote);
            if (!key)
            {
                outError = "Invalid JSON string while scanning images array";
                return false;
            }

            i = keyEndQuote;
            if (*key != "uri")
            {
                continue;
            }

            size_t valueOffset = SkipJsonWhitespace(sourceText, keyEndQuote + 1);
            if (valueOffset >= imagesEnd || sourceText[valueOffset] != ':')
            {
                continue;
            }

            valueOffset = SkipJsonWhitespace(sourceText, valueOffset + 1);
            if (valueOffset >= imagesEnd || sourceText[valueOffset] != '"')
            {
                continue;
            }

            size_t valueEndQuote = 0;
            std::optional<std::string> uri = DecodeJsonString(sourceText, valueOffset, valueEndQuote);
            if (!uri)
            {
                outError = "Invalid JSON uri string while scanning images array";
                return false;
            }

            if (std::optional<std::string> cookedUri =
                    ResolveCookedTextureUri(*uri,
                                            sourceRoot,
                                            outputRoot,
                                            sourceGltfPath,
                                            outputGltfPath,
                                            textureOutputBySource))
            {
                replacements.push_back({valueOffset, valueEndQuote + 1, EncodeJsonString(*cookedUri)});
            }

            i = valueEndQuote;
        }

        if (replacements.empty())
        {
            outText = sourceText;
            outRewriteCount = 0;
            outError.clear();
            return true;
        }

        outText.clear();
        outText.reserve(sourceText.size());
        size_t cursor = 0;
        for (const Replacement& replacement : replacements)
        {
            outText.append(sourceText, cursor, replacement.begin - cursor);
            outText += replacement.value;
            cursor = replacement.end;
        }
        outText.append(sourceText, cursor, std::string::npos);

        outRewriteCount = static_cast<uint32_t>(replacements.size());
        outError.clear();
        return true;
    }

    std::vector<std::filesystem::path> CollectGltfSourceFiles(const std::filesystem::path& sourceRoot,
                                                              bool recursive)
    {
        std::vector<std::filesystem::path> files;
        if (!std::filesystem::exists(sourceRoot) || !std::filesystem::is_directory(sourceRoot))
        {
            return files;
        }

        auto tryAdd = [&files](const std::filesystem::directory_entry& entry)
        {
            if (entry.is_regular_file() && IsGltfSourceFile(entry.path()))
            {
                files.push_back(entry.path());
            }
        };

        if (recursive)
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceRoot))
            {
                tryAdd(entry);
            }
        }
        else
        {
            for (const auto& entry : std::filesystem::directory_iterator(sourceRoot))
            {
                tryAdd(entry);
            }
        }

        std::sort(files.begin(), files.end(), [](const auto& lhs, const auto& rhs)
        {
            return lhs.generic_string() < rhs.generic_string();
        });
        return files;
    }

    std::unordered_map<std::string, std::string> BuildCookedTextureOutputMap(
        const RVX::Tools::CookManifest& manifest)
    {
        std::unordered_map<std::string, std::string> textureOutputBySource;
        for (const RVX::Tools::CookManifestEntry& entry : manifest.entries)
        {
            if (entry.type == RVX::Tools::AssetType::Texture && entry.success)
            {
                textureOutputBySource[NormalizeProfilePath(entry.sourcePath)] = entry.outputPath;
            }
        }
        return textureOutputBySource;
    }

    void AppendRuntimeGltfEntry(RVX::Tools::CookManifest& manifest,
                                const std::filesystem::path& sourceRoot,
                                const std::filesystem::path& outputRoot,
                                const std::filesystem::path& sourcePath,
                                const std::filesystem::path& outputPath,
                                bool success,
                                uint32_t rewriteCount,
                                std::string error)
    {
        std::error_code ec;
        RVX::Tools::CookManifestEntry entry;
        entry.sourcePath = ToGenericPathString(std::filesystem::relative(sourcePath, sourceRoot, ec));
        if (ec)
        {
            entry.sourcePath = ToGenericPathString(sourcePath);
        }
        ec.clear();
        entry.outputPath = ToGenericPathString(std::filesystem::relative(outputPath, outputRoot, ec));
        if (ec)
        {
            entry.outputPath = ToGenericPathString(outputPath);
        }
        entry.type = RVX::Tools::AssetType::Mesh;
        entry.success = success;
        entry.error = std::move(error);
        entry.sourceModTime = GetFileWriteTimeTicks(sourcePath);
        if (success && std::filesystem::exists(outputPath))
        {
            entry.outputModTime = GetFileWriteTimeTicks(outputPath);
            entry.outputSize = GetFileSizeBytes(outputPath);
            entry.warnings.push_back("runtimeGltfTextureUriRewrites=" + std::to_string(rewriteCount));
        }
        manifest.entries.push_back(std::move(entry));
    }

    void RewriteRuntimeGltfTextureUris(RVX::Tools::CookManifest& manifest,
                                       const std::filesystem::path& sourceRoot,
                                       const std::filesystem::path& outputRoot,
                                       bool recursive)
    {
        const auto textureOutputBySource = BuildCookedTextureOutputMap(manifest);
        for (const std::filesystem::path& gltfPath : CollectGltfSourceFiles(sourceRoot, recursive))
        {
            std::error_code ec;
            const std::filesystem::path relativeGltf = std::filesystem::relative(gltfPath, sourceRoot, ec);
            if (ec)
            {
                AppendRuntimeGltfEntry(manifest,
                                       sourceRoot,
                                       outputRoot,
                                       gltfPath,
                                       outputRoot / gltfPath.filename(),
                                       false,
                                       0,
                                       "Failed to make glTF path relative to source root: " + ec.message());
                continue;
            }

            const std::filesystem::path outputGltf = outputRoot / relativeGltf;
            std::ifstream in(gltfPath, std::ios::binary);
            if (!in.is_open())
            {
                AppendRuntimeGltfEntry(manifest,
                                       sourceRoot,
                                       outputRoot,
                                       gltfPath,
                                       outputGltf,
                                       false,
                                       0,
                                       "Failed to open source glTF for URI rewrite: " + gltfPath.string());
                continue;
            }

            const std::string sourceText((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());

            std::string rewrittenText;
            std::string rewriteError;
            uint32_t rewriteCount = 0;
            if (!RewriteGltfImageUris(sourceText,
                                      sourceRoot,
                                      outputRoot,
                                      gltfPath,
                                      outputGltf,
                                      textureOutputBySource,
                                      rewrittenText,
                                      rewriteCount,
                                      rewriteError))
            {
                AppendRuntimeGltfEntry(manifest,
                                       sourceRoot,
                                       outputRoot,
                                       gltfPath,
                                       outputGltf,
                                       false,
                                       rewriteCount,
                                       rewriteError);
                continue;
            }

            std::filesystem::create_directories(outputGltf.parent_path(), ec);
            if (ec)
            {
                AppendRuntimeGltfEntry(manifest,
                                       sourceRoot,
                                       outputRoot,
                                       gltfPath,
                                       outputGltf,
                                       false,
                                       rewriteCount,
                                       "Failed to create runtime glTF directory: " + ec.message());
                continue;
            }

            std::ofstream out(outputGltf, std::ios::binary);
            if (!out.is_open())
            {
                AppendRuntimeGltfEntry(manifest,
                                       sourceRoot,
                                       outputRoot,
                                       gltfPath,
                                       outputGltf,
                                       false,
                                       rewriteCount,
                                       "Failed to open runtime glTF output: " + outputGltf.string());
                continue;
            }

            out << rewrittenText;
            out.close();
            if (!out)
            {
                AppendRuntimeGltfEntry(manifest,
                                       sourceRoot,
                                       outputRoot,
                                       gltfPath,
                                       outputGltf,
                                       false,
                                       rewriteCount,
                                       "Failed while writing runtime glTF output: " + outputGltf.string());
                continue;
            }

            AppendRuntimeGltfEntry(manifest,
                                   sourceRoot,
                                   outputRoot,
                                   gltfPath,
                                   outputGltf,
                                   true,
                                   rewriteCount,
                                   {});
        }
    }

    void PrintUsage()
    {
        std::cout
            << "RVXCook\n"
            << "  --source <dir>        Source asset directory\n"
            << "  --output <dir>        Cooked asset output directory\n"
            << "  --manifest <path>     Manifest path, defaults to <output>/CookManifest.rvxmanifest\n"
            << "  --profile <path>      Versioned cook profile with path-specific options\n"
            << "  --texture-compression <auto|none|bc1|bc3|bc5|bc7>\n"
            << "                        Texture compression profile, defaults to auto\n"
            << "                        Profiles also support mesh.generateTangents,\n"
            << "                        mesh.optimizeMesh, mesh.generateLODs, mesh.lodCount,\n"
            << "                        and mesh.lodReductionFactor\n"
            << "  --rewrite-gltf-texture-uris\n"
            << "                        Emit runtime .gltf copies with external image URIs redirected to cooked .rva textures\n"
            << "  --non-recursive       Only cook files directly under --source\n"
            << "  --fail-on-errors      Return non-zero when any asset fails to cook\n"
            << "  --help                Show this help\n";
    }

    bool ParseOptions(int argc, char** argv, Options& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            const auto requireValue = [&](const char* name) -> const char*
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "Missing value for " << name << "\n";
                    return nullptr;
                }
                return argv[++i];
            };

            if (arg == "--help" || arg == "-h")
            {
                options.showHelp = true;
            }
            else if (arg == "--source")
            {
                const char* value = requireValue("--source");
                if (!value) return false;
                options.sourceRoot = value;
            }
            else if (arg == "--output")
            {
                const char* value = requireValue("--output");
                if (!value) return false;
                options.outputRoot = value;
            }
            else if (arg == "--manifest")
            {
                const char* value = requireValue("--manifest");
                if (!value) return false;
                options.manifestPath = value;
            }
            else if (arg == "--profile")
            {
                const char* value = requireValue("--profile");
                if (!value) return false;
                options.profilePath = value;
            }
            else if (arg == "--texture-compression")
            {
                const char* value = requireValue("--texture-compression");
                if (!value) return false;

                RVX::Tools::TextureCompressionMode mode = RVX::Tools::TextureCompressionMode::Auto;
                if (!ParseTextureCompressionMode(value, mode))
                {
                    std::cerr << "Unsupported texture compression profile: " << value << "\n";
                    return false;
                }

                SetTextureCompressionMode(options.textureOptions, mode);
                options.hasTextureOptions = true;
            }
            else if (arg == "--non-recursive")
            {
                options.recursive = false;
            }
            else if (arg == "--rewrite-gltf-texture-uris")
            {
                options.rewriteGltfTextureUris = true;
            }
            else if (arg == "--fail-on-errors")
            {
                options.failOnErrors = true;
            }
            else
            {
                std::cerr << "Unknown argument: " << arg << "\n";
                return false;
            }
        }

        if (options.showHelp)
        {
            return true;
        }

        if (options.sourceRoot.empty())
        {
            std::cerr << "--source is required\n";
            return false;
        }
        if (options.outputRoot.empty())
        {
            std::cerr << "--output is required\n";
            return false;
        }
        if (options.manifestPath.empty())
        {
            options.manifestPath = options.outputRoot / "CookManifest.rvxmanifest";
        }

        return true;
    }

    RVX::Tools::AssetPipeline CreateDefaultPipeline()
    {
        RVX::Tools::AssetPipeline pipeline;
        pipeline.RegisterImporter(std::make_unique<RVX::Tools::TextureImporter>());
        pipeline.RegisterImporter(std::make_unique<RVX::Tools::MeshImporter>());
        pipeline.RegisterImporter(std::make_unique<RVX::Tools::ShaderImporter>());
        pipeline.RegisterImporter(std::make_unique<RVX::Tools::AudioImporter>());
        return pipeline;
    }
} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        PrintUsage();
        return 1;
    }

    if (options.showHelp)
    {
        PrintUsage();
        return 0;
    }

    CookProfile cookProfile;
    if (!options.profilePath.empty())
    {
        std::string profileError;
        if (!LoadCookProfile(options.profilePath, cookProfile, profileError))
        {
            std::cerr << profileError << "\n";
            return 1;
        }

        if (cookProfile.hasDefaultTextureCompression && !options.hasTextureOptions)
        {
            SetTextureCompressionMode(options.textureOptions, cookProfile.defaultTextureCompression);
            options.hasTextureOptions = true;
        }
    }

    RVX::Log::Initialize();

    try
    {
        RVX::Tools::AssetPipeline pipeline = CreateDefaultPipeline();
        RVX::Tools::AssetPipeline::ImportOptionsProvider optionsProvider;
        RVX::Tools::TextureImportOptions resolvedTextureOptions = options.textureOptions;
        RVX::Tools::MeshImportOptions resolvedMeshOptions = cookProfile.meshOptions;
        if (options.hasTextureOptions ||
            !cookProfile.textureCompressionRules.empty() ||
            cookProfile.hasMeshOptions)
        {
            optionsProvider = [&options,
                               &cookProfile,
                               &resolvedTextureOptions,
                               &resolvedMeshOptions](const std::filesystem::path& sourcePath,
                                                     RVX::Tools::AssetType assetType) -> const void*
            {
                if (assetType == RVX::Tools::AssetType::Mesh)
                {
                    if (!cookProfile.hasMeshOptions)
                    {
                        return nullptr;
                    }

                    resolvedMeshOptions = cookProfile.meshOptions;
                    return &resolvedMeshOptions;
                }

                if (assetType != RVX::Tools::AssetType::Texture)
                {
                    return nullptr;
                }

                if (!options.hasTextureOptions && cookProfile.textureCompressionRules.empty())
                {
                    return nullptr;
                }

                resolvedTextureOptions = options.textureOptions;
                if (auto profileMode = MatchTextureCompressionRule(cookProfile, options.sourceRoot, sourcePath))
                {
                    SetTextureCompressionMode(resolvedTextureOptions, *profileMode);
                }

                return &resolvedTextureOptions;
            };
        }

        const std::filesystem::path pipelineManifestPath =
            options.rewriteGltfTextureUris ? std::filesystem::path{} : options.manifestPath;

        RVX::Tools::CookManifest manifest =
            pipeline.CookDirectory(options.sourceRoot,
                                   options.outputRoot,
                                   options.recursive,
                                   pipelineManifestPath,
                                   nullptr,
                                   optionsProvider);

        if (!manifest.manifestError.empty() && manifest.entries.empty())
        {
            std::cerr << "Cook failed: " << manifest.manifestError << "\n";
            RVX::Log::Shutdown();
            return 2;
        }

        if (options.rewriteGltfTextureUris)
        {
            RewriteRuntimeGltfTextureUris(manifest,
                                          options.sourceRoot,
                                          options.outputRoot,
                                          options.recursive);
            manifest.manifestWritten = manifest.Save(options.manifestPath, manifest.manifestError);
        }

        if (!manifest.manifestWritten)
        {
            std::cerr << "Cook manifest write failed: " << manifest.manifestError << "\n";
            RVX::Log::Shutdown();
            return 2;
        }

        std::cout << "Cook complete\n"
                  << "  Source: " << manifest.sourceRoot << "\n"
                  << "  Output: " << manifest.outputRoot << "\n"
                  << "  Manifest: " << std::filesystem::absolute(options.manifestPath).generic_string() << "\n"
                  << "  Entries: " << manifest.entries.size() << "\n"
                  << "  Success: " << manifest.GetSuccessCount() << "\n"
                  << "  Failed: " << manifest.GetFailureCount() << "\n";

        if (options.failOnErrors && manifest.GetFailureCount() > 0)
        {
            RVX::Log::Shutdown();
            return 3;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Cook failed: " << e.what() << "\n";
        RVX::Log::Shutdown();
        return 4;
    }

    RVX::Log::Shutdown();
    return 0;
}
