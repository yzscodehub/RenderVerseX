#include "Resource/Loader/TextureLoader.h"
#include "Resource/DefaultResources.h"
#include "Resource/ResourceCache.h"
#include "Core/Log.h"

#include <stb_image.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace RVX::Resource
{
    namespace
    {
        Diagnostics::TraceContext ResolveTraceContext(
            ResourceManager* manager,
            const Diagnostics::TraceContext& requested)
        {
            if (requested.IsEnabled())
            {
                return requested;
            }
            return manager != nullptr ? manager->GetStartupTraceContext()
                                      : Diagnostics::TraceContext{};
        }

        float ByteToUnit(uint8_t value)
        {
            return static_cast<float>(value) / 255.0f;
        }

        uint8_t UnitToByte(float value)
        {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            return static_cast<uint8_t>(std::round(clamped * 255.0f));
        }

        float SRGBToLinear(uint8_t value)
        {
            const float srgb = ByteToUnit(value);
            if (srgb <= 0.04045f)
            {
                return srgb / 12.92f;
            }

            return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
        }

        uint8_t LinearToSRGBByte(float value)
        {
            const float linear = std::clamp(value, 0.0f, 1.0f);
            const float srgb = linear <= 0.0031308f
                                   ? linear * 12.92f
                                   : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            return UnitToByte(srgb);
        }

        uint32_t CalculateMipLevelCount(uint32_t width, uint32_t height)
        {
            uint32_t mipLevels = 1;
            while (width > 1 || height > 1)
            {
                width = std::max(1u, width >> 1u);
                height = std::max(1u, height >> 1u);
                ++mipLevels;
            }
            return mipLevels;
        }

        size_t PixelOffset(uint32_t x, uint32_t y, uint32_t width)
        {
            return (static_cast<size_t>(y) * width + x) * 4u;
        }

        std::vector<uint8_t> GenerateNextMip(
            std::span<const uint8_t> source,
            uint32_t sourceWidth,
            uint32_t sourceHeight,
            TextureUsage usage,
            bool isSRGB,
            const std::function<bool()>& isCancellationRequested,
            bool& outCancelled)
        {
            const uint32_t targetWidth = std::max(1u, sourceWidth >> 1u);
            const uint32_t targetHeight = std::max(1u, sourceHeight >> 1u);
            std::vector<uint8_t> target(static_cast<size_t>(targetWidth) * targetHeight * 4u);

            for (uint32_t y = 0; y < targetHeight; ++y)
            {
                if (isCancellationRequested && isCancellationRequested())
                {
                    outCancelled = true;
                    return {};
                }
                const uint32_t sourceY0 = (y * sourceHeight) / targetHeight;
                const uint32_t sourceY1 = std::max(sourceY0 + 1u, ((y + 1u) * sourceHeight + targetHeight - 1u) / targetHeight);
                for (uint32_t x = 0; x < targetWidth; ++x)
                {
                    const uint32_t sourceX0 = (x * sourceWidth) / targetWidth;
                    const uint32_t sourceX1 = std::max(sourceX0 + 1u, ((x + 1u) * sourceWidth + targetWidth - 1u) / targetWidth);

                    float accum[4] = {};
                    uint32_t sampleCount = 0;
                    for (uint32_t sy = sourceY0; sy < sourceY1 && sy < sourceHeight; ++sy)
                    {
                        for (uint32_t sx = sourceX0; sx < sourceX1 && sx < sourceWidth; ++sx)
                        {
                            const size_t src = PixelOffset(sx, sy, sourceWidth);
                            if (usage == TextureUsage::Normal)
                            {
                                accum[0] += ByteToUnit(source[src + 0]) * 2.0f - 1.0f;
                                accum[1] += ByteToUnit(source[src + 1]) * 2.0f - 1.0f;
                                accum[2] += ByteToUnit(source[src + 2]) * 2.0f - 1.0f;
                            }
                            else if (usage == TextureUsage::Color && isSRGB)
                            {
                                accum[0] += SRGBToLinear(source[src + 0]);
                                accum[1] += SRGBToLinear(source[src + 1]);
                                accum[2] += SRGBToLinear(source[src + 2]);
                            }
                            else
                            {
                                accum[0] += ByteToUnit(source[src + 0]);
                                accum[1] += ByteToUnit(source[src + 1]);
                                accum[2] += ByteToUnit(source[src + 2]);
                            }
                            accum[3] += ByteToUnit(source[src + 3]);
                            ++sampleCount;
                        }
                    }

                    const float invSampleCount = sampleCount > 0 ? 1.0f / static_cast<float>(sampleCount) : 0.0f;
                    const size_t dst = PixelOffset(x, y, targetWidth);
                    if (usage == TextureUsage::Normal)
                    {
                        float nx = accum[0] * invSampleCount;
                        float ny = accum[1] * invSampleCount;
                        float nz = accum[2] * invSampleCount;
                        const float lengthSq = nx * nx + ny * ny + nz * nz;
                        if (lengthSq <= 1.0e-8f)
                        {
                            nx = 0.0f;
                            ny = 0.0f;
                            nz = 1.0f;
                        }
                        else
                        {
                            const float invLength = 1.0f / std::sqrt(lengthSq);
                            nx *= invLength;
                            ny *= invLength;
                            nz *= invLength;
                        }
                        target[dst + 0] = UnitToByte(nx * 0.5f + 0.5f);
                        target[dst + 1] = UnitToByte(ny * 0.5f + 0.5f);
                        target[dst + 2] = UnitToByte(nz * 0.5f + 0.5f);
                    }
                    else if (usage == TextureUsage::Color && isSRGB)
                    {
                        target[dst + 0] = LinearToSRGBByte(accum[0] * invSampleCount);
                        target[dst + 1] = LinearToSRGBByte(accum[1] * invSampleCount);
                        target[dst + 2] = LinearToSRGBByte(accum[2] * invSampleCount);
                    }
                    else
                    {
                        target[dst + 0] = UnitToByte(accum[0] * invSampleCount);
                        target[dst + 1] = UnitToByte(accum[1] * invSampleCount);
                        target[dst + 2] = UnitToByte(accum[2] * invSampleCount);
                    }
                    target[dst + 3] = UnitToByte(accum[3] * invSampleCount);
                }
            }

            return target;
        }

        std::vector<uint8_t> BuildMipChain(std::vector<uint8_t> basePixels,
                                           uint32_t width,
                                           uint32_t height,
                                           TextureUsage usage,
                                           bool isSRGB,
                                           uint32_t& outMipLevels,
                                           const std::function<bool()>&
                                               isCancellationRequested = {},
                                           bool* outCancelled = nullptr)
        {
            bool cancelled = false;
            if (isCancellationRequested && isCancellationRequested())
            {
                cancelled = true;
            }
            outMipLevels = CalculateMipLevelCount(width, height);
            if (cancelled || outMipLevels <= 1)
            {
                if (outCancelled != nullptr)
                {
                    *outCancelled = cancelled;
                }
                return basePixels;
            }

            size_t totalBytes = 0;
            uint32_t reserveWidth = width;
            uint32_t reserveHeight = height;
            for (uint32_t level = 0; level < outMipLevels; ++level)
            {
                totalBytes += static_cast<size_t>(reserveWidth) *
                              reserveHeight * 4u;
                reserveWidth = std::max(1u, reserveWidth >> 1u);
                reserveHeight = std::max(1u, reserveHeight >> 1u);
            }

            std::vector<uint8_t> mipChain = std::move(basePixels);
            mipChain.reserve(totalBytes);
            size_t previousOffset = 0;
            uint32_t previousWidth = width;
            uint32_t previousHeight = height;

            for (uint32_t mipLevel = 1; mipLevel < outMipLevels; ++mipLevel)
            {
                const size_t previousBytes =
                    static_cast<size_t>(previousWidth) *
                    previousHeight * 4u;
                const std::span<const uint8_t> previous(
                    mipChain.data() + previousOffset,
                    previousBytes);
                std::vector<uint8_t> next =
                    GenerateNextMip(previous,
                                    previousWidth,
                                    previousHeight,
                                    usage,
                                    isSRGB,
                                    isCancellationRequested,
                                    cancelled);
                if (cancelled)
                {
                    if (outCancelled != nullptr)
                    {
                        *outCancelled = true;
                    }
                    return {};
                }
                previousOffset = mipChain.size();
                mipChain.insert(mipChain.end(), next.begin(), next.end());
                previousWidth = std::max(1u, previousWidth >> 1u);
                previousHeight = std::max(1u, previousHeight >> 1u);
            }

            if (outCancelled != nullptr)
            {
                *outCancelled = false;
            }
            return mipChain;
        }

        struct CancellableImageReader
        {
            const stbi_uc* data = nullptr;
            size_t size = 0;
            size_t offset = 0;
            const std::function<bool()>* isCancellationRequested = nullptr;
        };

        bool IsImageReaderCancelled(const CancellableImageReader& reader)
        {
            return reader.isCancellationRequested != nullptr &&
                   *reader.isCancellationRequested &&
                   (*reader.isCancellationRequested)();
        }

        int ReadCancellableImage(void* user, char* destination, int count)
        {
            auto& reader = *static_cast<CancellableImageReader*>(user);
            if (count <= 0 || IsImageReaderCancelled(reader) ||
                reader.offset >= reader.size)
            {
                return 0;
            }
            const size_t available = reader.size - reader.offset;
            const size_t copied = std::min(available, static_cast<size_t>(count));
            std::memcpy(destination, reader.data + reader.offset, copied);
            reader.offset += copied;
            return static_cast<int>(copied);
        }

        void SkipCancellableImage(void* user, int count)
        {
            auto& reader = *static_cast<CancellableImageReader*>(user);
            if (IsImageReaderCancelled(reader))
            {
                reader.offset = reader.size;
                return;
            }
            if (count < 0)
            {
                const size_t rewind = std::min(
                    reader.offset,
                    static_cast<size_t>(-static_cast<int64>(count)));
                reader.offset -= rewind;
                return;
            }
            reader.offset = std::min(
                reader.size,
                reader.offset + static_cast<size_t>(count));
        }

        int IsCancellableImageEof(void* user)
        {
            const auto& reader = *static_cast<CancellableImageReader*>(user);
            return IsImageReaderCancelled(reader) || reader.offset >= reader.size;
        }

        std::optional<uint32_t> ParseUint32Field(
            const std::unordered_map<std::string, std::string>& fields,
            const std::string& key)
        {
            auto it = fields.find(key);
            if (it == fields.end())
            {
                return std::nullopt;
            }

            try
            {
                return static_cast<uint32_t>(std::stoul(it->second));
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::optional<bool> ParseBoolField(
            const std::unordered_map<std::string, std::string>& fields,
            const std::string& key)
        {
            auto value = ParseUint32Field(fields, key);
            if (!value)
            {
                return std::nullopt;
            }
            return *value != 0;
        }

        std::optional<TextureFormat> ParseTextureFormat(const std::string& value)
        {
            if (value == "RGBA8") return TextureFormat::RGBA8;
            if (value == "RGBA16F") return TextureFormat::RGBA16F;
            if (value == "RGBA32F") return TextureFormat::RGBA32F;
            if (value == "RGB8") return TextureFormat::RGB8;
            if (value == "RG8") return TextureFormat::RG8;
            if (value == "R8") return TextureFormat::R8;
            if (value == "BC1") return TextureFormat::BC1;
            if (value == "BC3") return TextureFormat::BC3;
            if (value == "BC5") return TextureFormat::BC5;
            if (value == "BC7") return TextureFormat::BC7;
            return std::nullopt;
        }

        std::optional<TextureUsage> ParseTextureUsage(const std::string& value)
        {
            if (value == "Color") return TextureUsage::Color;
            if (value == "Normal") return TextureUsage::Normal;
            if (value == "Data") return TextureUsage::Data;
            return std::nullopt;
        }

        const char* ToCompressionString(TextureFormat format)
        {
            switch (format)
            {
                case TextureFormat::BC1: return "BC1";
                case TextureFormat::BC3: return "BC3";
                case TextureFormat::BC5: return "BC5";
                case TextureFormat::BC7: return "BC7";
                default:                 return "";
            }
        }

        std::optional<size_t> GetExpectedCookedTextureDataSize(uint32_t width,
                                                               uint32_t height,
                                                               uint32_t mipLevels,
                                                               TextureFormat format)
        {
            size_t totalSize = 0;
            for (uint32_t mip = 0; mip < mipLevels; ++mip)
            {
                const uint32_t mipWidth = std::max(1u, width >> mip);
                const uint32_t mipHeight = std::max(1u, height >> mip);
                if (format == TextureFormat::RGBA8)
                {
                    totalSize += static_cast<size_t>(mipWidth) * mipHeight * 4u;
                }
                else if (format == TextureFormat::BC1)
                {
                    const uint32_t blocksX = (mipWidth + 3u) / 4u;
                    const uint32_t blocksY = (mipHeight + 3u) / 4u;
                    totalSize += static_cast<size_t>(blocksX) * blocksY * 8u;
                }
                else if (format == TextureFormat::BC3 ||
                         format == TextureFormat::BC5 ||
                         format == TextureFormat::BC7)
                {
                    const uint32_t blocksX = (mipWidth + 3u) / 4u;
                    const uint32_t blocksY = (mipHeight + 3u) / 4u;
                    totalSize += static_cast<size_t>(blocksX) * blocksY * 16u;
                }
                else
                {
                    return std::nullopt;
                }
            }
            return totalSize;
        }

        bool ParseCookedTextureArtifact(std::span<const uint8_t> fileData,
                                        TextureMetadata& outMetadata,
                                        std::vector<uint8_t>& outPixels,
                                        std::string& outError)
        {
            constexpr const char* magic = "RVX_TEXTURE_PREBAKE_V1\n";
            constexpr const char* dataMarker = "RVX_TEXTURE_DATA_BEGIN\n";
            constexpr const char* endMarker = "\nRVX_TEXTURE_PREBAKE_END\n";

            const std::string_view bytes(reinterpret_cast<const char*>(fileData.data()), fileData.size());
            if (!bytes.starts_with(magic))
            {
                outError = "Texture artifact missing RVX_TEXTURE_PREBAKE_V1 magic";
                return false;
            }

            const size_t dataMarkerOffset = bytes.find(dataMarker);
            if (dataMarkerOffset == std::string_view::npos)
            {
                outError = "Texture artifact missing data marker";
                return false;
            }

            std::unordered_map<std::string, std::string> fields;
            const std::string metadataText(bytes.substr(std::strlen(magic),
                                                        dataMarkerOffset - std::strlen(magic)));
            std::istringstream metadataStream(metadataText);
            std::string line;
            while (std::getline(metadataStream, line))
            {
                const size_t separator = line.find('=');
                if (separator == std::string::npos)
                {
                    continue;
                }
                fields[line.substr(0, separator)] = line.substr(separator + 1);
            }

            const auto width = ParseUint32Field(fields, "width");
            const auto height = ParseUint32Field(fields, "height");
            const auto depth = ParseUint32Field(fields, "depth");
            const auto mipLevels = ParseUint32Field(fields, "mipLevels");
            const auto arrayLayers = ParseUint32Field(fields, "arrayLayers");
            const auto isSRGB = ParseBoolField(fields, "srgb");
            const auto dataSize = ParseUint32Field(fields, "dataSize");
            auto formatIt = fields.find("format");
            auto usageIt = fields.find("usage");
            auto compressionIt = fields.find("compression");

            if (!width || !height || !depth || !mipLevels || !arrayLayers || !isSRGB ||
                !dataSize || formatIt == fields.end() || usageIt == fields.end() ||
                compressionIt == fields.end())
            {
                outError = "Texture artifact metadata is incomplete";
                return false;
            }

            const std::optional<TextureFormat> format = ParseTextureFormat(formatIt->second);
            const std::optional<TextureUsage> usage = ParseTextureUsage(usageIt->second);
            if (!format || !usage)
            {
                outError = "Texture artifact contains unsupported format or usage";
                return false;
            }

            if (*format == TextureFormat::RGBA8)
            {
                if (compressionIt->second != "UncompressedRGBA")
                {
                    outError = "Texture artifact RGBA8 payload must use UncompressedRGBA compression";
                    return false;
                }
            }
            else
            {
                const char* expectedCompression = ToCompressionString(*format);
                if (expectedCompression[0] == '\0' || compressionIt->second != expectedCompression)
                {
                    outError = "Texture artifact compression does not match compressed texture format";
                    return false;
                }
            }

            const std::optional<size_t> expectedDataSize =
                GetExpectedCookedTextureDataSize(*width, *height, *mipLevels, *format);
            if (!expectedDataSize)
            {
                outError = "Texture artifact contains unsupported runtime format";
                return false;
            }
            if (*dataSize != *expectedDataSize)
            {
                outError = "Texture artifact dataSize does not match format and mip layout";
                return false;
            }

            const size_t payloadOffset = dataMarkerOffset + std::strlen(dataMarker);
            if (payloadOffset + *dataSize > fileData.size())
            {
                outError = "Texture artifact data payload is truncated";
                return false;
            }

            const size_t endMarkerOffset = payloadOffset + *dataSize;
            if (endMarkerOffset + std::strlen(endMarker) > fileData.size() ||
                std::string_view(reinterpret_cast<const char*>(fileData.data() + endMarkerOffset),
                                 std::strlen(endMarker)) != endMarker)
            {
                outError = "Texture artifact missing end marker at expected payload boundary";
                return false;
            }

            outMetadata = {};
            outMetadata.width = *width;
            outMetadata.height = *height;
            outMetadata.depth = *depth;
            outMetadata.mipLevels = *mipLevels;
            outMetadata.arrayLayers = *arrayLayers;
            outMetadata.format = *format;
            outMetadata.usage = *usage;
            outMetadata.isSRGB = *isSRGB;

            outPixels.assign(fileData.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
                             fileData.begin() + static_cast<std::ptrdiff_t>(payloadOffset + *dataSize));
            return true;
        }
    } // namespace

    // =========================================================================
    // Construction
    // =========================================================================

    TextureLoader::TextureLoader(ResourceManager* manager, bool prepareOnly)
        : m_manager(manager)
        , m_prepareOnly(prepareOnly)
    {
    }

    // =========================================================================
    // IResourceLoader Interface
    // =========================================================================

    std::vector<std::string> TextureLoader::GetSupportedExtensions() const
    {
        return { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".gif", ".hdr", ".rva" };
    }

    bool TextureLoader::CanLoad(const std::string& path) const
    {
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        auto extensions = GetSupportedExtensions();
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
    }

    IResource* TextureLoader::Load(const std::string& path)
    {
        // Resolve to absolute path
        std::filesystem::path absPath = std::filesystem::absolute(path);
        const TextureUsage usage = InferTextureUsageFromPath(absPath.string());
        const bool isSRGB = usage == TextureUsage::Color;
        m_lastLoadStatus = TextureLoadStatus::None;
        m_lastLoadError.clear();
        return LoadFromFileWithPolicy(absPath.string(),
                                      usage,
                                      isSRGB,
                                      absPath.string(),
                                      ResolveTraceContext(m_manager, {}));
    }

    bool TextureLoader::Prepare(const ResourceLoadPreparationContext& context,
                                PreparedResourceBundle& outBundle,
                                ResourceLoadError& outError)
    {
        if (context.IsCancellationRequested())
        {
            outError = {ResourceLoadErrorCode::Cancelled, "Texture load was cancelled before decode."};
            return false;
        }

        // Use a per-request loader with no ResourceManager. This prevents this
        // worker from reaching cache, registry, lifecycle notifications or the
        // render upload gateway while keeping all decode helpers reusable.
        TextureLoader preparedLoader(nullptr, true);
        IResource* raw = preparedLoader.LoadFromFile(context.resolvedPath, context.traceContext);
        if (!raw)
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        preparedLoader.GetLastLoadError().empty()
                            ? "Texture decode failed."
                            : preparedLoader.GetLastLoadError()};
            return false;
        }
        if (context.IsCancellationRequested())
        {
            delete raw;
            outError = {ResourceLoadErrorCode::Cancelled, "Texture load was cancelled after decode."};
            return false;
        }

        raw->SetId(context.rootResourceId);
        raw->SetPath(context.requestedPath);
        raw->SetName(std::filesystem::path(context.requestedPath).stem().string());
        ResourceHandle<IResource> preparedResource(raw);
        if (!outBundle.SetRoot(std::move(preparedResource)))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "Texture loader could not construct a valid prepared bundle."};
            return false;
        }
        return true;
    }

    // =========================================================================
    // Extended Loading API
    // =========================================================================

    TextureResource* TextureLoader::LoadFromReference(const TextureReference& ref,
                                                        const std::string& modelPath,
                                                        const Diagnostics::TraceContext& traceContext,
                                                        const std::string& resourceIdentityBase)
    {
        const Diagnostics::TraceContext activeTraceContext =
            ResolveTraceContext(m_manager, traceContext);
        m_lastLoadStatus = TextureLoadStatus::None;
        m_lastLoadError.clear();

        if (!ref.IsValid())
        {
            m_lastLoadStatus = TextureLoadStatus::FallbackInvalidReference;
            m_lastLoadError = "Invalid texture reference";
            RVX_CORE_WARN("TextureLoader: Invalid texture reference; using default texture");
            return GetDefaultTexture(ref.usage);
        }

        const std::string sourceKey = ref.GetUniqueKey(modelPath);
        const std::string identityKey = resourceIdentityBase.empty()
            ? sourceKey
            : resourceIdentityBase + "#texture_" + std::to_string(ref.imageIndex) +
                  "_usage_" + std::to_string(static_cast<uint32>(ref.usage));
        const std::string cacheKey = BuildTexturePolicyCacheKey(identityKey, ref.usage, ref.isSRGB);
        ResourceId textureId = GenerateTextureId(cacheKey);

        // Check cache first
        {
            Diagnostics::TraceSpan cacheLookupSpan = Diagnostics::BeginTraceSpan(
                activeTraceContext,
                "CacheLookup",
                {{"assetId", textureId}, {"path", sourceKey}});
            if (m_manager && m_manager->IsInitialized())
            {
                if (auto* cached = m_manager->GetCache().Get(textureId))
                {
                    cacheLookupSpan.SetAttribute("cacheHit", true);
                    m_lastLoadStatus = TextureLoadStatus::Loaded;
                    m_lastLoadError.clear();
                    return static_cast<TextureResource*>(cached);
                }
            }
            cacheLookupSpan.SetAttribute("cacheHit", false);
        }

        TextureResource* texture = nullptr;

        if (ref.HasCapturedPayload())
        {
            const std::vector<uint8_t>& payload = ref.GetCapturedPayload();
            texture = LoadFromMemoryWithPolicy(payload.data(),
                                                payload.size(),
                                                sourceKey,
                                                cacheKey,
                                                ref.usage,
                                                ref.isSRGB,
                                                ref.isRawPixelData,
                                                ref.rawWidth,
                                                ref.rawHeight,
                                                activeTraceContext);
        }
        else if (ref.IsExternal())
        {
            texture = LoadFromFileWithPolicy(sourceKey,
                                              ref.usage,
                                              ref.isSRGB,
                                              cacheKey,
                                              activeTraceContext);
        }
        else
        {
            // Embedded texture
            if (ref.isRawPixelData)
            {
                // Already decoded raw RGBA pixels (from tinygltf)
                texture = LoadFromMemoryWithPolicy(ref.embeddedData.data(), ref.embeddedData.size(),
                                                    sourceKey, cacheKey, ref.usage, ref.isSRGB,
                                                    true, ref.rawWidth, ref.rawHeight,
                                                    activeTraceContext);
            }
            else
            {
                // Encoded image data (PNG/JPEG) - needs decoding
                texture = LoadFromMemoryWithPolicy(ref.embeddedData.data(), ref.embeddedData.size(),
                                                    sourceKey, cacheKey, ref.usage, ref.isSRGB,
                                                    false, 0, 0, activeTraceContext);
            }
        }

        if (texture)
        {
            m_lastLoadStatus = TextureLoadStatus::Loaded;
        }
        else
        {
            m_lastLoadStatus = TextureLoadStatus::FallbackLoadFailed;
            m_lastLoadError = "Failed to load texture source; using default texture";
            RVX_CORE_WARN("TextureLoader: Failed to load texture, using default");
            texture = GetDefaultTexture(ref.usage);
        }

        return texture;
    }

    TextureResource* TextureLoader::CreateStreamingPlaceholder(
        TextureReference& ref,
        const std::string& modelPath,
        const std::string& resourceIdentityBase)
    {
        if (!ref.IsValid())
            return nullptr;

        const std::string sourceKey = ref.GetUniqueKey(modelPath);
        const std::string identityKey = resourceIdentityBase.empty()
            ? sourceKey
            : resourceIdentityBase + "#texture_" +
                  std::to_string(ref.imageIndex) + "_usage_" +
                  std::to_string(static_cast<uint32>(ref.usage));
        const std::string cacheKey =
            BuildTexturePolicyCacheKey(identityKey, ref.usage, ref.isSRGB);
        const DefaultTexturePayload fallback =
            DefaultResources::GetTexturePayload(
                ref.fallbackSemantic,
                ref.usage,
                ref.isSRGB);

        auto* texture = new TextureResource();
        texture->SetId(GenerateTextureId(cacheKey));
        texture->SetPath(sourceKey);
        texture->SetName(std::filesystem::path(sourceKey).stem().string());
        texture->SetDataStorage(fallback.bytes, fallback.metadata);
        texture->SetEncodedSourceStorage(ref.ShareCapturedPayload());
        texture->MarkStreamingPlaceholder(true);
        return texture;
    }

    bool TextureLoader::RequiresDeferredDecode(
        const TextureReference& ref) const
    {
        if (ref.isRawPixelData)
            return false;

        std::string extension =
            std::filesystem::path(ref.path).extension().string();
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char value)
                       {
                           return static_cast<char>(std::tolower(value));
                       });
        return extension != ".rva";
    }

    bool TextureLoader::EstimateDecodedByteSize(
        const TextureReference& ref,
        const std::string& modelPath,
        uint64& outBytes,
        std::string& outError) const
    {
        outBytes = 0;
        outError.clear();
        if (!ref.IsValid())
        {
            outError = "Texture reference is invalid";
            return false;
        }

        uint32 width = ref.rawWidth;
        uint32 height = ref.rawHeight;
        if (!ref.isRawPixelData)
        {
            if (!ref.HasCapturedPayload())
            {
                outError = "Streaming texture has no importer-captured encoded payload: " +
                           ref.GetUniqueKey(modelPath);
                return false;
            }
            int decodedWidth = 0;
            int decodedHeight = 0;
            int channels = 0;
            const std::vector<uint8_t>& payload = ref.GetCapturedPayload();
            if (payload.size() >
                static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                outError = "Encoded texture exceeds stb_image input limits: " +
                           ref.GetUniqueKey(modelPath);
                return false;
            }
            if (stbi_info_from_memory(
                    reinterpret_cast<const stbi_uc*>(payload.data()),
                    static_cast<int>(payload.size()),
                    &decodedWidth,
                    &decodedHeight,
                    &channels) == 0 ||
                decodedWidth <= 0 || decodedHeight <= 0)
            {
                outError = "Texture header probe failed: " +
                           ref.GetUniqueKey(modelPath);
                return false;
            }
            width = static_cast<uint32>(decodedWidth);
            height = static_cast<uint32>(decodedHeight);
        }
        if (width == 0 || height == 0)
        {
            outError = "Texture dimensions are zero";
            return false;
        }

        uint64 total = 0;
        uint32 mipWidth = width;
        uint32 mipHeight = height;
        for (;;)
        {
            const uint64 levelBytes =
                static_cast<uint64>(mipWidth) * mipHeight * 4ull;
            if (levelBytes > std::numeric_limits<uint64>::max() - total)
            {
                outError = "Texture decoded-size estimate overflowed";
                return false;
            }
            total += levelBytes;
            if (mipWidth == 1 && mipHeight == 1)
                break;
            mipWidth = std::max(1u, mipWidth / 2u);
            mipHeight = std::max(1u, mipHeight / 2u);
        }
        const uint64 baseBytes =
            static_cast<uint64>(width) * height * 4ull;
        if (baseBytes > std::numeric_limits<uint64>::max() - total)
        {
            outError = "Texture decode working-set estimate overflowed";
            return false;
        }
        // stb_image temporarily owns the base RGBA allocation while it is
        // copied, and mip construction reserves the final chain while the
        // base vector is still alive. finalChain + base is therefore the
        // conservative per-task decoded working-set admission cost.
        outBytes = total + baseBytes;
        return true;
    }

    bool TextureLoader::DecodeReference(
        const TextureReference& ref,
        const std::string& modelPath,
        const Diagnostics::TraceContext& traceContext,
        DecodedTextureData& outData,
        std::string& outError,
        const std::function<bool()>& isCancellationRequested)
    {
        outData = {};
        outError.clear();
        if (isCancellationRequested && isCancellationRequested())
        {
            outError = "Texture decode cancelled before it began";
            return false;
        }
        if (!ref.IsValid() || !ref.HasCapturedPayload())
        {
            outError = "Texture decode requires importer-captured bytes";
            return false;
        }

        std::vector<uint8_t> pixels;
        uint32 width = ref.rawWidth;
        uint32 height = ref.rawHeight;
        int channels = 4;
        if (ref.isRawPixelData)
        {
            pixels = ref.GetCapturedPayload();
            if (isCancellationRequested && isCancellationRequested())
            {
                outError = "Texture decode cancelled while copying raw pixels";
                return false;
            }
        }
        else
        {
            const std::vector<uint8_t>& payload = ref.GetCapturedPayload();
            if (!DecodeImage(payload.data(),
                              payload.size(),
                              pixels,
                              width,
                              height,
                              channels,
                              ref.GetUniqueKey(modelPath),
                              traceContext,
                              isCancellationRequested))
            {
                outError = "Texture decode failed: " + ref.GetUniqueKey(modelPath);
                return false;
            }
        }

        TextureMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.format = TextureFormat::RGBA8;
        metadata.mipLevels = 1;
        metadata.usage = ref.usage;
        metadata.isSRGB = ref.isSRGB;
        const size_t expectedBaseSize =
            static_cast<size_t>(width) * height * 4u;
        if (pixels.size() != expectedBaseSize)
        {
            outError = "Decoded texture byte count does not match RGBA dimensions";
            return false;
        }
        bool cancelledWhileBuildingMips = false;
        pixels = BuildMipChain(std::move(pixels),
                               width,
                               height,
                               ref.usage,
                               ref.isSRGB,
                               metadata.mipLevels,
                               isCancellationRequested,
                               &cancelledWhileBuildingMips);
        if (cancelledWhileBuildingMips ||
            (isCancellationRequested && isCancellationRequested()))
        {
            outError = "Texture decode cancelled while generating mip levels";
            return false;
        }
        outData.bytes =
            std::make_shared<const std::vector<uint8_t>>(std::move(pixels));
        outData.metadata = metadata;
        outData.sourceKey = ref.GetUniqueKey(modelPath);
        return outData.IsValid();
    }

    TextureResource* TextureLoader::LoadFromFile(
        const std::string& absolutePath,
        const Diagnostics::TraceContext& traceContext)
    {
        m_lastLoadStatus = TextureLoadStatus::None;
        m_lastLoadError.clear();

        const TextureUsage usage = InferTextureUsageFromPath(absolutePath);
        const bool isSRGB = usage == TextureUsage::Color;
        const std::string cacheKey = BuildTexturePolicyCacheKey(absolutePath, usage, isSRGB);

        return LoadFromFileWithPolicy(
            absolutePath,
            usage,
            isSRGB,
            cacheKey,
            ResolveTraceContext(m_manager, traceContext));
    }

    TextureResource* TextureLoader::LoadFromFileWithPolicy(const std::string& absolutePath,
                                                            TextureUsage usage,
                                                            bool isSRGB,
                                                            const std::string& cacheKey,
                                                            const Diagnostics::TraceContext& traceContext)
    {
        ResourceId textureId = GenerateTextureId(cacheKey);

        {
            Diagnostics::TraceSpan cacheLookupSpan = Diagnostics::BeginTraceSpan(
                traceContext,
                "CacheLookup",
                {{"assetId", textureId}, {"path", absolutePath}});
            if (m_manager && m_manager->IsInitialized())
            {
                if (auto* cached = m_manager->GetCache().Get(textureId))
                {
                    cacheLookupSpan.SetAttribute("cacheHit", true);
                    m_lastLoadStatus = TextureLoadStatus::Loaded;
                    m_lastLoadError.clear();
                    return static_cast<TextureResource*>(cached);
                }
            }
            cacheLookupSpan.SetAttribute("cacheHit", false);
        }

        // Check if file exists
        if (!std::filesystem::exists(absolutePath))
        {
            m_lastLoadStatus = TextureLoadStatus::Failed;
            m_lastLoadError = "Texture file not found: " + absolutePath;
            RVX_CORE_WARN("TextureLoader: File not found: {}", absolutePath);
            return nullptr;
        }

        // Keep the encoded read separate from decode.  This covers the exact
        // stream read owned by TextureLoader; TinyGLTF's earlier image callback
        // remains explicitly accounted for by its composite BufferRead span.
        Diagnostics::TraceSpan encodedReadSpan = Diagnostics::BeginTraceSpan(
            traceContext,
            "TextureEncodedRead",
            {{"assetId", textureId}, {"path", absolutePath}});
        std::ifstream file(absolutePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            encodedReadSpan.SetAttribute("result", "open-failed");
            m_lastLoadStatus = TextureLoadStatus::Failed;
            m_lastLoadError = "Cannot open texture file: " + absolutePath;
            RVX_CORE_WARN("TextureLoader: Cannot open file: {}", absolutePath);
            return nullptr;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> fileData(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(fileData.data()), size))
        {
            encodedReadSpan.SetAttribute("result", "read-failed");
            m_lastLoadStatus = TextureLoadStatus::Failed;
            m_lastLoadError = "Failed to read texture file: " + absolutePath;
            RVX_CORE_WARN("TextureLoader: Failed to read file: {}", absolutePath);
            return nullptr;
        }
        encodedReadSpan.SetAttribute("bytes", static_cast<uint64>(fileData.size()));
        encodedReadSpan.SetAttribute("result", "read");

        const std::string extension = std::filesystem::path(absolutePath).extension().string();
        std::string lowerExtension = extension;
        std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(), ::tolower);
        if (lowerExtension == ".rva")
        {
            TextureMetadata metadata;
            std::vector<uint8_t> pixels;
            std::string parseError;
            if (!ParseCookedTextureArtifact(fileData, metadata, pixels, parseError))
            {
                m_lastLoadStatus = TextureLoadStatus::Failed;
                m_lastLoadError = parseError + ": " + absolutePath;
                RVX_CORE_WARN("TextureLoader: Failed to parse cooked texture: {}", m_lastLoadError);
                return nullptr;
            }

            auto* texture = new TextureResource();
            texture->SetId(textureId);
            texture->SetPath(absolutePath);
            texture->SetName(std::filesystem::path(absolutePath).stem().string());
            texture->SetData(std::move(pixels), metadata);
            if (!m_prepareOnly)
            {
                texture->NotifyLoaded();
            }

            if (m_manager && m_manager->IsInitialized())
            {
                m_manager->GetCache().Store(texture);
            }

            m_lastLoadStatus = TextureLoadStatus::Loaded;
            m_lastLoadError.clear();
            return texture;
        }

        // Decode image
        std::vector<uint8_t> pixels;
        uint32_t width, height;
        int channels;

        if (!DecodeImage(fileData.data(),
                         fileData.size(),
                         pixels,
                         width,
                         height,
                         channels,
                         absolutePath,
                         traceContext))
        {
            m_lastLoadStatus = TextureLoadStatus::Failed;
            m_lastLoadError = "Failed to decode texture file: " + absolutePath;
            RVX_CORE_WARN("TextureLoader: Failed to decode image: {}", absolutePath);
            return nullptr;
        }

        TextureResource* texture =
            CreateTextureResource(std::move(pixels), width, height, channels, absolutePath, cacheKey, usage, isSRGB);
        m_lastLoadStatus = texture ? TextureLoadStatus::Loaded : TextureLoadStatus::Failed;
        if (!texture)
        {
            m_lastLoadError = "Failed to create texture resource: " + absolutePath;
        }
        return texture;
    }

    TextureResource* TextureLoader::LoadFromMemory(
        const void* data,
        size_t size,
        const std::string& uniqueKey,
        TextureUsage usage,
        bool isRawRGBA,
        uint32_t width,
        uint32_t height,
        const Diagnostics::TraceContext& traceContext)
    {
        m_lastLoadStatus = TextureLoadStatus::None;
        m_lastLoadError.clear();

        const bool isSRGB = usage == TextureUsage::Color;
        const std::string cacheKey = BuildTexturePolicyCacheKey(uniqueKey, usage, isSRGB);
        return LoadFromMemoryWithPolicy(data,
                                        size,
                                        uniqueKey,
                                        cacheKey,
                                        usage,
                                        isSRGB,
                                        isRawRGBA,
                                        width,
                                        height,
                                        ResolveTraceContext(m_manager, traceContext));
    }

    TextureResource* TextureLoader::LoadFromMemoryWithPolicy(const void* data, size_t size,
                                                              const std::string& sourceKey,
                                                              const std::string& cacheKey,
                                                           TextureUsage usage,
                                                           bool isSRGB,
                                                           bool isRawRGBA,
                                                           uint32_t width,
                                                           uint32_t height,
                                                           const Diagnostics::TraceContext& traceContext)
    {
        ResourceId textureId = GenerateTextureId(cacheKey);

        // Check cache
        {
            Diagnostics::TraceSpan cacheLookupSpan = Diagnostics::BeginTraceSpan(
                traceContext,
                "CacheLookup",
                {{"assetId", textureId}, {"path", sourceKey}});
            if (m_manager && m_manager->IsInitialized())
            {
                if (auto* cached = m_manager->GetCache().Get(textureId))
                {
                    cacheLookupSpan.SetAttribute("cacheHit", true);
                    m_lastLoadStatus = TextureLoadStatus::Loaded;
                    m_lastLoadError.clear();
                    return static_cast<TextureResource*>(cached);
                }
            }
            cacheLookupSpan.SetAttribute("cacheHit", false);
        }

        TextureResource* texture = nullptr;
        std::string extension =
            std::filesystem::path(sourceKey).extension().string();
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char value)
                       {
                           return static_cast<char>(std::tolower(value));
                       });
        if (extension == ".rva")
        {
            TextureMetadata metadata;
            std::vector<uint8_t> pixels;
            std::string parseError;
            const auto* bytes = static_cast<const uint8_t*>(data);
            if (!ParseCookedTextureArtifact(
                    std::span<const uint8_t>(bytes, size),
                    metadata,
                    pixels,
                    parseError))
            {
                m_lastLoadStatus = TextureLoadStatus::Failed;
                m_lastLoadError = parseError + ": " + sourceKey;
                return nullptr;
            }

            texture = new TextureResource();
            texture->SetId(textureId);
            texture->SetPath(sourceKey);
            texture->SetName(
                std::filesystem::path(sourceKey).stem().string());
            texture->SetData(std::move(pixels), metadata);
            if (!m_prepareOnly)
                texture->NotifyLoaded();
            if (m_manager && m_manager->IsInitialized())
                m_manager->GetCache().Store(texture);
        }
        else if (isRawRGBA)
        {
            // Raw RGBA data
            std::vector<uint8_t> pixels(static_cast<const uint8_t*>(data),
                                         static_cast<const uint8_t*>(data) + size);
            texture = CreateTextureResource(std::move(pixels), width, height, 4, sourceKey, cacheKey, usage, isSRGB);
        }
        else
        {
            // Encoded image data - decode it
            std::vector<uint8_t> pixels;
            int channels;

            if (!DecodeImage(data,
                             size,
                             pixels,
                             width,
                             height,
                             channels,
                             sourceKey,
                             traceContext))
            {
                RVX_CORE_WARN("TextureLoader: Failed to decode embedded image: {}", sourceKey);
                m_lastLoadStatus = TextureLoadStatus::Failed;
                m_lastLoadError = "Failed to decode embedded texture: " + sourceKey;
                return nullptr;
            }

            texture = CreateTextureResource(std::move(pixels), width, height, channels, sourceKey, cacheKey, usage, isSRGB);
        }

        m_lastLoadStatus = texture ? TextureLoadStatus::Loaded : TextureLoadStatus::Failed;
        if (!texture)
        {
            m_lastLoadError = "Failed to create texture resource: " + sourceKey;
        }
        return texture;
    }

    // =========================================================================
    // Default Textures
    // =========================================================================

    TextureResource* TextureLoader::GetDefaultTexture(TextureUsage usage)
    {
        switch (usage)
        {
            case TextureUsage::Normal:
                return GetNormalTexture();
            case TextureUsage::Color:
            case TextureUsage::Data:
            default:
                return GetWhiteTexture();
        }
    }

    TextureResource* TextureLoader::GetWhiteTexture()
    {
        if (!m_whiteTexture)
        {
            // Create 1x1 white texture
            std::vector<uint8_t> whitePixel = { 255, 255, 255, 255 };
            m_whiteTexture = CreateTextureResource(std::move(whitePixel), 1, 1, 4,
                                                    "__default_white__", "__default_white__",
                                                    TextureUsage::Color, true, false);
            if (m_whiteTexture)
            {
                m_whiteTexture->MarkDefaultFallback("TextureLoader default white fallback");
            }
        }
        return m_whiteTexture;
    }

    TextureResource* TextureLoader::GetNormalTexture()
    {
        if (!m_normalTexture)
        {
            // Create 1x1 flat normal (pointing up in tangent space)
            // Normal: (0, 0, 1) -> encoded as (128, 128, 255)
            std::vector<uint8_t> normalPixel = { 128, 128, 255, 255 };
            m_normalTexture = CreateTextureResource(std::move(normalPixel), 1, 1, 4,
                                                     "__default_normal__", "__default_normal__",
                                                     TextureUsage::Normal, false, false);
            if (m_normalTexture)
            {
                m_normalTexture->SetSRGB(false);
                m_normalTexture->MarkDefaultFallback("TextureLoader default normal fallback");
            }
        }
        return m_normalTexture;
    }

    TextureResource* TextureLoader::GetErrorTexture()
    {
        if (!m_errorTexture)
        {
            // Create 2x2 magenta checkerboard
            std::vector<uint8_t> errorPixels = {
                255, 0, 255, 255,    0, 0, 0, 255,
                0, 0, 0, 255,        255, 0, 255, 255
            };
            m_errorTexture = CreateTextureResource(std::move(errorPixels), 2, 2, 4,
                                                    "__default_error__", "__default_error__",
                                                    TextureUsage::Color, true, false);
            if (m_errorTexture)
            {
                m_errorTexture->MarkDefaultFallback("TextureLoader default error fallback");
            }
        }
        return m_errorTexture;
    }

    // =========================================================================
    // Private Helpers
    // =========================================================================

    bool TextureLoader::DecodeImage(const void* data, size_t size,
                                     std::vector<uint8_t>& outPixels,
                                     uint32_t& outWidth, uint32_t& outHeight,
                                     int& outChannels,
                                     const std::string& sourcePath,
                                     const Diagnostics::TraceContext& traceContext,
                                     const std::function<bool()>&
                                         isCancellationRequested)
    {
        Diagnostics::TraceSpan decodeSpan = Diagnostics::BeginTraceSpan(
            traceContext,
            "TextureDecode",
            {{"path", sourcePath},
             {"bytes", static_cast<uint64>(size)},
             {"decoder", "stb_image"}});
        int width, height, channels;

        if (data == nullptr || size == 0 ||
            size > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            decodeSpan.SetAttribute("result", "invalid-input");
            return false;
        }

        if (isCancellationRequested && isCancellationRequested())
        {
            decodeSpan.SetAttribute("result", "cancelled-before-decode");
            return false;
        }

        // stbi does not accept a cancellation token. Its callback input path
        // does: ending reads when cancelled makes compressed-image decode
        // cooperatively fail at the next decoder input checkpoint.
        CancellableImageReader reader{
            static_cast<const stbi_uc*>(data),
            size,
            0,
            &isCancellationRequested};
        const stbi_io_callbacks callbacks{
            &ReadCancellableImage,
            &SkipCancellableImage,
            &IsCancellableImageEof};
        stbi_uc* pixels = stbi_load_from_callbacks(
            &callbacks,
            &reader,
            &width,
            &height,
            &channels,
            STBI_rgb_alpha);

        if (!pixels)
        {
            decodeSpan.SetAttribute(
                "result",
                isCancellationRequested && isCancellationRequested()
                    ? "cancelled-during-decode"
                    : "failed");
            RVX_CORE_WARN("TextureLoader: stb_image decode failed: {}", stbi_failure_reason());
            return false;
        }

        if (isCancellationRequested && isCancellationRequested())
        {
            stbi_image_free(pixels);
            decodeSpan.SetAttribute("result", "cancelled-after-decode");
            return false;
        }

        const size_t maxSize = std::numeric_limits<size_t>::max();
        if (width <= 0 || height <= 0 ||
            static_cast<size_t>(height) > maxSize / 4u ||
            static_cast<size_t>(width) >
                maxSize / (static_cast<size_t>(height) * 4u))
        {
            stbi_image_free(pixels);
            decodeSpan.SetAttribute("result", "invalid-dimensions");
            return false;
        }

        outWidth = static_cast<uint32_t>(width);
        outHeight = static_cast<uint32_t>(height);
        outChannels = 4; // We forced RGBA

        size_t pixelCount = static_cast<size_t>(width) * height * 4;
        outPixels.assign(pixels, pixels + pixelCount);

        stbi_image_free(pixels);
        decodeSpan.SetAttribute("result", "decoded");
        decodeSpan.SetAttribute("width", static_cast<uint64>(outWidth));
        decodeSpan.SetAttribute("height", static_cast<uint64>(outHeight));
        decodeSpan.SetAttribute("decodedBytes", static_cast<uint64>(outPixels.size()));
        return true;
    }

    TextureResource* TextureLoader::CreateTextureResource(std::vector<uint8_t> pixels,
                                                            uint32_t width, uint32_t height,
                                                            int channels,
                                                            const std::string& sourceKey,
                                                            const std::string& cacheKey,
                                                            TextureUsage usage,
                                                            bool isSRGB,
                                                            bool generateMipChain)
    {
        auto* texture = new TextureResource();

        ResourceId textureId = GenerateTextureId(cacheKey);
        texture->SetId(textureId);
        texture->SetPath(sourceKey);

        // Extract name from path
        std::filesystem::path filePath(sourceKey);
        texture->SetName(filePath.stem().string());

        // Set metadata
        TextureMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.format = (channels == 4) ? TextureFormat::RGBA8 : TextureFormat::RGB8;
        metadata.mipLevels = 1;
        metadata.usage = usage;
        metadata.isSRGB = isSRGB;

        const size_t expectedBaseSize = static_cast<size_t>(width) * height * 4u;
        if (generateMipChain && metadata.format == TextureFormat::RGBA8 && width > 0 && height > 0 &&
            pixels.size() == expectedBaseSize)
        {
            pixels = BuildMipChain(std::move(pixels),
                                   width,
                                   height,
                                   usage,
                                   isSRGB,
                                   metadata.mipLevels);
        }

        texture->SetData(std::move(pixels), metadata);
        if (!m_prepareOnly)
        {
            texture->NotifyLoaded();
        }

        // Store in cache
        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(texture);
        }

        return texture;
    }

    std::string TextureLoader::BuildTexturePolicyCacheKey(const std::string& sourceKey,
                                                          TextureUsage usage,
                                                          bool isSRGB) const
    {
        std::ostringstream stream;
        stream << sourceKey << "|usage=" << static_cast<int>(usage) << "|srgb=" << (isSRGB ? 1 : 0);
        return stream.str();
    }

    TextureUsage TextureLoader::InferTextureUsageFromPath(const std::string& path) const
    {
        TextureUsage usage = TextureUsage::Color;
        std::string lowerPath = path;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);

        if (lowerPath.find("normal") != std::string::npos ||
            lowerPath.find("_n.") != std::string::npos ||
            lowerPath.find("_norm.") != std::string::npos)
        {
            usage = TextureUsage::Normal;
        }
        else if (lowerPath.find("metallic") != std::string::npos ||
                 lowerPath.find("roughness") != std::string::npos ||
                 lowerPath.find("_mr.") != std::string::npos ||
                 lowerPath.find("_orm.") != std::string::npos ||
                 lowerPath.find("ao") != std::string::npos)
        {
            usage = TextureUsage::Data;
        }

        return usage;
    }

    ResourceId TextureLoader::GenerateTextureId(const std::string& uniqueKey)
    {
        // Use std::hash for simplicity
        std::hash<std::string> hasher;
        return static_cast<ResourceId>(hasher(uniqueKey));
    }

} // namespace RVX::Resource
