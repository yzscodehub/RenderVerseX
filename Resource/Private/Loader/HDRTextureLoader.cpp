#include "Resource/Loader/HDRTextureLoader.h"

#include "Core/Hash/SHA256.h"
#include "Core/Log.h"
#include "Resource/ResourceCache.h"

#include <stb_image.h>

// Optional: tinyexr for EXR support
#if __has_include(<tinyexr.h>)
    #define TINYEXR_IMPLEMENTATION
    #include <tinyexr.h>
    #define HAS_TINYEXR 1
#else
    #define HAS_TINYEXR 0
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>

namespace RVX::Resource
{
    HDRLoadOptions ResolveHDRIBLQualityProfile(
        HDRIBLQualityProfile profile,
        float exposure,
        bool applyGamma)
    {
        HDRLoadOptions options;
        options.generateCubemap = true;
        options.generateIBL = true;
        options.applyGamma = applyGamma;
        options.exposure = exposure;

        switch (profile)
        {
            case HDRIBLQualityProfile::Validation:
                options.cubemapResolution = 8;
                options.irradianceResolution = 2;
                options.prefilteredResolution = 8;
                options.prefilteredMipLevels = 4;
                options.brdfLUTResolution = 8;
                options.convolutionSamples = 16;
                break;
            case HDRIBLQualityProfile::Low:
                options.cubemapResolution = 32;
                options.irradianceResolution = 8;
                options.prefilteredResolution = 32;
                options.prefilteredMipLevels = 5;
                options.brdfLUTResolution = 32;
                options.convolutionSamples = 64;
                break;
            case HDRIBLQualityProfile::High:
                options.cubemapResolution = 128;
                options.irradianceResolution = 32;
                options.prefilteredResolution = 128;
                options.prefilteredMipLevels = 7;
                options.brdfLUTResolution = 128;
                options.convolutionSamples = 256;
                break;
            case HDRIBLQualityProfile::Default:
            default:
                options.cubemapResolution = 64;
                options.irradianceResolution = 16;
                options.prefilteredResolution = 64;
                options.prefilteredMipLevels = 6;
                options.brdfLUTResolution = 64;
                options.convolutionSamples = 128;
                break;
        }

        return options;
    }

    // =========================================================================
    // Constants
    // =========================================================================

    static constexpr float PI = 3.14159265358979323846f;
    static constexpr float TWO_PI = 2.0f * PI;
    static constexpr float HALF_PI = PI / 2.0f;

    namespace
    {
        class GeneratedHDRTextureResource final : public TextureResource
        {
        public:
            void MarkLoaded()
            {
                NotifyLoaded();
            }
        };

        bool IsCancellationRequested(const HDRTextureLoader::CancellationPredicate& cancellationRequested)
        {
            return cancellationRequested && cancellationRequested();
        }

        void DiscardIBLData(IBLData& ibl)
        {
            // IBLData intentionally carries raw pointers for the legacy API.
            // Adopt every completed texture before clearing the structure so a
            // cancelled prepare-only bake cannot leak a partial dependency.
            TextureHandle environment(ibl.environmentMap);
            TextureHandle irradiance(ibl.irradianceMap);
            TextureHandle prefiltered(ibl.prefilteredMap);
            TextureHandle brdfLUT(ibl.brdfLUT);
            ibl = {};
        }

        Vec3 SampleCubemapNearest(const CubemapFaces& envMap, const Vec3& direction)
        {
            if (envMap.faceSize == 0)
            {
                return Vec3(0.0f);
            }

            const Vec3 sampleVec = glm::normalize(direction);
            const float maxAxis = std::max({std::abs(sampleVec.x),
                                            std::abs(sampleVec.y),
                                            std::abs(sampleVec.z)});

            if (maxAxis <= 0.0f || !std::isfinite(maxAxis))
            {
                return Vec3(0.0f);
            }

            int sampleFace = 0;
            float sc = 0.0f;
            float tc = 0.0f;
            float ma = 1.0f;

            if (sampleVec.x > 0.0f && std::abs(sampleVec.x) >= maxAxis)
            {
                sampleFace = CubemapFaces::PositiveX;
                sc = -sampleVec.z;
                tc = -sampleVec.y;
                ma = sampleVec.x;
            }
            else if (sampleVec.x < 0.0f && std::abs(sampleVec.x) >= maxAxis)
            {
                sampleFace = CubemapFaces::NegativeX;
                sc = sampleVec.z;
                tc = -sampleVec.y;
                ma = -sampleVec.x;
            }
            else if (sampleVec.y > 0.0f && std::abs(sampleVec.y) >= maxAxis)
            {
                sampleFace = CubemapFaces::PositiveY;
                sc = sampleVec.x;
                tc = sampleVec.z;
                ma = sampleVec.y;
            }
            else if (sampleVec.y < 0.0f && std::abs(sampleVec.y) >= maxAxis)
            {
                sampleFace = CubemapFaces::NegativeY;
                sc = sampleVec.x;
                tc = -sampleVec.z;
                ma = -sampleVec.y;
            }
            else if (sampleVec.z > 0.0f && std::abs(sampleVec.z) >= maxAxis)
            {
                sampleFace = CubemapFaces::PositiveZ;
                sc = sampleVec.x;
                tc = -sampleVec.y;
                ma = sampleVec.z;
            }
            else
            {
                sampleFace = CubemapFaces::NegativeZ;
                sc = -sampleVec.x;
                tc = -sampleVec.y;
                ma = -sampleVec.z;
            }

            const std::vector<float>& faceData = envMap.faces[static_cast<size_t>(sampleFace)];
            if (faceData.empty())
            {
                return Vec3(0.0f);
            }

            const float sampleU = 0.5f * (sc / ma + 1.0f);
            const float sampleV = 0.5f * (tc / ma + 1.0f);
            const int px = std::clamp(static_cast<int>(sampleU * envMap.faceSize),
                                      0,
                                      static_cast<int>(envMap.faceSize) - 1);
            const int py = std::clamp(static_cast<int>(sampleV * envMap.faceSize),
                                      0,
                                      static_cast<int>(envMap.faceSize) - 1);

            const size_t idx = (static_cast<size_t>(py) * envMap.faceSize + static_cast<size_t>(px)) * 4u;
            if (idx + 2 >= faceData.size())
            {
                return Vec3(0.0f);
            }

            return Vec3(faceData[idx], faceData[idx + 1], faceData[idx + 2]);
        }

