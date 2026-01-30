#include "Resource/Loader/HDRTextureLoader.h"
#include "Resource/ResourceCache.h"
#include "Core/Log.h"

#include <stb_image.h>

// Optional: tinyexr for EXR support
#if __has_include(<tinyexr.h>)
    #define TINYEXR_IMPLEMENTATION
    #include <tinyexr.h>
    #define HAS_TINYEXR 1
#else
    #define HAS_TINYEXR 0
#endif

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <functional>

namespace RVX::Resource
{
    // =========================================================================
    // Constants
    // =========================================================================

    static constexpr float PI = 3.14159265358979323846f;
    static constexpr float TWO_PI = 2.0f * PI;
    static constexpr float HALF_PI = PI / 2.0f;

    // =========================================================================
    // Construction
    // =========================================================================

    HDRTextureLoader::HDRTextureLoader(ResourceManager* manager)
        : m_manager(manager)
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

    // =========================================================================
    // Extended Loading API
    // =========================================================================

    TextureResource* HDRTextureLoader::LoadWithOptions(const std::string& path,
                                                         const HDRLoadOptions& options)
    {
        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePath = absPath.string();

        // Load HDR data
        std::vector<float> pixels;
        uint32_t width, height;

        std::string ext = absPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool loaded = false;
        if (ext == ".hdr")
        {
            loaded = LoadHDR(absolutePath, pixels, width, height);
        }
        else if (ext == ".exr")
        {
            loaded = LoadEXR(absolutePath, pixels, width, height);
        }

        if (!loaded)
        {
            RVX_CORE_WARN("HDRTextureLoader: Failed to load: {}", absolutePath);
            return GetDefaultEnvironmentMap();
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

            return CreateCubemapTexture(cubemap, absolutePath + "_cubemap");
        }
        else
        {
            // Create equirectangular texture
            ResourceId texId = GenerateHDRTextureId(absolutePath);

            // Check cache
            if (m_manager && m_manager->IsInitialized())
            {
                if (auto* cached = m_manager->GetCache().Get(texId))
                {
                    return static_cast<TextureResource*>(cached);
                }
            }

            auto* texture = new TextureResource();
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

            texture->SetData(std::move(byteData), metadata);

            if (m_manager && m_manager->IsInitialized())
            {
                m_manager->GetCache().Store(texture);
            }

            return texture;
        }
    }

    IBLData HDRTextureLoader::LoadIBL(const std::string& path,
                                       const HDRLoadOptions& options)
    {
        IBLData ibl;

        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePath = absPath.string();

        // Load HDR data
        std::vector<float> pixels;
        uint32_t width, height;

        std::string ext = absPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool loaded = false;
        if (ext == ".hdr")
        {
            loaded = LoadHDR(absolutePath, pixels, width, height);
        }
        else if (ext == ".exr")
        {
            loaded = LoadEXR(absolutePath, pixels, width, height);
        }

        if (!loaded)
        {
            RVX_CORE_WARN("HDRTextureLoader: Failed to load for IBL: {}", absolutePath);
            return ibl;
        }

        RVX_CORE_INFO("HDRTextureLoader: Generating IBL from {}...", absPath.filename().string());

        // Apply exposure
        if (options.exposure != 1.0f)
        {
            for (size_t i = 0; i < pixels.size(); ++i)
            {
                if ((i % 4) != 3)
                {
                    pixels[i] *= options.exposure;
                }
            }
        }

        // Generate environment cubemap
        CubemapFaces envCubemap = EquirectangularToCubemap(
            pixels.data(), width, height, options.cubemapResolution);
        ibl.environmentMap = CreateCubemapTexture(envCubemap, absolutePath + "_env");

        // Generate irradiance map
        CubemapFaces irradianceCubemap = GenerateIrradianceMap(
            envCubemap, options.irradianceResolution, options.convolutionSamples);
        ibl.irradianceMap = CreateCubemapTexture(irradianceCubemap, absolutePath + "_irradiance");

        // Generate prefiltered map with mip chain
        std::vector<CubemapFaces> prefilteredMips = GeneratePrefilteredMap(
            envCubemap, options.prefilteredResolution, 
            options.prefilteredMipLevels, options.convolutionSamples);
        ibl.prefilteredMap = CreateCubemapTextureWithMips(prefilteredMips, absolutePath + "_prefiltered");
        ibl.prefilteredMipLevels = options.prefilteredMipLevels;

        // Generate BRDF LUT
        ibl.brdfLUT = GenerateBRDFLUT(options.brdfLUTResolution, options.convolutionSamples);

        RVX_CORE_INFO("HDRTextureLoader: IBL generation complete for {}", absPath.filename().string());

        return ibl;
    }

