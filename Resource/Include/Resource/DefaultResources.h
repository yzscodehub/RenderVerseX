#pragma once

/**
 * @file DefaultResources.h
 * @brief Default/fallback resources
 * 
 * Provides access to built-in default resources used when actual
 * resources are missing or fail to load.
 */

#include "Resource/Types/TextureResource.h"
#include "Resource/Types/MaterialResource.h"
#include <memory>

namespace RVX::Resource
{
    /**
     * @brief Default resource provider
     * 
     * Provides access to built-in default resources:
     * - White texture (1x1 white)
     * - Normal texture (1x1 flat normal)
     * - Black texture (1x1 black)
     * - Error texture (magenta checkerboard)
     * - Default material (basic PBR material)
     */
    class DefaultResources
    {
    public:
        // =====================================================================
        // Lifecycle
        // =====================================================================

        /// Initialize default resources
        static void Initialize();

        /// Shutdown and release default resources
        static void Shutdown();

        /// Check if initialized
        static bool IsInitialized();

        // =====================================================================
        // Default Textures
        // =====================================================================

        /// Get white texture (1x1 RGBA white)
        static TextureResource* WhiteTexture();

        /// Get normal texture (1x1 flat normal - (128, 128, 255))
        static TextureResource* NormalTexture();

        /// Get black texture (1x1 RGBA black)
        static TextureResource* BlackTexture();

        /// Get error texture (2x2 magenta checkerboard)
        static TextureResource* ErrorTexture();

        /// Get default texture for a specific usage
        static TextureResource* GetDefaultTexture(TextureUsage usage);

        // =====================================================================
        // Default Materials
        // =====================================================================

        /// Get default material (basic white PBR material)
        static MaterialResource* DefaultMaterial();

        /// Get error material (magenta material)
        static MaterialResource* ErrorMaterial();

    private:
        static bool s_initialized;
        static std::unique_ptr<TextureResource> s_whiteTexture;
        static std::unique_ptr<TextureResource> s_normalTexture;
        static std::unique_ptr<TextureResource> s_blackTexture;
        static std::unique_ptr<TextureResource> s_errorTexture;
        static std::unique_ptr<MaterialResource> s_defaultMaterial;
        static std::unique_ptr<MaterialResource> s_errorMaterial;

        static TextureResource* CreateSolidTexture(const std::string& name, 
                                                     uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                                     bool isSRGB = true);
        static TextureResource* CreateCheckerTexture(const std::string& name,
                                                       uint8_t r1, uint8_t g1, uint8_t b1,
                                                       uint8_t r2, uint8_t g2, uint8_t b2);
    };

} // namespace RVX::Resource