        Vec3 SanitizeFiniteColor(const Vec3& color)
        {
            return Vec3(std::isfinite(color.r) ? color.r : 0.0f,
                        std::isfinite(color.g) ? color.g : 0.0f,
                        std::isfinite(color.b) ? color.b : 0.0f);
        }

        float SanitizeFiniteNonNegative(float value)
        {
            return std::isfinite(value) ? std::max(0.0f, value) : 0.0f;
        }

        float GeometrySchlickGGX(float nDot, float roughness)
        {
            const float alpha = roughness * roughness;
            const float k = alpha * 0.5f;
            return nDot / std::max(nDot * (1.0f - k) + k, 1.0e-6f);
        }

        float GeometrySmithIBL(float nDotV, float nDotL, float roughness)
        {
            return GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
        }

        uint16_t FloatToHalfBits(float value)
        {
            const float sanitized = SanitizeFiniteNonNegative(value);
            if (sanitized == 0.0f)
            {
                return 0;
            }

            uint32_t bits = 0;
            std::memcpy(&bits, &sanitized, sizeof(bits));
            const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
            int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
            uint32_t mant = bits & 0x007FFFFFu;

            if (exp <= 0)
            {
                if (exp < -10)
                {
                    return sign;
                }

                mant |= 0x00800000u;
                const uint32_t shift = static_cast<uint32_t>(14 - exp);
                uint16_t half = static_cast<uint16_t>(mant >> shift);
                if ((mant >> (shift - 1u)) & 1u)
                {
                    ++half;
                }
                return static_cast<uint16_t>(sign | half);
            }

            if (exp >= 31)
            {
                return static_cast<uint16_t>(sign | 0x7C00u);
            }

            uint16_t half = static_cast<uint16_t>(sign |
                                                  (static_cast<uint16_t>(exp) << 10) |
                                                  static_cast<uint16_t>(mant >> 13));
            if (mant & 0x00001000u)
            {
                ++half;
            }
            return half;
        }

        void AppendFloatOption(std::ostringstream& stream, const char* name, float value)
        {
            stream << '|' << name << '=';
            if (std::isfinite(value))
            {
                stream << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
            }
            else
            {
                stream << "nonfinite";
            }
        }

        void AppendBoolOption(std::ostringstream& stream, const char* name, bool value)
        {
            stream << '|' << name << '=' << (value ? 1 : 0);
        }

        std::string BuildHDREquirectCacheKey(const std::string& absolutePath,
                                             const HDRLoadOptions& options)
        {
            std::ostringstream stream;
            stream << "hdr_equirect_v1|path=" << absolutePath;
            AppendBoolOption(stream, "applyGamma", options.applyGamma);
            AppendFloatOption(stream, "exposure", options.exposure);
            return stream.str();
        }

        std::string BuildHDRCubemapCacheKey(const std::string& absolutePath,
                                            const HDRLoadOptions& options)
        {
            std::ostringstream stream;
            stream << "hdr_cubemap_v1|path=" << absolutePath
                   << "|resolution=" << options.cubemapResolution;
            AppendBoolOption(stream, "applyGamma", options.applyGamma);
            AppendFloatOption(stream, "exposure", options.exposure);
            return stream.str();
        }

        std::string BuildIBLEnvironmentCacheKey(const std::string& absolutePath,
                                                const HDRLoadOptions& options)
        {
            std::ostringstream stream;
            stream << "ibl_environment_v1|path=" << absolutePath
                   << "|resolution=" << options.cubemapResolution;
            AppendBoolOption(stream, "applyGamma", options.applyGamma);
            AppendFloatOption(stream, "exposure", options.exposure);
            return stream.str();
        }

        std::string BuildIBLIrradianceCacheKey(const std::string& absolutePath,
                                               const HDRLoadOptions& options)
        {
            std::ostringstream stream;
            stream << "ibl_irradiance_v1|path=" << absolutePath
                   << "|envResolution=" << options.cubemapResolution
                   << "|irradianceResolution=" << options.irradianceResolution
                   << "|samples=" << std::max(1u, options.convolutionSamples);
            AppendBoolOption(stream, "applyGamma", options.applyGamma);
            AppendFloatOption(stream, "exposure", options.exposure);
            return stream.str();
        }

        std::string BuildIBLPrefilteredCacheKey(const std::string& absolutePath,
                                                const HDRLoadOptions& options)
        {
            std::ostringstream stream;
            stream << "ibl_prefiltered_v1|path=" << absolutePath
                   << "|envResolution=" << options.cubemapResolution
                   << "|prefilteredResolution=" << options.prefilteredResolution
                   << "|mips=" << options.prefilteredMipLevels
                   << "|samples=" << std::max(1u, options.convolutionSamples);
            AppendBoolOption(stream, "applyGamma", options.applyGamma);
            AppendFloatOption(stream, "exposure", options.exposure);
            return stream.str();
        }

        std::string BuildBRDFLUTCacheKey(uint32_t resolution, uint32_t numSamples)
        {
            std::ostringstream stream;
            stream << "ibl_brdf_lut_v1|resolution=" << resolution
                   << "|samples=" << std::max(1u, numSamples);
            return stream.str();
        }

        /**
         * @brief Read the encoded source exactly once before it is decoded.
         *
         * Prepared loading hashes this returned vector and passes the same
         * vector into stb_image/tinyexr; it must never hash one disk read and
         * decode another one.
         */
        bool ReadEncodedImageBytes(const std::filesystem::path& path,
                                   std::vector<uint8>& outBytes,
                                   std::string& outError)
        {
            outBytes.clear();
            outError.clear();

            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input.is_open())
            {
                outError = "Cannot open HDR/EXR source: " + path.string();
                return false;
            }

            const std::streamsize byteCount = input.tellg();
            if (byteCount < 0)
            {
                outError = "Cannot determine HDR/EXR source size: " + path.string();
                return false;
            }
            input.seekg(0, std::ios::beg);

            outBytes.resize(static_cast<size_t>(byteCount));
            if (byteCount != 0 &&
                !input.read(reinterpret_cast<char*>(outBytes.data()), byteCount))
            {
                outBytes.clear();
                outError = "Cannot read HDR/EXR source bytes: " + path.string();
                return false;
            }
            return true;
        }