    // =========================================================================
    // Equirectangular to Cubemap Conversion
    // =========================================================================

    CubemapFaces HDRTextureLoader::EquirectangularToCubemap(const float* equirectData,
                                                              uint32_t width, uint32_t height,
                                                              uint32_t cubemapSize)
    {
        CubemapFaces result;
        result.faceSize = cubemapSize;

        for (int face = 0; face < CubemapFaces::FACE_COUNT; ++face)
        {
            result.faces[face].resize(cubemapSize * cubemapSize * 4);

            for (uint32_t y = 0; y < cubemapSize; ++y)
            {
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
                                                           uint32_t numSamples)
    {
        CubemapFaces result;
        result.faceSize = outputSize;

        for (int face = 0; face < CubemapFaces::FACE_COUNT; ++face)
        {
            result.faces[face].resize(outputSize * outputSize * 4);

            for (uint32_t y = 0; y < outputSize; ++y)
            {
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

                    float sampleDelta = 0.025f;
                    int nSamples = 0;

                    for (float phi = 0.0f; phi < TWO_PI; phi += sampleDelta)
                    {
                        for (float theta = 0.0f; theta < HALF_PI; theta += sampleDelta)
                        {
                            // Spherical to cartesian in tangent space
                            Vec3 tangentSample(
                                std::sin(theta) * std::cos(phi),
                                std::sin(theta) * std::sin(phi),
                                std::cos(theta)
                            );

                            // Transform to world space
                            Vec3 sampleVec = tangentSample.x * right + 
                                            tangentSample.y * up + 
                                            tangentSample.z * N;

                            // Sample environment map
                            // (simplified - in production, sample the actual cubemap)
                            Vec2 envUV = DirectionToEquirectangular(sampleVec);
                            
                            // Find which face and coordinates
                            float maxAxis = std::max({std::abs(sampleVec.x), 
                                                      std::abs(sampleVec.y), 
                                                      std::abs(sampleVec.z)});
                            
                            int sampleFace = 0;
                            float sc, tc, ma;
                            
                            if (sampleVec.x > 0 && std::abs(sampleVec.x) >= maxAxis) {
                                sampleFace = 0; sc = -sampleVec.z; tc = -sampleVec.y; ma = sampleVec.x;
                            } else if (sampleVec.x < 0 && std::abs(sampleVec.x) >= maxAxis) {
                                sampleFace = 1; sc = sampleVec.z; tc = -sampleVec.y; ma = -sampleVec.x;
                            } else if (sampleVec.y > 0 && std::abs(sampleVec.y) >= maxAxis) {
                                sampleFace = 2; sc = sampleVec.x; tc = sampleVec.z; ma = sampleVec.y;
                            } else if (sampleVec.y < 0 && std::abs(sampleVec.y) >= maxAxis) {
                                sampleFace = 3; sc = sampleVec.x; tc = -sampleVec.z; ma = -sampleVec.y;
                            } else if (sampleVec.z > 0 && std::abs(sampleVec.z) >= maxAxis) {
                                sampleFace = 4; sc = sampleVec.x; tc = -sampleVec.y; ma = sampleVec.z;
                            } else {
                                sampleFace = 5; sc = -sampleVec.x; tc = -sampleVec.y; ma = -sampleVec.z;
                            }

                            float sampleU = 0.5f * (sc / ma + 1.0f);
                            float sampleV = 0.5f * (tc / ma + 1.0f);

                            int px = std::clamp(static_cast<int>(sampleU * envMap.faceSize), 
                                               0, static_cast<int>(envMap.faceSize) - 1);
                            int py = std::clamp(static_cast<int>(sampleV * envMap.faceSize), 
                                               0, static_cast<int>(envMap.faceSize) - 1);

                            size_t idx = (py * envMap.faceSize + px) * 4;
                            Vec3 envColor(
                                envMap.faces[sampleFace][idx],
                                envMap.faces[sampleFace][idx + 1],
                                envMap.faces[sampleFace][idx + 2]
                            );

                            irradiance += envColor * std::cos(theta) * std::sin(theta);
                            nSamples++;
                        }
                    }

                    irradiance = PI * irradiance / static_cast<float>(nSamples);

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
                                                                         uint32_t numSamples)
    {
        std::vector<CubemapFaces> mipChain;
        mipChain.reserve(numMipLevels);

        for (uint32_t mip = 0; mip < numMipLevels; ++mip)
        {
            float roughness = static_cast<float>(mip) / static_cast<float>(numMipLevels - 1);
            uint32_t mipSize = std::max(1u, outputSize >> mip);

            CubemapFaces mipFaces;
            mipFaces.faceSize = mipSize;

            for (int face = 0; face < CubemapFaces::FACE_COUNT; ++face)
            {
                mipFaces.faces[face].resize(mipSize * mipSize * 4);

                for (uint32_t y = 0; y < mipSize; ++y)
                {
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

                        for (uint32_t i = 0; i < numSamples; ++i)
                        {
                            Vec2 xi = Hammersley(i, numSamples);
                            Vec3 H = ImportanceSampleGGX(xi, N, roughness);
                            Vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);

                            float NdotL = std::max(glm::dot(N, L), 0.0f);
                            if (NdotL > 0.0f)
                            {
                                // Sample environment (simplified)
                                float maxAxis = std::max({std::abs(L.x), std::abs(L.y), std::abs(L.z)});
                                int sampleFace = 0;
                                float sc, tc, ma;

                                if (L.x > 0 && std::abs(L.x) >= maxAxis) {
                                    sampleFace = 0; sc = -L.z; tc = -L.y; ma = L.x;
                                } else if (L.x < 0 && std::abs(L.x) >= maxAxis) {
                                    sampleFace = 1; sc = L.z; tc = -L.y; ma = -L.x;
                                } else if (L.y > 0 && std::abs(L.y) >= maxAxis) {
                                    sampleFace = 2; sc = L.x; tc = L.z; ma = L.y;
                                } else if (L.y < 0 && std::abs(L.y) >= maxAxis) {
                                    sampleFace = 3; sc = L.x; tc = -L.z; ma = -L.y;
                                } else if (L.z > 0 && std::abs(L.z) >= maxAxis) {
                                    sampleFace = 4; sc = L.x; tc = -L.y; ma = L.z;
                                } else {
                                    sampleFace = 5; sc = -L.x; tc = -L.y; ma = -L.z;
                                }

                                float sampleU = 0.5f * (sc / ma + 1.0f);
                                float sampleV = 0.5f * (tc / ma + 1.0f);

                                int px = std::clamp(static_cast<int>(sampleU * envMap.faceSize),
                                                   0, static_cast<int>(envMap.faceSize) - 1);
                                int py = std::clamp(static_cast<int>(sampleV * envMap.faceSize),
                                                   0, static_cast<int>(envMap.faceSize) - 1);

                                size_t idx = (py * envMap.faceSize + px) * 4;
                                Vec3 envColor(
                                    envMap.faces[sampleFace][idx],
                                    envMap.faces[sampleFace][idx + 1],
                                    envMap.faces[sampleFace][idx + 2]
                                );

                                prefilteredColor += envColor * NdotL;
                                totalWeight += NdotL;
                            }
                        }

                        prefilteredColor /= totalWeight;

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

    TextureResource* HDRTextureLoader::GenerateBRDFLUT(uint32_t resolution, uint32_t numSamples)
    {
        ResourceId lutId = GenerateHDRTextureId("__brdf_lut__");

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(lutId))
            {
                return static_cast<TextureResource*>(cached);
            }
        }

        std::vector<float> lutData(resolution * resolution * 2);  // RG16F

        for (uint32_t y = 0; y < resolution; ++y)
        {
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

                for (uint32_t i = 0; i < numSamples; ++i)
                {
                    Vec2 xi = Hammersley(i, numSamples);
                    Vec3 H = ImportanceSampleGGX(xi, N, roughness);
                    Vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);

                    float NdotL = std::max(L.z, 0.0f);
                    float NdotH = std::max(H.z, 0.0f);
                    float VdotH = std::max(glm::dot(V, H), 0.0f);

                    if (NdotL > 0.0f)
                    {
                        float G = NdotL * NdotV;  // Simplified geometry term
                        float G_Vis = (G * VdotH) / (NdotH * NdotV + 0.0001f);
                        float Fc = std::pow(1.0f - VdotH, 5.0f);

                        A += (1.0f - Fc) * G_Vis;
                        B += Fc * G_Vis;
                    }
                }

                A /= static_cast<float>(numSamples);
                B /= static_cast<float>(numSamples);

                size_t idx = (y * resolution + x) * 2;
                lutData[idx] = A;
                lutData[idx + 1] = B;
            }
        }

        // Create texture
        auto* texture = new TextureResource();
        texture->SetId(lutId);
        texture->SetPath("__brdf_lut__");
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
            // Simple float to half conversion (approximate)
            auto floatToHalf = [](float f) -> uint16_t {
                // Simplified conversion - for production use proper half-float conversion
                if (f == 0.0f) return 0;
                uint32_t bits = *reinterpret_cast<uint32_t*>(&f);
                uint16_t sign = (bits >> 16) & 0x8000;
                int32_t exp = ((bits >> 23) & 0xFF) - 127 + 15;
                uint16_t mant = (bits >> 13) & 0x3FF;
                if (exp <= 0) return sign;
                if (exp >= 31) return sign | 0x7C00;
                return sign | (static_cast<uint16_t>(exp) << 10) | mant;
            };

            halfData[i * 4 + 0] = floatToHalf(lutData[i * 2]);
            halfData[i * 4 + 1] = floatToHalf(lutData[i * 2 + 1]);
            halfData[i * 4 + 2] = 0;
            halfData[i * 4 + 3] = floatToHalf(1.0f);
        }

        texture->SetData(std::move(byteData), metadata);

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
                                    std::vector<float>& outPixels,
                                    uint32_t& outWidth, uint32_t& outHeight)
    {
        int width, height, channels;
        float* data = stbi_loadf(path.c_str(), &width, &height, &channels, 4);

        if (!data)
        {
            RVX_CORE_WARN("HDRTextureLoader: stbi_loadf failed: {}", stbi_failure_reason());
            return false;
        }

        outWidth = static_cast<uint32_t>(width);
        outHeight = static_cast<uint32_t>(height);

        size_t pixelCount = static_cast<size_t>(width) * height * 4;
        outPixels.assign(data, data + pixelCount);

        stbi_image_free(data);
        return true;
    }

    bool HDRTextureLoader::LoadEXR(const std::string& path,
                                    std::vector<float>& outPixels,
                                    uint32_t& outWidth, uint32_t& outHeight)
    {
#if HAS_TINYEXR
        float* data = nullptr;
        int width, height;
        const char* err = nullptr;

        // Use :: prefix to call the global tinyexr function, not this member function
        int ret = ::LoadEXR(&data, &width, &height, path.c_str(), &err);
        if (ret != TINYEXR_SUCCESS)
        {
            if (err)
            {
                RVX_CORE_WARN("HDRTextureLoader: tinyexr failed: {}", err);
                ::FreeEXRErrorMessage(err);
            }
            return false;
        }

        outWidth = static_cast<uint32_t>(width);
        outHeight = static_cast<uint32_t>(height);

        size_t pixelCount = static_cast<size_t>(width) * height * 4;
        outPixels.assign(data, data + pixelCount);

        free(data);
        return true;
#else
        RVX_CORE_WARN("HDRTextureLoader: EXR support not compiled in (missing tinyexr)");
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

        auto* texture = new TextureResource();
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

        auto* texture = new TextureResource();
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
