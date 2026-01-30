#pragma once

/**
 * @file HDRTextureLoader.h
 * @brief HDR and EXR texture loader for environment maps
 * 
 * Loads high dynamic range textures for:
 * - Skyboxes (equirectangular or cubemap)
 * - Environment maps
 * - IBL (Image-Based Lighting) with precomputation
 * 
 * Supports:
 * - HDR (Radiance RGBE format) via stb_image
 * - EXR (OpenEXR) via tinyexr
 */

#include "Resource/ResourceManager.h"
#include "Resource/Types/TextureResource.h"
#include "Core/MathTypes.h"
#include <string>
#include <memory>
#include <array>

namespace RVX::Resource
{
    /**
     * @brief IBL data generated from environment map
     */
    struct IBLData
    {
        /// Environment cubemap (original, high resolution)
        TextureResource* environmentMap = nullptr;

        /// Irradiance cubemap (for diffuse IBL)
        TextureResource* irradianceMap = nullptr;

        /// Prefiltered environment map (for specular IBL, with mip chain)
        TextureResource* prefilteredMap = nullptr;

        /// BRDF LUT (2D lookup table)
        TextureResource* brdfLUT = nullptr;

        /// Number of mip levels in prefiltered map
        uint32_t prefilteredMipLevels = 0;

        bool IsValid() const 
        { 
            return environmentMap && irradianceMap && prefilteredMap && brdfLUT; 
        }
    };

    /**
     * @brief HDR loading options
     */
    struct HDRLoadOptions
    {
        /// Generate cubemap from equirectangular map
        bool generateCubemap = true;

        /// Cubemap face resolution (per face)
        uint32_t cubemapResolution = 1024;

        /// Generate IBL data
        bool generateIBL = true;

        /// Irradiance map resolution (per face)
        uint32_t irradianceResolution = 32;

        /// Prefiltered map resolution (per face)
        uint32_t prefilteredResolution = 512;

        /// Number of mip levels for prefiltered map
        uint32_t prefilteredMipLevels = 5;

        /// BRDF LUT resolution
        uint32_t brdfLUTResolution = 512;

        /// Number of samples for IBL convolution
        uint32_t convolutionSamples = 1024;

        /// Apply gamma correction (for non-linear HDR formats)
        bool applyGamma = false;

        /// Exposure multiplier
        float exposure = 1.0f;
    };

    /**
     * @brief Cubemap face data
     */
    struct CubemapFaces
    {
        static constexpr int FACE_COUNT = 6;

        enum Face
        {
            PositiveX = 0,  // Right
            NegativeX = 1,  // Left
            PositiveY = 2,  // Top
            NegativeY = 3,  // Bottom
            PositiveZ = 4,  // Front
            NegativeZ = 5   // Back
        };

        std::array<std::vector<float>, FACE_COUNT> faces;
        uint32_t faceSize = 0;
    };

    /**
     * @brief HDR and EXR texture loader
     * 
     * Features:
     * - Load HDR (Radiance) and EXR (OpenEXR) formats
     * - Convert equirectangular to cubemap
     * - Generate IBL data (irradiance, prefiltered, BRDF LUT)
     * - High precision float textures
     */
    class HDRTextureLoader : public IResourceLoader
    {
    public:
        explicit HDRTextureLoader(ResourceManager* manager);
        ~HDRTextureLoader() override = default;

        // =====================================================================
        // IResourceLoader Interface
        // =====================================================================

        ResourceType GetResourceType() const override { return ResourceType::Texture; }
        std::vector<std::string> GetSupportedExtensions() const override;
        IResource* Load(const std::string& path) override;
        bool CanLoad(const std::string& path) const override;

        // =====================================================================
        // Extended Loading API
        // =====================================================================

        /**
         * @brief Load HDR texture with options
         * 
         * @param path Path to HDR/EXR file
         * @param options Loading options
         * @return TextureResource (equirectangular or cubemap based on options)
         */
        TextureResource* LoadWithOptions(const std::string& path,
                                          const HDRLoadOptions& options);

        /**
         * @brief Load and generate full IBL data
         * 
         * Generates:
         * - Environment cubemap
         * - Irradiance cubemap (for diffuse)
         * - Prefiltered cubemap (for specular, with mips)
         * - BRDF LUT
         * 
         * @param path Path to HDR/EXR file
         * @param options Loading options
         * @return IBL data structure with all generated textures
         */
        IBLData LoadIBL(const std::string& path,
                        const HDRLoadOptions& options = HDRLoadOptions());

        /**
         * @brief Convert equirectangular map to cubemap
         * 
         * @param equirectData Float data (RGBA)
         * @param width Source width
         * @param height Source height
         * @param cubemapSize Target cubemap face size
         * @return Cubemap face data
         */
        CubemapFaces EquirectangularToCubemap(const float* equirectData,
                                               uint32_t width, uint32_t height,
                                               uint32_t cubemapSize);

        // =====================================================================
        // IBL Generation
        // =====================================================================

        /**
         * @brief Generate irradiance cubemap from environment map
         * 
         * Performs cosine-weighted hemisphere convolution for diffuse IBL.
         */
        CubemapFaces GenerateIrradianceMap(const CubemapFaces& envMap,
                                            uint32_t outputSize,
                                            uint32_t numSamples = 1024);

        /**
         * @brief Generate prefiltered environment map
         * 
         * Generates mip chain with increasing roughness for specular IBL.
         */
        std::vector<CubemapFaces> GeneratePrefilteredMap(const CubemapFaces& envMap,
                                                          uint32_t outputSize,
                                                          uint32_t numMipLevels,
                                                          uint32_t numSamples = 1024);

        /**
         * @brief Generate BRDF integration LUT
         * 
         * 2D lookup table for split-sum approximation.
         */
        TextureResource* GenerateBRDFLUT(uint32_t resolution = 512,
                                          uint32_t numSamples = 1024);

        // =====================================================================
        // Default Textures
        // =====================================================================

        /**
         * @brief Get a default black environment map
         */
        TextureResource* GetDefaultEnvironmentMap();

        /**
         * @brief Get a default BRDF LUT (if IBL not generated)
         */
        TextureResource* GetDefaultBRDFLUT();

    private:
        // Loading helpers
        bool LoadHDR(const std::string& path,
                     std::vector<float>& outPixels,
                     uint32_t& outWidth, uint32_t& outHeight);

        bool LoadEXR(const std::string& path,
                     std::vector<float>& outPixels,
                     uint32_t& outWidth, uint32_t& outHeight);

        // Cubemap helpers
        Vec3 GetCubemapDirection(CubemapFaces::Face face, float u, float v) const;
        Vec2 DirectionToEquirectangular(const Vec3& dir) const;

        // IBL helpers
        Vec3 ImportanceSampleGGX(Vec2 xi, Vec3 N, float roughness) const;
        float RadicalInverse_VdC(uint32_t bits) const;
        Vec2 Hammersley(uint32_t i, uint32_t N) const;

        // Texture creation
        TextureResource* CreateCubemapTexture(const CubemapFaces& faces,
                                               const std::string& uniqueKey);
        TextureResource* CreateCubemapTextureWithMips(const std::vector<CubemapFaces>& mipChain,
                                                       const std::string& uniqueKey);

        ResourceId GenerateHDRTextureId(const std::string& uniqueKey);

        ResourceManager* m_manager;
        TextureResource* m_defaultEnvMap = nullptr;
        TextureResource* m_defaultBRDFLUT = nullptr;
    };

} // namespace RVX::Resource