        ResourceContentIdentity BuildSelfContainedSourceIdentity(
            const std::vector<uint8>& sourceBytes)
        {
            ResourceContentIdentity identity;
            identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
            identity.domain = ResourceContentIdentityDomain::Source;
            identity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
            identity.algorithm = ResourceContentHashAlgorithm::SHA256;
            identity.digest = Hash::FormatSHA256Digest(
                Hash::ComputeSHA256(sourceBytes.data(), sourceBytes.size()));
            identity.byteCount = static_cast<uint64>(sourceBytes.size());
            identity.fileCount = 1;
            return identity;
        }
    } // namespace

    // =========================================================================
    // Construction
    // =========================================================================

    HDRTextureLoader::HDRTextureLoader(ResourceManager* manager, bool prepareOnly)
        : m_manager(manager)
        , m_prepareOnly(prepareOnly)
    {
    }

    // =========================================================================
    // IResourceLoader Interface
    // =========================================================================

    std::vector<std::string> HDRTextureLoader::GetSupportedExtensions() const
    {
        std::vector<std::string> extensions = { ".hdr" };
#if HAS_TINYEXR
        extensions.push_back(".exr");
#endif
        return extensions;
    }

    bool HDRTextureLoader::CanLoad(const std::string& path) const
    {
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".hdr") return true;
#if HAS_TINYEXR
        if (ext == ".exr") return true;
#endif
        return false;
    }

    IResource* HDRTextureLoader::Load(const std::string& path)
    {
        HDRLoadOptions options;
        return LoadWithOptions(path, options);
    }

    bool HDRTextureLoader::Prepare(const ResourceLoadPreparationContext& context,
                                   PreparedResourceBundle& outBundle,
                                   ResourceLoadError& outError)
    {
        if (context.IsCancellationRequested())
        {
            outError = {ResourceLoadErrorCode::Cancelled, "HDR load was cancelled before preparation."};
            return false;
        }

        // Capture encoded source bytes once. The identity is derived from this
        // exact immutable vector and this same vector is given to stb/tinyexr,
        // so a file replacement cannot occur between hashing and decoding.
        std::vector<uint8> sourceBytes;
        std::string readError;
        if (!ReadEncodedImageBytes(context.resolvedPath, sourceBytes, readError))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure, std::move(readError)};
            return false;
        }
        const ResourceContentIdentity observedContentIdentity =
            BuildSelfContainedSourceIdentity(sourceBytes);
        if (!observedContentIdentity.IsValid())
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "HDR loader could not construct an identity for consumed source bytes."};
            return false;
        }

        HDRTextureLoader preparedLoader(nullptr, true);
        HDRLoadOptions options;
        TextureResource* texture = preparedLoader.LoadWithOptionsFromBytes(
            context.resolvedPath,
            sourceBytes,
            options);
        if (!texture)
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "HDR loader failed to prepare an environment texture."};
            return false;
        }
        if (context.IsCancellationRequested())
        {
            delete texture;
            outError = {ResourceLoadErrorCode::Cancelled, "HDR load was cancelled after preparation."};
            return false;
        }

        texture->SetId(context.rootResourceId);
        texture->SetPath(context.requestedPath);
        texture->SetName(std::filesystem::path(context.requestedPath).stem().string());
        ResourceHandle<IResource> resource(texture);
        if (!outBundle.SetRoot(std::move(resource)))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "HDR loader could not construct a prepared bundle."};
            return false;
        }
        if (!outBundle.SetObservedContentIdentity(observedContentIdentity))
        {
            outError = {ResourceLoadErrorCode::LoaderFailure,
                        "HDR loader could not attach its consumed-byte identity."};
            return false;
        }
        return true;
    }

    // =========================================================================
    // Extended Loading API
    // =========================================================================

    TextureResource* HDRTextureLoader::LoadWithOptions(const std::string& path,
                                                         const HDRLoadOptions& options)
    {
        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::vector<uint8> sourceBytes;
        std::string readError;
        if (!ReadEncodedImageBytes(absPath, sourceBytes, readError))
        {
            if (Log::GetCoreLogger())
            {
                RVX_CORE_WARN("HDRTextureLoader: {}", readError);
            }
            return GetDefaultEnvironmentMap();
        }

        TextureResource* texture = LoadWithOptionsFromBytes(
            absPath.string(),
            sourceBytes,
            options);
        if (!texture && Log::GetCoreLogger())
        {
            RVX_CORE_WARN("HDRTextureLoader: Failed to load: {}", absPath.string());
        }
        return texture ? texture : GetDefaultEnvironmentMap();
    }

    TextureResource* HDRTextureLoader::LoadWithOptionsFromBytes(
        const std::string& path,
        const std::vector<uint8>& sourceBytes,
        const HDRLoadOptions& options)
    {
        std::filesystem::path absPath = std::filesystem::absolute(path);
        const std::string absolutePath = absPath.string();
        const Diagnostics::TraceContext traceContext =
            m_manager != nullptr ? m_manager->GetStartupTraceContext()
                                 : Diagnostics::TraceContext{};
        Diagnostics::TraceSpan prepareSpan = Diagnostics::BeginTraceSpan(
            traceContext,
            "CPUPrepare",
            {{"path", absolutePath}, {"assetKind", "environment"}});

        // Load HDR data
        std::vector<float> pixels;
        uint32_t width = 0;
        uint32_t height = 0;

        std::string ext = absPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool loaded = false;
        if (ext == ".hdr")
        {
            loaded = LoadHDR(absolutePath, sourceBytes, pixels, width, height);
        }
        else if (ext == ".exr")
        {
            loaded = LoadEXR(absolutePath, sourceBytes, pixels, width, height);
        }

        if (!loaded)
        {
            prepareSpan.SetAttribute("result", "failed");
            return nullptr;
        }

        // Apply exposure
        if (options.exposure != 1.0f)
        {
            for (size_t i = 0; i < pixels.size(); ++i)
            {
                // Only apply to RGB, not alpha
                if ((i % 4) != 3)
                {
                    pixels[i] *= options.exposure;
                }
            }
        }

        // Convert to cubemap if requested
        if (options.generateCubemap)
        {
            CubemapFaces cubemap = EquirectangularToCubemap(
                pixels.data(), width, height, options.cubemapResolution);

            TextureResource* texture = CreateCubemapTexture(
                cubemap,
                BuildHDRCubemapCacheKey(absolutePath, options));
            prepareSpan.SetAttribute("result", texture != nullptr ? "prepared" : "failed");
            return texture;
        }
        else
        {
            // Create equirectangular texture
            const std::string cacheKey = BuildHDREquirectCacheKey(absolutePath, options);
            ResourceId texId = GenerateHDRTextureId(cacheKey);

            // Check cache
            if (m_manager && m_manager->IsInitialized())
            {
                if (auto* cached = m_manager->GetCache().Get(texId))
                {
                    prepareSpan.SetAttribute("cacheHit", true);
                    prepareSpan.SetAttribute("result", "cached");
                    return static_cast<TextureResource*>(cached);
                }
            }

            auto* texture = new GeneratedHDRTextureResource();
            texture->SetId(texId);
            texture->SetPath(absolutePath);
            texture->SetName(absPath.stem().string());

            // Convert float to RGBA16F format (pack as uint16 half floats or keep as float)
            // For simplicity, we'll store as RGBA32F
            TextureMetadata metadata;
            metadata.width = width;
            metadata.height = height;
            metadata.format = TextureFormat::RGBA32F;
            metadata.mipLevels = 1;
            metadata.isSRGB = false;
            metadata.usage = TextureUsage::Color;

            // Copy float data to bytes
            std::vector<uint8_t> byteData(pixels.size() * sizeof(float));
            std::memcpy(byteData.data(), pixels.data(), byteData.size());
            const uint64 preparedBytes = static_cast<uint64>(byteData.size());

            texture->SetData(std::move(byteData), metadata);
            if (!m_prepareOnly)
            {
                texture->MarkLoaded();
            }

            if (m_manager && m_manager->IsInitialized())
            {
                m_manager->GetCache().Store(texture);
            }

            prepareSpan.SetAttribute("result", "prepared");
            prepareSpan.SetAttribute("bytes", preparedBytes);
            return texture;
        }
    }

    IBLData HDRTextureLoader::LoadIBL(const std::string& path,
                                       const HDRLoadOptions& options,
                                       const CancellationPredicate& cancellationRequested)
    {
        if (IsCancellationRequested(cancellationRequested))
        {
            return {};
        }

        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::vector<uint8> sourceBytes;
        std::string readError;
        if (!ReadEncodedImageBytes(absPath, sourceBytes, readError))
        {
            if (Log::GetCoreLogger())
            {
                RVX_CORE_WARN("HDRTextureLoader: {}", readError);
            }
            return {};
        }

        return LoadIBLFromBytes(absPath.string(), sourceBytes, options, cancellationRequested);
    }

    IBLData HDRTextureLoader::LoadIBLFromBytes(
        const std::string& path,
        const std::vector<uint8>& sourceBytes,
        const HDRLoadOptions& options,
        const CancellationPredicate& cancellationRequested)
    {
        IBLData ibl;
        if (IsCancellationRequested(cancellationRequested))
        {
            return ibl;
        }

        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePath = absPath.string();
        const Diagnostics::TraceContext traceContext =
            m_manager != nullptr ? m_manager->GetStartupTraceContext()
                                 : Diagnostics::TraceContext{};
        Diagnostics::TraceSpan prepareSpan = Diagnostics::BeginTraceSpan(
            traceContext,
            "CPUPrepare",
            {{"path", absolutePath}, {"assetKind", "environmentIBL"}});

        // Load HDR data
        std::vector<float> pixels;
        uint32_t width = 0;
        uint32_t height = 0;

        std::string ext = absPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool loaded = false;
        if (ext == ".hdr")
        {
            loaded = LoadHDR(absolutePath, sourceBytes, pixels, width, height);
        }
        else if (ext == ".exr")
        {
            loaded = LoadEXR(absolutePath, sourceBytes, pixels, width, height);
        }

        if (!loaded)
        {
            prepareSpan.SetAttribute("result", "failed");
            if (Log::GetCoreLogger())
            {
                RVX_CORE_WARN("HDRTextureLoader: Failed to load for IBL: {}", absolutePath);
            }
            return ibl;
        }

        if (IsCancellationRequested(cancellationRequested))
        {
            prepareSpan.SetAttribute("result", "cancelled");
            return ibl;
        }

        if (Log::GetCoreLogger())
        {
            RVX_CORE_INFO("HDRTextureLoader: Generating IBL from {}...", absPath.filename().string());
        }

        // Apply exposure
        if (options.exposure != 1.0f)
        {
            for (size_t i = 0; i < pixels.size(); ++i)
            {
                if ((i % 4096u) == 0u && IsCancellationRequested(cancellationRequested))
                {
                    prepareSpan.SetAttribute("result", "cancelled");
                    return ibl;
                }
                if ((i % 4) != 3)
                {
                    pixels[i] *= options.exposure;
                }
            }
        }

        // Generate environment cubemap
        CubemapFaces envCubemap = EquirectangularToCubemap(
            pixels.data(), width, height, options.cubemapResolution, cancellationRequested);
        const bool cancelledAfterCubemap = IsCancellationRequested(cancellationRequested);
        if (cancelledAfterCubemap || envCubemap.faceSize == 0)
        {
            prepareSpan.SetAttribute(
                "result",
                cancelledAfterCubemap ? "cancelled" : "failed");
            return ibl;
        }
        ibl.environmentMap = CreateCubemapTexture(
            envCubemap, BuildIBLEnvironmentCacheKey(absolutePath, options));
        if (!ibl.environmentMap)
        {
            prepareSpan.SetAttribute("result", "failed");
            return ibl;
        }

        // Generate irradiance map
        CubemapFaces irradianceCubemap = GenerateIrradianceMap(
            envCubemap, options.irradianceResolution, options.convolutionSamples, cancellationRequested);
        const bool cancelledAfterIrradiance = IsCancellationRequested(cancellationRequested);
        if (cancelledAfterIrradiance || irradianceCubemap.faceSize == 0)
        {
            DiscardIBLData(ibl);
            prepareSpan.SetAttribute(
                "result",
                cancelledAfterIrradiance ? "cancelled" : "failed");
            return ibl;
        }
        ibl.irradianceMap = CreateCubemapTexture(
            irradianceCubemap, BuildIBLIrradianceCacheKey(absolutePath, options));
        if (!ibl.irradianceMap)
        {
            DiscardIBLData(ibl);
            prepareSpan.SetAttribute("result", "failed");
            return ibl;
        }

        // Generate prefiltered map with mip chain
        std::vector<CubemapFaces> prefilteredMips = GeneratePrefilteredMap(
            envCubemap, options.prefilteredResolution, 
            options.prefilteredMipLevels, options.convolutionSamples, cancellationRequested);
        const bool cancelledAfterPrefilter = IsCancellationRequested(cancellationRequested);
        if (cancelledAfterPrefilter || prefilteredMips.empty())
        {
            DiscardIBLData(ibl);
            prepareSpan.SetAttribute(
                "result",
                cancelledAfterPrefilter ? "cancelled" : "failed");
            return ibl;
        }
        ibl.prefilteredMap = CreateCubemapTextureWithMips(
            prefilteredMips, BuildIBLPrefilteredCacheKey(absolutePath, options));
        ibl.prefilteredMipLevels = ibl.prefilteredMap
            ? ibl.prefilteredMap->GetMipLevels()
            : static_cast<uint32_t>(prefilteredMips.size());
        if (!ibl.prefilteredMap)
        {
            DiscardIBLData(ibl);
            prepareSpan.SetAttribute("result", "failed");
            return ibl;
        }

        // Generate BRDF LUT
        ibl.brdfLUT = GenerateBRDFLUT(
            options.brdfLUTResolution, options.convolutionSamples, cancellationRequested);
        if (IsCancellationRequested(cancellationRequested))
        {
            DiscardIBLData(ibl);
            prepareSpan.SetAttribute("result", "cancelled");
            return ibl;
        }
        if (!ibl.brdfLUT)
        {
            DiscardIBLData(ibl);
            prepareSpan.SetAttribute("result", "failed");
            return ibl;
        }

        if (Log::GetCoreLogger())
        {
            RVX_CORE_INFO("HDRTextureLoader: IBL generation complete for {}", absPath.filename().string());
        }

        prepareSpan.SetAttribute("result", "prepared");
        prepareSpan.SetAttribute("environmentReady", ibl.environmentMap != nullptr);
        prepareSpan.SetAttribute("irradianceReady", ibl.irradianceMap != nullptr);
        prepareSpan.SetAttribute("prefilteredReady", ibl.prefilteredMap != nullptr);
        prepareSpan.SetAttribute("brdfReady", ibl.brdfLUT != nullptr);

        ibl.observedContentIdentity = BuildSelfContainedSourceIdentity(sourceBytes);
        if (!ibl.observedContentIdentity.IsValid())
        {
            DiscardIBLData(ibl);
            prepareSpan.SetAttribute("result", "identity-failed");
        }

        return ibl;
    }

    IBLData HDRTextureLoader::LoadIBL(const std::string& path,
                                      HDRIBLQualityProfile profile,
                                      float exposure,
                                      bool applyGamma,
                                      const CancellationPredicate& cancellationRequested)
    {
        return LoadIBL(
            path,
            ResolveHDRIBLQualityProfile(profile, exposure, applyGamma),
            cancellationRequested);
    }

    // =========================================================================
    // Equirectangular to Cubemap Conversion
    // =========================================================================

    CubemapFaces HDRTextureLoader::EquirectangularToCubemap(const float* equirectData,
                                                              uint32_t width, uint32_t height,
                                                              uint32_t cubemapSize,
                                                              const CancellationPredicate& cancellationRequested)
    {
        CubemapFaces result;
        result.faceSize = cubemapSize;

        for (int face = 0; face < CubemapFaces::FACE_COUNT; ++face)
        {
            if (IsCancellationRequested(cancellationRequested))
            {
                return {};
            }
            result.faces[face].resize(cubemapSize * cubemapSize * 4);

            for (uint32_t y = 0; y < cubemapSize; ++y)
            {
                if (IsCancellationRequested(cancellationRequested))
                {
                    return {};
                }
                for (uint32_t x = 0; x < cubemapSize; ++x)
                {
                    // Convert pixel coordinates to direction
                    float u = (static_cast<float>(x) + 0.5f) / cubemapSize * 2.0f - 1.0f;
                    float v = (static_cast<float>(y) + 0.5f) / cubemapSize * 2.0f - 1.0f;

                    Vec3 dir = GetCubemapDirection(static_cast<CubemapFaces::Face>(face), u, v);
                    dir = glm::normalize(dir);

                    // Convert direction to equirectangular coordinates
                    Vec2 uv = DirectionToEquirectangular(dir);

                    // Sample equirectangular map (bilinear interpolation)
                    float fx = uv.x * (width - 1);
                    float fy = uv.y * (height - 1);
                    
                    int x0 = static_cast<int>(fx);
                    int y0 = static_cast<int>(fy);
                    int x1 = std::min(x0 + 1, static_cast<int>(width) - 1);
                    int y1 = std::min(y0 + 1, static_cast<int>(height) - 1);
                    
                    float tx = fx - x0;
                    float ty = fy - y0;

                    size_t dstIdx = (y * cubemapSize + x) * 4;

                    for (int c = 0; c < 4; ++c)
                    {
                        float c00 = equirectData[(y0 * width + x0) * 4 + c];
                        float c10 = equirectData[(y0 * width + x1) * 4 + c];
                        float c01 = equirectData[(y1 * width + x0) * 4 + c];
                        float c11 = equirectData[(y1 * width + x1) * 4 + c];

                        float c0 = c00 * (1.0f - tx) + c10 * tx;
                        float c1 = c01 * (1.0f - tx) + c11 * tx;

                        result.faces[face][dstIdx + c] = c0 * (1.0f - ty) + c1 * ty;
                    }
                }
            }
        }

        return result;
    }

    Vec3 HDRTextureLoader::GetCubemapDirection(CubemapFaces::Face face, float u, float v) const
    {
        switch (face)
        {
            case CubemapFaces::PositiveX: return Vec3( 1.0f, -v, -u);
            case CubemapFaces::NegativeX: return Vec3(-1.0f, -v,  u);
            case CubemapFaces::PositiveY: return Vec3( u,  1.0f,  v);
            case CubemapFaces::NegativeY: return Vec3( u, -1.0f, -v);
            case CubemapFaces::PositiveZ: return Vec3( u, -v,  1.0f);
            case CubemapFaces::NegativeZ: return Vec3(-u, -v, -1.0f);
            default: return Vec3(0.0f);
        }
    }

    Vec2 HDRTextureLoader::DirectionToEquirectangular(const Vec3& dir) const
    {
        float phi = std::atan2(dir.z, dir.x);
        float theta = std::asin(std::clamp(dir.y, -1.0f, 1.0f));

        float u = (phi + PI) / TWO_PI;
        float v = (theta + HALF_PI) / PI;

        return Vec2(u, 1.0f - v);  // Flip V for texture coordinates
    }

    // =========================================================================
    // IBL Generation
    // =========================================================================

    CubemapFaces HDRTextureLoader::GenerateIrradianceMap(const CubemapFaces& envMap,
                                                           uint32_t outputSize,
                                                           uint32_t numSamples,
                                                           const CancellationPredicate& cancellationRequested)
    {
        CubemapFaces result;
        result.faceSize = outputSize;

        for (int face = 0; face < CubemapFaces::FACE_COUNT; ++face)
        {
            if (IsCancellationRequested(cancellationRequested))
            {
                return {};
            }
            result.faces[face].resize(outputSize * outputSize * 4);

            for (uint32_t y = 0; y < outputSize; ++y)
            {
                if (IsCancellationRequested(cancellationRequested))
                {
                    return {};
                }
                for (uint32_t x = 0; x < outputSize; ++x)
                {
                    float u = (static_cast<float>(x) + 0.5f) / outputSize * 2.0f - 1.0f;
                    float v = (static_cast<float>(y) + 0.5f) / outputSize * 2.0f - 1.0f;

                    Vec3 N = glm::normalize(GetCubemapDirection(
                        static_cast<CubemapFaces::Face>(face), u, v));

                    // Hemisphere convolution
                    Vec3 irradiance(0.0f);

                    // Create tangent space
                    Vec3 up = std::abs(N.y) < 0.999f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);
                    Vec3 right = glm::normalize(glm::cross(up, N));
                    up = glm::cross(N, right);

                    const uint32_t sampleCount = std::max(1u, numSamples);
                    for (uint32_t i = 0; i < sampleCount; ++i)
                    {
                        if ((i % 16u) == 0u && IsCancellationRequested(cancellationRequested))
                        {
                            return {};
                        }
                        const Vec2 xi = Hammersley(i, sampleCount);
                        const float radius = std::sqrt(xi.y);
                        const float phi = TWO_PI * xi.x;
                        const Vec3 tangentSample(radius * std::cos(phi),
                                                 radius * std::sin(phi),
                                                 std::sqrt(std::max(0.0f, 1.0f - xi.y)));
                        const Vec3 sampleVec = glm::normalize(tangentSample.x * right +
                                                              tangentSample.y * up +
                                                              tangentSample.z * N);
                        irradiance += SampleCubemapNearest(envMap, sampleVec);
                    }

                    irradiance = SanitizeFiniteColor(PI * irradiance / static_cast<float>(sampleCount));

                    size_t dstIdx = (y * outputSize + x) * 4;
                    result.faces[face][dstIdx] = irradiance.r;
                    result.faces[face][dstIdx + 1] = irradiance.g;
                    result.faces[face][dstIdx + 2] = irradiance.b;
                    result.faces[face][dstIdx + 3] = 1.0f;
                }
            }
        }

        return result;
    }

    std::vector<CubemapFaces> HDRTextureLoader::GeneratePrefilteredMap(const CubemapFaces& envMap,
                                                                         uint32_t outputSize,
                                                                         uint32_t numMipLevels,
                                                                         uint32_t numSamples,
                                                                         const CancellationPredicate& cancellationRequested)
    {
        std::vector<CubemapFaces> mipChain;
        mipChain.reserve(numMipLevels);
        const uint32_t sampleCount = std::max(1u, numSamples);

        for (uint32_t mip = 0; mip < numMipLevels; ++mip)
        {
            if (IsCancellationRequested(cancellationRequested))
            {
                return {};
            }
            const float roughness = numMipLevels > 1 ?
                static_cast<float>(mip) / static_cast<float>(numMipLevels - 1) :
                0.0f;
            const uint32_t mipSize = std::max(1u, outputSize >> mip);

            CubemapFaces mipFaces;
            mipFaces.faceSize = mipSize;

            for (int face = 0; face < CubemapFaces::FACE_COUNT; ++face)
            {
                if (IsCancellationRequested(cancellationRequested))
                {
                    return {};
                }
                mipFaces.faces[face].resize(mipSize * mipSize * 4);

                for (uint32_t y = 0; y < mipSize; ++y)
                {
                    if (IsCancellationRequested(cancellationRequested))
                    {
                        return {};
                    }
                    for (uint32_t x = 0; x < mipSize; ++x)
                    {
                        float u = (static_cast<float>(x) + 0.5f) / mipSize * 2.0f - 1.0f;
                        float v = (static_cast<float>(y) + 0.5f) / mipSize * 2.0f - 1.0f;

                        Vec3 N = glm::normalize(GetCubemapDirection(
                            static_cast<CubemapFaces::Face>(face), u, v));
                        Vec3 R = N;
                        Vec3 V = R;

                        Vec3 prefilteredColor(0.0f);
                        float totalWeight = 0.0f;

                        for (uint32_t i = 0; i < sampleCount; ++i)
                        {
                            if ((i % 16u) == 0u && IsCancellationRequested(cancellationRequested))
                            {
                                return {};
                            }
                            Vec2 xi = Hammersley(i, sampleCount);
                            Vec3 H = ImportanceSampleGGX(xi, N, roughness);
                            Vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);

                            float NdotL = std::max(glm::dot(N, L), 0.0f);
                            if (NdotL > 0.0f)
                            {
                                const Vec3 envColor = SampleCubemapNearest(envMap, L);
                                prefilteredColor += envColor * NdotL;
                                totalWeight += NdotL;
                            }
                        }

                        if (totalWeight > 0.0f && std::isfinite(totalWeight))
                        {
                            prefilteredColor /= totalWeight;
                        }
                        else
                        {
                            prefilteredColor = SampleCubemapNearest(envMap, N);
                        }

                        prefilteredColor = SanitizeFiniteColor(prefilteredColor);

                        size_t dstIdx = (y * mipSize + x) * 4;
                        mipFaces.faces[face][dstIdx] = prefilteredColor.r;
                        mipFaces.faces[face][dstIdx + 1] = prefilteredColor.g;
                        mipFaces.faces[face][dstIdx + 2] = prefilteredColor.b;
                        mipFaces.faces[face][dstIdx + 3] = 1.0f;
                    }
                }
            }

            mipChain.push_back(std::move(mipFaces));
        }

        return mipChain;
    }

    TextureResource* HDRTextureLoader::GenerateBRDFLUT(
        uint32_t resolution,
        uint32_t numSamples,
        const CancellationPredicate& cancellationRequested)
    {
        if (IsCancellationRequested(cancellationRequested))
        {
            return nullptr;
        }
        const std::string cacheKey = BuildBRDFLUTCacheKey(resolution, numSamples);
        ResourceId lutId = GenerateHDRTextureId(cacheKey);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(lutId))
            {
                return static_cast<TextureResource*>(cached);
            }
        }

        std::vector<float> lutData(resolution * resolution * 2);  // RG16F
        const uint32_t sampleCount = std::max(1u, numSamples);

        for (uint32_t y = 0; y < resolution; ++y)
        {
            if (IsCancellationRequested(cancellationRequested))
            {
                return nullptr;
            }
            float roughness = static_cast<float>(y + 1) / static_cast<float>(resolution);

            for (uint32_t x = 0; x < resolution; ++x)
            {
                float NdotV = static_cast<float>(x + 1) / static_cast<float>(resolution);
                
                Vec3 V;
                V.x = std::sqrt(1.0f - NdotV * NdotV);
                V.y = 0.0f;
                V.z = NdotV;

                float A = 0.0f;
                float B = 0.0f;

                Vec3 N(0.0f, 0.0f, 1.0f);

                for (uint32_t i = 0; i < sampleCount; ++i)
                {
                    if ((i % 16u) == 0u && IsCancellationRequested(cancellationRequested))
                    {
                        return nullptr;
                    }
                    Vec2 xi = Hammersley(i, sampleCount);
                    Vec3 H = ImportanceSampleGGX(xi, N, roughness);
                    Vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);

                    float NdotL = std::max(L.z, 0.0f);
                    float NdotH = std::max(H.z, 0.0f);
                    float VdotH = std::max(glm::dot(V, H), 0.0f);

                    if (NdotL > 0.0f)
                    {
                        float G = GeometrySmithIBL(NdotV, NdotL, roughness);
                        float G_Vis = (G * VdotH) / std::max(NdotH * NdotV, 1.0e-6f);
                        float Fc = std::pow(1.0f - VdotH, 5.0f);

                        A += (1.0f - Fc) * G_Vis;
                        B += Fc * G_Vis;
                    }
                }

                A /= static_cast<float>(sampleCount);
                B /= static_cast<float>(sampleCount);

                size_t idx = (y * resolution + x) * 2;
                lutData[idx] = SanitizeFiniteNonNegative(A);
                lutData[idx + 1] = SanitizeFiniteNonNegative(B);
            }
        }

        // Create texture
        auto* texture = new GeneratedHDRTextureResource();
        texture->SetId(lutId);
        texture->SetPath(cacheKey);
        texture->SetName("BRDF_LUT");

        TextureMetadata metadata;
        metadata.width = resolution;
        metadata.height = resolution;
        metadata.format = TextureFormat::RGBA16F;  // Store as RGBA for compatibility
        metadata.mipLevels = 1;
        metadata.isSRGB = false;
        metadata.usage = TextureUsage::Data;

        // Convert to RGBA16F (pack RG into RGBA)
        std::vector<uint8_t> byteData(resolution * resolution * 4 * 2);  // 4 channels, 2 bytes each
        uint16_t* halfData = reinterpret_cast<uint16_t*>(byteData.data());

        for (uint32_t i = 0; i < resolution * resolution; ++i)
        {
            halfData[i * 4 + 0] = FloatToHalfBits(lutData[i * 2]);
            halfData[i * 4 + 1] = FloatToHalfBits(lutData[i * 2 + 1]);
            halfData[i * 4 + 2] = 0;
            halfData[i * 4 + 3] = FloatToHalfBits(1.0f);
        }

        texture->SetData(std::move(byteData), metadata);
        if (!m_prepareOnly)
        {
            texture->MarkLoaded();
        }

        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(texture);
        }

        return texture;
    }

    // =========================================================================
    // IBL Helpers
    // =========================================================================

    Vec3 HDRTextureLoader::ImportanceSampleGGX(Vec2 xi, Vec3 N, float roughness) const
    {
        float a = roughness * roughness;

        float phi = TWO_PI * xi.x;
        float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
        float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);

        Vec3 H;
        H.x = std::cos(phi) * sinTheta;
        H.y = std::sin(phi) * sinTheta;
        H.z = cosTheta;

        Vec3 up = std::abs(N.z) < 0.999f ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(1.0f, 0.0f, 0.0f);
        Vec3 tangent = glm::normalize(glm::cross(up, N));
        Vec3 bitangent = glm::cross(N, tangent);

        return glm::normalize(tangent * H.x + bitangent * H.y + N * H.z);
    }

    float HDRTextureLoader::RadicalInverse_VdC(uint32_t bits) const
    {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    }

    Vec2 HDRTextureLoader::Hammersley(uint32_t i, uint32_t N) const
    {
        return Vec2(static_cast<float>(i) / static_cast<float>(N), RadicalInverse_VdC(i));
    }

    // =========================================================================
    // Loading Helpers
    // =========================================================================

    bool HDRTextureLoader::LoadHDR(const std::string& path,
                                    const std::vector<uint8>& sourceBytes,
                                    std::vector<float>& outPixels,
                                    uint32_t& outWidth, uint32_t& outHeight)
    {
        const Diagnostics::TraceContext traceContext =
            m_manager != nullptr ? m_manager->GetStartupTraceContext()
                                 : Diagnostics::TraceContext{};
        // The caller owns the only source read. Decode exactly that immutable
        // byte vector so observed content identity and parser input cannot
        // diverge due to a between-read file replacement.
        Diagnostics::TraceSpan decodeSpan = Diagnostics::BeginTraceSpan(
            traceContext,
            "TextureDecode",
            {{"path", path},
             {"decoder", "stb_image_hdr"},
             {"inMemorySource", true}});
        int width, height, channels;
        float* data = stbi_loadf_from_memory(sourceBytes.data(),
                                             static_cast<int>(sourceBytes.size()),
                                             &width,
                                             &height,
                                             &channels,
                                             4);

        if (!data)
        {
            decodeSpan.SetAttribute("result", "failed");
            if (Log::GetCoreLogger())
            {
                RVX_CORE_WARN("HDRTextureLoader: stbi_loadf failed: {}", stbi_failure_reason());
            }
            return false;
        }

        outWidth = static_cast<uint32_t>(width);
        outHeight = static_cast<uint32_t>(height);

        size_t pixelCount = static_cast<size_t>(width) * height * 4;
        outPixels.assign(data, data + pixelCount);

        stbi_image_free(data);
        decodeSpan.SetAttribute("result", "decoded");
        decodeSpan.SetAttribute("width", static_cast<uint64>(outWidth));
        decodeSpan.SetAttribute("height", static_cast<uint64>(outHeight));
        decodeSpan.SetAttribute("decodedBytes",
                                static_cast<uint64>(outPixels.size() * sizeof(float)));
        return true;
    }

    bool HDRTextureLoader::LoadEXR(const std::string& path,
                                    const std::vector<uint8>& sourceBytes,
                                    std::vector<float>& outPixels,
                                    uint32_t& outWidth, uint32_t& outHeight)
    {
        const Diagnostics::TraceContext traceContext =
            m_manager != nullptr ? m_manager->GetStartupTraceContext()
                                 : Diagnostics::TraceContext{};
        Diagnostics::TraceSpan decodeSpan = Diagnostics::BeginTraceSpan(
            traceContext,
            "TextureDecode",
            {{"path", path},
             {"decoder", "tinyexr"},
             {"inMemorySource", true}});
#if HAS_TINYEXR
        float* data = nullptr;
        int width, height;
        const char* err = nullptr;

        // Use the memory API rather than tinyexr's file API so the parser
        // receives the exact bytes whose SHA-256 is recorded for publication.
        int ret = ::LoadEXRFromMemory(&data,
                                      &width,
                                      &height,
                                      sourceBytes.data(),
                                      sourceBytes.size(),
                                      &err);
        if (ret != TINYEXR_SUCCESS)
        {
            decodeSpan.SetAttribute("result", "failed");
            if (err)
            {
                if (Log::GetCoreLogger())
                {
                    RVX_CORE_WARN("HDRTextureLoader: tinyexr failed: {}", err);
                }
                ::FreeEXRErrorMessage(err);
            }
            return false;
        }

        outWidth = static_cast<uint32_t>(width);
        outHeight = static_cast<uint32_t>(height);

        size_t pixelCount = static_cast<size_t>(width) * height * 4;
        outPixels.assign(data, data + pixelCount);

        free(data);
        decodeSpan.SetAttribute("result", "decoded");
        decodeSpan.SetAttribute("width", static_cast<uint64>(outWidth));
        decodeSpan.SetAttribute("height", static_cast<uint64>(outHeight));
        decodeSpan.SetAttribute("decodedBytes",
                                static_cast<uint64>(outPixels.size() * sizeof(float)));
        return true;
#else
        decodeSpan.SetAttribute("result", "unsupported");
        if (Log::GetCoreLogger())
        {
            RVX_CORE_WARN("HDRTextureLoader: EXR support not compiled in (missing tinyexr)");
        }
        return false;
#endif
    }

    // =========================================================================
    // Texture Creation
    // =========================================================================

    TextureResource* HDRTextureLoader::CreateCubemapTexture(const CubemapFaces& faces,
                                                              const std::string& uniqueKey)
    {
        ResourceId texId = GenerateHDRTextureId(uniqueKey);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(texId))
            {
                return static_cast<TextureResource*>(cached);
            }
        }

        auto* texture = new GeneratedHDRTextureResource();
        texture->SetId(texId);
        texture->SetPath(uniqueKey);
        
        std::filesystem::path keyPath(uniqueKey);
        texture->SetName(keyPath.stem().string());

        // Pack all faces into a single buffer
        size_t faceDataSize = faces.faceSize * faces.faceSize * 4 * sizeof(float);
        std::vector<uint8_t> cubemapData(faceDataSize * 6);

        for (int face = 0; face < 6; ++face)
        {
            std::memcpy(cubemapData.data() + face * faceDataSize,
                       faces.faces[face].data(),
                       faceDataSize);
        }

        TextureMetadata metadata;
        metadata.width = faces.faceSize;
        metadata.height = faces.faceSize;
        metadata.depth = 1;
        metadata.arrayLayers = 6;
        metadata.mipLevels = 1;
        metadata.format = TextureFormat::RGBA32F;
        metadata.isCubemap = true;
        metadata.isSRGB = false;
        metadata.usage = TextureUsage::Color;

        texture->SetData(std::move(cubemapData), metadata);
        if (!m_prepareOnly)
        {
            texture->MarkLoaded();
        }

        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(texture);
        }

        return texture;
    }

    TextureResource* HDRTextureLoader::CreateCubemapTextureWithMips(
        const std::vector<CubemapFaces>& mipChain,
        const std::string& uniqueKey)
    {
        if (mipChain.empty())
        {
            return nullptr;
        }

        ResourceId texId = GenerateHDRTextureId(uniqueKey);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(texId))
            {
                return static_cast<TextureResource*>(cached);
            }
        }

        auto* texture = new GeneratedHDRTextureResource();
        texture->SetId(texId);
        texture->SetPath(uniqueKey);

        std::filesystem::path keyPath(uniqueKey);
        texture->SetName(keyPath.stem().string());

        // Calculate total size
        size_t totalSize = 0;
        for (const auto& mip : mipChain)
        {
            totalSize += mip.faceSize * mip.faceSize * 4 * sizeof(float) * 6;
        }

        std::vector<uint8_t> cubemapData(totalSize);
        size_t offset = 0;

        for (const auto& mip : mipChain)
        {
            size_t faceDataSize = mip.faceSize * mip.faceSize * 4 * sizeof(float);
            for (int face = 0; face < 6; ++face)
            {
                std::memcpy(cubemapData.data() + offset,
                           mip.faces[face].data(),
                           faceDataSize);
                offset += faceDataSize;
            }
        }

        TextureMetadata metadata;
        metadata.width = mipChain[0].faceSize;
        metadata.height = mipChain[0].faceSize;
        metadata.depth = 1;
        metadata.arrayLayers = 6;
        metadata.mipLevels = static_cast<uint32_t>(mipChain.size());
        metadata.format = TextureFormat::RGBA32F;
        metadata.isCubemap = true;
        metadata.isSRGB = false;
        metadata.usage = TextureUsage::Color;

        texture->SetData(std::move(cubemapData), metadata);
        if (!m_prepareOnly)
        {
            texture->MarkLoaded();
        }

        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(texture);
        }

        return texture;
    }

    // =========================================================================
    // Default Textures
    // =========================================================================

    TextureResource* HDRTextureLoader::GetDefaultEnvironmentMap()
    {
        if (!m_defaultEnvMap)
        {
            // Create a 1x1 black cubemap
            CubemapFaces faces;
            faces.faceSize = 1;
            for (int i = 0; i < 6; ++i)
            {
                faces.faces[i] = { 0.0f, 0.0f, 0.0f, 1.0f };
            }
            m_defaultEnvMap = CreateCubemapTexture(faces, "__default_env_map__");
        }
        return m_defaultEnvMap;
    }

    TextureResource* HDRTextureLoader::GetDefaultBRDFLUT()
    {
        if (!m_defaultBRDFLUT)
        {
            m_defaultBRDFLUT = GenerateBRDFLUT(64, 64);
        }
        return m_defaultBRDFLUT;
    }

    ResourceId HDRTextureLoader::GenerateHDRTextureId(const std::string& uniqueKey)
    {
        std::hash<std::string> hasher;
        return static_cast<ResourceId>(hasher(uniqueKey));
    }

} // namespace RVX::Resource
